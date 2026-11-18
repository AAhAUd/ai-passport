// main/aquarium.h —— ASCII 鱼缸应用的对外接口。
//
// 应用层不依赖 LVGL：渲染任务直接持有面板句柄刷屏，按键回调只投递请求，
// 鱼群状态由渲染任务单线程持有。
#pragma once

#include "esp_err.h"

// 初始化调色板/场景并启动渲染任务（约 55 fps，模拟按 27.5 Hz 推进）。
esp_err_t aquarium_start(void);
esp_err_t aquarium_stop(void);

// 投喂一次（三粒鱼食从顶部下落）。可从按键回调调用，内部走队列。
void aquarium_feed(void);

// 背光增减；夹在 20..100 %。
void aquarium_brightness_step(int delta);

// 任何按键都调一次：用于把省电计时归零，并从息屏状态唤醒。
void aquarium_note_activity(void);
