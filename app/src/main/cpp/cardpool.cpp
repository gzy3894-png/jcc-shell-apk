// JCC 2.6.6 混合内核（不 hook 资源/加载）
// - 覆盖层防闪退：禁止后台调 UnityEngine.Screen；默认不 PUSH 站位；
//   海克斯仅在 pos+pred 就绪后非 EMPTY；预测/rank 优先字段、少 invoke
// - 三星预警 / 阵容自动买 / battleState=4
#include "cardpool.h"
#include "il2cpp-class.h"
#include "jcc_log.h"
#include "log.h"
#include "season_offsets.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define DO_API(r, n, p) extern r (*n) p
#include "il2cpp-api-functions.h"
#undef DO_API

static constexpr int PORT = 31338;
static constexpr int MAX_ID = 12000;
static constexpr int MAX_CLIENTS = 8;

static char g_dir[512]{};
static std::atomic<bool> g_run{false};
static std::mutex g_mu;
static std::mutex g_cli_mu;
static int g_clients[MAX_CLIENTS];
static int g_ncli = 0;

static std::string g_pool;
static std::string g_status = "init";
static std::string g_log;
static std::string g_hex = "EMPTY"; // UI: 非 EMPTY 才当 inBattle
static std::string g_warn = "EMPTY";
static std::string g_pos = "EMPTY";
static std::string g_pred = "EMPTY";
static std::string g_opp_info = "EMPTY";
static std::string g_board_push; // last OPPONENT_BOARD payload (no prefix)
static std::string g_lineup_dbg = "none";

static std::atomic<bool> g_auto_buy{false};
static std::atomic<bool> g_popup_block{false};
static std::atomic<bool> g_shop_show{true};
// 默认关：PUSH 站位会走 FloatingService.showOpponentBoard，未开覆盖层时易踩 UI 空指针
static std::atomic<bool> g_op_board{false};
static std::atomic<bool> g_overlay_ready{false}; // hex/pos/pred 成套就绪后才让 UI inBattle

struct Hero {
    int id{}, cost{}, total{}, rem{};
    std::string name, icon;
};
static std::vector<Hero> g_heroes;
static std::map<int, int> g_owned; // heroId -> piece count (all players)

static int tier_total(int c) {
    switch (c) {
        case 1: return 29;
        case 2: return 22;
        case 3: return 18;
        case 4: return 12;
        case 5: return 10;
        default: return 0;
    }
}

static int star_pieces(int star) {
    if (star <= 1) return 1;
    if (star == 2) return 3;
    if (star >= 3) return 9;
    return 1;
}

static void slog(const char *m) {
    if (!m) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_log.size() > 40 * 1024) g_log.erase(0, 20 * 1024);
        g_log.append(m);
        g_log.push_back('\n');
        g_status = m;
    }
    JLOGI("%s", m);
}

static std::string u16(const Il2CppChar *p, int n) {
    std::string o;
    if (!p || n <= 0) return o;
    for (int i = 0; i < n; i++) {
        uint32_t c = p[i];
        if (c < 0x80)
            o.push_back((char)c);
        else if (c < 0x800) {
            o.push_back((char)(0xC0 | (c >> 6)));
            o.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            o.push_back((char)(0xE0 | (c >> 12)));
            o.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            o.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    return o;
}

static std::string istr(Il2CppString *s) {
    if (!s || !il2cpp_string_chars || !il2cpp_string_length) return {};
    return u16(il2cpp_string_chars(s), il2cpp_string_length(s));
}

static void scrub(std::string &s) {
    for (char &c : s)
        if (c == ':' || c == ',' || c == '|' || c == ';' || c == '\n' || c == '\r') c = '_';
}

static Il2CppClass *find_class(const char *ns, const char *name) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_assembly_get_image ||
        !il2cpp_class_from_name)
        return nullptr;
    size_t n = 0;
    auto d = il2cpp_domain_get();
    if (!d) return nullptr;
    auto asms = il2cpp_domain_get_assemblies(d, &n);
    if (!asms) return nullptr;
    for (size_t i = 0; i < n; i++) {
        auto img = il2cpp_assembly_get_image(asms[i]);
        if (!img) continue;
        if (ns && ns[0]) {
            auto k = il2cpp_class_from_name(img, ns, name);
            if (k) return k;
        }
        auto k2 = il2cpp_class_from_name(img, "", name);
        if (k2) return k2;
        if (ns && strcmp(ns, "ZGameChess") != 0) {
            auto k3 = il2cpp_class_from_name(img, "ZGameChess", name);
            if (k3) return k3;
        }
        if (ns && strcmp(ns, "ZGame") != 0) {
            auto k4 = il2cpp_class_from_name(img, "ZGame", name);
            if (k4) return k4;
        }
        if (ns && strcmp(ns, "ZGameClient") != 0) {
            auto k5 = il2cpp_class_from_name(img, "ZGameClient", name);
            if (k5) return k5;
        }
    }
    return nullptr;
}

static const MethodInfo *meth(Il2CppClass *k, const char *n, int a) {
    if (!k || !il2cpp_class_get_method_from_name) return nullptr;
    return il2cpp_class_get_method_from_name(k, n, a);
}

static void ensure_il2cpp_thread() {
    if (!il2cpp_domain_get || !il2cpp_thread_attach) return;
    if (il2cpp_thread_current && il2cpp_thread_current()) return;
    auto d = il2cpp_domain_get();
    if (d) il2cpp_thread_attach(d);
}

static Il2CppObject *inv(const MethodInfo *m, void *obj, void **p) {
    if (!m || !il2cpp_runtime_invoke) return nullptr;
    ensure_il2cpp_thread();
    Il2CppException *e = nullptr;
    auto r = il2cpp_runtime_invoke(m, obj, p, &e);
    return e ? nullptr : r;
}

static Il2CppObject *singleton(Il2CppClass *k) {
    auto m = meth(k, "get_Instance", 0);
    if (!m) m = meth(k, "get_instance", 0);
    return m ? inv(m, nullptr, nullptr) : nullptr;
}

static int unbox_i32(Il2CppObject *r) {
    if (!r) return -1;
    if (il2cpp_object_unbox) return *(int *)il2cpp_object_unbox(r);
    return *(int *)((char *)r + 0x10);
}

static bool unbox_bool(Il2CppObject *r) {
    if (!r) return false;
    if (il2cpp_object_unbox) return *(bool *)il2cpp_object_unbox(r);
    return *(bool *)((char *)r + 0x10);
}

static bool list_view(Il2CppObject *list, void **items_out, int *size_out) {
    if (!list) return false;
    char *b = (char *)list;
    *items_out = *(void **)(b + JCC_LIST_ITEMS);
    *size_out = *(int *)(b + JCC_LIST_SIZE);
    return *items_out && *size_out > 0;
}

static Il2CppObject *list_obj_at(void *items, int i) {
    if (!items || i < 0) return nullptr;
    return *(Il2CppObject **)((char *)items + JCC_ARR_FIRST + i * (int)sizeof(void *));
}

static int list_i32_at(void *items, int i) {
    if (!items || i < 0) return 0;
    return *(int *)((char *)items + JCC_ARR_FIRST + i * 4);
}

static void client_add(int fd) {
    std::lock_guard<std::mutex> lk(g_cli_mu);
    if (g_ncli < MAX_CLIENTS) g_clients[g_ncli++] = fd;
}

static void client_del(int fd) {
    std::lock_guard<std::mutex> lk(g_cli_mu);
    for (int i = 0; i < g_ncli; i++) {
        if (g_clients[i] == fd) {
            g_clients[i] = g_clients[--g_ncli];
            break;
        }
    }
}

static void push_all(const std::string &line) {
    // line must already include trailing \n; full "PUSH:...\n"
    std::lock_guard<std::mutex> lk(g_cli_mu);
    for (int i = 0; i < g_ncli;) {
        int fd = g_clients[i];
        if (send(fd, line.data(), line.size(), MSG_NOSIGNAL) < 0) {
            close(fd);
            g_clients[i] = g_clients[--g_ncli];
        } else {
            i++;
        }
    }
}

// ---- battle model ----
static Il2CppObject *get_battle(Il2CppClass **cbm_out) {
    Il2CppClass *cbm = find_class("ZGameChess", "ChessBattleModel");
    if (!cbm) cbm = find_class("", "ChessBattleModel");
    if (cbm_out) *cbm_out = cbm;
    if (!cbm) return nullptr;
    Il2CppClass *cmm = find_class("ZGameChess", "ChessModelManager");
    if (!cmm) cmm = find_class("", "ChessModelManager");
    if (cmm) {
        auto inst = singleton(cmm);
        auto m = meth(cmm, "GetBattleModel", 0);
        if (inst && m) {
            auto b = inv(m, inst, nullptr);
            if (b) return b;
        }
    }
    return singleton(cbm);
}

static Il2CppObject *get_my_pm(Il2CppObject *battle, Il2CppClass *cbm) {
    if (!battle || !cbm) return nullptr;
    auto m = meth(cbm, "GetMyPlayerModel", 0);
    return m ? inv(m, battle, nullptr) : nullptr;
}

static Il2CppObject *get_player(Il2CppObject *battle, Il2CppClass *cbm, int pid) {
    if (!battle || !cbm || pid < 0) return nullptr;
    auto m = meth(cbm, "GetPlayer", 1);
    if (!m) return nullptr;
    int32_t a = pid;
    void *p[1] = {&a};
    return inv(m, battle, p);
}

static int get_my_id(Il2CppObject *battle, Il2CppClass *cbm) {
    if (!battle || !cbm) return -1;
    auto m = meth(cbm, "get_MyPlayerId", 0);
    if (!m) return -1;
    return unbox_i32(inv(m, battle, nullptr));
}

static bool pid_ok(int pid) { return pid >= 0 && pid < 100000000; }

static int get_match_id(Il2CppObject *battle, Il2CppClass *cbm, int myId) {
    if (!battle || !cbm || !pid_ok(myId)) return -1;
    // 优先字段：少 invoke 降闪退（后台线程调托管方法高风险）
    auto my = get_my_pm(battle, cbm);
    if (my) {
        int enemy = *(int *)((char *)my + JCC_PM_ENEMY_PLAYER);
        if (pid_ok(enemy) && enemy != myId) return enemy;
        int le = *(int *)((char *)my + JCC_PM_LAST_ENEMY);
        if (pid_ok(le) && le != myId) return le;
    }
    auto m = meth(cbm, "GetMatchPlayerId", 1);
    if (!m) return -1;
    int32_t a = myId;
    void *p[1] = {&a};
    int r = unbox_i32(inv(m, battle, p));
    return pid_ok(r) ? r : -1;
}

static int get_rank(Il2CppObject *battle, Il2CppClass *cbm, int pid) {
    if (!battle || !cbm || !pid_ok(pid)) return -1;
    // 优先读 PlayerModel.Rank@0x4c
    auto pm = get_player(battle, cbm, pid);
    if (pm) {
        int r = *(int *)((char *)pm + JCC_PM_RANK);
        if (r >= 0 && r <= 7) return r;
    }
    auto m = meth(cbm, "GetPlayerRankByID", 1);
    if (!m) return -1;
    int32_t a = pid;
    void *p[1] = {&a};
    int r = unbox_i32(inv(m, battle, p));
    if (r >= 0 && r <= 7) return r;
    return -1;
}

// 禁止后台线程调 UnityEngine.Screen（会闪退）
static void screen_size(int *w, int *h) {
    *w = 1080;
    *h = 2400;
}

// ---- 牌库 ----
static std::string build_pool() {
    std::string o;
    o.reserve(g_heroes.size() * 40);
    for (auto &h : g_heroes) {
        int own = 0;
        auto it = g_owned.find(h.id);
        if (it != g_owned.end()) own = it->second;
        int rem = h.total - own;
        if (rem < 0) rem = 0;
        h.rem = rem;
        char b[512];
        if (!h.icon.empty())
            snprintf(b, sizeof(b), "%d:%s:%d:%d:%d:%s", h.id, h.name.c_str(), h.cost, rem, h.total,
                     h.icon.c_str());
        else
            snprintf(b, sizeof(b), "%d:%s:%d:%d:%d", h.id, h.name.c_str(), h.cost, rem, h.total);
        if (!o.empty()) o.push_back(',');
        o.append(b);
    }
    return o;
}

static void save_pool(const std::string &p) {
    if (!g_dir[0]) return;
    char path[600];
    snprintf(path, sizeof(path), "%s/files/jcc_cardpool.txt", g_dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(p.data(), 1, p.size(), f);
        fputc('\n', f);
        fflush(f);
        fclose(f);
    }
}

static bool scan_heroes() {
    JCKPT("scan_heroes");
    slog("scan_heroes begin " JCC_SEASON_TAG);
    Il2CppClass *db = find_class("ZGame", "DataBaseManager");
    if (!db) db = find_class("", "DataBaseManager");
    if (!db) {
        slog("FAIL DataBaseManager");
        return false;
    }
    auto m = meth(db, "SearchACGHero2", 1);
    if (!m) m = meth(db, "SearchACGHero", 1);
    if (!m) {
        slog("FAIL SearchACGHero*");
        return false;
    }
    auto inst = singleton(db);
    if (!inst) {
        slog("FAIL db instance");
        return false;
    }
    std::vector<Hero> rows;
    for (int id = 1; id <= MAX_ID; id++) {
        int32_t hid = id;
        void *params[1] = {&hid};
        auto hero = inv(m, inst, params);
        if (!hero) continue;
        char *b = (char *)hero;
        int iid = *(int *)(b + JCC_HERO_IID);
        int cost = *(int *)(b + JCC_HERO_ICOST);
        auto *ns = *(Il2CppString **)(b + JCC_HERO_SNAME);
        auto *ic = *(Il2CppString **)(b + JCC_HERO_PAINT_SMALL);
        if (iid <= 0) iid = id;
        if (cost < 1 || cost > 5) continue;
        std::string name = istr(ns);
        if (name.empty()) continue;
        std::string icon = istr(ic);
        scrub(name);
        scrub(icon);
        Hero h{};
        h.id = iid;
        h.cost = cost;
        h.total = tier_total(cost);
        h.name = name;
        h.icon = icon;
        rows.push_back(h);
    }
    if (rows.empty()) {
        slog("FAIL no heroes");
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_heroes.swap(rows);
        g_pool = build_pool();
    }
    save_pool(g_pool);
    char msg[80];
    snprintf(msg, sizeof(msg), "scan_heroes ok n=%zu", g_heroes.size());
    slog(msg);
    JCKPT("scan_heroes_ok");
    return true;
}

// ---- HA quality: PlayerModel._HAConfigIDs + TACG_HABasicConfig.iLevel ----
static int ha_level_of(int cfgId) {
    if (cfgId <= 0) return 0;
    Il2CppClass *db = find_class("ZGame", "DataBaseManager");
    if (!db) return 0;
    auto m = meth(db, "SearchTACG_HABasicConfig_Client", 1);
    if (!m) m = meth(db, "SearchHABasicConfigClient", 1);
    if (!m) m = meth(db, "SearchHABasicConfigClient2", 1);
    if (!m) return 0;
    auto inst = singleton(db);
    if (!inst) return 0;
    int32_t id = cfgId;
    void *p[1] = {&id};
    auto cfg = inv(m, inst, p);
    if (!cfg) return 0;
    int lv = *(int *)((char *)cfg + JCC_HA_ILEVEL);
    if (lv < 0) lv = 0;
    if (lv > 3) lv = 3;
    return lv;
}

static void read_ha_ids(Il2CppObject *list, int *out, int maxn, int *n_out) {
    *n_out = 0;
    void *items = nullptr;
    int size = 0;
    if (!list_view(list, &items, &size)) return;
    if (size > maxn) size = maxn;
    // try as List<int>
    int ok = 0;
    for (int i = 0; i < size; i++) {
        int v = list_i32_at(items, i);
        if (v > 0 && v < 10000000) {
            out[(*n_out)++] = v;
            ok++;
        }
    }
    if (ok > 0) return;
    *n_out = 0;
    // fallback List<object> with iID@0x10
    for (int i = 0; i < size; i++) {
        auto *o = list_obj_at(items, i);
        if (!o) continue;
        int v = *(int *)((char *)o + JCC_HA_IID);
        if (v > 0) out[(*n_out)++] = v;
        if (*n_out >= maxn) break;
    }
}

static void refresh_hex() {
    // UI: 海克斯品质非 EMPTY → inBattle，会立刻 GET 预测/头像/预警。
    // 未成套就绪时必须 EMPTY，否则一点「预测覆盖层」就进重解析路径 → 闪退风险。
    Il2CppClass *cbm = nullptr;
    auto battle = get_battle(&cbm);
    auto pm = battle ? get_my_pm(battle, cbm) : nullptr;
    int myId = battle ? get_my_id(battle, cbm) : -1;
    if (!battle || !pm || !pid_ok(myId)) {
        g_overlay_ready.store(false);
        std::lock_guard<std::mutex> lk(g_mu);
        g_hex = "EMPTY";
        return;
    }
    char *b = (char *)pm;
    int ids[8]{};
    int n = 0;
    auto *active = *(Il2CppObject **)(b + JCC_PM_ACTIVE_HA_IDS);
    auto *cfg = *(Il2CppObject **)(b + JCC_PM_HA_CONFIG_IDS);
    auto *store = *(Il2CppObject **)(b + JCC_PM_HA_STORE_S6);
    if (active) read_ha_ids(active, ids, 3, &n);
    if (n == 0 && cfg) read_ha_ids(cfg, ids, 3, &n);
    if (n == 0 && store) read_ha_ids(store, ids, 3, &n);

    int q[3] = {0, 0, 0};
    for (int i = 0; i < n && i < 3; i++) q[i] = ha_level_of(ids[i]);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d,%d,%d", q[0], q[1], q[2]);

    // 仅当头像位置已写出时才开放 inBattle（覆盖层成套）
    bool pos_ok = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        pos_ok = !g_pos.empty() && g_pos != "EMPTY" && g_pos.find('x') != std::string::npos;
        if (pos_ok) {
            g_hex = buf;
            g_overlay_ready.store(true);
        } else {
            g_hex = "EMPTY";
            g_overlay_ready.store(false);
        }
    }
    if (pos_ok) {
        char msg[96];
        snprintf(msg, sizeof(msg), "hex q=%s n=%d ready=1", buf, n);
        JLOGI("%s", msg);
    }
}

// ---- unit helpers ----
static void foreach_unit_list(Il2CppObject *list,
                              void (*fn)(Il2CppObject *ud, void *ctx), void *ctx) {
    void *items = nullptr;
    int size = 0;
    if (!list_view(list, &items, &size)) return;
    if (size > 64) size = 64;
    for (int i = 0; i < size; i++) {
        auto *ud = list_obj_at(items, i);
        if (ud) fn(ud, ctx);
    }
}

struct PieceAcc {
    std::map<int, int> *pieces;
    std::map<int, std::string> *names;
};

static void acc_unit(Il2CppObject *ud, void *ctx) {
    auto *a = (PieceAcc *)ctx;
    char *b = (char *)ud;
    int hid = *(int *)(b + JCC_UD_HERO_ID);
    int lv = *(int *)(b + JCC_UD_LEVEL);
    if (hid <= 0) return;
    (*a->pieces)[hid] += star_pieces(lv);
    if (a->names && a->names->find(hid) == a->names->end()) {
        auto *ns = *(Il2CppString **)(b + JCC_UD_HERO_NAME);
        std::string name = istr(ns);
        scrub(name);
        if (!name.empty()) (*a->names)[hid] = name;
    }
}

static void collect_player_pieces(Il2CppObject *pm, std::map<int, int> &pieces,
                                  std::map<int, std::string> *names) {
    if (!pm) return;
    char *b = (char *)pm;
    PieceAcc acc{&pieces, names};
    foreach_unit_list(*(Il2CppObject **)(b + JCC_PM_BATTLE_UNITS), acc_unit, &acc);
    foreach_unit_list(*(Il2CppObject **)(b + JCC_PM_WAIT_UNITS), acc_unit, &acc);
}

// ---- 三星预警（对齐 BattleOverlayView.parseWarning / drawThreeStarWarning）----
// 每人一段：
//   |{rank1based},{money},{level},{hp},{name}:{hid},{name},{pieces},{cost},{max9};...
// rank1based = 头像槽序号（UI 用 rank-1 对齐 parsePositions）
// pieces = 1★=1 / 2★=3 / 3★=9 折算；UI 默认 pieces≥3 才画
// cost = 费用 1–5；max 固定 9（三星）
static int hero_cost_of(int hid) {
    for (auto &h : g_heroes)
        if (h.id == hid) return h.cost;
    return 1;
}

static std::string hero_name_of(int hid, const std::map<int, std::string> &names) {
    auto it = names.find(hid);
    if (it != names.end() && !it->second.empty() && it->second != "?") return it->second;
    for (auto &h : g_heroes)
        if (h.id == hid && !h.name.empty()) return h.name;
    return "?";
}

static std::string player_name_of(Il2CppObject *pm) {
    if (!pm) return "";
    auto *rd = *(Il2CppObject **)((char *)pm + JCC_PM_RANK_DATA);
    if (!rd) return "";
    auto *ns = *(Il2CppString **)((char *)rd + JCC_UMD_NAME);
    std::string n = istr(ns);
    scrub(n);
    return n;
}

static int player_level_of(Il2CppObject *pm) {
    if (!pm) return 1;
    int lv = *(int *)((char *)pm + JCC_PM_MAX_HERO_NUM);
    if (lv < 1 || lv > 20) lv = 1;
    return lv;
}

// 收集 PlayerModel：不用 GetAllPlayers（后台 invoke 易闪退）
// 仅 my + match/enemy 字段 + chair 列表有限 GetPlayer
static void collect_all_pms(Il2CppObject *battle, Il2CppClass *cbm,
                            std::vector<Il2CppObject *> &out) {
    out.clear();
    if (!battle || !cbm) return;
    std::map<int, Il2CppObject *> uniq;
    auto add = [&](Il2CppObject *pm) {
        if (!pm) return;
        int pid = *(int *)((char *)pm + JCC_PM_PLAYER_ID);
        if (!pid_ok(pid)) return;
        uniq[pid] = pm;
    };
    auto my = get_my_pm(battle, cbm);
    add(my);
    int myId = get_my_id(battle, cbm);
    if (my) {
        int enemy = *(int *)((char *)my + JCC_PM_ENEMY_PLAYER);
        int le = *(int *)((char *)my + JCC_PM_LAST_ENEMY);
        if (pid_ok(enemy) && enemy != myId) add(get_player(battle, cbm, enemy));
        if (pid_ok(le) && le != myId) add(get_player(battle, cbm, le));
    }
    int matchId = get_match_id(battle, cbm, myId);
    if (pid_ok(matchId) && matchId != myId) add(get_player(battle, cbm, matchId));

    auto *chairs = *(Il2CppObject **)((char *)battle + JCC_CBM_CHAIR_LIST);
    void *items = nullptr;
    int size = 0;
    if (list_view(chairs, &items, &size)) {
        if (size > 8) size = 8;
        for (int i = 0; i < size; i++) {
            int pid = list_i32_at(items, i);
            if (pid_ok(pid)) add(get_player(battle, cbm, pid));
        }
    }
    for (auto &kv : uniq) out.push_back(kv.second);
}

static void refresh_warning() {
    Il2CppClass *cbm = nullptr;
    auto battle = get_battle(&cbm);
    if (!battle) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_warn = "EMPTY";
        return;
    }

    std::vector<Il2CppObject *> pms;
    collect_all_pms(battle, cbm, pms);
    if (pms.empty()) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_warn = "EMPTY";
        return;
    }

    // 前缀任意非空；UI 从 split("|")[1..] 读
    std::string out = "1";
    int added = 0;

    for (auto *pm : pms) {
        if (!pm) continue;
        char *b = (char *)pm;
        int pid = *(int *)(b + JCC_PM_PLAYER_ID);
        int money = *(int *)(b + JCC_PM_MONEY);
        int hp = *(int *)(b + JCC_PM_HP);
        int level = player_level_of(pm);
        // rank：优先 GetPlayerRankByID（0-based），再字段 Rank@0x4c
        int rank0 = get_rank(battle, cbm, pid);
        if (rank0 < 0 || rank0 > 7) {
            rank0 = *(int *)(b + JCC_PM_RANK);
            if (rank0 < 0 || rank0 > 7) continue;
        }
        int rank1 = rank0 + 1; // UI 1-based

        std::map<int, int> pieces;
        std::map<int, std::string> names;
        collect_player_pieces(pm, pieces, &names);

        std::string heroes;
        for (auto &kv : pieces) {
            if (kv.second < 3) continue; // 接近二星/三星才报
            int hid = kv.first;
            int pcs = kv.second;
            if (pcs > 9) pcs = 9;
            int cost = hero_cost_of(hid);
            if (cost < 1 || cost > 5) cost = 1;
            std::string nm = hero_name_of(hid, names);
            scrub(nm);
            char hb[160];
            // hid,name,pieces,cost,max(=9 for 3★)
            snprintf(hb, sizeof(hb), "%d,%s,%d,%d,9", hid, nm.c_str(), pcs, cost);
            if (!heroes.empty()) heroes.push_back(';');
            heroes.append(hb);
        }
        if (heroes.empty()) continue;

        std::string pname = player_name_of(pm);
        if (pname.empty()) pname = "?";
        scrub(pname);

        char head[128];
        // rank,money,level,hp,name
        snprintf(head, sizeof(head), "|%d,%d,%d,%d,%s:", rank1, money, level, hp, pname.c_str());
        out.append(head);
        out.append(heroes);
        added++;
    }

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_warn = added > 0 ? out : "EMPTY";
    }
    if (added > 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "warn players=%d pms=%zu", added, pms.size());
        JLOGI("%s", msg);
    }
}

// ---- owned counts (rem) — 全员 pieces 汇总 ----
static void refresh_owned() {
    Il2CppClass *cbm = nullptr;
    auto battle = get_battle(&cbm);
    if (!battle) return;
    std::map<int, int> total;
    std::vector<Il2CppObject *> pms;
    collect_all_pms(battle, cbm, pms);
    for (auto *pm : pms) {
        std::map<int, int> p;
        collect_player_pieces(pm, p, nullptr);
        for (auto &kv : p) total[kv.first] += kv.second;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_owned.swap(total);
        if (!g_heroes.empty()) g_pool = build_pool();
    }
}

// ---- 对手预测（UI 要 ≥9 个 ':' 字段）----
// parsePrediction: [4]=myRankIdx [6]=battleState [7]=turn [9]=oppRank1based
// drawOpponentPrediction: 仅当 battleState == 4 时画剑标
static void refresh_opponent() {
    Il2CppClass *cbm = nullptr;
    auto battle = get_battle(&cbm);
    if (!battle) {
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
    }
    auto pm = get_my_pm(battle, cbm);
    int lastEnemy = -1, money = 0, hp = 0, turn = 0, myRank = 0;
    if (pm) {
        char *b = (char *)pm;
        lastEnemy = *(int *)(b + JCC_PM_LAST_ENEMY);
        money = *(int *)(b + JCC_PM_MONEY);
        hp = *(int *)(b + JCC_PM_HP);
        myRank = *(int *)(b + JCC_PM_RANK);
        if (money < 0 || money > 9999) money = 0;
        if (hp < 0 || hp > 999) hp = 0;
        if (myRank < 0 || myRank > 7) myRank = 0;
    }
    turn = *(int *)((char *)battle + JCC_CBM_CUR_TURN);
    if (turn < 0 || turn > 99) turn = 0;

    int matchId = get_match_id(battle, cbm, myId);
    int oppId = pid_ok(matchId) ? matchId : (pid_ok(lastEnemy) ? lastEnemy : -1);
    int oppRank = -1;
    if (pid_ok(oppId)) {
        auto opm = get_player(battle, cbm, oppId);
        if (opm) {
            oppRank = *(int *)((char *)opm + JCC_PM_RANK);
            if (oppRank < 0 || oppRank > 7) oppRank = -1;
        }
    }
    int oppRank1 = (oppRank >= 0) ? (oppRank + 1) : 0;
    // battleState=4 才画剑；无合法对手 rank 则 EMPTY（勿发脏包）
    int state = (oppRank1 >= 1 && oppRank1 <= 8) ? 4 : 0;

    char info[256];
    snprintf(info, sizeof(info),
             "my=%d match=%d lastEnemy=%d money=%d hp=%d myRank=%d oppRank=%d state=%d turn=%d",
             myId, matchId, lastEnemy, money, hp, myRank, oppRank, state, turn);

    char pred[256];
    snprintf(pred, sizeof(pred), "0:0:0:0:%d:%d:%d:%d:0:%d", myRank, money, state, turn,
             oppRank1);

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_opp_info = info;
        g_pred = state ? pred : "EMPTY";
    }
    if (state) JLOGI("%s", info);
}

// ---- 头像位置：右侧几何 8 槽（无 FindObjectOfType / 无 Unity.Screen）----
// UI: "{W}x{H}:{count}[:opIdx]|x,y,isSelf|..."  y 底原点；x 必须 ≥ 2/3 W
static void refresh_positions() {
    int W = 1080, H = 2400;
    screen_size(&W, &H);

    Il2CppClass *cbm = nullptr;
    auto battle = get_battle(&cbm);
    if (!battle) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pos = "EMPTY";
        return;
    }
    int myId = get_my_id(battle, cbm);
    if (!pid_ok(myId)) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pos = "EMPTY";
        return;
    }
    auto my = get_my_pm(battle, cbm);
    int myRank = 0;
    if (my) {
        myRank = *(int *)((char *)my + JCC_PM_RANK);
        if (myRank < 0 || myRank > 7) myRank = 0;
    }
    int matchId = get_match_id(battle, cbm, myId);
    int oppRank = -1;
    if (pid_ok(matchId)) {
        auto opm = get_player(battle, cbm, matchId);
        if (opm) {
            oppRank = *(int *)((char *)opm + JCC_PM_RANK);
            if (oppRank < 0 || oppRank > 7) oppRank = -1;
        }
    }

    int n = 8;
    // 必须 ≥ 0.6667W，否则 parsePositions 直接丢弃
    float x = (float)W * 0.90f;
    char head[64];
    int opIdx = (oppRank >= 0 && oppRank < n) ? oppRank : -1;
    if (opIdx >= 0)
        snprintf(head, sizeof(head), "%dx%d:%d:%d", W, H, n, opIdx);
    else
        snprintf(head, sizeof(head), "%dx%d:%d", W, H, n);

    std::string out = head;
    for (int i = 0; i < n; i++) {
        float y = (float)H * (0.10f + (float)i * 0.10f);
        int isSelf = (i == myRank) ? 1 : 0;
        char e[64];
        snprintf(e, sizeof(e), "|%.0f,%.0f,%d", x, y, isSelf);
        out.append(e);
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pos = out;
    }
}

// ---- 对手站位 PUSH:OPPONENT_BOARD:W,H|x,y,cost,star,heroId,icon|... ----
// UnitData.col@0x30 row@0x34 — 无 LoadBody 也能读棋盘格
static void refresh_opponent_board() {
    if (!g_op_board.load()) return;
    Il2CppClass *cbm = nullptr;
    auto battle = get_battle(&cbm);
    if (!battle) return;
    int myId = get_my_id(battle, cbm);
    int matchId = get_match_id(battle, cbm, myId);
    if (matchId < 0) {
        auto pm = get_my_pm(battle, cbm);
        if (pm) matchId = *(int *)((char *)pm + JCC_PM_LAST_ENEMY);
    }
    if (matchId < 0) {
        push_all("PUSH:OPPONENT_BOARD_CLEAR\n");
        return;
    }
    auto opm = get_player(battle, cbm, matchId);
    if (!opm) return;

    const int BW = 700, BH = 400; // logical board canvas
    std::string body;
    char head[32];
    snprintf(head, sizeof(head), "%d,%d", BW, BH);
    body = head;

    int nunit = 0;
    auto *blist = *(Il2CppObject **)((char *)opm + JCC_PM_BATTLE_UNITS);
    void *items = nullptr;
    int size = 0;
    if (list_view(blist, &items, &size)) {
        if (size > 20) size = 20;
        for (int i = 0; i < size; i++) {
            auto *ud = list_obj_at(items, i);
            if (!ud) continue;
            char *b = (char *)ud;
            int hid = *(int *)(b + JCC_UD_HERO_ID);
            int col = *(int *)(b + JCC_UD_COL);
            int row = *(int *)(b + JCC_UD_ROW);
            int lv = *(int *)(b + JCC_UD_LEVEL);
            if (hid <= 0) continue;
            int cost = 1;
            std::string icon, name;
            for (auto &h : g_heroes)
                if (h.id == hid) {
                    cost = h.cost;
                    icon = h.icon;
                    name = h.name;
                    break;
                }
            // grid → logical board coords (center of cell)
            float x = ((float)col + 0.5f) / 7.0f * (float)BW;
            float y = ((float)row + 0.5f) / 4.0f * (float)BH;
            scrub(icon);
            char e[160];
            snprintf(e, sizeof(e), "|%.0f,%.0f,%d,%d,%d,%s", x, y, cost, lv > 0 ? lv : 1, hid,
                     icon.c_str());
            body.append(e);
            nunit++;
        }
    }

    if (nunit <= 0) {
        push_all("PUSH:OPPONENT_BOARD_CLEAR\n");
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_board_push = body;
    }
    std::string line = "PUSH:OPPONENT_BOARD:" + body + "\n";
    push_all(line);
    char msg[64];
    snprintf(msg, sizeof(msg), "op_board units=%d opp=%d", nunit, matchId);
    JLOGI("%s", msg);
}

// ---- 金铲铲自带阵容 TeamRecommend（纯内存，用户确认）----
// TeamRecommendController(Singleton)._teamRecommendModel
//   → CurrentStageRecommendData @0x50
//   → StageRecommendTeamData.HasHero(heroId) / CoreHeroId / HeroAndEquipments
static Il2CppObject *get_team_recommend_model() {
    Il2CppClass *trc = find_class("ZGameChess", "TeamRecommendController");
    if (!trc) trc = find_class("", "TeamRecommendController");
    if (!trc) return nullptr;
    auto ctrl = singleton(trc);
    if (!ctrl) return nullptr;
    auto m = meth(trc, "get_TeamRecommendModel", 0);
    if (m) {
        auto model = inv(m, ctrl, nullptr);
        if (model) return model;
    }
    return *(Il2CppObject **)((char *)ctrl + JCC_TRC_MODEL);
}

static Il2CppObject *get_stage_recommend_data() {
    auto model = get_team_recommend_model();
    if (!model) return nullptr;
    Il2CppClass *trm = find_class("ZGameChess", "TeamRecommendModel");
    if (!trm) trm = find_class("", "TeamRecommendModel");
    if (trm) {
        auto m = meth(trm, "get_CurrentStageRecommendData", 0);
        if (m) {
            auto st = inv(m, model, nullptr);
            if (st) return st;
        }
    }
    return *(Il2CppObject **)((char *)model + JCC_TRM_CUR_STAGE_DATA);
}

// TKDictionary / Dictionary.ContainsKey(int)
static bool dict_contains_int(Il2CppObject *dict, int key) {
    if (!dict || key <= 0 || !il2cpp_object_get_class) return false;
    auto *klass = il2cpp_object_get_class(dict);
    if (!klass) return false;
    auto m = meth(klass, "ContainsKey", 1);
    if (!m) return false;
    int32_t a = key;
    void *p[1] = {&a};
    auto r = inv(m, dict, p);
    return r ? unbox_bool(r) : false;
}

// 当前应用阵容是否包含该英雄 conf id（纯内存）
static bool lineup_has_hero(Il2CppObject *stage, int heroId) {
    if (!stage || heroId <= 0) return false;
    Il2CppClass *sr = find_class("ZGameChess", "StageRecommendTeamData");
    if (!sr) sr = find_class("", "StageRecommendTeamData");
    if (sr) {
        auto m = meth(sr, "HasHero", 1);
        if (m) {
            int32_t a = heroId;
            void *p[1] = {&a};
            auto r = inv(m, stage, p);
            if (r && unbox_bool(r)) return true;
        }
        auto m2 = meth(sr, "IsCoreHero", 1);
        if (m2) {
            int32_t a = heroId;
            void *p[1] = {&a};
            auto r = inv(m2, stage, p);
            if (r && unbox_bool(r)) return true;
        }
        // GetHeroDataDic → ContainsKey
        auto m3 = meth(sr, "GetHeroDataDic", 0);
        if (m3) {
            auto dic = inv(m3, stage, nullptr);
            if (dic && dict_contains_int(dic, heroId)) return true;
        }
    }
    // HeroAndEquipments @0x10 直接 ContainsKey（阵容英雄表）
    auto *he = *(Il2CppObject **)((char *)stage + JCC_SRTD_HERO_EQUIPS);
    if (he && dict_contains_int(he, heroId)) return true;

    int core = *(int *)((char *)stage + JCC_SRTD_CORE_HERO);
    if (core == heroId) return true;
    return false;
}

// ---- 商店槽：ChessBattleGlobal → BattleScreenMgr → GetBuyHeroView → _listHeroRoot ----
// 禁止 FindObjectOfType（跨线程崩）
static Il2CppObject *find_buy_hero_view() {
    Il2CppClass *cbg = find_class("ZGameChess", "ChessBattleGlobal");
    if (!cbg) cbg = find_class("", "ChessBattleGlobal");
    Il2CppObject *global = cbg ? singleton(cbg) : nullptr;
    if (global) {
        auto *smgr = *(Il2CppObject **)((char *)global + JCC_CBG_SCREEN_MGR);
        if (smgr) {
            Il2CppClass *bsm = find_class("ZGameChess", "BattleScreenMgr");
            if (!bsm) bsm = find_class("", "BattleScreenMgr");
            Il2CppObject *screen = nullptr;
            if (bsm) {
                auto m = meth(bsm, "GetBattleScreen", 0);
                if (m) screen = inv(m, smgr, nullptr);
            }
            if (!screen) screen = *(Il2CppObject **)((char *)smgr + JCC_BSM_SCREEN);
            if (screen) {
                Il2CppClass *bbs = find_class("ZGameChess", "BaseBattleScreen");
                if (!bbs) bbs = find_class("", "BaseBattleScreen");
                if (bbs) {
                    auto m = meth(bbs, "GetBuyHeroView", 0);
                    if (m) {
                        auto v = inv(m, screen, nullptr);
                        if (v) return v;
                    }
                }
            }
        }
    }
    // last resort: singleton only（无 FindObjectOfType）
    Il2CppClass *bv = find_class("ZGameChess", "BuyHeroView");
    if (!bv) bv = find_class("", "BuyHeroView");
    return bv ? singleton(bv) : nullptr;
}

static int hero_conf_of_root(Il2CppObject *hr) {
    if (!hr) return 0;
    char *b = (char *)hr;
    int dataId = *(int *)(b + JCC_HR_DATA_ID);
    if (dataId > 0) return dataId;
    // TAC_BuyHero.stHeroEntity.iHeroConfID
    auto *tb = *(Il2CppObject **)(b + JCC_HR_TAC_BUY);
    if (!tb) return 0;
    auto *ent = *(Il2CppObject **)((char *)tb + JCC_TB_HERO_ENTITY);
    if (!ent) return 0;
    int conf = *(int *)((char *)ent + JCC_HE_HERO_CONF);
    return conf > 0 ? conf : 0;
}

static int read_shop_slots(Il2CppObject **slots_out, int *ids_out, int maxn) {
    auto view = find_buy_hero_view();
    if (!view) return 0;
    char *vb = (char *)view;
    auto *list = *(Il2CppObject **)(vb + JCC_BH_LIST_HERO);
    if (!list) return 0;
    void *items = nullptr;
    int size = 0;
    if (!list_view(list, &items, &size)) return 0;
    if (size > maxn) size = maxn;
    if (size > 5) size = 5;
    int n = 0;
    for (int i = 0; i < size; i++) {
        auto *hr = list_obj_at(items, i);
        if (!hr) continue;
        int dataId = hero_conf_of_root(hr);
        slots_out[n] = hr;
        ids_out[n] = dataId;
        n++;
    }
    return n;
}

// 自动拿牌 = 纯内存：
// 1) 读当前应用阵容 StageRecommendTeamData
// 2) 读商店 HeroRoot 槽 conf id
// 3) 阵容含该英雄 → HeroRoot.ReqBuyHero()（游戏自带买牌协议入口，非 UI 点击）
static void try_auto_buy() {
    if (!g_auto_buy.load()) return;
    static int cool;
    if ((++cool % 2) != 0) return; // ~4s @ 2s tick

    auto stage = get_stage_recommend_data();
    if (!stage) {
        static int miss;
        if ((++miss % 15) == 1) slog("auto_buy: no CurrentStageRecommendData (未应用阵容?)");
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_lineup_dbg = "no_stage";
        }
        return;
    }

    int core = *(int *)((char *)stage + JCC_SRTD_CORE_HERO);
    char ldb[96];
    snprintf(ldb, sizeof(ldb), "stage_ok core=%d", core);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_lineup_dbg = ldb;
    }

    Il2CppClass *hrc = find_class("ZGameChess", "HeroRoot");
    if (!hrc) hrc = find_class("", "HeroRoot");
    if (!hrc) {
        slog("auto_buy: HeroRoot class missing");
        return;
    }
    auto mBuy = meth(hrc, "ReqBuyHero", 0);
    if (!mBuy) {
        slog("auto_buy: ReqBuyHero missing");
        return;
    }

    Il2CppObject *slots[5]{};
    int ids[5]{};
    int n = read_shop_slots(slots, ids, 5);
    if (n <= 0) {
        static int noshop;
        if ((++noshop % 10) == 1) slog("auto_buy: shop empty / no BuyHeroView");
        return;
    }

    char shopmsg[160];
    snprintf(shopmsg, sizeof(shopmsg), "auto_buy shop n=%d ids=%d,%d,%d,%d,%d core=%d", n, ids[0],
             n > 1 ? ids[1] : 0, n > 2 ? ids[2] : 0, n > 3 ? ids[3] : 0, n > 4 ? ids[4] : 0, core);
    slog(shopmsg);

    for (int i = 0; i < n; i++) {
        if (!slots[i] || ids[i] <= 0) continue;
        if (!lineup_has_hero(stage, ids[i])) continue;
        inv(mBuy, slots[i], nullptr);
        char msg[96];
        snprintf(msg, sizeof(msg), "auto_buy BUY slot=%d heroId=%d (lineup match)", i, ids[i]);
        slog(msg);
        return; // 每轮最多买一张，避免连点
    }
    static int nomatch;
    if ((++nomatch % 8) == 1) slog("auto_buy: shop has no lineup hero this tick");
}

static std::string handle(const char *req) {
    if (!req || !*req) return "RSP:ERR\n";
    JLOGI("REQ %s", req);

    if (strncmp(req, "SET:", 4) == 0) {
        const char *p = req + 4;
        bool on = strstr(p, ":1") != nullptr;
        if (strstr(p, "自动购买")) {
            g_auto_buy.store(on);
            slog(on ? "SET auto_buy=1 lineup-match" : "SET auto_buy=0");
            if (on) try_auto_buy();
            return "RSP:OK\n";
        }
        if (strstr(p, "弹窗拦截")) {
            g_popup_block.store(on);
            return "RSP:OK\n";
        }
        if (strstr(p, "商店显示")) {
            g_shop_show.store(on);
            if (on && g_pool.empty()) scan_heroes();
            return "RSP:OK\n";
        }
        if (strstr(p, "对手站位")) {
            g_op_board.store(on);
            if (!on) push_all("PUSH:OPPONENT_BOARD_CLEAR\n");
            return "RSP:OK\n";
        }
        if (strstr(p, "转区")) return "RSP:OK\n";
        return "RSP:OK\n";
    }

    if (strncmp(req, "DO:", 3) == 0) {
        const char *p = req + 3;
        if (strstr(p, "清空日志")) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_log.clear();
            return "RSP:OK\n";
        }
        if (strstr(p, "刷新") || strstr(p, "RESCAN")) {
            scan_heroes();
            refresh_opponent();
            refresh_hex();
            refresh_positions();
            refresh_warning();
            refresh_owned();
            refresh_opponent_board();
            return "RSP:OK\n";
        }
        if (strstr(p, "退出游戏")) return "RSP:OK\n";
        return "RSP:OK\n";
    }

    if (strncmp(req, "GET:", 4) == 0) {
        const char *p = req + 4;
        std::lock_guard<std::mutex> lk(g_mu);
        if (strstr(p, "牌库")) {
            if (g_pool.empty()) return "RSP:\n";
            return "RSP:" + g_pool + "\n";
        }
        if (strstr(p, "日志")) return "RSP:" + g_log + "\n";
        if (strstr(p, "海克斯品质")) return "RSP:" + g_hex + "\n";
        if (strstr(p, "三星预警")) return "RSP:" + g_warn + "\n";
        if (strstr(p, "头像位置")) return "RSP:" + g_pos + "\n";
        if (strstr(p, "对手预测")) return "RSP:" + g_pred + "\n";
        if (strstr(p, "转区")) return "RSP:0\n";
        return "RSP:EMPTY\n";
    }
    return "RSP:ERR\n";
}

static void *session(void *arg) {
    // 会话线程只回缓存字符串，不 attach/不 invoke，避免多线程 IL2CPP 闪退
    int c = (int)(intptr_t)arg;
    client_add(c);
    char buf[2048];
    while (g_run.load()) {
        ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = 0;
        char *line = buf;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (*line) {
                auto body = handle(line);
                if (send(c, body.data(), body.size(), MSG_NOSIGNAL) < 0) {
                    client_del(c);
                    close(c);
                    return nullptr;
                }
            }
            if (!nl) break;
            line = nl + 1;
        }
    }
    client_del(c);
    close(c);
    return nullptr;
}

static void *server(void *) {
    JCKPT("server_start");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        slog("FAIL socket");
        return nullptr;
    }
    int y = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &y, sizeof(y));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(PORT);
    if (bind(fd, (sockaddr *)&a, sizeof(a)) < 0) {
        slog("FAIL bind 31338");
        close(fd);
        return nullptr;
    }
    listen(fd, 16);
    slog("server 31338 ready hybrid " JCC_SEASON_TAG " no resource hooks");
    while (g_run.load()) {
        int c = accept(fd, nullptr, nullptr);
        if (c < 0) continue;
        pthread_t t;
        pthread_create(&t, nullptr, session, (void *)(intptr_t)c);
        pthread_detach(t);
    }
    close(fd);
    return nullptr;
}

static void *worker(void *) {
    ensure_il2cpp_thread();
    JCKPT("worker_start_2_6_3");
    for (int i = 0; i < 60; i++) {
        if (scan_heroes()) break;
        sleep(2);
    }
    int tick = 0;
    while (g_run.load()) {
        sleep(2);
        tick++;
        ensure_il2cpp_thread();
        if (g_shop_show.load() || g_pool.empty()) {
            if ((tick % 30) == 0) scan_heroes();
        }
        // 顺序：先 pos/pred，再 hex（hex 依赖 pos 就绪才放行 inBattle）
        refresh_positions();
        refresh_opponent();
        refresh_hex();
        if ((tick % 2) == 0) refresh_warning();
        if ((tick % 3) == 0) refresh_owned();
        if (g_op_board.load() && (tick % 3) == 0) refresh_opponent_board();
        if (g_auto_buy.load()) try_auto_buy();
        if ((tick % 10) == 0) {
            std::string lineup, hex, pred, pos;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                if (!g_heroes.empty()) {
                    g_pool = build_pool();
                    save_pool(g_pool);
                }
                lineup = g_lineup_dbg;
                hex = g_hex;
                pred = g_pred;
                pos = g_pos.size() > 40 ? g_pos.substr(0, 40) : g_pos;
            }
            char hb[320];
            snprintf(hb, sizeof(hb), "hb hex=%s pred=%s pos=%s ready=%d lineup=%s", hex.c_str(),
                     pred.c_str(), pos.c_str(), g_overlay_ready.load() ? 1 : 0, lineup.c_str());
            slog(hb);
        }
    }
    return nullptr;
}

void cardpool_start(const char *game_data_dir) {
    if (!game_data_dir) return;
    strncpy(g_dir, game_data_dir, sizeof(g_dir) - 1);
    JccFileLog::I().init(game_data_dir);
    g_run.store(true);
    JCKPT("cardpool_start_2_6_6");
    slog("hybrid_kernel_" JCC_SEASON_TAG " overlay-safe no-UnityScreen " JCC_SEASON_SCAN_DATE);

    if (il2cpp_domain_get && il2cpp_thread_attach) {
        auto d = il2cpp_domain_get();
        if (d) {
            il2cpp_thread_attach(d);
            slog("il2cpp_thread_attach ok");
        }
    }

    pthread_t t1, t2;
    pthread_create(&t1, nullptr, server, nullptr);
    pthread_detach(t1);
    pthread_create(&t2, nullptr, worker, nullptr);
    pthread_detach(t2);
}
