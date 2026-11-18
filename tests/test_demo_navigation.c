#include <assert.h>

#include "demo_navigation.h"

int main(void) {
    demo_navigation_t navigation;
    demo_navigation_init(&navigation, 3);

    demo_nav_result_t result = demo_navigation_handle(
        &navigation, DEMO_NAV_INPUT_UP_CLICK, true);
    assert(result.action == DEMO_NAV_ACTION_REFRESH);
    assert(navigation.selected == 2);

    result = demo_navigation_handle(&navigation, DEMO_NAV_INPUT_OK_CLICK, false);
    assert(result.action == DEMO_NAV_ACTION_NONE);
    assert(navigation.active == -1);

    result = demo_navigation_handle(&navigation, DEMO_NAV_INPUT_DOWN_CLICK, true);
    assert(result.action == DEMO_NAV_ACTION_REFRESH);
    assert(navigation.selected == 0);

    result = demo_navigation_handle(&navigation, DEMO_NAV_INPUT_OK_CLICK, true);
    assert(result.action == DEMO_NAV_ACTION_ENTER);
    assert(result.index == 0);
    assert(navigation.active == 0);

    result = demo_navigation_handle(&navigation, DEMO_NAV_INPUT_OTHER, true);
    assert(result.action == DEMO_NAV_ACTION_FORWARD);
    assert(result.index == 0);

    // 页面里长按 OK 是页面自己的动作（鱼缸页=投喂），不再退出
    result = demo_navigation_handle(&navigation, DEMO_NAV_INPUT_OK_LONG, true);
    assert(result.action == DEMO_NAV_ACTION_FORWARD);
    assert(navigation.active == 0);

    // 退出改为显式的 DOWN 长按
    result = demo_navigation_handle(&navigation, DEMO_NAV_INPUT_EXIT_LONG, true);
    assert(result.action == DEMO_NAV_ACTION_EXIT);
    assert(navigation.active == 0);       // 退出要等 EXIT 处理完再清 active
    assert(navigation.active == 0);

    demo_navigation_complete_exit(&navigation);
    assert(navigation.active == -1);
    return 0;
}
