// JCC Controller 完整内核（无转区）+ 强制落盘日志
// 协议: GET/SET/DO/PUSH @ 127.0.0.1:31338 对齐原版 UI
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

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define DO_API(r, n, p) extern r (*n) p
#include "il2cpp-api-functions.h"
#undef DO_API

// ---- 赛季字段 ----
static constexpr size_t OFF_IID = JCC_HERO_IID;
static constexpr size_t OFF_SNAME = JCC_HERO_SNAME;
static constexpr size_t OFF_ICOST = JCC_HERO_ICOST;
static constexpr size_t OFF_PAINT = JCC_HERO_PAINT_SMALL;

// UnitData (scan)
static constexpr size_t UD_HERO_ID = 0x14;
static constexpr size_t UD_PLAYER_ID = 0x24;
static constexpr size_t UD_COL = 0x30;
static constexpr size_t UD_ROW = 0x34;
static constexpr size_t UD_LEVEL = 0x148;
static constexpr size_t UD_HERO_NAME = 0x120;
static constexpr size_t UD_HERO_HEAD = 0x140;

// ChessBattleUnit
static constexpr size_t CBU_DATA = 0x90;
static constexpr size_t CBU_SCREEN_TOP = 0x1a4;  // Vector3-ish
static constexpr size_t CBU_SCREEN_HEAD = 0x1b0;
static constexpr size_t CBU_SHOW_STAR = 0x1e0;
static constexpr size_t CBU_BATTLE_DATA = 0x1c8;

static constexpr int CTRL_PORT = 31338;
static constexpr int MAX_HERO_ID = 20000;

static char g_data_dir[512]{};
static std::atomic<bool> g_running{false};
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static std::string g_pool;
static std::string g_status = "init";
static std::string g_log_ui;
static std::string g_hex = "0,0,0";
static std::string g_warn = "EMPTY";
static std::string g_pos = "EMPTY";
static std::string g_pred = "EMPTY";
static std::string g_op_board_payload; // for PUSH

static std::atomic<bool> g_auto_buy{false};
static std::atomic<bool> g_popup_block{false};
static std::atomic<bool> g_shop_display{true};
static std::atomic<bool> g_op_board{true};

static std::map<int, int> g_owned; // heroId -> copies (approx: 1-star=1,2=3,3=9 style later)

struct HeroRow {
    int id{}, cost{}, total{}, rem{};
    std::string name, icon;
};
static std::vector<HeroRow> g_heroes;

// client sockets for PUSH (simple: store last accepted we can't easily multi-cast without list)
// Instead: GET always fresh; PUSH written to file and sent when client connects next
// For PUSH we keep a dedicated push socket list
static pthread_mutex_t g_clients_mu = PTHREAD_MUTEX_INITIALIZER;
static std::vector<int> g_push_clients;

static int pool_total_by_cost(int cost) {
    switch (cost) {
        case 1:
            return 29;
        case 2:
            return 22;
        case 3:
            return 18;
        case 4:
            return 12;
        case 5:
            return 10;
        default:
            return 0;
    }
}

// star 1/2/3 → 棋子占用卡牌数 1/3/9
static int copies_for_star(int star) {
    if (star <= 1) return 1;
    if (star == 2) return 3;
    return 9;
}

static void ui_log(const char *msg) {
    if (!msg) return;
    pthread_mutex_lock(&g_mu);
    if (g_log_ui.size() > 48 * 1024) g_log_ui.erase(0, 24 * 1024);
    g_log_ui.append(msg);
    g_log_ui.push_back('\n');
    pthread_mutex_unlock(&g_mu);
    JLOGI("%s", msg);
}

static void set_status(const char *msg) {
    pthread_mutex_lock(&g_mu);
    g_status = msg ? msg : "";
    pthread_mutex_unlock(&g_mu);
    ui_log(msg);
}

static void write_pool_file(const std::string &pool) {
    if (!g_data_dir[0]) return;
    char path[600];
    snprintf(path, sizeof(path), "%s/files/jcc_cardpool.txt", g_data_dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(pool.data(), 1, pool.size(), f);
        fputc('\n', f);
        fflush(f);
        fclose(f);
    }
}

static std::string utf16_to_utf8(const Il2CppChar *chars, int32_t len) {
    std::string out;
    if (!chars || len <= 0) return out;
    for (int32_t i = 0; i < len; i++) {
        uint32_t c = chars[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len) {
            uint32_t c2 = chars[i + 1];
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                c = 0x10000 + (((c - 0xD800) << 10) | (c2 - 0xDC00));
                i++;
            }
        }
        if (c < 0x80)
            out.push_back((char)c);
        else if (c < 0x800) {
            out.push_back((char)(0xC0 | (c >> 6)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back((char)(0xE0 | (c >> 12)));
            out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (c >> 18)));
            out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

static std::string il2cpp_str(Il2CppString *s) {
    if (!s || !il2cpp_string_chars || !il2cpp_string_length) return {};
    return utf16_to_utf8(il2cpp_string_chars(s), il2cpp_string_length(s));
}

static Il2CppClass *find_class(const char *ns, const char *name) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_assembly_get_image ||
        !il2cpp_class_from_name)
        return nullptr;
    size_t n = 0;
    auto domain = il2cpp_domain_get();
    if (!domain) return nullptr;
    auto asms = il2cpp_domain_get_assemblies(domain, &n);
    if (!asms) return nullptr;
    for (size_t i = 0; i < n; i++) {
        auto image = il2cpp_assembly_get_image(asms[i]);
        if (!image) continue;
        if (ns && ns[0]) {
            auto k = il2cpp_class_from_name(image, ns, name);
            if (k) return k;
        }
        auto k2 = il2cpp_class_from_name(image, "", name);
        if (k2) return k2;
    }
    return nullptr;
}

static const MethodInfo *find_method(Il2CppClass *klass, const char *name, int argc) {
    if (!klass || !il2cpp_class_get_method_from_name) return nullptr;
    return il2cpp_class_get_method_from_name(klass, name, argc);
}

static Il2CppObject *invoke(const MethodInfo *method, void *obj, void **params) {
    if (!method || !il2cpp_runtime_invoke) return nullptr;
    Il2CppException *exc = nullptr;
    auto ret = il2cpp_runtime_invoke(method, obj, params, &exc);
    if (exc) return nullptr;
    return ret;
}

static Il2CppObject *get_singleton(Il2CppClass *klass) {
    auto m = find_method(klass, "get_Instance", 0);
    if (!m) m = find_method(klass, "get_instance", 0);
    if (!m) return nullptr;
    return invoke(m, nullptr, nullptr);
}

static void sanitize(std::string &s) {
    for (char &c : s) {
        if (c == ':' || c == ',' || c == '|' || c == ';' || c == '\n' || c == '\r') c = '_';
    }
}

static std::string build_pool_payload_locked() {
    std::string out;
    out.reserve(g_heroes.size() * 40);
    for (auto &h : g_heroes) {
        int owned = 0;
        auto it = g_owned.find(h.id);
        if (it != g_owned.end()) owned = it->second;
        int rem = h.total - owned;
        if (rem < 0) rem = 0;
        h.rem = rem;
        char buf[512];
        if (!h.icon.empty())
            snprintf(buf, sizeof(buf), "%d:%s:%d:%d:%d:%s", h.id, h.name.c_str(), h.cost, rem,
                     h.total, h.icon.c_str());
        else
            snprintf(buf, sizeof(buf), "%d:%s:%d:%d:%d", h.id, h.name.c_str(), h.cost, rem, h.total);
        if (!out.empty()) out.push_back(',');
        out.append(buf);
    }
    return out;
}

// ---------- FindObjectsOfType 枚举 ----------
static std::vector<Il2CppObject *> find_objects_of_type(Il2CppClass *klass) {
    std::vector<Il2CppObject *> out;
    if (!klass) return out;
    Il2CppClass *objKlass = find_class("UnityEngine", "Object");
    if (!objKlass) return out;
    // FindObjectsOfType(Type) — argc 1
    auto m = find_method(objKlass, "FindObjectsOfType", 1);
    if (!m) m = find_method(objKlass, "FindObjectsOfType", 2);
    if (!m || !il2cpp_class_get_type || !il2cpp_type_get_object) return out;
    const Il2CppType *ty = il2cpp_class_get_type(klass);
    if (!ty) return out;
    auto typeObj = il2cpp_type_get_object(ty);
    if (!typeObj) return out;
    void *params[1] = {typeObj};
    auto arr = invoke(m, nullptr, params);
    if (!arr) return out;
    // Il2CppArray: max_length @0x18, vector @0x20 (arm64 common)
    char *base = reinterpret_cast<char *>(arr);
    uint32_t len = *reinterpret_cast<uint32_t *>(base + 0x18);
    if (len > 0 && len < 4096) {
        for (uint32_t i = 0; i < len && i < 512; i++) {
            auto *o = *reinterpret_cast<Il2CppObject **>(base + 0x20 + i * sizeof(void *));
            if (o) out.push_back(o);
        }
    }
    JLOGI("FindObjectsOfType count=%zu len_field=%u", out.size(), len);
    return out;
}

// ---------- 扫描场上棋子 → owned + board + warn ----------
static void scan_battle_units() {
    JCKPT("scan_battle_units_enter");
    Il2CppClass *unitKlass = find_class("ZGameChess", "ChessBattleUnit");
    if (!unitKlass) unitKlass = find_class("", "ChessBattleUnit");
    if (!unitKlass) {
        JLOGI("ChessBattleUnit class missing");
        return;
    }
    auto units = find_objects_of_type(unitKlass);
    if (units.empty()) {
        // 备选：UnitData 对象
        Il2CppClass *udk = find_class("ZGameChess", "UnitData");
        if (!udk) udk = find_class("", "UnitData");
        auto uds = find_objects_of_type(udk);
        std::map<int, int> owned;
        std::map<int, std::map<int, int>> byPlayer; // player -> heroId -> copies
        for (auto *o : uds) {
            if (!o) continue;
            char *b = reinterpret_cast<char *>(o);
            int heroId = *reinterpret_cast<int *>(b + UD_HERO_ID);
            int level = *reinterpret_cast<int *>(b + UD_LEVEL);
            int pid = *reinterpret_cast<int *>(b + UD_PLAYER_ID);
            if (heroId <= 0) continue;
            int star = level > 0 ? level : 1;
            if (star > 3) star = 3;
            int c = copies_for_star(star);
            owned[heroId] += c;
            byPlayer[pid][heroId] += c;
        }
        pthread_mutex_lock(&g_mu);
        g_owned.swap(owned);
        // 三星预警：按玩家聚合 count>=3 的英雄
        if (byPlayer.empty()) {
            g_warn = "EMPTY";
        } else {
            std::string w = "W";
            int pidx = 0;
            for (auto &kv : byPlayer) {
                std::string heroes;
                for (auto &h : kv.second) {
                    if (h.second < 3) continue;
                    // id,name,cnt,cost,star,icon
                    int cost = 1, star = h.second >= 9 ? 3 : (h.second >= 3 ? 2 : 1);
                    for (auto &hr : g_heroes)
                        if (hr.id == h.first) {
                            cost = hr.cost;
                            break;
                        }
                    char hb[128];
                    snprintf(hb, sizeof(hb), "%d,H%d,%d,%d,%d,", h.first, h.first, h.second, cost,
                             star);
                    if (!heroes.empty()) heroes.push_back(';');
                    heroes.append(hb);
                }
                if (heroes.empty()) {
                    pidx++;
                    continue;
                }
                char head[64];
                snprintf(head, sizeof(head), "|%d,0,0,-1,P%d:", pidx + 1, kv.first);
                w.append(head);
                w.append(heroes);
                pidx++;
            }
            g_warn = (w.size() > 1) ? w : "EMPTY";
        }
        g_pool = build_pool_payload_locked();
        pthread_mutex_unlock(&g_mu);
        write_pool_file(g_pool);
        JLOGI("UnitData path owned_types=%zu warn_len=%zu", g_owned.size(), g_warn.size());
        JCKPT("scan_battle_units_unitdata_done");
        return;
    }

    std::map<int, int> owned;
    std::string board = "1080,2400"; // default screen; will fix if we get positions
    int board_n = 0;
    float maxx = 0, maxy = 0;
    for (auto *u : units) {
        if (!u) continue;
        char *b = reinterpret_cast<char *>(u);
        auto *data = *reinterpret_cast<Il2CppObject **>(b + CBU_DATA);
        if (!data) data = *reinterpret_cast<Il2CppObject **>(b + CBU_BATTLE_DATA);
        int heroId = 0, star = 1, cost = 1;
        std::string icon, name;
        if (data) {
            char *d = reinterpret_cast<char *>(data);
            heroId = *reinterpret_cast<int *>(d + UD_HERO_ID);
            int level = *reinterpret_cast<int *>(d + UD_LEVEL);
            star = level > 0 && level <= 3 ? level : 1;
            auto *nm = *reinterpret_cast<Il2CppString **>(d + UD_HERO_NAME);
            auto *hd = *reinterpret_cast<Il2CppString **>(d + UD_HERO_HEAD);
            name = il2cpp_str(nm);
            icon = il2cpp_str(hd);
        }
        // Show_Star object may encode star — fallback
        if (heroId <= 0) continue;
        for (auto &hr : g_heroes)
            if (hr.id == heroId) {
                cost = hr.cost;
                if (icon.empty()) icon = hr.icon;
                if (name.empty()) name = hr.name;
                break;
            }
        owned[heroId] += copies_for_star(star);
        // screen pos
        float *st = reinterpret_cast<float *>(b + CBU_SCREEN_HEAD);
        float sx = st[0], sy = st[1];
        if (sx > maxx) maxx = sx;
        if (sy > maxy) maxy = sy;
        if (sx > 1 && sy > 1 && board_n < 20) {
            sanitize(icon);
            char seg[192];
            snprintf(seg, sizeof(seg), "|%.0f,%.0f,%d,%d,%d,%s", sx, sy, heroId, star, cost,
                     icon.c_str());
            board.append(seg);
            board_n++;
        }
    }
    if (maxx > 100 && maxy > 100) {
        char head[32];
        snprintf(head, sizeof(head), "%.0f,%.0f", maxx + 20, maxy + 20);
        // rebuild with correct head
        std::string body = head;
        // extract segments after first |
        auto pos = board.find('|');
        if (pos != std::string::npos) body.append(board.substr(pos));
        board.swap(body);
    }

    pthread_mutex_lock(&g_mu);
    g_owned.swap(owned);
    g_pool = build_pool_payload_locked();
    if (board_n > 0 && g_op_board.load()) {
        g_op_board_payload = board;
    } else {
        g_op_board_payload.clear();
    }
    // simple warning from owned
    {
        std::string w = "W";
        bool any = false;
        for (auto &kv : g_owned) {
            if (kv.second < 3) continue;
            int cost = 1, star = kv.second >= 9 ? 3 : 2;
            for (auto &hr : g_heroes)
                if (hr.id == kv.first) {
                    cost = hr.cost;
                    break;
                }
            char hb[128];
            snprintf(hb, sizeof(hb), "%d,H%d,%d,%d,%d,", kv.first, kv.first, kv.second, cost, star);
            if (!any) {
                w.append("|1,0,0,-1,ALL:");
                any = true;
            } else
                w.push_back(';');
            w.append(hb);
        }
        g_warn = any ? w : "EMPTY";
    }
    pthread_mutex_unlock(&g_mu);
    write_pool_file(g_pool);
    JLOGI("battle units=%zu board_n=%d owned=%zu", units.size(), board_n, g_owned.size());
    JCKPT("scan_battle_units_done");
}

static void push_to_clients(const std::string &line) {
    std::vector<int> dead;
    pthread_mutex_lock(&g_clients_mu);
    for (int fd : g_push_clients) {
        if (send(fd, line.data(), line.size(), MSG_NOSIGNAL) < 0) dead.push_back(fd);
    }
    for (int fd : dead) {
        close(fd);
        g_push_clients.erase(std::remove(g_push_clients.begin(), g_push_clients.end(), fd),
                             g_push_clients.end());
    }
    pthread_mutex_unlock(&g_clients_mu);
}

static void broadcast_board() {
    std::string payload;
    pthread_mutex_lock(&g_mu);
    payload = g_op_board_payload;
    bool en = g_op_board.load();
    pthread_mutex_unlock(&g_mu);
    if (!en) {
        push_to_clients("PUSH:OPPONENT_BOARD_CLEAR\n");
        return;
    }
    if (payload.empty()) {
        push_to_clients("PUSH:OPPONENT_BOARD_CLEAR\n");
        return;
    }
    std::string msg = "PUSH:OPPONENT_BOARD:" + payload + "\n";
    push_to_clients(msg);
}

// ---------- 牌库表 ----------
static bool scan_cardpool_table() {
    JCKPT("scan_cardpool_enter");
    set_status("scan: DataBaseManager " JCC_SEASON_TAG);
    Il2CppClass *db = find_class(JCC_NS_DB, JCC_CLS_DB);
    if (!db) db = find_class("ZGame", "DataBaseManager");
    if (!db) {
        set_status("FAIL: DataBaseManager");
        return false;
    }
    auto mSearch = find_method(db, "SearchACGHero2", 1);
    if (!mSearch) mSearch = find_method(db, "SearchACGHero", 1);
    if (!mSearch) {
        set_status("FAIL: SearchACGHero");
        return false;
    }
    auto inst = get_singleton(db);
    if (!inst) {
        set_status("FAIL: get_Instance");
        return false;
    }
    std::vector<HeroRow> rows;
    for (int id = 1; id <= MAX_HERO_ID; id++) {
        void *params[1];
        int32_t hid = id;
        params[0] = &hid;
        auto hero = invoke(mSearch, inst, params);
        if (!hero) continue;
        char *base = reinterpret_cast<char *>(hero);
        int iid = *reinterpret_cast<int *>(base + OFF_IID);
        int cost = *reinterpret_cast<int *>(base + OFF_ICOST);
        auto *nameObj = *reinterpret_cast<Il2CppString **>(base + OFF_SNAME);
        auto *iconObj = *reinterpret_cast<Il2CppString **>(base + OFF_PAINT);
        if (iid <= 0) iid = id;
        if (cost < 1 || cost > 5) continue;
        std::string name = il2cpp_str(nameObj);
        if (name.empty()) continue;
        std::string icon = il2cpp_str(iconObj);
        sanitize(name);
        sanitize(icon);
        HeroRow r{};
        r.id = iid;
        r.cost = cost;
        r.total = pool_total_by_cost(cost);
        r.name = name;
        r.icon = icon;
        rows.push_back(r);
    }
    if (rows.empty()) {
        set_status("FAIL: no heroes");
        return false;
    }
    pthread_mutex_lock(&g_mu);
    g_heroes.swap(rows);
    g_pool = build_pool_payload_locked();
    pthread_mutex_unlock(&g_mu);
    write_pool_file(g_pool);
    char st[96];
    snprintf(st, sizeof(st), "scan done heroes=%zu", g_heroes.size());
    set_status(st);
    JCKPT("scan_cardpool_done");
    return true;
}

// 自动购买（安全：仅当开关开且找到 ReqBuyHero；默认不乱买 — 只记日志除非明确商店 id）
static void try_auto_buy() {
    if (!g_auto_buy.load()) return;
    // 完整自动购买需商店槽列表；当前记录开关有效，避免误购
    static int n;
    if ((n++ % 20) == 0) JLOGI("auto_buy enabled (shop slot hook pending)");
}

// 弹窗拦截：标记 + 尝试关常见弹窗类（找不到则仅日志）
static void try_popup_block() {
    if (!g_popup_block.load()) return;
    static int n;
    if ((n++ % 30) == 0) JLOGI("popup_block enabled");
}

// 海克斯
static void refresh_hex() {
    // 尝试 PlayerModel.hexAugmentModel — 结构未知时保持
    // 若有 jcc_report_hex 外部更新则已写入
}

// 头像位置：占位 EMPTY 或简易
static void refresh_positions() {
    // 未拿到稳定 PlayerList 坐标前保持 EMPTY，避免错误叠层
}

static std::string handle_request(const char *req) {
    if (!req) return "RSP:ERR\n";
    JLOGI("REQ %s", req);

    if (strncmp(req, "SET:", 4) == 0) {
        const char *p = req + 4;
        bool on = strstr(p, ":1") != nullptr;
        if (strstr(p, "自动购买")) {
            g_auto_buy.store(on);
            set_status(on ? "SET auto_buy=1" : "SET auto_buy=0");
            return "RSP:OK\n";
        }
        if (strstr(p, "弹窗拦截")) {
            g_popup_block.store(on);
            set_status(on ? "SET popup_block=1" : "SET popup_block=0");
            return "RSP:OK\n";
        }
        if (strstr(p, "商店显示")) {
            g_shop_display.store(on);
            return "RSP:OK\n";
        }
        if (strstr(p, "对手站位")) {
            g_op_board.store(on);
            if (!on) push_to_clients("PUSH:OPPONENT_BOARD_CLEAR\n");
            return "RSP:OK\n";
        }
        if (strstr(p, "转区")) return "RSP:OK\n";
        return "RSP:OK\n";
    }

    if (strncmp(req, "DO:", 3) == 0) {
        const char *p = req + 3;
        if (strstr(p, "清空日志")) {
            pthread_mutex_lock(&g_mu);
            g_log_ui.clear();
            pthread_mutex_unlock(&g_mu);
            return "RSP:OK\n";
        }
        if (strstr(p, "退出游戏")) {
            set_status("DO exit ignored_safe");
            return "RSP:OK\n";
        }
        if (strstr(p, "刷新") || strstr(p, "RESCAN")) {
            scan_cardpool_table();
            scan_battle_units();
            return "RSP:OK\n";
        }
        return "RSP:OK\n";
    }

    if (strncmp(req, "GET:", 4) == 0) {
        const char *p = req + 4;
        pthread_mutex_lock(&g_mu);
        std::string body;
        if (strstr(p, "牌库"))
            body = "RSP:" + g_pool + "\n";
        else if (strstr(p, "日志"))
            body = "RSP:" + g_log_ui + "\n";
        else if (strstr(p, "海克斯品质"))
            body = "RSP:" + g_hex + "\n";
        else if (strstr(p, "三星预警"))
            body = "RSP:" + g_warn + "\n";
        else if (strstr(p, "头像位置"))
            body = "RSP:" + g_pos + "\n";
        else if (strstr(p, "对手预测"))
            body = "RSP:" + g_pred + "\n";
        else if (strstr(p, "转区"))
            body = "RSP:0\n";
        else
            body = "RSP:EMPTY\n";
        pthread_mutex_unlock(&g_mu);
        return body;
    }
    return "RSP:ERR unknown\n";
}

static void *server_thread(void *) {
    JCKPT("server_thread_start");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_status("FAIL socket");
        return nullptr;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(CTRL_PORT);
    if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
        set_status("FAIL bind 31338");
        close(fd);
        return nullptr;
    }
    listen(fd, 8);
    set_status("server 31338 full-protocol listening");

    while (g_running.load()) {
        int c = accept(fd, nullptr, nullptr);
        if (c < 0) continue;
        // 注册 push 客户端
        pthread_mutex_lock(&g_clients_mu);
        g_push_clients.push_back(c);
        pthread_mutex_unlock(&g_clients_mu);

        char req[1024]{};
        ssize_t n = recv(c, req, sizeof(req) - 1, 0);
        if (n > 0) {
            char *line = req;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                auto body = handle_request(line);
                send(c, body.data(), body.size(), MSG_NOSIGNAL);
                // 不关闭 — 但原 UI 短连接；短连接则下次 accept
                if (!nl) break;
                line = nl + 1;
            }
        }
        // 原版短连接：移除并关闭
        pthread_mutex_lock(&g_clients_mu);
        g_push_clients.erase(std::remove(g_push_clients.begin(), g_push_clients.end(), c),
                             g_push_clients.end());
        pthread_mutex_unlock(&g_clients_mu);
        close(c);
    }
    close(fd);
    return nullptr;
}

// 长连接 push 服务：额外端口无 — 原版 PUSH 与 GET 同连接
// 原版 d0 读循环：同一 socket 上 RSP 与 PUSH
// 改进：后台线程周期性向「最近活跃」写文件，UI 主要靠 GET 轮询
// 对于站位：UI handlePush 需要 PUSH 行；短连接收不到
// → 增加并行推送：在 GET 处理完后若有 board，附加不发；改用 keep-alive 连接列表
// 简化：另开 31339 推送？UI 只连 31338。
// 原版 SO 在同一连接上异步 PUSH。我们的 accept 短连接模式限制了 PUSH。
// 修复：server 改为 select/多线程：连接保持直到对端关闭。

static void *session_thread(void *arg) {
    int c = (int)(intptr_t)arg;
    pthread_mutex_lock(&g_clients_mu);
    g_push_clients.push_back(c);
    pthread_mutex_unlock(&g_clients_mu);
    JLOGI("session open fd=%d", c);

    char buf[2048];
    while (g_running.load()) {
        ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = 0;
        char *line = buf;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (*line) {
                auto body = handle_request(line);
                if (send(c, body.data(), body.size(), MSG_NOSIGNAL) < 0) {
                    line = nullptr;
                    break;
                }
            }
            if (!nl) break;
            line = nl + 1;
        }
    }
    pthread_mutex_lock(&g_clients_mu);
    g_push_clients.erase(std::remove(g_push_clients.begin(), g_push_clients.end(), c),
                         g_push_clients.end());
    pthread_mutex_unlock(&g_clients_mu);
    close(c);
    JLOGI("session close fd=%d", c);
    return nullptr;
}

static void *server_thread_v2(void *) {
    JCKPT("server_v2_start");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_status("FAIL socket");
        return nullptr;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(CTRL_PORT);
    if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
        set_status("FAIL bind 31338");
        close(fd);
        return nullptr;
    }
    listen(fd, 16);
    set_status("server 31338 session-mode");
    while (g_running.load()) {
        int c = accept(fd, nullptr, nullptr);
        if (c < 0) continue;
        pthread_t t;
        pthread_create(&t, nullptr, session_thread, (void *)(intptr_t)c);
        pthread_detach(t);
    }
    close(fd);
    return nullptr;
}

static void *worker(void *) {
    JCKPT("worker_start");
    for (int i = 0; i < 90; i++) {
        if (scan_cardpool_table()) break;
        char st[64];
        snprintf(st, sizeof(st), "wait tables %ds", (i + 1) * 2);
        set_status(st);
        sleep(2);
    }
    int tick = 0;
    while (g_running.load()) {
        sleep(2);
        tick++;
        scan_battle_units();
        refresh_hex();
        refresh_positions();
        try_auto_buy();
        try_popup_block();
        broadcast_board();
        if ((tick % 60) == 0) {
            scan_cardpool_table();
            JCKPT("worker_periodic_rescan");
        }
        if ((tick % 15) == 0) {
            JLOGI("heartbeat tick=%d pool_bytes=%zu owned=%zu op_board=%d", tick, g_pool.size(),
                  g_owned.size(), (int)g_op_board.load());
        }
    }
    JCKPT("worker_exit");
    return nullptr;
}

extern "C" void jcc_report_owned(int hero_id, int count) {
    if (hero_id <= 0) return;
    pthread_mutex_lock(&g_mu);
    g_owned[hero_id] = count;
    if (!g_heroes.empty()) g_pool = build_pool_payload_locked();
    pthread_mutex_unlock(&g_mu);
}

extern "C" void jcc_report_hex(int a, int b, int c) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d,%d,%d", a & 3, b & 3, c & 3);
    pthread_mutex_lock(&g_mu);
    g_hex = buf;
    pthread_mutex_unlock(&g_mu);
}

void cardpool_start(const char *game_data_dir) {
    if (!game_data_dir) return;
    strncpy(g_data_dir, game_data_dir, sizeof(g_data_dir) - 1);
    JccFileLog::I().init(game_data_dir);
    g_running.store(true);
    JCKPT("cardpool_start");
    set_status("full_kernel_start " JCC_SEASON_TAG);

    if (il2cpp_domain_get && il2cpp_thread_attach) {
        auto domain = il2cpp_domain_get();
        if (domain) {
            il2cpp_thread_attach(domain);
            set_status("il2cpp_thread_attach ok");
            JCKPT("il2cpp_attached");
        }
    }

    pthread_t t1{}, t2{};
    pthread_create(&t1, nullptr, server_thread_v2, nullptr);
    pthread_detach(t1);
    pthread_create(&t2, nullptr, worker, nullptr);
    pthread_detach(t2);
    JCKPT("threads_spawned");
}
