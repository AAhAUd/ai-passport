// main/idle_policy.c —— 见 idle_policy.h。
#include "idle_policy.h"

power_state_t power_state_next(power_state_t current, int64_t idle_us) {
    if (idle_us >= IDLE_TO_SLEEP_US) return POWER_SLEEP;
    if (idle_us >= IDLE_TO_REDUCED_US) {
        // 从 SLEEP 醒来但还没按键的情形不该出现；保守起见允许回到 REDUCED。
        return POWER_REDUCED;
    }
    (void)current;
    return POWER_ACTIVE;
}

int power_state_target_fps(power_state_t state) {
    switch (state) {
    case POWER_ACTIVE:  return 55;
    case POWER_REDUCED: return 15;
    default:            return 0;
    }
}
