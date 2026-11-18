// main/main.c —— ASCII 鱼缸固件入口。
//
// 这是一个独立的 play：开机直接进鱼缸，不经过 demo 菜单，也不初始化 LVGL。
// 面板由渲染任务独占（见 aquarium.c），所以不存在"应用和 LVGL 抢同一块面板、
// 互相打乱刷屏状态"的问题 —— 那正是菜单版本退出时卡死的根因。
//
// 按键（三键）：
//   OK  短按 / 长按   投喂
//   UP  短按          调亮一档
//   DOWN 短按         调暗一档
//
// 关机走硬件电源键；换玩法是重新烧录；恢复出厂扫机身背面二维码。
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_pins.h"
#include "aquarium.h"
#include "key_guard.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";

// 深睡按键唤醒后,守卫最多拦这么久(按键卡住时的兜底)。
#define KEY_GUARD_MAX_US (10 * 1000 * 1000)

static key_guard_t s_key_guard;

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;

    // 守卫跨 main 任务与按键回调(esp_timer 任务)访问,状态在 key_guard 内部维护。
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }

    aquarium_note_activity();   // 任何按键都算"有人在看"：重置省电计时并唤醒

    // 回调跑在 esp_timer 任务里，只做投递/写 LEDC，不能阻塞。
    switch (btn) {
    case BSP_BTN_OK:
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) aquarium_feed();
        break;
    case BSP_BTN_UP:
        if (ev == BSP_BTN_CLICK) aquarium_brightness_step(20);
        break;
    case BSP_BTN_DOWN:
        if (ev == BSP_BTN_CLICK) aquarium_brightness_step(-20);
        break;
    default:
        break;
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "启动: ASCII 鱼缸");

    // 按键唤醒时,那颗键在整个启动期间还按着;先挡住它的动作,直到松手。
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    esp_err_t err = bsp_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "显示初始化失败: %s", esp_err_to_name(err));
        return;
    }
    bsp_display_backlight(100);

    err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) {
        // 按键不可用不影响观赏，画面继续跑。
        ESP_LOGW(TAG, "按键初始化失败: %s（画面仍会运行）", esp_err_to_name(err));
    } else if (s_key_guard.armed) {
        // 若唤醒键在按键驱动启动前就松了手,组件不会产生 release 事件,
        // 守卫会一直拦到兜底。这里按真实电压同步一次:已松手立即撤防。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
        }
    }

    err = aquarium_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "鱼缸启动失败: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "就绪: OK 投喂, UP/DOWN 调亮度");
}
