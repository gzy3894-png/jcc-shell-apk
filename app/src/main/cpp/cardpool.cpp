// JCC 2.6 混合内核（不 hook 资源/加载）
// - 协议对齐原版 Controller UI（31338）
// - 用现赛季扫描偏移 + 方法 invoke 读内存（不重写业务算法）
// - 禁止 Dobby/资源 hook，避免加载页「资源损坏」
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

// ---- season fields (scan-verified) ----
static constexpr size_t H_IID = JCC_HERO_IID;
static constexpr size_t H_SNAME = JCC_HERO_SNAME;
static constexpr size_t H_ICOST = JCC_HERO_ICOST;
static constexpr size_t H_PAINT = JCC_HERO_PAINT_SMALL;
static constexpr size_t H_ISTAR = JCC_HERO_ISTAR;
static constexpr size_t H_IQUAL = JCC_HERO_IQUALITY;

static constexpr size_t PM_BATTLE_TURN = 0x20;
static constexpr size_t PM_HEX = 0x28;
static constexpr size_t PM_MONEY = 0x5c;
static constexpr size_t PM_LAST_ENEMY = 0x64;
static constexpr size_t PM_HP = 0xbc;

static constexpr size_t CBM_PLAYER_DICT = 0x38;
static constexpr size_t CBM_CUR_PLAYER = 0x100;
static constexpr size_t CBM_MATCH_PLAYERS = 0x248;
static constexpr size_t CBM_MY_MATCH_LIST = 0x268;

static constexpr size_t UD_HERO_ID = 0x14;
static constexpr size_t UD_PLAYER_ID = 0x24;
static constexpr size_t UD_LEVEL = 0x148;

static constexpr int PORT = 31338;
static constexpr int MAX_ID = 12000;

static char g_dir[512]{};
static std::atomic<bool> g_run{false};
static std::mutex g_mu;

static std::string g_pool;
static std::string g_status = "init";
static std::string g_log;
static std::string g_hex = "0,0,0";
static std::string g_warn = "EMPTY";
static std::string g_pos = "EMPTY";
static std::string g_pred = "EMPTY";
static std::string g_opp_info = "EMPTY"; // 下一局对手可读摘要

static std::atomic<bool> g_auto_buy{false};
static std::atomic<bool> g_popup_block{false};
static std::atomic<bool> g_shop_show{true}; // 商店显示：只影响是否积极刷新，GET 始终有数据
static std::atomic<bool> g_op_board{true};

struct Hero {
    int id{}, cost{}, total{}, rem{};
    std::string name, icon;
};
static std::vector<Hero> g_heroes;
static std::map<int, int> g_owned;

static int tier_total(int c) {
    switch (c) {
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

// ---- 牌库表（2.5.2 已验证路径）----
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
        int iid = *(int *)(b + H_IID);
        int cost = *(int *)(b + H_ICOST);
        auto *ns = *(Il2CppString **)(b + H_SNAME);
        auto *ic = *(Il2CppString **)(b + H_PAINT);
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

// ---- 下一局对手：读内存（非猜行为）----
static void refresh_opponent() {
    // ChessBattleModel.GetMatchPlayerId(int) / get_MyPlayerId / GetMyPlayerModel
    Il2CppClass *cbm = find_class("ZGameChess", "ChessBattleModel");
    if (!cbm) cbm = find_class("", "ChessBattleModel");
    if (!cbm) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_opp_info = "EMPTY";
        g_pred = "EMPTY";
        return;
    }
    // need battle model instance via ChessModelManager.GetBattleModel
    Il2CppClass *cmm = find_class("ZGameChess", "ChessModelManager");
    if (!cmm) cmm = find_class("", "ChessModelManager");
    Il2CppObject *battle = nullptr;
    if (cmm) {
        auto inst = singleton(cmm);
        auto m = meth(cmm, "GetBattleModel", 0);
        if (inst && m) battle = inv(m, inst, nullptr);
    }
    if (!battle) {
        // try get_Instance on CBM if any
        battle = singleton(cbm);
    }
    if (!battle) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_opp_info = "EMPTY";
        g_pred = "EMPTY";
        return;
    }

    auto mMy = meth(cbm, "get_MyPlayerId", 0);
    auto mMatch = meth(cbm, "GetMatchPlayerId", 1);
    auto mGetPm = meth(cbm, "GetMyPlayerModel", 0);
    auto mGetPl = meth(cbm, "GetPlayer", 1);
    if (!mGetPl) {
        Il2CppClass *cpc = find_class("ZGameChess", "ChessPlayerController");
        if (cpc) mGetPl = meth(cpc, "GetPlayer", 1);
    }

    int myId = -1;
    if (mMy) {
        auto r = inv(mMy, battle, nullptr);
        if (r) myId = *(int *)((char *)r + 0x10); // boxed Int32
    }

    int matchId = -1;
    if (mMatch && myId >= 0) {
        int32_t arg = myId;
        void *params[1] = {&arg};
        auto r = inv(mMatch, battle, params);
        if (r) matchId = *(int *)((char *)r + 0x10);
    }

    // also LastEnemyId from my PlayerModel
    int lastEnemy = -1;
    int money = -1, hp = -1;
    if (mGetPm) {
        auto pm = inv(mGetPm, battle, nullptr);
        if (pm) {
            char *b = (char *)pm;
            lastEnemy = *(int *)(b + PM_LAST_ENEMY);
            money = *(int *)(b + PM_MONEY);
            hp = *(int *)(b + PM_HP);
        }
    }

    char buf[256];
    if (matchId >= 0 || lastEnemy >= 0) {
        snprintf(buf, sizeof(buf), "my=%d match=%d lastEnemy=%d money=%d hp=%d", myId, matchId,
                 lastEnemy, money, hp);
        std::lock_guard<std::mutex> lk(g_mu);
        g_opp_info = buf;
        // 对手预测协议：给 UI 可读非 EMPTY；格式兼容 parsePrediction 宽松路径
        // 使用 rank 风格简表：对手 id
        int oid = matchId >= 0 ? matchId : lastEnemy;
        char pred[128];
        snprintf(pred, sizeof(pred), "1|0,0,0,%d,OPP%d:", oid, oid);
        g_pred = pred;
        slog(buf);
    } else {
        std::lock_guard<std::mutex> lk(g_mu);
        g_opp_info = "EMPTY";
        // keep last pred or empty
    }
}

// ---- 海克斯：从 PlayerModel.hexAugmentModel 尽量读 ----
static void refresh_hex() {
    Il2CppClass *cbm = find_class("ZGameChess", "ChessBattleModel");
    if (!cbm) return;
    Il2CppClass *cmm = find_class("ZGameChess", "ChessModelManager");
    Il2CppObject *battle = nullptr;
    if (cmm) {
        auto inst = singleton(cmm);
        auto m = meth(cmm, "GetBattleModel", 0);
        if (inst && m) battle = inv(m, inst, nullptr);
    }
    if (!battle) return;
    auto mGetPm = meth(cbm, "GetMyPlayerModel", 0);
    if (!mGetPm) return;
    auto pm = inv(mGetPm, battle, nullptr);
    if (!pm) return;
    auto *hex = *(Il2CppObject **)((char *)pm + PM_HEX);
    if (!hex) return;
    // 无稳定字段文档时：尝试读对象开头几个 int32 作为品质试探（失败保持）
    // 更稳：只记录 hex 非空
    char msg[64];
    snprintf(msg, sizeof(msg), "hex_obj=%p", (void *)hex);
    JLOGI("%s", msg);
}

// ---- 自动拿牌：内存 ReqBuyHero（argc=0 on HeroRoot）----
static void try_auto_buy() {
    if (!g_auto_buy.load()) return;
    // 安全策略：仅当商店显示开启且能找到 HeroRoot 实例时调用；
    // 无槽位列表时不盲目连点，只每 30s 记一次状态
    static int n;
    if ((++n % 15) != 0) return;
    Il2CppClass *hr = find_class("ZGameChess", "HeroRoot");
    if (!hr) {
        slog("auto_buy: HeroRoot missing");
        return;
    }
    auto m = meth(hr, "ReqBuyHero", 0);
    if (!m) {
        slog("auto_buy: ReqBuyHero missing");
        return;
    }
    // 无可靠实例来源时不 invoke，避免乱买
    slog("auto_buy: armed (instance path pending stable shop slot read)");
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
            slog(on ? "SET popup=1" : "SET popup=0");
            return "RSP:OK\n";
        }
        if (strstr(p, "商店显示")) {
            g_shop_show.store(on);
            slog(on ? "SET shop_show=1" : "SET shop_show=0");
            // 开关关闭时仍保留池数据，UI 自己决定是否显示
            if (on && g_pool.empty()) scan_heroes();
            return "RSP:OK\n";
        }
        if (strstr(p, "对手站位")) {
            g_op_board.store(on);
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
            return "RSP:OK\n";
        }
        if (strstr(p, "退出游戏")) return "RSP:OK\n";
        return "RSP:OK\n";
    }

    if (strncmp(req, "GET:", 4) == 0) {
        const char *p = req + 4;
        std::lock_guard<std::mutex> lk(g_mu);
        // 商店显示关闭时仍返回牌库（原 UI 自己控制窗口；数据不能空）
        if (strstr(p, "牌库")) {
            if (g_pool.empty())
                return "RSP:\n";
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
                    close(c);
                    return nullptr;
                }
            }
            if (!nl) break;
            line = nl + 1;
        }
    }
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
    slog("server 31338 ready (hybrid 2.6 no resource hooks)");
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
        try_auto_buy();
        if ((tick % 10) == 0) {
            std::lock_guard<std::mutex> lk(g_mu);
            if (!g_heroes.empty()) {
                g_pool = build_pool();
                save_pool(g_pool);
            }
            JLOGI("hb pool=%zu shop=%d auto=%d opp=%s", g_pool.size(), (int)g_shop_show.load(),
                  (int)g_auto_buy.load(), g_opp_info.c_str());
        }
    }
    return nullptr;
}

void cardpool_start(const char *game_data_dir) {
    if (!game_data_dir) return;
    strncpy(g_dir, game_data_dir, sizeof(g_dir) - 1);
    JccFileLog::I().init(game_data_dir);
    g_run.store(true);
    JCKPT("cardpool_start_2_6");
    slog("hybrid_kernel_2.6_no_asset_hooks " JCC_SEASON_TAG);

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
