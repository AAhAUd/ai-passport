// main/idle_policy.h —— 省电策略：按"多久没按键"决定帧率与背光。
//
// 纯逻辑：不碰 FreeRTOS、不碰 ESP-IDF，时间由调用方传进来，所以能在主机上测。
// 三档的设计取舍：
//   ACTIVE  满帧（约 55 fps），正常观赏
//   REDUCED 15 fps，画面还在动但明显省电；鱼速不受影响，因为模拟按墙钟推进
//   SLEEP   停止渲染并把背光关到 0；任何按键立刻回到 ACTIVE
#pragma once

#include <stdint.h>

#define IDLE_TO_REDUCED_US  (30LL * 1000 * 1000)    // 30 秒
#define IDLE_TO_SLEEP_US    (150LL * 1000 * 1000)   // 2 分 30 秒

typedef enum {
    POWER_ACTIVE = 0,
    POWER_REDUCED,
    POWER_SLEEP,
} power_state_t;

// 由当前状态与空闲时长推出下一状态。任何空闲时长归零都由调用方直接置 ACTIVE。
power_state_t power_state_next(power_state_t current, int64_t idle_us);

// 该状态下渲染循环的目标帧率；SLEEP 返回 0 表示不渲染。
int power_state_target_fps(power_state_t state);
