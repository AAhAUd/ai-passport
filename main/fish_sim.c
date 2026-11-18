// main/fish_sim.c —— 鱼群状态机实现。见 fish_sim.h 的设计说明。
//
// 每条鱼每帧只做三件事：避让最近的邻居、追最近的鱼食、没有目标就继续游（偶尔
// 随机转一次向）。这三条足以让画面不像各自乱飘，而计算量是 O(n²) 且 n ≤ 16，
// 在这颗芯片上可以忽略。
#include "fish_sim.h"

#include <stddef.h>
#include <string.h>

#define DIR_COUNT 8

static const int8_t DIR_X[DIR_COUNT] = { 1, 1, 0, -1, -1, -1, 0, 1 };
static const int8_t DIR_Y[DIR_COUNT] = { 0, 1, 1, 1, 0, -1, -1, -1 };

#define X_MAX ((FISH_SIM_COLS - 1) * FISH_SIM_ONE)
#define Y_MAX ((FISH_SIM_ROWS - 1) * FISH_SIM_ONE)

int8_t fish_sim_dir_dx(uint8_t dir) { return DIR_X[dir % DIR_COUNT]; }
int8_t fish_sim_dir_dy(uint8_t dir) { return DIR_Y[dir % DIR_COUNT]; }

static uint32_t rng_next(fish_sim_t *sim) {
    uint32_t x = sim->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    sim->rng = x;
    return x;
}

static int16_t clamp_q8(int32_t v, int32_t max) {
    if (v < 0) return 0;
    if (v > max) return (int16_t)max;
    return (int16_t)v;
}

static int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

// 把任意方向的向量映射到最接近的八方向索引。
static uint8_t dir_from_vector(int32_t dx, int32_t dy) {
    if (dx == 0 && dy == 0) return 0;
    uint8_t best = 0;
    int32_t best_dot = -0x7FFFFFFF;
    for (uint8_t i = 0; i < DIR_COUNT; i++) {
        int32_t dot = dx * DIR_X[i] + dy * DIR_Y[i];
        if (dot > best_dot) {
            best_dot = dot;
            best = i;
        }
    }
    return best;
}

// 相邻两格之间能走的最短方向，用于避免 0 与 7 之间绕远路。
static int8_t dir_delta(uint8_t from, uint8_t to) {
    int8_t d = (int8_t)((to + DIR_COUNT - from) % DIR_COUNT);
    if (d > DIR_COUNT / 2) d = (int8_t)(d - DIR_COUNT);
    return d;
}

static int32_t dist2(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    int32_t dx = ax - bx;
    int32_t dy = ay - by;
    return dx * dx + dy * dy;
}

void fish_sim_init(fish_sim_t *sim, uint8_t fish_count, uint32_t seed) {
    if (!sim) return;
    // 整体清零：结构体尾部有对齐填充，不清会带上栈上的垃圾值，
    // 使"同一 seed 逐位一致"这个可回归的前提失效。
    memset(sim, 0, sizeof(*sim));
    sim->fish_count = fish_count > FISH_SIM_MAX_FISH ? FISH_SIM_MAX_FISH : fish_count;
    sim->food_count = 0;
    sim->rng = seed ? seed : 0x1234567u;   // 0 会让 xorshift 退化
    sim->tick = 0;

    for (uint8_t i = 0; i < FISH_SIM_MAX_FOOD; i++) {
        sim->food[i].x = 0;
        sim->food[i].y = 0;
        sim->food[i].ttl = 0;
    }

    for (uint8_t i = 0; i < sim->fish_count; i++) {
        // 初始位置铺开一点，避免全部叠在中心导致开头几秒画面发闷。
        sim->fish[i].x = (int16_t)((int32_t)(i % FISH_SIM_COLS) * FISH_SIM_ONE
                                   + FISH_SIM_ONE / 2);
        sim->fish[i].y = (int16_t)(((int32_t)(i / FISH_SIM_COLS) * 5 + 4) * FISH_SIM_ONE);
        sim->fish[i].dir = (uint8_t)(i % DIR_COUNT);
        // 每三条里有一条换配色：同一种鱼、不同颜色，画面才不死板。
        sim->fish[i].tint = (uint8_t)((i % 3 == 2) ? 1 : 0);
    }
}

int fish_sim_drop_food_ex(fish_sim_t *sim, int16_t x, int16_t y, int16_t vy, uint16_t ttl) {
    if (!sim || sim->food_count >= FISH_SIM_MAX_FOOD) return -1;
    int idx = sim->food_count;
    sim->food[idx].x = clamp_q8(x, X_MAX);
    sim->food[idx].y = clamp_q8(y, Y_MAX);
    sim->food[idx].vy = vy;
    sim->food[idx].ttl = ttl;
    sim->food_count++;
    return idx;
}

int fish_sim_drop_food(fish_sim_t *sim, int16_t x, int16_t y, uint16_t ttl) {
    return fish_sim_drop_food_ex(sim, x, y, 0, ttl);
}

int fish_sim_drop_food_center(fish_sim_t *sim, uint16_t ttl) {
    return fish_sim_drop_food(sim, (int16_t)((FISH_SIM_COLS / 2) * FISH_SIM_ONE),
                              (int16_t)((FISH_SIM_ROWS / 2) * FISH_SIM_ONE), ttl);
}

static void remove_food(fish_sim_t *sim, int idx) {
    sim->food_count--;
    if (idx != (int)sim->food_count) {
        sim->food[idx] = sim->food[sim->food_count];
    }
}

static void step_fish(fish_sim_t *sim, fish_sim_fish_t *fish, uint8_t index) {
    // 1) 找最近的邻居和最近的鱼食。
    int32_t nearest_fish_d2 = 0x7FFFFFFF;
    int32_t nearest_fish_dx = 0, nearest_fish_dy = 0;
    for (uint8_t j = 0; j < sim->fish_count; j++) {
        if (j == index) continue;
        int32_t d2 = dist2(fish->x, fish->y, sim->fish[j].x, sim->fish[j].y);
        if (d2 < nearest_fish_d2) {
            nearest_fish_d2 = d2;
            nearest_fish_dx = fish->x - sim->fish[j].x;
            nearest_fish_dy = fish->y - sim->fish[j].y;
        }
    }

    int32_t nearest_food_d2 = 0x7FFFFFFF;
    int32_t nearest_food_dx = 0, nearest_food_dy = 0;
    for (uint8_t f = 0; f < sim->food_count; f++) {
        int32_t d2 = dist2(fish->x, fish->y, sim->food[f].x, sim->food[f].y);
        if (d2 < nearest_food_d2) {
            nearest_food_d2 = d2;
            nearest_food_dx = sim->food[f].x - fish->x;
            nearest_food_dy = sim->food[f].y - fish->y;
        }
    }

    // 2) 决定目标朝向：避让 > 追食 > 保持/随机。
    uint8_t target = fish->dir;
    if (nearest_fish_d2 < (int32_t)FISH_SIM_SEPARATION * FISH_SIM_SEPARATION) {
        target = dir_from_vector(nearest_fish_dx, nearest_fish_dy);
    } else if (nearest_food_d2 < (int32_t)FISH_SIM_SIGHT * FISH_SIM_SIGHT) {
        target = dir_from_vector(nearest_food_dx, nearest_food_dy);
    } else if (fish->hold == 0 && (rng_next(sim) & 0x1F) == 0) {
        // 空闲时偶尔随机转向（1/32，且进食后 8 步内不转）。
        target = (uint8_t)((fish->dir + ((rng_next(sim) & 1) ? 1 : DIR_COUNT - 1)) % DIR_COUNT);
    }

    // 3) 边界处理优先于一切：贴边就掉头朝里。
    if (fish->x < FISH_SIM_ONE * 2 && DIR_X[target] < 0) target = (uint8_t)((target + 4) % DIR_COUNT);
    if (fish->x > X_MAX - FISH_SIM_ONE * 2 && DIR_X[target] > 0) target = (uint8_t)((target + 4) % DIR_COUNT);
    if (fish->y < FISH_SIM_ONE * 2 && DIR_Y[target] < 0) target = (uint8_t)((target + 4) % DIR_COUNT);
    if (fish->y > Y_MAX - FISH_SIM_ONE * 2 && DIR_Y[target] > 0) target = (uint8_t)((target + 4) % DIR_COUNT);

    // 4) 每步最多转 45°，游动才像鱼而不是瞬移。
    int8_t delta = dir_delta(fish->dir, target);
    if (delta > 0) {
        fish->dir = (uint8_t)((fish->dir + 1) % DIR_COUNT);
    } else if (delta < 0) {
        fish->dir = (uint8_t)((fish->dir + DIR_COUNT - 1) % DIR_COUNT);
    }

    // 5) 位移并夹回网格（夹回而不是反弹，保证任何情况下都不会跑出屏幕）。
    int32_t nx = fish->x + DIR_X[fish->dir] * FISH_SIM_SPEED;
    int32_t ny = fish->y + DIR_Y[fish->dir] * FISH_SIM_SPEED;
    fish->x = clamp_q8(nx, X_MAX);
    fish->y = clamp_q8(ny, Y_MAX);
}

static void eat_nearby_food(fish_sim_t *sim, fish_sim_fish_t *fish) {
    const int32_t reach = FISH_SIM_EAT;
    for (int f = (int)sim->food_count - 1; f >= 0; f--) {
        if (abs32(fish->x - sim->food[f].x) <= reach &&
            abs32(fish->y - sim->food[f].y) <= reach) {
            remove_food(sim, f);
            fish->hold = FISH_SIM_EAT_HOLD;   // 刚吃完，保持航向，别立刻掉头
        }
    }
}

void fish_sim_step(fish_sim_t *sim) {
    if (!sim) return;

    sim->tick++;

    // 先推进鱼食：先下落，再老化。落到底部的鱼食停在原地等鱼来吃。
    for (int f = (int)sim->food_count - 1; f >= 0; f--) {
        if (sim->food[f].vy > 0) {
            int32_t ny = (int32_t)sim->food[f].y + sim->food[f].vy;
            if (ny >= Y_MAX) {
                sim->food[f].y = Y_MAX;
                sim->food[f].vy = 0;      // 触底停住
            } else {
                sim->food[f].y = (int16_t)ny;
            }
        }
        if (sim->food[f].ttl == 0) {
            remove_food(sim, f);
        } else {
            sim->food[f].ttl--;
        }
    }

    for (uint8_t i = 0; i < sim->fish_count; i++) {
        if (sim->fish[i].hold > 0) sim->fish[i].hold--;
        step_fish(sim, &sim->fish[i], i);
        eat_nearby_food(sim, &sim->fish[i]);
    }
}
