#!/usr/bin/env python3
from pathlib import Path
import re
import sys

MARKER = "LIGHT_SLEEP_GT911_INVALIDATE_STALE_POINTS_V6"

def matching_brace(text: str, open_pos: int) -> int:
    depth = 0
    state = "code"
    i = open_pos
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "/" and nxt == "/":
                state = "line"
                i += 1
            elif ch == "/" and nxt == "*":
                state = "block"
                i += 1
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return i
        elif state == "string":
            if ch == "\\":
                i += 1
            elif ch == '"':
                state = "code"
        elif state == "char":
            if ch == "\\":
                i += 1
            elif ch == "'":
                state = "code"
        elif state == "line":
            if ch == "\n":
                state = "code"
        elif state == "block":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 1
        i += 1
    raise RuntimeError("Could not find closing brace")

def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} /path/to/esp_lcd_touch_gt911.c", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    if not path.is_file():
        raise RuntimeError(f"GT911 source not found: {path}")

    text = path.read_text()

    fn = re.search(
        r'\bstatic\s+esp_err_t\s+esp_lcd_touch_gt911_read_data\s*'
        r'\(\s*esp_lcd_touch_handle_t\s+tp\s*\)\s*\{',
        text,
        re.S,
    )
    if not fn:
        raise RuntimeError(
            "esp_lcd_touch_gt911_read_data() was not found; "
            "refusing to patch an unknown driver layout"
        )

    open_pos = text.find("{", fn.start())
    close_pos = matching_brace(text, open_pos)
    block = text[fn.start():close_pos + 1]

    if MARKER in block:
        print(f"[OK] GT911 stale-point invalidation already present: {path}")
        return 0

    # Restrict the patch to the driver's DATA_READY=0 branch.
    branch = re.search(
        r'if\s*\(\s*\(buf\[0\]\s*&\s*0x80\)\s*==\s*0x00\s*\)\s*\{',
        block,
    )
    if not branch:
        raise RuntimeError(
            "Expected GT911 DATA_READY=0 branch was not found; "
            "refusing to patch an unknown driver version"
        )

    branch_abs_open = fn.start() + branch.end() - 1

    # Confirm the branch really is the no-new-data path that clears 0x814E.
    branch_tail = text[branch_abs_open: min(branch_abs_open + 800, close_pos)]
    if "ESP_LCD_TOUCH_GT911_READ_XY_REG" not in branch_tail:
        raise RuntimeError("GT911 status-clear statement not found near DATA_READY=0 branch")

    line_start = text.rfind("\n", fn.start(), branch_abs_open) + 1
    indent_match = re.match(r"[ \t]*", text[line_start:])
    base_indent = indent_match.group(0) if indent_match else ""
    body_indent = base_indent + "    "

    injection = (
        "\n"
        f"{body_indent}/* {MARKER}: DATA_READY=0 means there is no fresh touch packet.\n"
        f"{body_indent} * Invalidate cached coordinates before any getter can reuse them. */\n"
        f"{body_indent}portENTER_CRITICAL(&tp->data.lock);\n"
        f"{body_indent}tp->data.points = 0;\n"
        f"{body_indent}portEXIT_CRITICAL(&tp->data.lock);\n"
    )

    updated = text[:branch_abs_open + 1] + injection + text[branch_abs_open + 1:]
    path.write_text(updated)

    print(f"[OK] Patched GT911 stale-point cache bug: {path}")
    print("[OK] DATA_READY=0 now forces tp->data.points = 0")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
