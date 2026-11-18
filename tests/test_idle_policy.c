// tests/test_idle_policy.c —— 省电策略的主机测试。
#include <assert.h>
#include "idle_policy.h"

static void test_thresholds(void) {
    // 30 秒之前保持满帧
    assert(power_state_next(POWER_ACTIVE, 0) == POWER_ACTIVE);
    assert(power_state_next(POWER_ACTIVE, IDLE_TO_REDUCED_US - 1) == POWER_ACTIVE);
    // 到 30 秒降帧
    assert(power_state_next(POWER_ACTIVE, IDLE_TO_REDUCED_US) == POWER_REDUCED);
    // 到 2 分 30 秒息屏
    assert(power_state_next(POWER_REDUCED, IDLE_TO_SLEEP_US) == POWER_SLEEP);
    // 从满帧直接跨过两个门限也要进 SLEEP（比如开机后一直没人动）
    assert(power_state_next(POWER_ACTIVE, IDLE_TO_SLEEP_US + 1) == POWER_SLEEP);
}

static void test_fps_mapping(void) {
    assert(power_state_target_fps(POWER_ACTIVE) >= 30);
    assert(power_state_target_fps(POWER_REDUCED) > 0);
    assert(power_state_target_fps(POWER_REDUCED) < power_state_target_fps(POWER_ACTIVE));
    assert(power_state_target_fps(POWER_SLEEP) == 0);
}

int main(void) {
    test_thresholds();
    test_fps_mapping();
    return 0;
}
