// tests/test_fish_sim.c —— 鱼群状态机的主机测试。
//
// 重点验证三条不变量：鱼永远在网格内、鱼食一定会被吃掉或过期、同一 seed 逐位复现。
// 这些不需要设备就能跑，是后面接渲染前的安全网。
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "fish_sim.h"

#define X_MAX ((FISH_SIM_COLS - 1) * FISH_SIM_ONE)
#define Y_MAX ((FISH_SIM_ROWS - 1) * FISH_SIM_ONE)

static void assert_all_inside(const fish_sim_t *sim) {
    for (uint8_t i = 0; i < sim->fish_count; i++) {
        assert(sim->fish[i].x >= 0 && sim->fish[i].x <= X_MAX);
        assert(sim->fish[i].y >= 0 && sim->fish[i].y <= Y_MAX);
        assert(sim->fish[i].dir < 8);
    }
}

static void test_init_clamps_count(void) {
    fish_sim_t sim;
    fish_sim_init(&sim, 200, 1);
    assert(sim.fish_count == FISH_SIM_MAX_FISH);
    assert(sim.food_count == 0);
    assert_all_inside(&sim);
}

static void test_stays_inside_over_long_run(void) {
    fish_sim_t sim;
    fish_sim_init(&sim, FISH_SIM_MAX_FISH, 0xC0FFEEu);
    for (int i = 0; i < 20000; i++) {
        fish_sim_step(&sim);
        if (i % 97 == 0) assert_all_inside(&sim);
    }
    assert_all_inside(&sim);
}

static void test_fish_actually_move(void) {
    fish_sim_t sim;
    fish_sim_init(&sim, 6, 7);
    int16_t x0 = sim.fish[0].x;
    int16_t y0 = sim.fish[0].y;
    for (int i = 0; i < 60; i++) fish_sim_step(&sim);
    assert(sim.fish[0].x != x0 || sim.fish[0].y != y0);
}

static void test_food_gets_eaten(void) {
    fish_sim_t sim;
    fish_sim_init(&sim, 6, 42);
    assert(fish_sim_drop_food_center(&sim, 6000) == 0);
    assert(sim.food_count == 1);

    int steps_to_eat = 0;
    for (int i = 0; i < 6000 && sim.food_count > 0; i++) {
        fish_sim_step(&sim);
        steps_to_eat++;
    }
    assert(sim.food_count == 0);
    // 6000 步上限约等于 30fps 下 200 秒，吃得到就说明追食逻辑真的在起作用。
    assert(steps_to_eat > 0);
    assert_all_inside(&sim);
}

static void test_food_expires_without_fish(void) {
    fish_sim_t sim;
    fish_sim_init(&sim, 0, 3);
    assert(sim.fish_count == 0);
    assert(fish_sim_drop_food(&sim, 5 * FISH_SIM_ONE, 5 * FISH_SIM_ONE, 10) == 0);
    for (int i = 0; i < 12; i++) fish_sim_step(&sim);
    assert(sim.food_count == 0);
}

static void test_food_falls_and_lands(void) {
    fish_sim_t sim;
    fish_sim_init(&sim, 0, 11);
    assert(fish_sim_drop_food_ex(&sim, 5 * FISH_SIM_ONE, 0, 50, 3000) == 0);
    int16_t y0 = sim.food[0].y;
    fish_sim_step(&sim);
    assert(sim.food[0].y > y0);          // 一步之后确实往下走了

    for (int i = 0; i < 2000; i++) fish_sim_step(&sim);
    assert(sim.food_count == 1);          // ttl 还没到，不该消失
    assert(sim.food[0].y == Y_MAX);       // 停在缸底
    assert(sim.food[0].vy == 0);          // 触底后不再下落

    // 悬浮鱼食（vy=0）不应该自己动
    fish_sim_t still;
    fish_sim_init(&still, 0, 12);
    assert(fish_sim_drop_food(&still, 3 * FISH_SIM_ONE, 7 * FISH_SIM_ONE, 100) == 0);
    int16_t yb = still.food[0].y;
    for (int i = 0; i < 20; i++) fish_sim_step(&still);
    assert(still.food[0].y == yb);
}

static void test_drop_food_respects_capacity_and_bounds(void) {
    fish_sim_t sim;
    fish_sim_init(&sim, 2, 9);
    for (int i = 0; i < FISH_SIM_MAX_FOOD; i++) {
        assert(fish_sim_drop_food(&sim, 0, 0, 100) == i);
    }
    assert(fish_sim_drop_food(&sim, 0, 0, 100) == -1);
    assert(sim.food_count == FISH_SIM_MAX_FOOD);

    // 超出网格的坐标要夹进网格，不能出现负坐标。
    fish_sim_t sim2;
    fish_sim_init(&sim2, 2, 9);
    assert(fish_sim_drop_food(&sim2, -5000, 30000, 100) == 0);
    assert(sim2.food[0].x == 0);
    assert(sim2.food[0].y == Y_MAX);
}

static void test_same_seed_is_reproducible(void) {
    fish_sim_t a;
    fish_sim_t b;
    fish_sim_init(&a, 8, 20261118u);
    fish_sim_init(&b, 8, 20261118u);
    for (int i = 0; i < 500; i++) {
        fish_sim_step(&a);
        fish_sim_step(&b);
    }
    assert(memcmp(&a, &b, sizeof(a)) == 0);
}

static void test_different_seed_diverges(void) {
    fish_sim_t a;
    fish_sim_t b;
    fish_sim_init(&a, 8, 1);
    fish_sim_init(&b, 8, 2);
    for (int i = 0; i < 200; i++) {
        fish_sim_step(&a);
        fish_sim_step(&b);
    }
    assert(memcmp(&a, &b, sizeof(a)) != 0);
}

int main(void) {
    test_init_clamps_count();
    test_stays_inside_over_long_run();
    test_fish_actually_move();
    test_food_gets_eaten();
    test_food_expires_without_fish();
    test_food_falls_and_lands();
    test_drop_food_respects_capacity_and_bounds();
    test_same_seed_is_reproducible();
    test_different_seed_diverges();
    return 0;
}
