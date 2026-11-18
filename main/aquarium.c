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
#include "ascii_font.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
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

#define FISH_COUNT       14
#define BUBBLE_COUNT     7
#define SEAWEED_COUNT    7
#define PLANKTON_COUNT   10

// 默认不打印：USB-JTAG 控制台在没有主机读取时会阻塞，页内持续打日志会让渲染
// 任务卡死（真机复现：连按 OK 投喂后画面冻结）。要调性能时临时改成 1。
#define AQUARIUM_DIAG_LOG 0
#define LOG_EVERY        300

static char     s_ch[ROWS][COLS];
static uint16_t s_fg[ROWS][COLS];
// 上一帧的内容，用来判断哪条带真的变了。静止的行不必重推，撕裂窗口随之大幅缩短。
static char     s_prev_ch[ROWS][COLS];
static uint16_t s_prev_fg[ROWS][COLS];
// 背景按“像素行”算而不是按字符行算：字符行只有 40 条，8 像素一条台阶会看成色带。
static uint16_t s_bg_px[ROWS * CELL];

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
static uint8_t  s_backlight = 100;

static uint16_t s_c_fish;
static uint16_t s_c_fish_dim;
static uint16_t s_c_weed;
static uint16_t s_c_bubble;
static uint16_t s_c_food;
static uint16_t s_c_plankton;

typedef struct {
    int16_t x, y;
    uint8_t period;     // 每几帧上浮一格
    uint8_t phase;
} bubble_t;

static bubble_t   s_bubbles[BUBBLE_COUNT];

// RGB565 打包，并把高/低字节对调好再返回。
//
// 面板要求高字节先发（bsp 给 LVGL 设的 swap_bytes=true 就是这个原因），而
// uint16_t 在内存里是小端，直接推出去等于每个像素的字节被反着发 —— 橙色会
// 变成浅蓝、深蓝水会变成紫红。所有颜色都在这里一次换好，内层填像素不再付代价。
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

// 屏幕圆角遮罩。BSP 的圆角是 LVGL 刷屏时做的，我们绕过 LVGL，就得自己在
// 填像素时把圆角外的像素涂黑，否则四角会是直角（和产品外观不符）。
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
            // 取"仍在圆内"的最大 dx：找最小的切边。反过来从小到大找，会把整行
            // 都判成圆外，四角就变成方块缺口（真机就是这个现象）。
            for (int dx = CORNER_R - 1; dx >= 0; dx--) {
                if (dx * dx + dy * dy <= CORNER_R * CORNER_R) {
                    inset = (uint8_t)(CORNER_R - dx);
                    break;
                }
            }
            for (int dx = CORNER_R - 1; dx >= 0; dx--) {
                if (dx * dx + dy * dy <= (CORNER_R - 1) * (CORNER_R - 1)) {
                    soft = (uint8_t)(CORNER_R - dx);
                    break;
                }
            }
        }
        s_inset[py] = inset;
        s_inset_soft[py] = soft;
    }
}

static void palette_init(void) {
    s_c_fish      = rgb565(255, 150, 60);
    s_c_fish_dim  = rgb565(180, 95, 40);
    s_c_weed      = rgb565(52, 150, 92);
    s_c_bubble    = rgb565(120, 190, 210);
    s_c_food      = rgb565(255, 245, 170);
    s_c_plankton  = rgb565(26, 52, 74);   // 压暗一档，别和鱼混在一起

    for (int py = 0; py < ROWS * CELL; py++) {
        // 竖直渐变：水面在顶部略亮，越往下越暗。
        int t = py * 255 / (ROWS * CELL - 1);
        uint8_t r = (uint8_t)((14 * (255 - t) + 2 * t) / 255);
        uint8_t g = (uint8_t)((48 * (255 - t) + 7 * t) / 255);
        uint8_t b = (uint8_t)((80 * (255 - t) + 18 * t) / 255);
        s_bg_px[py] = rgb565(r, g, b);
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

static void scene_plankton(void) {
    // 固定的微尘，让画面不是纯空背景
    for (int i = 0; i < PLANKTON_COUNT; i++) {
        int x = (i * 7 + 3) % COLS;
        int y = (i * 11 + 5) % (ROWS - 6);
        put_cell(x, y, '.', s_c_plankton);
    }
}

static void scene_seaweed(void) {
    for (int i = 0; i < SEAWEED_COUNT; i++) {
        int col = (i * 5 + 2) % COLS;
        int height = 5 + ((i * 3) % 6);
        for (int h = 0; h < height; h++) {
            int row = ROWS - 1 - h;
            char ch = '|';
            // 每 10 帧换一次摆动方向，摆动要慢才像水草。
            int sway = (int)((s_tick / 10 + (uint32_t)h / 2) % 3);
            if (sway == 0) ch = '(';
            else if (sway == 1) ch = ')';
            put_cell(col, row, ch, s_c_weed);
        }
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
    for (uint8_t i = 0; i < s_sim.food_count; i++) {
        int x = fish_sim_cell_x(s_sim.food[i].x);
        int y = fish_sim_cell_y(s_sim.food[i].y);
        put_cell(x, y, '@', s_c_food);
    }
}

static void scene_fish(void) {
    for (uint8_t i = 0; i < s_sim.fish_count; i++) {
        const fish_sim_fish_t *f = &s_sim.fish[i];
        int x = fish_sim_cell_x(f->x);
        int y = fish_sim_cell_y(f->y);
        int8_t dx = fish_sim_dir_dx(f->dir);
        int8_t dy = fish_sim_dir_dy(f->dir);

        // 三格一条鱼：头 + 身体 + 尾巴。头用亮色，身体和尾巴压暗，看起来才有体积。
        char head;
        if (dx > 0) head = '>';
        else if (dx < 0) head = '<';
        else if (dy > 0) head = 'v';
        else if (dy < 0) head = '^';
        else head = 'o';

        put_cell(x, y, head, s_c_fish);
        put_cell(x - dx, y - dy, dx != 0 ? '=' : '|', s_c_fish_dim);
        put_cell(x - 2 * dx, y - 2 * dy, head, s_c_fish_dim);
    }
}

static void scene_compose(void) {
    scene_clear();
    scene_plankton();
    scene_seaweed();
    scene_food();
    scene_fish();
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
    for (int row = 0; row < STRIP_ROWS; row++) {
        int gy = strip * STRIP_ROWS + row;
        for (int y = 0; y < CELL; y++) {
            uint16_t bg = s_bg_px[gy * CELL + y];
            uint16_t *line = dst + (size_t)(row * CELL + y) * (COLS * CELL);
            int inset = s_inset[gy * CELL + y];
            int soft = s_inset_soft[gy * CELL + y];
            for (int col = 0; col < COLS; col++) {
                const uint8_t *bits = ascii_font_glyph(s_ch[gy][col]);
                uint8_t b = bits[y];
                uint16_t fg = s_fg[gy][col];
                uint16_t *out = line + col * CELL;
                if (b == 0) {
                    for (int x = 0; x < CELL; x++) out[x] = bg;
                } else {
                    for (int x = 0; x < CELL; x++) out[x] = (b & (0x80u >> x)) ? fg : bg;
                }
                // 圆角遮罩：圆外纯黑，圆边一像素半亮（廉价抗锯齿，避免硬齿）
                if (inset > 0 || soft > 0) {
                    const int w = COLS * CELL;
                    for (int x = 0; x < CELL; x++) {
                        int px = col * CELL + x;
                        if (px < inset || px >= w - inset) {
                            out[x] = 0x0000;
                        } else if (px < soft || px >= w - soft) {
                            out[x] = (uint16_t)((out[x] >> 1) & 0x7BEF);
                        }
                    }
                }
            }
        }
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
    bool force_full = true;
    int64_t last_sim_us = esp_timer_get_time();
    const int64_t sim_period_us = 33333;   // 30 Hz

    ESP_LOGI(TAG, "start: %dx%d cells, %d fish, %d strips of %u B, heap %u",
             COLS, ROWS, FISH_COUNT, STRIPS, (unsigned)STRIP_BYTES,
             (unsigned)esp_get_free_heap_size());

    while (s_run) {
#if AQUARIUM_DIAG_LOG
        int64_t t0 = esp_timer_get_time();
#endif

        uint8_t feed_req;
        if (xQueueReceive(s_feed_q, &feed_req, 0) == pdTRUE) {
            // 鱼食从顶部撒下（真实缸投喂的样子），三粒横向散开、下落速度略不同，
            // 鱼群才会往上追着吃。vy≈0.16 格/步，约 9 秒落到缸底。
            const int16_t cx = (int16_t)((COLS / 2) * FISH_SIM_ONE);
            fish_sim_drop_food_ex(&s_sim, (int16_t)(cx - 4 * FISH_SIM_ONE), 0, 38, 1500);
            fish_sim_drop_food_ex(&s_sim, cx, 0, 44, 1500);
            fish_sim_drop_food_ex(&s_sim, (int16_t)(cx + 4 * FISH_SIM_ONE), 0, 38, 1500);
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
        }
        scene_compose();

        for (int s = 0; s < STRIPS; s++) {
            if (!force_full && !strip_changed(s)) continue;
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
        force_full = false;
        if (sim_steps > 0) s_tick++;

#if AQUARIUM_DIAG_LOG
        int64_t total = esp_timer_get_time() - t0;
        sum += total;
        frames++;
#endif
#if AQUARIUM_DIAG_LOG
        if (frames >= LOG_EVERY) {
            float work = (float)sum / frames / 1000.0f;
            ESP_LOGI(TAG, "work=%.1f ms/帧 (%.1f fps 上限) heap=%u food=%u",
                     work, work > 0.01f ? 1000.0f / work : 0.0f,
                     (unsigned)esp_get_free_heap_size(), s_sim.food_count);
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
    mask_init();
    init_scene_objects();
    fish_sim_init(&s_sim, FISH_COUNT, 20261118u);
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
