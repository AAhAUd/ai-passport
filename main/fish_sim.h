// main/fish_sim.h —— 鱼缸里三种生物的状态机。
//
// 纯计算模块：不碰 LVGL、不碰 ESP-IDF，可编译进主机测试。坐标用 Q8 定点
// （1 格 = 256），避免浮点在无 FPU 的 ESP32-C3 上做软件模拟，也让主机与设备
// 的结果逐位一致 —— 同一 seed 必定得到同一帧，便于回归测试。
//
// 三种生物（kind）各有参数（游速、活动层带、是否追食、是否参与避让、停顿概率）：
//   喷火灯  中层成群，会俯冲追食
//   黄金胡子 底部沉木区慢游，走走停停，不追食
//   黑壳虾  前景草皮爬行，只在食物落到缸底后才冲过去
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FISH_SIM_COLS        30
#define FISH_SIM_ROWS        40
#define FISH_SIM_MAX_FISH    16
#define FISH_SIM_MAX_FOOD    6

#define FISH_SIM_ONE         256      // 一格 = 256（Q8）
#define FISH_SIM_SEPARATION  320      // 1.25 格内开始避让（仅群游种类）
#define FISH_SIM_SIGHT       5120     // 20 格内感知食物：中层也能看到落底的食物
#define FISH_SIM_EAT         256      // 1 格内算吃到
#define FISH_SIM_EDGE_MARGIN (FISH_SIM_ONE)   // 贴边 1 格以内
#define FISH_SIM_EAT_HOLD    8        // 吃到后保持航向的步数
// 食物入水后多久才被注意到（步，30 Hz）。不设延迟时食物一出现鱼就冲，
// 几乎看不到下沉过程；0.5~2 秒是真实的反应时间。
#define FISH_SIM_FOOD_GRACE  90

typedef enum {
    CRE_TETRA = 0,     // 喷火灯
    CRE_PLECO,         // 黄金胡子
    CRE_SHRIMP,        // 黑壳虾
    CRE_KIND_COUNT
} creature_kind_t;

typedef struct {
    int16_t  x, y;      // Q8 格坐标
    uint8_t  dir;       // 0..7 八方向索引
    uint8_t  kind;      // creature_kind_t
    uint8_t  hold;      // 进食后的惯性计数：>0 时不做随机转向
    uint8_t  pause;     // 停顿剩余步数（胡子用）：>0 时完全不动
} fish_sim_fish_t;

typedef struct {
    int16_t  x, y;      // Q8 格坐标
    int16_t  vy;        // 下落速度（Q8/步）；0 表示静止悬浮
    uint16_t ttl;       // 剩余存活步数
    uint16_t age;       // 入水后的步数（配合 FISH_SIM_FOOD_GRACE）
} fish_sim_food_t;

typedef struct {
    fish_sim_fish_t fish[FISH_SIM_MAX_FISH];
    fish_sim_food_t food[FISH_SIM_MAX_FOOD];
    uint8_t  fish_count;
    uint8_t  food_count;
    uint32_t rng;
    uint32_t tick;
} fish_sim_t;

// 兼容旧接口：全部按喷火灯初始化（现有测试依赖这个行为）。
void fish_sim_init(fish_sim_t *sim, uint8_t fish_count, uint32_t seed);

// 鱼缸场景：6 条喷火灯 + 1 条黄金胡子 + 3 只黑壳虾，各自放在草图的初始位置。
void fish_sim_init_mixed(fish_sim_t *sim, uint32_t seed);

// 修改已有个体的种类（测试用）。
void fish_sim_set_kind(fish_sim_t *sim, uint8_t index, uint8_t kind);

// 推进一步。行为与步数绑定，调用方按固定频率调用（本项目 30 Hz 墙钟）。
void fish_sim_step(fish_sim_t *sim);

// 撒一份鱼食（Q8 坐标）。vy 为每步下落速度，0 表示悬浮。成功返回下标，
// 表满返回 -1，越界坐标会被夹进网格。
int fish_sim_drop_food_ex(fish_sim_t *sim, int16_t x, int16_t y, int16_t vy, uint16_t ttl);
int fish_sim_drop_food(fish_sim_t *sim, int16_t x, int16_t y, uint16_t ttl);
int fish_sim_drop_food_center(fish_sim_t *sim, uint16_t ttl);

// 朝向索引对应的单位向量。
int8_t fish_sim_dir_dx(uint8_t dir);
int8_t fish_sim_dir_dy(uint8_t dir);

// Q8 坐标转格坐标。
static inline int fish_sim_cell_x(int16_t q8) { return q8 / FISH_SIM_ONE; }
static inline int fish_sim_cell_y(int16_t q8) { return q8 / FISH_SIM_ONE; }
