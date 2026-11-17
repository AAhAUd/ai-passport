// main/demo_fish.c —— ASCII 鱼缸可行性冒烟页（阶段 1 专用，不进正式固件）。
//
// 目的只有一个：量出 240×320 全屏字符网格在 ESP32-C3 上的真实成本，
// 用数据回答「这台设备能不能养鱼」。不做鱼、不做美化、不接声音。
//
// 测量模型与真鱼缸一致：
//   每帧 = 20 条 240×16 的横条；每条先在内存里按 8×8 字符格填 RGB565，
//   再 esp_lcd_panel_draw_bitmap 推给 ST7789。渲染与推送交替，累加即帧时间。
//
// 为什么绕过 LVGL：1200 个 label 必然爆内存，而逐字形贴图的开销与最终实现无关。
// 这里直接打 bsp_display_panel() 的底层面板句柄，测的是「塞满一屏」这件事本身。
//
// 方向说明：竖屏 30×40 与横屏 40×30 格数同为 1200、像素同为 153600 字节，
// 渲染+推送成本相同，所以本页只跑竖屏即可覆盖帧率问题。方向真正的代价分两种：
// 走 ST7789 madctl 硬件交换（免费），或走 LVGL 软件旋转（无 PSRAM，需一次全屏
// 逐像素搬移，那才是额外成本，等定了方向再单独测）。
#include "demo.h"
#include "bsp_display.h"
#include "lvgl.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "fish_smoke";

#define COLS        30          /* 240 / 8 */
#define ROWS        40          /* 320 / 8 */
#define CELL        8
#define STRIP_ROWS  2           /* 每条横带 2 行字符 = 16 像素高 */
#define STRIP_H     (STRIP_ROWS * CELL)
#define STRIPS      (ROWS / STRIP_ROWS)
#define STRIP_PX    (COLS * CELL * STRIP_H)

#define LOG_EVERY   30          /* 每 30 帧打一行统计 */

static lv_obj_t *s_scr;
static esp_lcd_panel_handle_t s_panel;
static TaskHandle_t s_task;
static volatile bool s_run;
static uint16_t *s_strip;                /* STRIP_PX 个 RGB565 像素 */
static uint8_t s_grid[ROWS][COLS];       /* 0..15 = 字模索引 */
static uint8_t s_glyph[16][8];           /* 合成 8×8 字模，密度约 50%，与 ASCII 字符接近 */
static uint16_t s_palette[16];
static const uint16_t BG = 0x0861;       /* 深蓝背景 RGB565 */

static void glyph_init(void) {
    for (int g = 0; g < 16; g++)
        for (int r = 0; r < 8; r++)
            s_glyph[g][r] = (uint8_t)((g * 0x9E + r * 0x3B) ^ (r * 0x11));

    /* 16 档冷色，模拟 ASCII 鱼缸的单一色相层次 */
    for (int i = 0; i < 16; i++)
        s_palette[i] = (uint16_t)((0x1F << 11) | ((uint16_t)(6 + i) << 5) | (uint16_t)(14 - i / 2));
}

static void grid_init(void) {
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            s_grid[y][x] = (uint8_t)((x * 7 + y * 3) & 0x0F);
}

/* 每帧让图案动起来，避免编译器/缓存把负载理想化 */
static void grid_step(void) {
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS - 1; x++)
            s_grid[y][x] = s_grid[y][x + 1];
}

static void fill_strip(uint16_t *dst, int strip) {
    for (int row = 0; row < STRIP_ROWS; row++) {
        int gy = strip * STRIP_ROWS + row;
        for (int col = 0; col < COLS; col++) {
            const uint8_t *bits = s_glyph[s_grid[gy][col]];
            uint16_t color = s_palette[s_grid[gy][col]];
            for (int y = 0; y < CELL; y++) {
                uint8_t b = bits[y];
                uint16_t *out = dst + (size_t)(row * CELL + y) * (COLS * CELL) + col * CELL;
                for (int x = 0; x < CELL; x++)
                    out[x] = (b & (0x80u >> x)) ? color : BG;
            }
        }
    }
}

static void render_task(void *arg) {
    (void)arg;
    int64_t sum_total = 0, sum_render = 0, sum_push = 0;
    int frames = 0;
    int64_t worst = 0;

    ESP_LOGI(TAG, "start: %dx%d grid, %d strips, strip buf %u B, free heap %u B",
             COLS, ROWS, STRIPS, (unsigned)STRIP_PX * 2u,
             (unsigned)esp_get_free_heap_size());

    while (s_run) {
        int64_t t0 = esp_timer_get_time();
        int64_t t_render = 0, t_push = 0;

        grid_step();

        for (int s = 0; s < STRIPS; s++) {
            int64_t a = esp_timer_get_time();
            fill_strip(s_strip, s);
            int64_t b = esp_timer_get_time();
            esp_lcd_panel_draw_bitmap(s_panel, 0, s * STRIP_H, COLS * CELL,
                                      (s + 1) * STRIP_H, s_strip);
            int64_t c = esp_timer_get_time();
            t_render += b - a;
            t_push += c - b;
        }

        int64_t total = esp_timer_get_time() - t0;
        sum_total += total;
        sum_render += t_render;
        sum_push += t_push;
        if (total > worst) worst = total;
        frames++;

        if (frames >= LOG_EVERY) {
            float avg = (float)sum_total / frames / 1000.0f;
            ESP_LOGI(TAG, "frames=%d avg=%.1f ms (%.1f fps) render=%.1f push=%.1f worst=%.1f ms heap=%u",
                     frames, avg, avg > 0.01f ? 1000.0f / avg : 0.0f,
                     (float)sum_render / frames / 1000.0f,
                     (float)sum_push / frames / 1000.0f,
                     (float)worst / 1000.0f,
                     (unsigned)esp_get_free_heap_size());
            sum_total = sum_render = sum_push = 0;
            frames = 0;
            worst = 0;
        }
    }

    ESP_LOGI(TAG, "task stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t demo_fish_start(void) {
    s_panel = bsp_display_panel();
    if (!s_panel) {
        ESP_LOGE(TAG, "no panel handle, bsp_display_init() failed?");
        return ESP_ERR_INVALID_STATE;
    }
    s_strip = heap_caps_malloc((size_t)STRIP_PX * 2u, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_strip) {
        ESP_LOGE(TAG, "strip alloc failed (%u B)", (unsigned)STRIP_PX * 2u);
        return ESP_ERR_NO_MEM;
    }

    glyph_init();
    grid_init();
    s_run = true;
    if (xTaskCreate(render_task, "fish_smoke", 4096, NULL, 5, &s_task) != pdPASS) {
        s_run = false;
        heap_caps_free(s_strip);
        s_strip = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t demo_fish_stop(void) {
    s_run = false;
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_strip) {
        heap_caps_free(s_strip);
        s_strip = NULL;
    }
    return ESP_OK;
}

void demo_fish_enter(void) {
    /* 空屏：不建任何控件，避免 LVGL 反复刷新盖掉我们自己画的画面 */
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_screen_load(s_scr);
    bsp_display_backlight(100);
}

void demo_fish_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
}

void demo_fish_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) {
        ESP_LOGI(TAG, "manual: heap=%u largest=%u", (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    }
}
