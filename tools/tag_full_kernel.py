from pathlib import Path

root = Path(__file__).resolve().parents[1]
cpp = root / "app/src/main/cpp/cardpool.cpp"
hdr = root / "app/src/main/cpp/season_offsets.h"

t = cpp.read_text(encoding="utf-8")
t = t.replace("cardpool_start_2_6_6", "full_kernel_1_0_0_start")
t = t.replace("hybrid_kernel_", "FULL_KERNEL_")
if "Full-Kernel" not in t[:800]:
    banner = (
        "// =============================================================================\n"
        "// JCC Full-Kernel full-1.0.0  side branch: dump-backed full features\n"
        "// Spec: workspace/jcc-full-kernel/FEATURES.md PROTOCOL.md MEMORY.md\n"
        "// F1 pool F2 lineup-buy F3 predict F4 positions F5 warn F6 hex F7 board\n"
        "// F8 shop F9 log F10 no asset hooks | NO region-switch / FindObjectOfType / Unity.Screen\n"
        "// =============================================================================\n"
    )
    lines = t.splitlines(True)
    i = 0
    while i < len(lines) and (lines[i].startswith("//") or lines[i].strip() == ""):
        i += 1
    t = banner + "".join(lines[i:])
cpp.write_text(t, encoding="utf-8", newline="\n")

ot = hdr.read_text(encoding="utf-8")
ot = ot.replace('"2.6.6"', '"full-1.0.0"')
if "jcc-full-kernel" not in ot:
    ot = ot.replace(
        '#define JCC_SEASON_SCAN_DATE "2026-07-19"',
        '#define JCC_SEASON_SCAN_DATE "2026-07-19"\n/* branch jcc-full-kernel: dump-backed F1-F10 */',
    )
hdr.write_text(ot, encoding="utf-8", newline="\n")

t2 = cpp.read_text(encoding="utf-8")
for s in [
    "screen_size",
    "g_op_board",
    "lineup_has_hero",
    'std::string out = "1"',
    "FULL_KERNEL",
    "full_kernel_1_0_0",
]:
    print(s, s in t2)
print("braces", t2.count("{"), t2.count("}"))
print("done")
