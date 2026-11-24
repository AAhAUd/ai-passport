// main/fish_sim.c —— 三种生物的状态机。见 fish_sim.h。
//
// 每个个体每步只做四件事：要不要停（胡子）、避让最近的同类（喷火灯）、
// 追最近的鱼食（喷火灯与虾）、否则继续游并偶尔转向。计算量 O(n²)，n ≤ 16，
// 在这颗芯片上可以忽略。
#include "fish_sim.h"

#include <stddef.h>
#include <string.h>

#define DIR_COUNT 8

static const int8_t DIR_X[DIR_COUNT] = { 1, 1, 0, -1, -1, -1, 0, 1 };
static const int8_t DIR_Y[DIR_COUNT] = { 0, 1, 1, 1, 0, -1, -1, -1 };

#define X_MAX ((FISH_SIM_COLS - 1) * FISH_SIM_ONE)
#define Y_MAX ((FISH_SIM_ROWS - 1) * FISH_SIM_ONE)

// 每种生物的脾气。
typedef struct {
    uint8_t speed;        // 巡游速度（Q8/步）
    uint8_t chase_speed;  // 追食速度
    uint8_t y_min;        // 活动层带（字符行，含）
    uint8_t y_max;
    uint8_t hard_band;    // 1 = 追食也不越界（虾）
    uint8_t seek_food;    // 1 = 会追食
    uint8_t school;       // 1 = 参与避让/群游（喷火灯）
    uint8_t pause_chance; // 每步停顿概率（/256，0 = 不停）
} kind_params_t;

static const kind_params_t PARAMS[CRE_KIND_COUNT] = {
    // 喷火灯：中层成群，看见食物会俯冲下去
    [CRE_TETRA]  = { .speed = 30, .chase_speed = 30, .y_min = 6,  .y_max = 24,
                     .hard_band = 0, .seek_food = 1, .school = 1, .pause_chance = 0 },
    // 黄金胡子：贴底慢游，走走停停，不追食
    [CRE_PLECO]  = { .speed = 8,  .chase_speed = 8,  .y_min = 28, .y_max = 35,
                     .hard_band = 0, .seek_food = 0, .school = 0, .pause_chance = 3 },
    // 黑壳虾：只在草皮那一层爬，食物落到缸底才加速冲
    [CRE_SHRIMP] = { .speed = 6,  .chase_speed = 22, .y_min = 34, .y_max = 37,
                     .hard_band = 1, .seek_food = 1, .school = 0, .pause_chance = 0 },
};

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

// 最短转向：避免 0 与 7 之间绕远路。
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
    sim->rng = seed ? seed : 0x1234567u;
    sim->tick = 0;

    for (uint8_t i = 0; i < sim->fish_count; i++) {
        sim->fish[i].x = (int16_t)((int32_t)(i % FISH_SIM_COLS) * FISH_SIM_ONE + FISH_SIM_ONE / 2);
        sim->fish[i].y = (int16_t)(((int32_t)(i / FISH_SIM_COLS) * 5 + 4) * FISH_SIM_ONE);
        sim->fish[i].dir = (uint8_t)(i % DIR_COUNT);
        sim->fish[i].kind = CRE_TETRA;      // 旧接口：全部按喷火灯
    }
}

void fish_sim_set_kind(fish_sim_t *sim, uint8_t index, uint8_t kind) {
    if (!sim || index >= sim->fish_count) return;
    sim->fish[index].kind = (uint8_t)(kind % CRE_KIND_COUNT);
}

void fish_sim_init_mixed(fish_sim_t *sim, uint32_t seed) {
    // 初始位置取自草图（屏幕像素 / 8 = 字符格）：6 条喷火灯中层成群，
    // 1 条胡子贴沉木，3 只虾在前景草皮。
    static const struct { uint8_t kind; int16_t x, y; int8_t dx; } START[] = {
        { CRE_TETRA,   70, 108, 1 }, { CRE_TETRA,  92, 120, 1 },
        { CRE_TETRA,   80, 134, -1 }, { CRE_TETRA, 58, 142, 1 },
        { CRE_TETRA,  104,  98, -1 }, { CRE_TETRA, 122, 150, 1 },
        { CRE_PLECO,  172, 244, 1 },
        { CRE_SHRIMP, 150, 282, 1 }, { CRE_SHRIMP, 196, 282, -1 },
        { CRE_SHRIMP,  34, 282, 1 },
    };
    const uint8_t n = (uint8_t)(sizeof(START) / sizeof(START[0]));
    fish_sim_init(sim, n, seed);
    for (uint8_t i = 0; i < n; i++) {
        sim->fish[i].kind = START[i].kind;
        sim->fish[i].x = (int16_t)(START[i].x * FISH_SIM_ONE / 8);
        sim->fish[i].y = (int16_t)(START[i].y * FISH_SIM_ONE / 8);
        sim->fish[i].dir = START[i].dx > 0 ? 0 : 4;
    }
}

int fish_sim_drop_food_ex(fish_sim_t *sim, int16_t x, int16_t y, int16_t vy, uint16_t ttl) {
    if (!sim || sim->food_count >= FISH_SIM_MAX_FOOD) return -1;
    int idx = sim->food_count;
    sim->food[idx].x = clamp_q8(x, X_MAX);
    sim->food[idx].y = clamp_q8(y, Y_MAX);
    sim->food[idx].vy = vy;
    sim->food[idx].ttl = ttl;
    sim->food[idx].age = 0;
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
    const kind_params_t *p = &PARAMS[fish->kind % CRE_KIND_COUNT];

    // 0) 停顿（胡子）：停住时完全不动，像贴着沉木休息。
    if (fish->pause > 0) {
        fish->pause--;
        return;
    }
    if (p->pause_chance && (rng_next(sim) & 0xFF) < p->pause_chance) {
        fish->pause = (uint8_t)(30 + (rng_next(sim) % 50));   // 1~2.6 秒
        return;
    }

    // 1) 最近的同类（只有群游种类才在意）
    int32_t nearest_fish_d2 = 0x7FFFFFFF;
    int32_t nearest_fish_dx = 0, nearest_fish_dy = 0;
    if (p->school) {
        for (uint8_t j = 0; j < sim->fish_count; j++) {
            if (j == index || sim->fish[j].kind != fish->kind) continue;
            int32_t d2 = dist2(fish->x, fish->y, sim->fish[j].x, sim->fish[j].y);
            if (d2 < nearest_fish_d2) {
                nearest_fish_d2 = d2;
                nearest_fish_dx = fish->x - sim->fish[j].x;
                nearest_fish_dy = fish->y - sim->fish[j].y;
            }
        }
    }

    // 2) 最近的鱼食（虾只认落到缸底附近的）
    int32_t nearest_food_d2 = 0x7FFFFFFF;
    int32_t nearest_food_dx = 0, nearest_food_dy = 0;
    bool food_ok = false;
    if (p->seek_food) {
        for (uint8_t f = 0; f < sim->food_count; f++) {
            // 硬层带的种类（虾）只对"进入自己这一层"的食物有反应
            if (sim->food[f].age < FISH_SIM_FOOD_GRACE) continue;   // 刚入水，还没被注意到
            if (p->hard_band && sim->food[f].y < (int32_t)(p->y_min - 1) * FISH_SIM_ONE) continue;
            int32_t d2 = dist2(fish->x, fish->y, sim->food[f].x, sim->food[f].y);
            if (d2 < nearest_food_d2) {
                nearest_food_d2 = d2;
                nearest_food_dx = sim->food[f].x - fish->x;
                nearest_food_dy = sim->food[f].y - fish->y;
            }
        }
        food_ok = (nearest_food_d2 < (int32_t)FISH_SIM_SIGHT * FISH_SIM_SIGHT);
    }

    // 3) 决定目标朝向：避让 > 追食 > 保持/随机
    uint8_t target = fish->dir;
    if (p->school && nearest_fish_d2 < (int32_t)FISH_SIM_SEPARATION * FISH_SIM_SEPARATION) {
        target = dir_from_vector(nearest_fish_dx, nearest_fish_dy);
    } else if (food_ok) {
        target = dir_from_vector(nearest_food_dx, nearest_food_dy);
    } else if (fish->hold == 0 && (rng_next(sim) & 0x1F) == 0) {
        target = (uint8_t)((fish->dir + ((rng_next(sim) & 1) ? 1 : DIR_COUNT - 1)) % DIR_COUNT);
    }

    // 4) 没在追食时，把游动拉回自己的层带（2 = 向下，6 = 向上）
    if (!food_ok) {
        if (fish->y < (int32_t)p->y_min * FISH_SIM_ONE) target = 2;
        else if (fish->y > (int32_t)p->y_max * FISH_SIM_ONE) target = 6;
    }

    // 5) 边界掉头（留 1 格余量）
    if (fish->x < FISH_SIM_EDGE_MARGIN && DIR_X[target] < 0) target = (uint8_t)((target + 4) % DIR_COUNT);
    if (fish->x > X_MAX - FISH_SIM_EDGE_MARGIN && DIR_X[target] > 0) target = (uint8_t)((target + 4) % DIR_COUNT);
    if (fish->y < FISH_SIM_EDGE_MARGIN && DIR_Y[target] < 0) target = (uint8_t)((target + 4) % DIR_COUNT);
    if (fish->y > Y_MAX - FISH_SIM_EDGE_MARGIN && DIR_Y[target] > 0) target = (uint8_t)((target + 4) % DIR_COUNT);

    // 6) 每步最多转 45°
    int8_t delta = dir_delta(fish->dir, target);
    if (delta > 0) {
        fish->dir = (uint8_t)((fish->dir + 1) % DIR_COUNT);
    } else if (delta < 0) {
        fish->dir = (uint8_t)((fish->dir + DIR_COUNT - 1) % DIR_COUNT);
    }

    // 7) 位移
    const int32_t speed = food_ok ? p->chase_speed : p->speed;
    int32_t nx = fish->x + DIR_X[fish->dir] * speed;
    int32_t ny = fish->y + DIR_Y[fish->dir] * speed;
    fish->x = clamp_q8(nx, X_MAX);
    fish->y = clamp_q8(ny, Y_MAX);

    // 8) 硬层带（虾）：追食也不许离开自己那一条
    if (p->hard_band) {
        if (fish->y < (int16_t)(p->y_min * FISH_SIM_ONE)) fish->y = (int16_t)(p->y_min * FISH_SIM_ONE);
        if (fish->y > (int16_t)(p->y_max * FISH_SIM_ONE)) fish->y = (int16_t)(p->y_max * FISH_SIM_ONE);
    }
}

static void eat_nearby_food(fish_sim_t *sim, fish_sim_fish_t *fish) {
    for (int f = (int)sim->food_count - 1; f >= 0; f--) {
        if (abs32(fish->x - sim->food[f].x) <= FISH_SIM_EAT &&
            abs32(fish->y - sim->food[f].y) <= FISH_SIM_EAT) {
            remove_food(sim, f);
            fish->hold = FISH_SIM_EAT_HOLD;
        }
    }
}

void fish_sim_step(fish_sim_t *sim) {
    if (!sim) return;

    sim->tick++;

    // 先推进鱼食：下落，落到"鱼够得到"的高度停住，然后老化
    for (int f = (int)sim->food_count - 1; f >= 0; f--) {
        if (sim->food[f].vy > 0) {
            int32_t ny = (int32_t)sim->food[f].y + sim->food[f].vy;
            const int32_t rest = Y_MAX - FISH_SIM_EDGE_MARGIN;
            if (ny >= rest) {
                sim->food[f].y = (int16_t)rest;
                sim->food[f].vy = 0;
            } else {
                sim->food[f].y = (int16_t)ny;
            }
        }
        if (sim->food[f].age < 0xFFFF) sim->food[f].age++;
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
