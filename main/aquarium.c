// main/aquarium.c —— ASCII 鱼缸应用主体（独立 play，不经过 demo 菜单，不使用 LVGL）。
//
// 30x40 字符网格（每格 8x8 像素）。场景每帧合成到字符网格，再按 20 条横带把
// 字符点阵画进 RGB565 缓冲、直接推给面板。绕开 LVGL 控件是有意为之：1200 个
// label 放不进没有 PSRAM 的 RAM，而这条路实测 17.9 ms/帧。
//
// 真机调参记录（2026-11-18 第一版上机后修正）：
//   1. 面板是 BGR 顺序，打包必须把 R 与 B 对调，否则橙鱼显成蓝鱼、深蓝水显成紫黄
//   2. 固定 30 fps 节拍：不节拍时循环自由跑到 50+ fps，动作快得没有水下感，还伴随时快时慢
//   3. 鱼、气泡、鱼食都放慢，鱼食改成固定 '*' 并一次撒三粒，不再闪烁
#include "bsp_display.h"
#include "fish_sim.h"
#include "idle_policy.h"
#include "ascii_font.h"
#include "aquarium_bg.h"
#include "sprites.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "aquarium";

#define COLS        FISH_SIM_COLS
#define ROWS        FISH_SIM_ROWS
#define CELL        8
#define STRIP_ROWS  1        // 一条带 = 一个字符行（8 像素）
#define STRIP_H     (STRIP_ROWS * CELL)
#define STRIPS      (ROWS / STRIP_ROWS)
#define STRIP_BYTES (STRIP_PX * 2)
// 一条带在 80 MHz 上的理论传输时间；加 15% 余量覆盖排队与开销。
#define STRIP_TRANSFER_US ((int64_t)STRIP_BYTES * 8 / 80 * 115 / 100)
#define STRIP_PX    (COLS * CELL * STRIP_H)

// 生物的像素精灵在 sprites.c 里；这里只按种类取尺寸并叠到条带上。
#define SPRITE_W         26
#define SPRITE_H         16
#define SPR_PIX_NONE     0
#define SPR_PIX_BODY     1
#define SPR_PIX_DARK     2
#define SPR_PIX_LIGHT    3
#define SPR_PIX_EYE      4
#define SPR_PIX_PUPIL    5
#define BUBBLE_COUNT     7

// 默认不打印：USB-JTAG 控制台在没有主机读取时会阻塞，页内持续打日志会让渲染
// 任务卡死（真机复现：连按 OK 投喂后画面冻结）。要调性能时临时改成 1。
#define AQUARIUM_DIAG_LOG 0
#define LOG_EVERY        300

static char     s_ch[ROWS][COLS];
static uint16_t s_fg[ROWS][COLS];
// 上一帧的内容，用来判断哪条带真的变了。静止的行不必重推，撕裂窗口随之大幅缩短。
static char     s_prev_ch[ROWS][COLS];
static uint16_t s_prev_fg[ROWS][COLS];
// 水体颜色按像素行算（底图里 index 0 就表示水）：三段渐变，字符行只有 40 条，
// 8 像素一条台阶会看成色带。底图只存植物/沉木/底床/灯条，不存水。
static uint16_t s_water_px[ROWS * CELL];

#define NBUF 2
static uint16_t *s_strip[NBUF];   // 双缓冲：一块在 DMA 里传时，另一块可写
static esp_lcd_panel_handle_t s_panel;
static TaskHandle_t s_task;
static volatile bool s_run;

static fish_sim_t s_sim;

// 投喂请求队列：按键任务只投递，渲染任务独占 s_sim 消费。
// 早先两个任务同时改鱼群状态，连按时会互相踩，真机上表现为画面冻结。
static QueueHandle_t s_feed_q;
static uint32_t s_tick;
// 精灵覆盖的字符行掩码（40 行）。字符网格的脏检测看不到精灵，所以鱼游过或游走
// 的那几行必须强制重绘 —— 否则屏幕上会留下残影（真机验证过）。
static uint64_t s_sprite_rows_now;
static uint64_t s_sprite_rows_prev;
// 草尖摇摆：只挑 3 丛前景草，最上面几像素左右偏 1 px。
// 这些行每帧都从底图重铺再画，所以不会重影（和精灵一个道理），只需强制标脏。
#define SWAY_CLUMPS 3
#define SWAY_BASE_Y 272        // 草皮上沿（像素行）
#define SWAY_HEIGHT 6          // 摆动的那几像素（4 太短，看不出摆动）

typedef struct {
    int16_t x;                 // 草尖基准列（像素）
    int8_t  off;               // 当前偏移 -1 / 0 / 1
} sway_t;

static sway_t s_sway[SWAY_CLUMPS];
static uint64_t s_sway_rows;   // 需要强制重绘的字符行

// 摆动每 10 个 tick 才换一次相位，所以只在相位变化的那一帧重绘这些行。
// 每帧都重绘会把这 15 行变成常驻开销，实测 work 从 8.4 ms 涨到 19.8 ms 并触发看门狗。
static bool s_sway_dirty;
static bool s_sway_allowed = true;
// 常驻动画行的预算：摆动 + 气泡 + 精灵合计不超过这么多行，超了先砍摆动。
// 大面积整行位移实测会把 work 从 8.4 ms 推到 19.8 ms 并触发看门狗，所以这里设硬上限。
#define ANIM_ROW_BUDGET 22

static uint8_t  s_backlight = 100;
static power_state_t s_power = POWER_ACTIVE;
static int64_t  s_last_input_us;
static volatile bool s_force_full;   // 从息屏唤醒后要整屏重画一次

static uint16_t s_c_bubble;
static uint16_t s_c_blade_light, s_c_blade_dark;
static uint16_t s_c_food;
// 气泡：每 period 帧上浮一格（远慢于帧率，才有水里慢慢升起的感觉）。
typedef struct {
    int16_t x, y;
    uint8_t period;
    uint8_t phase;
} bubble_t;

static bubble_t s_bubbles[BUBBLE_COUNT];

// 屏幕圆角遮罩。BSP 的圆角是 LVGL 刷屏时做的，我们绕过 LVGL，就得自己在
// 填像素时把圆角外的像素涂黑，否则四角会是直角。
#define CORNER_R BSP_LVGL_SCREEN_RADIUS
static uint8_t s_inset[ROWS * CELL];        // 圆外：纯黑
static uint8_t s_inset_soft[ROWS * CELL];   // 圆边上的一像素：半亮，做抗锯齿

static void mask_init(void) {
    for (int py = 0; py < ROWS * CELL; py++) {
        int dy = 0;
        if (py < CORNER_R) dy = CORNER_R - py;
        else if (py >= ROWS * CELL - CORNER_R) dy = py - (ROWS * CELL - CORNER_R - 1);
        uint8_t inset = 0, soft = 0;
        if (dy > 0) {
            // 取"仍在圆内"的最大 dx：反过来从小到大找会把整行都判成圆外，
            // 四角会变成方块缺口（真机出现过）。
            for (int dx = CORNER_R - 1; dx >= 0; dx--) {
                if (dx * dx + dy * dy <= CORNER_R * CORNER_R) { inset = (uint8_t)(CORNER_R - dx); break; }
            }
            for (int dx = CORNER_R - 1; dx >= 0; dx--) {
                if (dx * dx + dy * dy <= (CORNER_R - 1) * (CORNER_R - 1)) { soft = (uint8_t)(CORNER_R - dx); break; }
            }
        }
        s_inset[py] = inset;
        s_inset_soft[py] = soft;
    }
}

// RGB565 打包，并把高/低字节对调好再返回。
//
// 面板要求高字节先发（bsp 给 LVGL 设的 swap_bytes=true 就是这个原因），而
// uint16_t 在内存里是小端，直接推出去等于每个像素的字节被反着发 —— 橙色会
// 变成浅蓝、深蓝水会变成紫红。所有颜色都在这里一次换好。
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

static void palette_init(void) {
    // 动态元素配色。上一轮清理旧配色时把这两行一起删掉了，导致气泡和鱼食
    // 拿到 0（纯黑）—— 真机上就是"气泡变黑"。
    s_c_bubble = rgb565(72, 132, 152);   // 压暗一档：屏上像"水里的气泡"，不是发光点
    s_c_blade_light = rgb565(125, 220, 88);
    s_c_blade_dark  = rgb565(44, 138, 57);
    s_c_food   = rgb565(255, 245, 170);

    // 水体三段渐变（取自草图）：顶部偏绿、中部深蓝、底部近黑。
    static const uint8_t stops[3][3] = {
        { 0x1b, 0x6a, 0x58 },   // 顶
        { 0x0b, 0x33, 0x40 },   // 中
        { 0x06, 0x1a, 0x22 },   // 底
    };
    for (int py = 0; py < ROWS * CELL; py++) {
        int q = py * 2 * 1000 / (ROWS * CELL - 1);        // 0..2000
        int seg = q < 1000 ? 0 : 1;
        int local = (q - seg * 1000) * 255 / 1000;        // 0..255
        const uint8_t *a = stops[seg];
        const uint8_t *b = stops[seg + 1];
        uint8_t r = (uint8_t)((a[0] * (255 - local) + b[0] * local) / 255);
        uint8_t g = (uint8_t)((a[1] * (255 - local) + b[1] * local) / 255);
        uint8_t bb = (uint8_t)((a[2] * (255 - local) + b[2] * local) / 255);
        s_water_px[py] = rgb565(r, g, bb);
    }
}

static void put_cell(int x, int y, char ch, uint16_t fg) {
    if (x < 0 || x >= COLS || y < 0 || y >= ROWS) return;
    s_ch[y][x] = ch;
    s_fg[y][x] = fg;
}

static void scene_clear(void) {
    for (int y = 0; y < ROWS; y++) {
        memset(s_ch[y], ' ', COLS);
    }
}

static void bubble_step(void) {
    for (int i = 0; i < BUBBLE_COUNT; i++) {
        bubble_t *b = &s_bubbles[i];
        b->phase++;
        if (b->phase < b->period) continue;
        b->phase = 0;
        b->y--;
        if (b->y < 0) {
            b->y = ROWS - 1;
            b->x = (int16_t)((b->x + 6 + i) % COLS);
        }
    }
}

static void scene_bubbles(void) {
    for (int i = 0; i < BUBBLE_COUNT; i++) {
        const bubble_t *b = &s_bubbles[i];
        put_cell(b->x, b->y, 'o', s_c_bubble);
    }
}

static void scene_food(void) {
    // 碎屑感：不用整格的 '@'/'*'，改用点状字符，三种交替，像散落的饲料碎屑。
    // 字模里 '.' 是 2x2 的小点、',' 是带尾的小点、'\'' 是很短的撇，都远小于一格。
    static const char CRUMB[3] = { '.', ',', '\'' };
    for (uint8_t i = 0; i < s_sim.food_count; i++) {
        int x = fish_sim_cell_x(s_sim.food[i].x);
        int y = fish_sim_cell_y(s_sim.food[i].y);
        put_cell(x, y, CRUMB[i % 3], s_c_food);
    }
}

static void scene_compose(void) {
    scene_clear();
    scene_food();
    scene_bubbles();
}

static bool strip_changed(int strip) {
    for (int r = strip * STRIP_ROWS; r < (strip + 1) * STRIP_ROWS; r++) {
        if (memcmp(s_ch[r], s_prev_ch[r], COLS) != 0) return true;
        if (memcmp(s_fg[r], s_prev_fg[r], COLS * sizeof(uint16_t)) != 0) return true;
    }
    return false;
}

static void strip_mark_drawn(int strip) {
    for (int r = strip * STRIP_ROWS; r < (strip + 1) * STRIP_ROWS; r++) {
        memcpy(s_prev_ch[r], s_ch[r], COLS);
        memcpy(s_prev_fg[r], s_fg[r], COLS * sizeof(uint16_t));
    }
}

static void fill_strip(uint16_t *dst, int strip) {
    // 1) 底图：每个像素来自索引图；index 0 用该行的水色
    for (int row = 0; row < STRIP_ROWS; row++) {
        int gy = strip * STRIP_ROWS + row;
        for (int y = 0; y < CELL; y++) {
            int py = gy * CELL + y;
            const uint8_t *src = &s_bg_idx[(size_t)py * BG_W];
            uint16_t *line = dst + (size_t)(row * CELL + y) * (COLS * CELL);
            const uint16_t water = s_water_px[py];
            for (int x = 0; x < BG_W; x++) {
                uint8_t idx = src[x];
                line[x] = idx ? s_bg_pal[idx] : water;
            }
        }
    }

    // 1a) 草尖：在底图之上补几像素，尖端随相位偏 1 px
    {
        const int top = strip * STRIP_H;
        for (int c = 0; c < SWAY_CLUMPS; c++) {
            const int8_t off = s_sway[c].off;
            // 每丛两根叶片：左边高的跟摆动，右边矮的慢半拍
            for (int blade = 0; blade < 2; blade++) {
                const int bx = s_sway[c].x + blade * 3;
                const int height = SWAY_HEIGHT - blade * 2;
                const int8_t boff = blade ? (int8_t)(off / 2) : off;   // 第二片慢半拍（整数，避免无 FPU 的浮点）
                for (int k = 0; k < height; k++) {
                    const int y = SWAY_BASE_Y - k;
                    if (y < top || y >= top + STRIP_H) continue;
                    // 越靠尖越跟得上摆动，根部不动
                    const int lean = (k * 2 >= height) ? boff : 0;
                    const int px = bx + lean;
                    if (px < 0 || px >= COLS * CELL) continue;
                    uint16_t *line = dst + (size_t)(y - top) * (COLS * CELL);
                    line[px] = (k < 2) ? s_c_blade_light : s_c_blade_dark;
                }
            }
        }
    }

    // 1b) 字符层：只叠动态内容（气泡、鱼食），空白格跳过
    for (int row = 0; row < STRIP_ROWS; row++) {
        int gy = strip * STRIP_ROWS + row;
        for (int y = 0; y < CELL; y++) {
            uint16_t *line = dst + (size_t)(row * CELL + y) * (COLS * CELL);
            for (int col = 0; col < COLS; col++) {
                char ch = s_ch[gy][col];
                if (ch == ' ') continue;
                const uint8_t *bits = ascii_font_glyph(ch);
                uint8_t b = bits[y];
                if (b == 0) continue;
                uint16_t fg = s_fg[gy][col];
                uint16_t *out = line + col * CELL;
                for (int x = 0; x < CELL; x++) {
                    if (b & (0x80u >> x)) out[x] = fg;
                }
            }
        }
    }

    // 2) 精灵层：按各自种类取精灵与尺寸（三种生物共用同一条 blit）
    const int sprite_dy = -strip * STRIP_H;
    for (uint8_t i = 0; i < s_sim.fish_count; i++) {
        const fish_sim_fish_t *f = &s_sim.fish[i];
        const sprite_kind_t kind = (sprite_kind_t)(f->kind % CRE_KIND_COUNT);
        const int w = sprite_w(kind), h = sprite_h(kind);
        const int px = (int)((int32_t)f->x * CELL / FISH_SIM_ONE) - w / 2;
        const int py = (int)((int32_t)f->y * CELL / FISH_SIM_ONE) - h / 2;
        sprite_blit(dst, COLS * CELL, STRIP_H, kind, px, py + sprite_dy,
                    fish_sim_dir_dx(f->dir) < 0);
    }

    // 3) 圆角遮罩（放在最后，精灵也不能越过圆角）
    for (int row = 0; row < STRIP_ROWS; row++) {
        int gy = strip * STRIP_ROWS + row;
        for (int y = 0; y < CELL; y++) {
            int inset = s_inset[gy * CELL + y];
            int soft = s_inset_soft[gy * CELL + y];
            if (inset == 0 && soft == 0) continue;
            uint16_t *line = dst + (size_t)(row * CELL + y) * (COLS * CELL);
            const int w = COLS * CELL;
            for (int x = 0; x < w; x++) {
                if (x < inset || x >= w - inset) line[x] = 0x0000;
                else if (x < soft || x >= w - soft) line[x] = (uint16_t)((line[x] >> 1) & 0x7BEF);
            }
        }
    }
}

static uint64_t bubble_rows_mask(void) {
    uint64_t mask = 0;
    for (int i = 0; i < BUBBLE_COUNT; i++) {
        int r = s_bubbles[i].y;
        if (r >= 0 && r < ROWS) mask |= (1ULL << r);
    }
    return mask;
}

static void init_sway(void) {
    // 选在草皮上方的"空隙"列：这样尖端是贴着水体摆的，看得见。
    // （选在已有的高草丛里，摆动会被草色吃掉。）
    static const int16_t xs[SWAY_CLUMPS] = { 44, 148, 202 };
    for (int i = 0; i < SWAY_CLUMPS; i++) {
        s_sway[i].x = xs[i];
        s_sway[i].off = 0;
    }
    s_sway_rows = 0;
    // 只有 3 丛草尖会动，所以脏行就这两行
    for (int y = SWAY_BASE_Y - SWAY_HEIGHT; y <= SWAY_BASE_Y; y++) {
        s_sway_rows |= (1ULL << (y / CELL));
    }
}

static void sway_step(void) {
    // 每 16 个 tick（30 Hz）换一档，8 档一个来回 ≈ 4.3 秒；只在这一帧标脏。
    // 之前每 10 tick 一档（2.7 秒）偏快，长时间看会晕。
    static const int8_t pattern[8] = { 0, 1, 2, 1, 0, -1, -2, -1 };
    static uint32_t last_phase = 0xFFFFFFFFu;
    const uint32_t phase = (s_tick / 16) & 7;
    if (phase != last_phase) {
        last_phase = phase;
        s_sway_dirty = true;
    }
    for (int i = 0; i < SWAY_CLUMPS; i++) {
        s_sway[i].off = pattern[(phase + (uint32_t)i * 3) & 7];
    }
}

static void init_scene_objects(void) {
    for (int i = 0; i < BUBBLE_COUNT; i++) {
        s_bubbles[i].x = (int16_t)((i * 4 + 2) % COLS);
        s_bubbles[i].y = (int16_t)((i * 6 + 3) % ROWS);
        s_bubbles[i].period = (uint8_t)(7 + (i % 5));   // 每 7~11 帧升一格
        s_bubbles[i].phase = 0;
    }
}

static void aquarium_task(void *arg) {
    (void)arg;
#if AQUARIUM_DIAG_LOG
    int64_t sum = 0;
    int frames = 0;
#endif
    s_last_input_us = esp_timer_get_time();
    s_force_full = true;
    int64_t last_sim_us = esp_timer_get_time();
    const int64_t sim_period_us = 33333;   // 30 Hz

    ESP_LOGI(TAG, "start: %dx%d cells, %u creatures, %d strips of %u B, heap %u",
             COLS, ROWS, (unsigned)s_sim.fish_count, STRIPS, (unsigned)STRIP_BYTES,
             (unsigned)esp_get_free_heap_size());

    while (s_run) {
        // 省电：按"多久没按键"推进状态。SLEEP 时完全不渲染，只等按键。
        const int64_t idle_us = esp_timer_get_time() - s_last_input_us;
        const power_state_t next = power_state_next(s_power, idle_us);
        if (next != s_power) {
            if (next == POWER_SLEEP) bsp_display_backlight(0);
            s_power = next;
        }
        const int target_fps = power_state_target_fps(s_power);
        if (target_fps == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const int64_t frame_start = esp_timer_get_time();

        uint8_t feed_req;
        if (xQueueReceive(s_feed_q, &feed_req, 0) == pdTRUE) {
            // 每次投喂的粒数、落点、下落速度都是随机的：手撒鱼食本来就不均匀，
            // 固定三粒会像按了"发放三次"的按钮。1~4 粒，落点散布在中间区域。
            const uint8_t pieces = (uint8_t)(1 + (esp_random() % 4));
            for (uint8_t i = 0; i < pieces; i++) {
                int spread = (int)(esp_random() % 7) - 3;          // -3..3 格
                int16_t x = (int16_t)(((COLS / 2) + spread) * FISH_SIM_ONE);
                if (x < FISH_SIM_ONE) x = FISH_SIM_ONE;
                if (x > (COLS - 2) * FISH_SIM_ONE) x = (int16_t)((COLS - 2) * FISH_SIM_ONE);
                int16_t vy = (int16_t)(42 + (esp_random() % 10));  // 0.16~0.20 格/步
                fish_sim_drop_food_ex(&s_sim, x, 0, vy, 1500);
            }
        }

        // 模拟按墙钟 30 Hz 推进：帧率怎么变，鱼速都不变（以实际时刻对齐，
        // 不做累加，避免长时间运行后的漂移）。
        int64_t now_us = esp_timer_get_time();
        int sim_steps = 0;
        if (now_us - last_sim_us >= sim_period_us) {
            last_sim_us = now_us;
            sim_steps = 1;
        }
        if (sim_steps > 0) {
            fish_sim_step(&s_sim);
            bubble_step();
            s_tick++;
            sway_step();
        }
        scene_compose();

        // 记录精灵这一帧覆盖了哪些字符行（按像素包围盒换算）。
        // 字符层的脏检测看不到精灵，所以精灵覆盖的行必须强制重绘，否则留残影。
        s_sprite_rows_prev = s_sprite_rows_now;
        s_sprite_rows_now = 0;
        for (uint8_t i = 0; i < s_sim.fish_count; i++) {
            const fish_sim_fish_t *f = &s_sim.fish[i];
            const int h = sprite_h((sprite_kind_t)(f->kind % CRE_KIND_COUNT));
            int py = (int)((int32_t)f->y * CELL / FISH_SIM_ONE) - h / 2;
            int r0 = py / CELL, r1 = (py + h - 1) / CELL;
            if (r0 < 0) r0 = 0;
            if (r1 >= ROWS) r1 = ROWS - 1;
            for (int r = r0; r <= r1; r++) s_sprite_rows_now |= (1ULL << r);
        }

        // 常驻动画行预算：精灵覆盖行 + 气泡所在行 + 摆动行，超了就砍摆动
        const uint64_t anim_rows = (s_sprite_rows_now | s_sprite_rows_prev) |
                                   bubble_rows_mask() | (s_sway_dirty ? s_sway_rows : 0);
        const bool s_sway_allowed_now = __builtin_popcountll(anim_rows) <= ANIM_ROW_BUDGET;
        if (!s_sway_allowed_now && s_sway_dirty) {
            // 超预算：这一帧只把摆动行让出来，精灵和气泡照常
        }
        s_sway_allowed = s_sway_allowed_now;

        for (int s = 0; s < STRIPS; s++) {
            const uint64_t row_bit = (1ULL << s);
            const bool sprite_touched = ((s_sprite_rows_now | s_sprite_rows_prev) & row_bit) != 0;
            const bool sway_touched = s_sway_dirty && (s_sway_rows & row_bit) != 0 &&
                                      s_sway_allowed;
            if (!s_force_full && !sprite_touched && !sway_touched && !strip_changed(s)) continue;
            int buf = s & (NBUF - 1);
            fill_strip(s_strip[buf], s);
            esp_lcd_panel_draw_bitmap(s_panel, 0, s * STRIP_H, COLS * CELL,
                                      (s + 1) * STRIP_H, s_strip[buf]);
            // 硬等一个传输时间再碰这块缓冲。之前用"按时间戳累加"估算队列，
            // 帧率一高估算就偏小，于是写到了正在被 DMA 读的内存 —— 满屏花。
            // 40 行 × 约 0.45 ms ≈ 18 ms/帧，正好是 SPI 带宽的天花板。
            esp_rom_delay_us((uint32_t)STRIP_TRANSFER_US);
            strip_mark_drawn(s);
        }
        s_force_full = false;
        s_sway_dirty = false;

        // 按目标帧间隔补齐延时。tick 是 1 ms（CONFIG_FREERTOS_HZ=1000），所以之前
        // 的固定 vTaskDelay(1) 只让出 1 ms，满档实际跑到 110+ fps —— 纯浪费电。
        // 现在用 idle_policy 给的目标帧率算预算：55 fps → 18.2 ms/帧。
        // 同时也保证每帧至少让出一次 CPU，避免饿死 IDLE 触发看门狗。
        const int64_t budget_us = 1000000 / target_fps;
        const int64_t spent_us = esp_timer_get_time() - frame_start;
        if (spent_us < budget_us) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)((budget_us - spent_us) / 1000)));
        }
#if AQUARIUM_DIAG_LOG
        sum += spent_us;
        frames++;
#endif
#if AQUARIUM_DIAG_LOG
        if (frames >= LOG_EVERY) {
            float work = (float)sum / frames / 1000.0f;
            ESP_LOGI(TAG, "work=%.1f ms/帧 heap=%u food=%u 动画行: 精灵%u 气泡%u 摆动%u 预算%d",
                     work, (unsigned)esp_get_free_heap_size(), s_sim.food_count,
                     (unsigned)__builtin_popcountll(s_sprite_rows_now | s_sprite_rows_prev),
                     (unsigned)__builtin_popcountll(bubble_rows_mask()),
                     (unsigned)__builtin_popcountll(s_sway_rows),
                     ANIM_ROW_BUDGET);
            ESP_LOGI(TAG, "动画行合计 %u（预算 %d，%s）",
                     (unsigned)__builtin_popcountll(
                         (s_sprite_rows_now | s_sprite_rows_prev) | bubble_rows_mask() | s_sway_rows),
                     ANIM_ROW_BUDGET, s_sway_allowed ? "摆动保留" : "摆动被砍");
            sum = 0;
            frames = 0;
        }
#endif

    }

    ESP_LOGI(TAG, "task stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t aquarium_start(void) {
    s_panel = bsp_display_panel();
    if (!s_panel) {
        ESP_LOGE(TAG, "no panel handle");
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < NBUF; i++) {
        s_strip[i] = heap_caps_malloc((size_t)STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!s_strip[i]) {
            ESP_LOGE(TAG, "strip alloc failed (%d)", i);
            for (int j = 0; j < i; j++) heap_caps_free(s_strip[j]);
            for (int j = 0; j < NBUF; j++) s_strip[j] = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    s_feed_q = xQueueCreate(4, sizeof(uint8_t));
    if (!s_feed_q) {
        ESP_LOGE(TAG, "feed queue alloc failed");
        for (int j = 0; j < NBUF; j++) { heap_caps_free(s_strip[j]); s_strip[j] = NULL; }
        return ESP_ERR_NO_MEM;
    }

    palette_init();
    init_sway();
    sprites_init();
    mask_init();
    init_scene_objects();
    fish_sim_init_mixed(&s_sim, 20261118u);
    s_tick = 0;
    s_run = true;

    if (xTaskCreate(aquarium_task, "aquarium", 4096, NULL, 5, &s_task) != pdPASS) {
        s_run = false;
        for (int j = 0; j < NBUF; j++) { heap_caps_free(s_strip[j]); s_strip[j] = NULL; }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void aquarium_feed(void) {
    if (!s_feed_q) return;
    uint8_t req = 1;
    (void)xQueueSend(s_feed_q, &req, 0);   // 队列满就丢，连按不会堆积
}

void aquarium_brightness_step(int delta) {
    int level = (int)s_backlight + delta;
    if (level < 20) level = 20;
    if (level > 100) level = 100;
    s_backlight = (uint8_t)level;
    bsp_display_backlight(s_backlight);
}

void aquarium_note_activity(void) {
    s_last_input_us = esp_timer_get_time();
    if (s_power != POWER_ACTIVE) {
        s_power = POWER_ACTIVE;
        bsp_display_backlight(s_backlight);   // 从息屏回来：恢复用户的亮度
        s_force_full = true;                  // 屏上可能是旧的，整屏重画
    }
}

esp_err_t aquarium_stop(void) {
    s_run = false;
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    // 任务退出时最后一批条带可能还在 SPI 里传。等足一个明显宽的余量再释放：
    // 一旦 DMA 读到已释放的内存，LVGL 的刷屏会永远等不到完成信号，退出后
    // 菜单画不出来 —— 真机上就是"画面静止不动"。宁可多等 120 ms。
    esp_rom_delay_us(60 * 1000);
    for (int j = 0; j < NBUF; j++) {
        if (s_strip[j]) { heap_caps_free(s_strip[j]); s_strip[j] = NULL; }
    }
    if (s_feed_q) {
        vQueueDelete(s_feed_q);
        s_feed_q = NULL;
    }
    return ESP_OK;
}
