// main/fish_sim.h —— ASCII 鱼缸的鱼群状态机。
//
// 纯计算模块：不碰 LVGL、不碰 ESP-IDF，可编译进主机测试。坐标用 Q8 定点
// （1 格 = 256），避免浮点在无 FPU 的 ESP32-C3 上做软件模拟，也让主机与设备
// 的结果逐位一致 —— 同一 seed 必定得到同一帧，便于回归测试。
//
// 朝向用八方向索引表示，每帧最多转向 45°，因此游动是圆滑的折线而不是瞬移。
#pragma once

#include <stdint.h>

#define FISH_SIM_COLS        30
#define FISH_SIM_ROWS        40
#define FISH_SIM_MAX_FISH    16
#define FISH_SIM_MAX_FOOD    6

#define FISH_SIM_ONE         256      // 一格 = 256（Q8）
#define FISH_SIM_SPEED       42       // 每步位移，约 0.164 格；一格约 6 步
#define FISH_SIM_SEPARATION  320      // 1.25 格内开始避让
#define FISH_SIM_SIGHT       3584     // 14 格内能感知鱼食

typedef struct {
    int16_t  x, y;      // Q8 格坐标
    uint8_t  dir;       // 0..7 八方向索引
    uint8_t  pad;
} fish_sim_fish_t;

typedef struct {
    int16_t  x, y;      // Q8 格坐标
    uint16_t ttl;       // 剩余存活步数
} fish_sim_food_t;

typedef struct {
    fish_sim_fish_t fish[FISH_SIM_MAX_FISH];
    fish_sim_food_t food[FISH_SIM_MAX_FOOD];
    uint8_t  fish_count;
    uint8_t  food_count;
    uint32_t rng;
    uint32_t tick;
} fish_sim_t;

// 按给定数量与种子初始化；数量会被夹到 [0, FISH_SIM_MAX_FISH]。
void fish_sim_init(fish_sim_t *sim, uint8_t fish_count, uint32_t seed);

// 推进一步（调用方按固定帧率调用，行为与步数绑定，不依赖真实时间）。
void fish_sim_step(fish_sim_t *sim);

// 在 (x, y) 撒一份鱼食（Q8 坐标），ttl 为存活步数。
// 成功返回下标，表满返回 -1。超出网格的坐标会被夹进网格内。
int fish_sim_drop_food(fish_sim_t *sim, int16_t x, int16_t y, uint16_t ttl);

// 把鱼食放到网格正中，方便按键投喂时不必自己算坐标。
int fish_sim_drop_food_center(fish_sim_t *sim, uint16_t ttl);
