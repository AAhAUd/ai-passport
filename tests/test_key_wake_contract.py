#!/usr/bin/env python3
"""Static contracts for the deep-sleep key-wake guard wiring in main.c."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source)
    if not match:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
    raise AssertionError(f"function is unterminated: {name}")


class KeyWakeGuardWiringTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.main = read("main/main.c")

    def test_on_key_consumes_the_guard_before_dispatching_actions(self) -> None:
        # 鱼缸应用没有独立的输入任务（渲染任务独占面板），因此基线 demo 里
        # "dispatch 未就绪就早退"的分支不存在。守卫的契约不变：先消费守卫，
        # 再执行任何按键动作，否则唤醒那一下会被当成真实操作。
        body = function_body(self.main, "on_key")
        self.assertIn("key_guard_consume(", body)
        guard_at = body.index("key_guard_consume(")
        for action in ("aquarium_feed(", "aquarium_brightness_step("):
            self.assertIn(action, body)
            self.assertLess(guard_at, body.index(action))


if __name__ == "__main__":
    unittest.main()
