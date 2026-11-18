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

#define FISH_COUNT       3

// 鱼的像素精灵。它在字符网格之上直接写像素，所以鱼能画出真正的形状，
// 而水草/气泡仍然是字符 —— 这是"环境是 ASCII、主角是像素"的混合渲染。
#define SPRITE_W         26
#define SPRITE_H         16
#define SPR_PIX_NONE     0
#define SPR_PIX_BODY     1
#define SPR_PIX_DARK     2
#define SPR_PIX_LIGHT    3
#define SPR_PIX_EYE      4
#define SPR_PIX_PUPIL    5
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
// 精灵覆盖的字符行掩码（40 行）。字符网格的脏检测看不到精灵，所以鱼游过或游走
// 的那几行必须强制重绘 —— 否则屏幕上会留下残影（真机验证过）。
static uint64_t s_sprite_rows_now;
static uint64_t s_sprite_rows_prev;
static uint8_t  s_backlight = 100;
static power_state_t s_power = POWER_ACTIVE;
static int64_t  s_last_input_us;
static volatile bool s_force_full;   // 从息屏唤醒后要整屏重画一次

static uint16_t s_c_fish;
static uint16_t s_c_fish_dim;
static uint16_t s_c_weed;
static uint16_t s_c_bubble;
static uint16_t s_c_food;
static uint16_t s_c_plankton;
// 两套鱼的配色：身体 / 描边 / 高光 / 眼白 / 瞳孔。
typedef struct {
    uint16_t body, dark, light, eye, pupil;
} fish_palette_t;

static fish_palette_t s_palettes[2];
static uint16_t s_c_eye, s_c_pupil;
// 两帧摆尾的鱼精灵（手绘像素，1:1 上屏）。
static const uint8_t s_sprite_f0[SPRITE_H][SPRITE_W] = {
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0 },
    { 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0 },
    { 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0 },
    { 0, 0, 2, 2, 2, 2, 0, 0, 0, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 2, 2, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 2, 2, 2, 2, 0, 0, 2, 1, 1, 3, 3, 3, 3, 3, 3, 4, 4, 4, 2, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 2, 2, 2, 2, 0, 2, 1, 1, 3, 3, 3, 3, 3, 3, 4, 5, 4, 2, 0, 0, 0 },
    { 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 1, 2, 0, 0 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0 },
    { 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0 },
    { 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 2, 2, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static const uint8_t s_sprite_f1[SPRITE_H][SPRITE_W] = {
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0 },
    { 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0 },
    { 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0 },
    { 0, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 2, 2, 0, 0, 0, 0 },
    { 0, 0, 2, 2, 2, 2, 0, 0, 0, 0, 2, 1, 1, 3, 3, 3, 3, 3, 3, 4, 4, 4, 2, 0, 0, 0 },
    { 0, 0, 0, 2, 2, 2, 2, 2, 2, 0, 2, 1, 1, 3, 3, 3, 3, 3, 3, 4, 5, 4, 2, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 1, 2, 0, 0 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0 },
    { 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 2, 2, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static const uint8_t s_sprite_f2[SPRITE_H][SPRITE_W] = {
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0 },
    { 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 2, 2, 0, 0, 0, 0 },
    { 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 2, 1, 1, 3, 3, 3, 3, 3, 3, 4, 4, 4, 2, 0, 0, 0 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 2, 1, 1, 3, 3, 3, 3, 3, 3, 4, 5, 4, 2, 0, 0, 0 },
    { 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 1, 2, 0, 0 },
    { 0, 0, 0, 0, 0, 2, 2, 2, 2, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0 },
    { 0, 0, 0, 0, 2, 2, 2, 2, 0, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0 },
    { 0, 0, 2, 2, 2, 2, 0, 0, 0, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 2, 2, 0, 0, 0, 0 },
    { 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0 },
    { 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static const uint8_t s_sprite_f3[SPRITE_H][SPRITE_W] = {
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0 },
    { 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 2, 2, 0, 0, 0, 0 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 2, 1, 1, 3, 3, 3, 3, 3, 3, 4, 4, 4, 2, 0, 0, 0 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 2, 1, 1, 3, 3, 3, 3, 3, 3, 4, 5, 4, 2, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 1, 2, 0, 0 },
    { 0, 0, 0, 2, 2, 2, 2, 2, 2, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0 },
    { 0, 0, 2, 2, 2, 2, 0, 0, 0, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0 },
    { 0, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 2, 2, 0, 0, 0, 0 },
    { 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0 },
    { 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

// 四帧摆尾：尾巴用行波（相位沿身体到尾部递增）而不是整块摆动，
// 所以看起来是"甩"而不是"翻"。帧序 0→1→2→3 循环。
static const uint8_t (*s_sprite_frame(uint8_t phase))[SPRITE_W] {
    static const uint8_t (*const frames[4])[SPRITE_W] = {
        s_sprite_f0, s_sprite_f1, s_sprite_f2, s_sprite_f3
    };
    return frames[phase & 3];
}

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

static uint16_t sprite_color(uint8_t px, const fish_palette_t *pal) {
    switch (px) {
    case SPR_PIX_BODY:  return pal->body;
    case SPR_PIX_DARK:  return pal->dark;
    case SPR_PIX_LIGHT: return pal->light;
    case SPR_PIX_EYE:   return s_c_eye;
    case SPR_PIX_PUPIL: return s_c_pupil;
    default:            return 0;
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
    // 单一形态、单一配色：8 像素的格子里，多种体型只会互相干扰、看不出层次。
    s_c_fish     = rgb565(255, 170, 70);
    s_c_fish_dim = rgb565(190, 105, 45);
    s_c_weed      = rgb565(52, 150, 92);
    s_c_bubble    = rgb565(120, 190, 210);
    s_c_food      = rgb565(255, 245, 170);
    s_c_plankton  = rgb565(26, 52, 74);   // 压暗一档，别和鱼混在一起
    // 暖橙
    s_palettes[0].body  = rgb565(255, 170, 70);
    s_palettes[0].dark  = rgb565(184, 100, 44);
    s_palettes[0].light = rgb565(255, 215, 150);
    // 银蓝：和橙色在深蓝水里对比明显，但仍是冷色，不会抢眼
    s_palettes[1].body  = rgb565(170, 215, 245);
    s_palettes[1].dark  = rgb565(70, 110, 150);
    s_palettes[1].light = rgb565(235, 250, 255);

    s_c_eye   = rgb565(255, 246, 234);
    s_c_pupil = rgb565(20, 31, 40);

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

static void scene_compose(void) {
    scene_clear();
    scene_plankton();
    scene_seaweed();
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

// 把一条鱼画进当前条带（条带是一行 8 像素，鱼高 16 像素会跨两条带）。
static void draw_sprite_in_strip(uint16_t *dst, int strip, int px, int py,
                                 const uint8_t spr[SPRITE_H][SPRITE_W], bool mirror,
                                 const fish_palette_t *pal) {
    const int strip_top = strip * STRIP_H;
    for (int sy = 0; sy < SPRITE_H; sy++) {
        int abs_y = py + sy;
        if (abs_y < strip_top || abs_y >= strip_top + STRIP_H) continue;
        int local_y = abs_y - strip_top;
        for (int sx = 0; sx < SPRITE_W; sx++) {
            uint8_t pxv = spr[sy][mirror ? (SPRITE_W - 1 - sx) : sx];
            if (pxv == SPR_PIX_NONE) continue;
            int abs_x = px + sx;
            if (abs_x < 0 || abs_x >= COLS * CELL) continue;
            dst[(size_t)local_y * (COLS * CELL) + abs_x] = sprite_color(pxv, pal);
        }
    }
}

static void fill_strip(uint16_t *dst, int strip) {
    // 1) 背景 + 字符（水草、气泡、鱼食）
    for (int row = 0; row < STRIP_ROWS; row++) {
        int gy = strip * STRIP_ROWS + row;
        for (int y = 0; y < CELL; y++) {
            uint16_t bg = s_bg_px[gy * CELL + y];
            uint16_t *line = dst + (size_t)(row * CELL + y) * (COLS * CELL);
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
            }
        }
    }

    // 2) 精灵：鱼直接写像素，不受字符格子限制
    const uint8_t flap = (uint8_t)((s_tick / 2) & 3);
    for (uint8_t i = 0; i < s_sim.fish_count; i++) {
        const fish_sim_fish_t *f = &s_sim.fish[i];
        int px = (int)((int32_t)f->x * CELL / FISH_SIM_ONE) - SPRITE_W / 2;
        int py = (int)((int32_t)f->y * CELL / FISH_SIM_ONE) - SPRITE_H / 2;
        bool mirror = fish_sim_dir_dx(f->dir) < 0;
        const fish_palette_t *pal = &s_palettes[f->tint < 2 ? f->tint : 0];
        draw_sprite_in_strip(dst, strip, px, py, s_sprite_frame(flap), mirror, pal);
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

    ESP_LOGI(TAG, "start: %dx%d cells, %d fish, %d strips of %u B, heap %u",
             COLS, ROWS, FISH_COUNT, STRIPS, (unsigned)STRIP_BYTES,
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
#if AQUARIUM_DIAG_LOG
        int64_t t0 = esp_timer_get_time();
#endif

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
                int16_t vy = (int16_t)(34 + (esp_random() % 14));  // 0.13~0.19 格/步
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
        }
        scene_compose();

        // 记录精灵这一帧覆盖了哪些字符行（按像素包围盒换算）
        s_sprite_rows_prev = s_sprite_rows_now;
        s_sprite_rows_now = 0;
        for (uint8_t i = 0; i < s_sim.fish_count; i++) {
            const fish_sim_fish_t *f = &s_sim.fish[i];
            int py = (int)((int32_t)f->y * CELL / FISH_SIM_ONE) - SPRITE_H / 2;
            int r0 = py / CELL;
            int r1 = (py + SPRITE_H - 1) / CELL;
            if (r0 < 0) r0 = 0;
            if (r1 >= ROWS) r1 = ROWS - 1;
            for (int r = r0; r <= r1; r++) s_sprite_rows_now |= (1ULL << r);
        }

        for (int s = 0; s < STRIPS; s++) {
            const uint64_t row_bit = (1ULL << s);
            const bool sprite_touched = ((s_sprite_rows_now | s_sprite_rows_prev) & row_bit) != 0;
            if (!s_force_full && !sprite_touched && !strip_changed(s)) continue;
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
        if (sim_steps > 0) s_tick++;

        if (target_fps < 40) {
            vTaskDelay(pdMS_TO_TICKS(1000 / target_fps));
        }
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
