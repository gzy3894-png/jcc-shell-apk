# -*- coding: utf-8 -*-
from pathlib import Path

p = Path(__file__).resolve().parents[1] / "app/src/main/cpp/cardpool.cpp"
t = p.read_text(encoding="utf-8")

repls = [
    (
        'slog("FAIL SearchACGHero*");',
        'JERRF("F1", "SearchACGHero_missing"); slog("FAIL SearchACGHero*");',
    ),
    (
        'slog("FAIL db instance");',
        'JERRF("F1", "db_instance_null"); slog("FAIL db instance");',
    ),
    (
        'slog("auto_buy: HeroRoot class missing");',
        'JERRF("F2", "HeroRoot_class_missing"); slog("auto_buy: HeroRoot class missing");',
    ),
    (
        'slog("auto_buy: ReqBuyHero missing");',
        'JERRF("F2", "ReqBuyHero_method_missing"); slog("auto_buy: ReqBuyHero missing");',
    ),
    (
        'if ((++noshop % 10) == 1) slog("auto_buy: shop empty / no BuyHeroView");',
        'if ((++noshop % 10) == 1) { err_throttle("F2", "no_shop", 1, "BuyHeroView empty"); slog("auto_buy: shop empty"); }',
    ),
    (
        'JCKPT("worker_start_2_6_3");',
        'JCKPT("worker_start_full");',
    ),
    (
        'slog("FAIL bind 31338");',
        'JERRF("SRV", "bind_31338_failed"); slog("FAIL bind 31338");',
    ),
    (
        'slog("server 31338 ready hybrid " JCC_SEASON_TAG " no resource hooks");',
        'slog("server 31338 ready " JCC_SEASON_TAG); JOKF("SRV", "listen_31338");',
    ),
]

# no_stage line may have chinese - match prefix
import re

t2 = t
for a, b in repls:
    if a in t2:
        t2 = t2.replace(a, b)
        print("ok", a[:40])
    else:
        print("miss", a[:50])

# auto_buy no stage
t2, n = re.subn(
    r'if \(\(\+\+miss % 15\) == 1\) slog\("auto_buy:[^"]*"\);',
    'if ((++miss % 15) == 1) { err_throttle("F2", "no_stage", 1, "apply TeamRecommend lineup"); slog("auto_buy: no_stage"); }',
    t2,
    count=1,
)
print("no_stage", n)

# nomatch
t2, n = re.subn(
    r'if \(\(\+\+nomatch % 8\) == 1\) slog\("auto_buy:[^"]*"\);',
    'if ((++nomatch % 8) == 1) { err_throttle("F2", "no_lineup_hero_in_shop", 1, "shop ids not in lineup"); slog("auto_buy: no_match"); }',
    t2,
    count=1,
)
print("nomatch", n)

# BUY success
if 'JOKF("F2"' not in t2:
    t2 = t2.replace(
        'slog(msg);\n        return; // 每轮最多买一张',
        'slog(msg); JOKF("F2", "BUY %s", msg); return; // one per tick',
    )
    # fallback english comment version
    t2 = t2.replace(
        'slog(msg);\n        return; // 每轮最多买一张，避免连点',
        'slog(msg); JOKF("F2", "%s", msg); return;',
    )

# heartbeat expand
old_hb = '''            char hb[320];
            snprintf(hb, sizeof(hb), "hb hex=%s pred=%s pos=%s ready=%d lineup=%s", hex.c_str(),
                     pred.c_str(), pos.c_str(), g_overlay_ready.load() ? 1 : 0, lineup.c_str());
            slog(hb);'''
new_hb = '''            char hb[360];
            snprintf(hb, sizeof(hb),
                     "hb hex=%s pred=%s pos=%s ready=%d lineup=%s auto=%d board=%d",
                     hex.c_str(), pred.c_str(), pos.c_str(), g_overlay_ready.load() ? 1 : 0,
                     lineup.c_str(), g_auto_buy.load() ? 1 : 0, g_op_board.load() ? 1 : 0);
            slog(hb);
            JLOGI("STATUS F1pool=%zu F3pred=%s F5warn_len=%zu ready=%d F2lineup=%s",
                  g_pool.size(), pred.c_str(), g_warn.size(), g_overlay_ready.load() ? 1 : 0,
                  lineup.c_str());'''
if old_hb in t2:
    t2 = t2.replace(old_hb, new_hb)
    print("hb ok")
else:
    print("hb miss")

# start banner log paths
if "LOG_PATHS" not in t2:
    t2 = t2.replace(
        'JCKPT("full_kernel_1_0_0_start");',
        'JCKPT("full_kernel_1_0_0_start");\n'
        '    JLOGI("LOG_PATHS game/files/jcc_full.log + /sdcard/Download/jcc-scan/jcc_full.log");\n'
        '    JLOGI("LOG_ERR jcc_last_error.txt + jcc_errors.log (same dirs)");\n'
        '    JLOGI("LOG_PULL adb pull /sdcard/Download/jcc-scan/");',
    )
    print("boot paths ok")

# predict empty / ok
if 'ERR["F3"' not in t2 and 'JERRF("F3"' not in t2:
    # after g_pred = EMPTY when no battle in refresh_opponent
    t2 = t2.replace(
        """    if (!battle) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_opp_info = "EMPTY";
        g_pred = "EMPTY";
        return;
    }
    int myId = get_my_id(battle, cbm);
    if (!pid_ok(myId)) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pred = "EMPTY";
        return;
    }""",
        """    if (!battle) {
        err_throttle("F3", "no_battle", 30, "predict");
        std::lock_guard<std::mutex> lk(g_mu);
        g_opp_info = "EMPTY";
        g_pred = "EMPTY";
        return;
    }
    int myId = get_my_id(battle, cbm);
    if (!pid_ok(myId)) {
        err_throttle("F3", "bad_myId", 20, "get_MyPlayerId failed");
        std::lock_guard<std::mutex> lk(g_mu);
        g_pred = "EMPTY";
        return;
    }""",
    )
    print("F3 err ok")

# when state==0 after compute
if 'no_opp_rank' not in t2:
    t2 = t2.replace(
        "g_pred = state ? pred : \"EMPTY\";\n    }\n    if (state) JLOGI(\"%s\", info);",
        "g_pred = state ? pred : \"EMPTY\";\n    }\n"
        "    if (state) { JOKF(\"F3\", \"%s\", info); }\n"
        "    else { err_throttle(\"F3\", \"no_opp_rank\", 20, info); }",
    )
    print("F3 state ok")

# positions
if 'JERRF("F4"' not in t2 and 'err_throttle("F4"' not in t2:
    t2 = t2.replace(
        """    if (!battle) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pos = "EMPTY";
        return;
    }
    int myId = get_my_id(battle, cbm);
    if (!pid_ok(myId)) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pos = "EMPTY";
        return;
    }""",
        """    if (!battle) {
        err_throttle("F4", "no_battle", 30, "positions");
        std::lock_guard<std::mutex> lk(g_mu);
        g_pos = "EMPTY";
        return;
    }
    int myId = get_my_id(battle, cbm);
    if (!pid_ok(myId)) {
        err_throttle("F4", "bad_myId", 20, "positions");
        std::lock_guard<std::mutex> lk(g_mu);
        g_pos = "EMPTY";
        return;
    }""",
    )
    print("F4 err ok")

# F1 scan empty
t2 = t2.replace(
    'slog("scan_heroes empty");',
    'JERRF("F1", "scan_empty"); slog("scan_heroes empty");',
)
if "scan_heroes empty" not in t2 and "scan empty" not in t2:
    # find success path end
    pass

# handle SET/GET log errors
if 'JERRF("PROTO"' not in t2:
    t2 = t2.replace(
        'if (!req || !*req) return "RSP:ERR\\n";',
        'if (!req || !*req) { JERRF("PROTO", "empty_req"); return "RSP:ERR\\n"; }',
    )

p.write_text(t2, encoding="utf-8", newline="\n")
print("JERRF count", t2.count("JERRF"))
print("err_throttle count", t2.count("err_throttle"))
print("brace", t2.count("{"), t2.count("}"))
