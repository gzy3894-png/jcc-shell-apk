from pathlib import Path
import re


def strip_c_comments(text: str) -> str:
    out = []
    i = 0
    n = len(text)
    in_str = False
    str_ch = ""
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == str_ch:
                in_str = False
            i += 1
            continue
        if c in "\"'":
            in_str = True
            str_ch = c
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2 if i + 1 < n else 1
            continue
        out.append(c)
        i += 1
    return re.sub(r"\n{3,}", "\n\n", "".join(out))


files = [
    Path("app/src/main/cpp/inject_entry.cpp"),
    Path("app/src/main/cpp/mini_inject.cpp"),
    Path("app/src/main/cpp/jcc_log.h"),
    Path("app/src/main/cpp/season_offsets.h"),
    Path("app/src/main/cpp/cardpool.h"),
    Path("app/src/main/cpp/cardpool.cpp"),
    Path("app/src/main/cpp/CMakeLists.txt"),
]
root = Path(__file__).resolve().parents[1]
for rel in files:
    p = root / rel
    if not p.exists():
        continue
    t = p.read_text(encoding="utf-8")
    p.write_text(strip_c_comments(t), encoding="utf-8", newline="\n")
    print("ok", rel)
