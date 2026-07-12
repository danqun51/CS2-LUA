#!/usr/bin/env python3
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = Path(r"D:\Documents\CS2 LUA\output")
OUT_LUA = ROOT / "scripts" / "offsets.lua"


def lua_value(v, indent=0):
    sp = "  " * indent
    if isinstance(v, dict):
        lines = ["{"]
        for k, val in v.items():
            key = f"[{json.dumps(str(k), ensure_ascii=False)}]"
            lines.append(f"{sp}  {key} = {lua_value(val, indent + 1)},")
        lines.append(f"{sp}}}")
        return "\n".join(lines)
    if isinstance(v, list):
        lines = ["{"]
        for val in v:
            lines.append(f"{sp}  {lua_value(val, indent + 1)},")
        lines.append(f"{sp}}}")
        return "\n".join(lines)
    if isinstance(v, str):
        return json.dumps(v, ensure_ascii=False)
    if isinstance(v, bool):
        return "true" if v else "false"
    if v is None:
        return "nil"
    return str(v)


def main():
    output = Path(__import__('sys').argv[1]) if len(__import__('sys').argv) > 1 else DEFAULT_OUTPUT
    payload = {
        "source_dir": str(output),
        "info": json.loads((output / "info.json").read_text(encoding="utf-8")),
        "offsets": json.loads((output / "offsets.json").read_text(encoding="utf-8")),
        "buttons": json.loads((output / "buttons.json").read_text(encoding="utf-8")),
    }
    text = "-- Auto-generated from CS2 offset JSON. Do not edit by hand.\n"
    text += "-- Source: " + str(output).replace("\\", "\\\\") + "\n\n"
    text += "return " + lua_value(payload) + "\n"
    OUT_LUA.write_text(text, encoding="utf-8")
    print(f"wrote {OUT_LUA}")

if __name__ == "__main__":
    main()
