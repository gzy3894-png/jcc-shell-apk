// JCC 2.6.2 混合内核（不 hook 资源/加载）
// - 协议对齐原版 Controller UI（GET/SET/DO + PUSH:OPPONENT_BOARD）
// - 读点全部来自 dump/offsets 扫描 + 原 SO 0x7d7a4 商店链
// - 禁止 Dobby 资源/LoadBody hook（避免「资源损坏」）
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
static std::string g_hex = "0,0,0";
static std::string g_warn = "EMPTY";
static std::string g_pos = "EMPTY";
static std::string g_pred = "EMPTY";
static std::string g_opp_info = "EMPTY";
static std::string g_board_push; // last OPPONENT_BOARD payload (no prefix)

static std::atomic<bool> g_auto_buy{false};
static std::atomic<bool> g_popup_block{false};
static std::atomic<bool> g_shop_show{true};
static std::atomic<bool> g_op_board{true};

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

static Il2CppObject *inv(const MethodInfo *m, void *obj, void **p) {
    if (!m || !il2cpp_runtime_invoke) return nullptr;
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
    return *(int *)((char *)r + 0x10);
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

static int get_match_id(Il2CppObject *battle, Il2CppClass *cbm, int myId) {
    if (!battle || !cbm || myId < 0) return -1;
    auto m = meth(cbm, "GetMatchPlayerId", 1);
    if (!m) return -1;
    int32_t a = myId;
    void *p[1] = {&a};
    return unbox_i32(inv(m, battle, p));
}

static int get_rank(Il2CppObject *battle, Il2CppClass *cbm, int pid) {
    if (!battle || !cbm || pid < 0) return -1;
    auto m = meth(cbm, "GetPlayerRankByID", 1);
    if (!m) return -1;
    int32_t a = pid;
    void *p[1] = {&a};
    return unbox_i32(inv(m, battle, p));
}

static void screen_size(int *w, int *h) {
    *w = 1080;
    *h = 2400;
    Il2CppClass *sc = find_class("UnityEngine", "Screen");
    if (!sc) return;
    auto mw = meth(sc, "get_width", 0);
    auto mh = meth(sc, "get_height", 0);
    if (mw) {
        int v = unbox_i32(inv(mw, nullptr, nullptr));
        if (v > 0) *w = v;
    }
    if (mh) {
        int v = unbox_i32(inv(mh, nullptr, nullptr));
        if (v > 0) *h = v;
    }
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
    Il2CppClass *cbm = nullptr;
    auto battle = get_battle(&cbm);
    if (!battle) return;
    auto pm = get_my_pm(battle, cbm);
    if (!pm) return;
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
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_hex = buf;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "hex q=%s n=%d ids=%d,%d,%d", buf, n, n > 0 ? ids[0] : 0,
             n > 1 ? ids[1] : 0, n > 2 ? ids[2] : 0);
    JLOGI("%s", msg);
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

// ---- 三星预警 ----
static void refresh_warning() {
    Il2CppClass *cbm = nullptr;
    auto battle = get_battle(&cbm);
    if (!battle) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_warn = "EMPTY";
        return;
    }
    // iterate playerModelDict if available; else my + match only
    std::string out = "1";
    int added = 0;

    auto emit_pm = [&](Il2CppObject *pm) {
        if (!pm) return;
        char *b = (char *)pm;
        int pid = *(int *)(b + JCC_PM_PLAYER_ID);
        int money = *(int *)(b + JCC_PM_MONEY);
        int hp = *(int *)(b + JCC_PM_HP);
        std::map<int, int> pieces;
        std::map<int, std::string> names;
        collect_player_pieces(pm, pieces, &names);
        std::string heroes;
        for (auto &kv : pieces) {
            if (kv.second < 3) continue; // only near/ready 2-3 star
            std::string nm = names.count(kv.first) ? names[kv.first] : "?";
            // resolve name from table if missing
            if (nm == "?" || nm.empty()) {
                for (auto &h : g_heroes)
                    if (h.id == kv.first) {
                        nm = h.name;
                        break;
                    }
            }
            scrub(nm);
            char hb[128];
            snprintf(hb, sizeof(hb), "%d,%s,%d,0,0", kv.first, nm.c_str(), kv.second);
            if (!heroes.empty()) heroes.push_back(';');
            heroes.append(hb);
        }
        if (heroes.empty()) return;
        char head[64];
        snprintf(head, sizeof(head), "|%d,%d,%d:", pid, money, hp);
        out.append(head);
        out.append(heroes);
        added++;
    };

    auto my = get_my_pm(battle, cbm);
    emit_pm(my);
    int myId = get_my_id(battle, cbm);
    int matchId = get_match_id(battle, cbm, myId);
    if (matchId >= 0 && matchId != myId) emit_pm(get_player(battle, cbm, matchId));

    // also scan dict values via known match list slots is hard without dictionary iter;
    // my+match covers warning UI primary use

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_warn = added > 0 ? out : "EMPTY";
    }
}

// ---- owned counts (rem) ----
static void refresh_owned() {
    Il2CppClass *cbm = nullptr;
    auto battle = get_battle(&cbm);
    if (!battle) return;
    std::map<int, int> total;
    auto add_pm = [&](Il2CppObject *pm) {
        std::map<int, int> p;
        collect_player_pieces(pm, p, nullptr);
        for (auto &kv : p) total[kv.first] += kv.second;
    };
    add_pm(get_my_pm(battle, cbm));
    int myId = get_my_id(battle, cbm);
    // try rank 0..7 players via GetPlayer if id known from GetMatch chain only is incomplete;
    // use LastEnemy + match + my
    int matchId = get_match_id(battle, cbm, myId);
    if (matchId >= 0) add_pm(get_player(battle, cbm, matchId));
    auto mypm = get_my_pm(battle, cbm);
    if (mypm) {
        int le = *(int *)((char *)mypm + JCC_PM_LAST_ENEMY);
        if (le >= 0 && le != myId && le != matchId) add_pm(get_player(battle, cbm, le));
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_owned.swap(total);
        if (!g_heroes.empty()) g_pool = build_pool();
    }
}

// ---- 对手预测（UI 要 ≥9 个 ':' 字段）----
// parsePrediction: [4]=myRankIdx [6]=battleState [7]=turn [9]=oppRank1based
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
    int matchId = get_match_id(battle, cbm, myId);
    int lastEnemy = -1, money = -1, hp = -1, turn = 0;
    auto pm = get_my_pm(battle, cbm);
    if (pm) {
        char *b = (char *)pm;
        lastEnemy = *(int *)(b + JCC_PM_LAST_ENEMY);
        money = *(int *)(b + JCC_PM_MONEY);
        hp = *(int *)(b + JCC_PM_HP);
        // battleTurnModel pointer — not the turn int; leave turn 0 if unknown
        (void)b;
    }
    int oppId = matchId >= 0 ? matchId : lastEnemy;
    int myRank = get_rank(battle, cbm, myId);
    int oppRank = get_rank(battle, cbm, oppId);
    if (myRank < 0) myRank = 0;
    if (myRank > 7) myRank = 7;
    int oppRank1 = (oppRank >= 0) ? (oppRank + 1) : 0;
    int state = (oppId >= 0) ? 1 : 0;

    char info[256];
    snprintf(info, sizeof(info), "my=%d match=%d lastEnemy=%d money=%d hp=%d myRank=%d oppRank=%d",
             myId, matchId, lastEnemy, money, hp, myRank, oppRank);

    // 9+ colon fields before optional |
    char pred[256];
    snprintf(pred, sizeof(pred), "0:0:0:0:%d:%d:%d:%d:0:%d", myRank, money, state, turn,
             oppRank1);

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_opp_info = info;
        if (state)
            g_pred = pred;
        else
            g_pred = "EMPTY";
    }
    if (state) slog(info);
}

// ---- 头像位置：PlayerListPanel.PlayerItemList 几何/索引 ----
// UI: "{W}x{H}:{count}[:opIdx]|x,y,isSelf|..."  y 为游戏底原点，UI 再反转
static void refresh_positions() {
    int W = 1080, H = 2400;
    screen_size(&W, &H);

    Il2CppClass *cbm = nullptr;
    auto battle = get_battle(&cbm);
    int myId = battle ? get_my_id(battle, cbm) : -1;
    int matchId = battle ? get_match_id(battle, cbm, myId) : -1;
    int oppRank = (battle && matchId >= 0) ? get_rank(battle, cbm, matchId) : -1;

    struct Slot {
        float x, y;
        int isSelf;
        int rank;
    };
    std::vector<Slot> slots;

    // Prefer PlayerListPanel item list
    Il2CppClass *plp = find_class("ZGameChess", "PlayerListPanel");
    if (!plp) plp = find_class("", "PlayerListPanel");
    Il2CppObject *panel = nullptr;
    if (plp) {
        panel = singleton(plp);
        if (!panel) {
            Il2CppClass *obj = find_class("UnityEngine", "Object");
            if (obj && il2cpp_class_get_type && il2cpp_type_get_object) {
                auto m = meth(obj, "FindObjectOfType", 1);
                if (m) {
                    auto typeObj = il2cpp_type_get_object(il2cpp_class_get_type(plp));
                    if (typeObj) {
                        void *p[1] = {typeObj};
                        panel = inv(m, nullptr, p);
                    }
                }
            }
        }
    }

    if (panel) {
        auto *list = *(Il2CppObject **)((char *)panel + JCC_PLP_ITEM_LIST);
        void *items = nullptr;
        int size = 0;
        if (list_view(list, &items, &size)) {
            if (size > 8) size = 8;
            for (int i = 0; i < size; i++) {
                auto *it = list_obj_at(items, i);
                if (!it) continue;
                char *ib = (char *)it;
                int idx = *(int *)(ib + JCC_PLI_INDEX);
                auto *ipm = *(Il2CppObject **)(ib + JCC_PLI_PLAYER_MODEL);
                int pid = -1;
                if (ipm) pid = *(int *)((char *)ipm + JCC_PM_PLAYER_ID);
                int isSelf = (pid >= 0 && pid == myId) ? 1 : 0;
                // right-side vertical layout (UI requires x >= 2/3 W)
                float x = W * 0.88f;
                float y = H * (0.12f + (float)i * 0.095f); // bottom-origin-ish
                if (idx >= 0 && idx < 8) {
                    // keep rank order by list index
                }
                slots.push_back({x, y, isSelf, idx >= 0 ? idx : i});
            }
        }
    }

    // geometric fallback 8 slots on right if panel empty but in battle
    if (slots.empty() && matchId >= 0) {
        for (int i = 0; i < 8; i++) {
            float x = W * 0.88f;
            float y = H * (0.12f + (float)i * 0.095f);
            int isSelf = (i == 0) ? 1 : 0;
            slots.push_back({x, y, isSelf, i});
        }
    }

    if (slots.empty()) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pos = "EMPTY";
        return;
    }

    int opIdx = oppRank;
    if (opIdx < 0 || opIdx >= (int)slots.size()) opIdx = -1;

    char head[64];
    if (opIdx >= 0)
        snprintf(head, sizeof(head), "%dx%d:%d:%d", W, H, (int)slots.size(), opIdx);
    else
        snprintf(head, sizeof(head), "%dx%d:%d", W, H, (int)slots.size());

    std::string out = head;
    for (auto &s : slots) {
        char e[64];
        snprintf(e, sizeof(e), "|%.0f,%.0f,%d", s.x, s.y, s.isSelf);
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

// ---- 商店槽 0x7d7a4 ----
static Il2CppObject *find_buy_hero_view() {
    Il2CppClass *bv = find_class("ZGameChess", "BuyHeroView");
    if (!bv) bv = find_class("", "BuyHeroView");
    if (!bv) return nullptr;
    auto inst = singleton(bv);
    if (inst) return inst;
    Il2CppClass *obj = find_class("UnityEngine", "Object");
    if (!obj || !il2cpp_class_get_type || !il2cpp_type_get_object) return nullptr;
    auto m = meth(obj, "FindObjectOfType", 1);
    if (!m) return nullptr;
    auto typeObj = il2cpp_type_get_object(il2cpp_class_get_type(bv));
    if (!typeObj) return nullptr;
    void *params[1] = {typeObj};
    return inv(m, nullptr, params);
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
        int dataId = *(int *)((char *)hr + JCC_HR_DATA_ID);
        slots_out[n] = hr;
        ids_out[n] = dataId;
        n++;
    }
    return n;
}

static void try_auto_buy() {
    if (!g_auto_buy.load()) return;
    static int cool;
    if ((++cool % 5) != 0) return;
    Il2CppClass *hrc = find_class("ZGameChess", "HeroRoot");
    if (!hrc) return;
    auto mBuy = meth(hrc, "ReqBuyHero", 0);
    if (!mBuy) {
        slog("auto_buy: ReqBuyHero missing");
        return;
    }
    Il2CppObject *slots[5]{};
    int ids[5]{};
    int n = read_shop_slots(slots, ids, 5);
    if (n <= 0) return;
    for (int i = 0; i < n; i++) {
        if (!slots[i] || ids[i] <= 0) continue;
        inv(mBuy, slots[i], nullptr);
        char msg[80];
        snprintf(msg, sizeof(msg), "auto_buy invoke slot=%d dataId=%d", i, ids[i]);
        slog(msg);
        break;
    }
}

static std::string handle(const char *req) {
    if (!req || !*req) return "RSP:ERR\n";
    JLOGI("REQ %s", req);

    if (strncmp(req, "SET:", 4) == 0) {
        const char *p = req + 4;
        bool on = strstr(p, ":1") != nullptr;
        if (strstr(p, "自动购买")) {
            g_auto_buy.store(on);
            slog(on ? "SET auto_buy=1" : "SET auto_buy=0");
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
    JCKPT("worker_start");
    for (int i = 0; i < 60; i++) {
        if (scan_heroes()) break;
        sleep(2);
    }
    int tick = 0;
    while (g_run.load()) {
        sleep(2);
        tick++;
        if (g_shop_show.load() || g_pool.empty()) {
            if ((tick % 30) == 0) scan_heroes();
        }
        refresh_opponent();
        refresh_hex();
        refresh_positions();
        refresh_warning();
        refresh_owned();
        if ((tick % 2) == 0) refresh_opponent_board();
        if ((tick % 3) == 0) {
            Il2CppObject *slots[5]{};
            int ids[5]{};
            int n = read_shop_slots(slots, ids, 5);
            if (n > 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "shop_slots n=%d ids=%d,%d,%d,%d,%d", n, ids[0],
                         n > 1 ? ids[1] : 0, n > 2 ? ids[2] : 0, n > 3 ? ids[3] : 0,
                         n > 4 ? ids[4] : 0);
                slog(msg);
            }
        }
        try_auto_buy();
        if ((tick % 10) == 0) {
            std::lock_guard<std::mutex> lk(g_mu);
            if (!g_heroes.empty()) {
                g_pool = build_pool();
                save_pool(g_pool);
            }
            JLOGI("hb pool=%zu hex=%s pred=%s pos=%s warn=%s", g_pool.size(), g_hex.c_str(),
                  g_pred.c_str(), g_pos.c_str(), g_warn.c_str());
        }
    }
    return nullptr;
}

void cardpool_start(const char *game_data_dir) {
    if (!game_data_dir) return;
    strncpy(g_dir, game_data_dir, sizeof(g_dir) - 1);
    JccFileLog::I().init(game_data_dir);
    g_run.store(true);
    JCKPT("cardpool_start_2_6_2");
    slog("hybrid_kernel_" JCC_SEASON_TAG "_certified_static " JCC_SEASON_SCAN_DATE);

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
