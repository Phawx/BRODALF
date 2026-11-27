"""Print the window app's control, menu and dialog IDs as NAME=VALUE lines.

The screenshot driver (Shots.cs) sends these IDs to brodalf.exe, the same
WM_COMMAND messages a click sends, so it reads them from the source each
run instead of keeping a copy that could go stale.

usage: python ids.py gui/main.c gui/resource.h > ids.txt
"""
import re
import sys


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def enums(text, env):
    for body in re.findall(r"\benum\s*\{(.*?)\}", text, flags=re.S):
        value = -1
        local = {}
        try:
            for item in body.split(","):
                item = item.strip()
                if not item:
                    continue
                if "=" in item:
                    name, expr = (s.strip() for s in item.split("=", 1))
                    value = int(eval(expr, {"__builtins__": {}}, {**env, **local}))
                else:
                    name = item
                    value += 1
                if not re.fullmatch(r"[A-Za-z_]\w*", name):
                    raise ValueError(name)
                local[name] = value
        except Exception:
            continue  # an enum built on Windows' own constants (WM_APP + 1): not needed
        env.update(local)


def defines(text, env):
    for name, value in re.findall(r"^\s*#define\s+(ID[A-Z]?_\w+)\s+(\d+)\s*$", text, flags=re.M):
        env[name] = int(value)


def main(paths):
    env = {}
    for path in paths:
        with open(path, encoding="utf-8") as f:
            text = strip_comments(f.read())
        if path.endswith(".h"):
            defines(text, env)
        else:
            enums(text, env)
    for name in sorted(env):
        print(f"{name}={env[name]}")


if __name__ == "__main__":
    main(sys.argv[1:])
