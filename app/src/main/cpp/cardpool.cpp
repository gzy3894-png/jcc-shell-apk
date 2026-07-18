// JCC Controller 2.5.x 内核 — 完整协议（不含转区）
// 对接原版 UI：GET/SET/DO/PUSH @ 127.0.0.1:31338
#include "cardpool.h"
#include "log.h"
#include "il2cpp-class.h"
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
#include <string>
#include <vector>

#define DO_API(r, n, p) extern r (*n) p
#include "il2cpp-api-functions.h"
#undef DO_API

static constexpr size_t OFF_IID = JCC_HERO_IID;
static constexpr size_t OFF_SNAME = JCC_HERO_SNAME;
static constexpr size_t OFF_ICOST = JCC_HERO_ICOST;
static constexpr size_t OFF_PAINT_SMALL = JCC_HERO_PAINT_SMALL;
static constexpr size_t OFF_SETNUM = JCC_HERO_SETNUM;

static constexpr int CTRL_PORT = 31338;
static constexpr int MAX_HERO_ID = 20000;

static char g_data_dir[512]{};
static std::atomic<bool> g_running{false};
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

// 协议状态
static std::string g_pool;          // 牌库 payload
static std::string g_status = "init";
static std::string g_log_buf;
static std::string g_hex = "0,0,0";
static std::string g_warn = "EMPTY";
static std::string g_pos = "EMPTY";
static std::string g_pred = "EMPTY";
static std::string g_last_push_board;

// 开关（原版 SET）
static std::atomic<bool> g_auto_buy{false};
static std::atomic<bool> g_popup_block{false};
static std::atomic<bool> g_shop_display{true};
static std::atomic<bool> g_op_board{true};

// 剩余量：owned[heroId]
static std::map<int, int> g_owned;

// TFT 经典池大小（按费用）
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

static void append_log(const char *msg) {
    if (!msg) return;
    pthread_mutex_lock(&g_mu);
    if (g_log_buf.size() > 32 * 1024) g_log_buf.erase(0, g_log_buf.size() / 2);
    g_log_buf.append(msg);
    g_log_buf.push_back('\n');
    pthread_mutex_unlock(&g_mu);
    LOGI("%s", msg);
}

static void set_status(const char *msg) {
    pthread_mutex_lock(&g_mu);
    g_status = msg ? msg : "";
    pthread_mutex_unlock(&g_mu);
    append_log(msg);
    if (g_data_dir[0]) {
        char path[600];
        snprintf(path, sizeof(path), "%s/files/jcc_shell_status.txt", g_data_dir);
        FILE *f = fopen(path, "a");
        if (f) {
            fprintf(f, "%s\n", msg);
            fclose(f);
        }
    }
}

static void write_pool_file(const std::string &pool) {
    if (!g_data_dir[0]) return;
    char path[600];
    snprintf(path, sizeof(path), "%s/files/jcc_cardpool.txt", g_data_dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(pool.data(), 1, pool.size(), f);
        fputc('\n', f);
        fclose(f);
    }
}

static std::string utf16_to_utf8(const Il2CppChar *chars, int32_t len) {
    std::string out;
    if (!chars || len <= 0) return out;
    out.reserve(static_cast<size_t>(len) * 3);
    for (int32_t i = 0; i < len; i++) {
        uint32_t c = chars[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len) {
            uint32_t c2 = chars[i + 1];
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                c = 0x10000 + (((c - 0xD800) << 10) | (c2 - 0xDC00));
                i++;
            }
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
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

static Il2CppObject *get_db_instance(Il2CppClass *dbKlass) {
    auto m = find_method(dbKlass, "get_Instance", 0);
    if (!m) m = find_method(dbKlass, "get_instance", 0);
    if (!m) return nullptr;
    return invoke(m, nullptr, nullptr);
}

static void sanitize_field(std::string &s) {
    for (char &c : s) {
        if (c == ':' || c == ',' || c == '\n' || c == '\r' || c == '|' || c == ';') c = '_';
    }
}

struct HeroRow {
    int id;
    int cost;
    int total;
    int rem;
    std::string name;
    std::string icon;
};

static std::vector<HeroRow> g_heroes;

static std::string build_pool_payload() {
    std::string out;
    out.reserve(g_heroes.size() * 48);
    for (auto &h : g_heroes) {
        int owned = 0;
        auto it = g_owned.find(h.id);
        if (it != g_owned.end()) owned = it->second;
        int rem = h.total - owned;
        if (rem < 0) rem = 0;
        h.rem = rem;
        char buf[512];
        if (!h.icon.empty()) {
            snprintf(buf, sizeof(buf), "%d:%s:%d:%d:%d:%s", h.id, h.name.c_str(), h.cost, rem,
                     h.total, h.icon.c_str());
        } else {
            snprintf(buf, sizeof(buf), "%d:%s:%d:%d:%d", h.id, h.name.c_str(), h.cost, rem, h.total);
        }
        if (!out.empty()) out.push_back(',');
        out.append(buf);
    }
    return out;
}

static bool scan_cardpool_table() {
    set_status("scan: DataBaseManager (" JCC_SEASON_TAG ")");
    Il2CppClass *db = find_class(JCC_NS_DB, JCC_CLS_DB);
    if (!db) db = find_class("ZGame", "DataBaseManager");
    if (!db) {
        set_status("FAIL: DataBaseManager missing");
        return false;
    }
    auto mSearch = find_method(db, "SearchACGHero2", 1);
    if (!mSearch) mSearch = find_method(db, "SearchACGHero", 1);
    if (!mSearch) {
        set_status("FAIL: SearchACGHero missing");
        return false;
    }
    auto inst = get_db_instance(db);
    if (!inst) {
        set_status("FAIL: get_Instance null");
        return false;
    }

    std::vector<HeroRow> rows;
    rows.reserve(256);
    for (int id = 1; id <= MAX_HERO_ID; id++) {
        void *params[1];
        int32_t hid = id;
        params[0] = &hid;
        auto hero = invoke(mSearch, inst, params);
        if (!hero) continue;
        char *base = reinterpret_cast<char *>(hero);
        int32_t iid = *reinterpret_cast<int32_t *>(base + OFF_IID);
        int32_t cost = *reinterpret_cast<int32_t *>(base + OFF_ICOST);
        auto *nameObj = *reinterpret_cast<Il2CppString **>(base + OFF_SNAME);
        auto *iconObj = *reinterpret_cast<Il2CppString **>(base + OFF_PAINT_SMALL);
        if (iid <= 0) iid = id;
        if (cost < 1 || cost > 5) continue;
        std::string name = il2cpp_str(nameObj);
        if (name.empty()) continue;
        std::string icon = il2cpp_str(iconObj);
        sanitize_field(name);
        sanitize_field(icon);
        HeroRow row{};
        row.id = iid;
        row.cost = cost;
        row.total = pool_total_by_cost(cost);
        row.rem = row.total;
        row.name = name;
        row.icon = icon;
        rows.push_back(row);
    }
    if (rows.empty()) {
        set_status("FAIL: no heroes");
        return false;
    }
    pthread_mutex_lock(&g_mu);
    g_heroes.swap(rows);
    g_pool = build_pool_payload();
    pthread_mutex_unlock(&g_mu);
    write_pool_file(g_pool);
    char st[96];
    snprintf(st, sizeof(st), "scan done heroes=%zu", g_heroes.size());
    set_status(st);
    return true;
}

// 尝试：从 BuyHeroView / 商店读当前可买英雄，用于自动购买（简化）
static void try_auto_buy() {
    if (!g_auto_buy.load()) return;
    // 完整自动购买依赖商店槽位 hook；此处占位：标记状态，后续接 HandleRefreshBuyHero
    // 避免误购
}

// 尝试读海克斯（HextechAugments）— 找不到则保持
static void refresh_hex_quality() {
    // 原版: "q0,q1,q2" 0-3
    // 未定位到稳定字段时保持上次值或 EMPTY 等价 0,0,0
    Il2CppClass *k = find_class("", "HextechAugmentsCtrl");
    if (!k) k = find_class("ZGameChess", "HextechAugmentsCtrl");
    if (!k) {
        // keep
        return;
    }
    // 暂无稳定偏移：保留 g_hex
}

// 头像位置：WxH:count:opIdx|x,y,isSelf|...
// 三星预警：|rank,a,b,c,name:id,name,cnt,cost,star,icon;...
// 对手预测：EMPTY 或自定义
// 这些依赖局内 UI 节点，当前以安全 EMPTY 为主，找到 PlayerList 再填
static void refresh_battle_info() {
    // 尝试 PlayerListPanel 存在与否
    Il2CppClass *pl = find_class("", "PlayerListPanel");
    if (!pl) pl = find_class("ZGameChess", "PlayerListPanel");
    if (!pl) {
        pthread_mutex_lock(&g_mu);
        g_pos = "EMPTY";
        g_warn = "EMPTY";
        g_pred = "EMPTY";
        pthread_mutex_unlock(&g_mu);
        return;
    }
    // 有类但未完整还原字段时，仍给合法 EMPTY，避免 UI 崩
    // TODO: 用 dump 字段走 PlayerListItem 坐标
    pthread_mutex_lock(&g_mu);
    // 保持非空格式兼容：空局
    if (g_pos.empty()) g_pos = "EMPTY";
    if (g_warn.empty()) g_warn = "EMPTY";
    if (g_pred.empty()) g_pred = "EMPTY";
    pthread_mutex_unlock(&g_mu);
}

// 推送对手站位
static void maybe_push_board_to_clients() {
    // 格式: PUSH:OPPONENT_BOARD:w,h|x,y,id,star,cost,icon|...
    // 无数据时 PUSH CLEAR
}

static std::string handle_request(const char *req) {
    if (!req) return "RSP:ERR\n";

    // SET:key:0|1
    if (strncmp(req, "SET:", 4) == 0) {
        const char *p = req + 4;
        int on = strstr(p, ":1") != nullptr;
        if (strstr(p, "自动购买")) {
            g_auto_buy.store(on);
            set_status(on ? "auto_buy=1" : "auto_buy=0");
            return "RSP:OK\n";
        }
        if (strstr(p, "弹窗拦截")) {
            g_popup_block.store(on);
            set_status(on ? "popup_block=1" : "popup_block=0");
            return "RSP:OK\n";
        }
        if (strstr(p, "商店显示")) {
            g_shop_display.store(on);
            return "RSP:OK\n";
        }
        if (strstr(p, "对手站位")) {
            g_op_board.store(on);
            return "RSP:OK\n";
        }
        // 转区：明确忽略
        if (strstr(p, "转区")) {
            return "RSP:OK\n";
        }
        return "RSP:OK\n";
    }

    // DO:
    if (strncmp(req, "DO:", 3) == 0) {
        const char *p = req + 3;
        if (strstr(p, "清空日志")) {
            pthread_mutex_lock(&g_mu);
            g_log_buf.clear();
            pthread_mutex_unlock(&g_mu);
            return "RSP:OK\n";
        }
        if (strstr(p, "退出游戏")) {
            // 不强制 kill，回 OK；真退出由用户
            set_status("DO:退出游戏 ignored_safe");
            return "RSP:OK\n";
        }
        if (strstr(p, "RESCAN") || strstr(p, "刷新")) {
            scan_cardpool_table();
            return "RSP:OK\n";
        }
        return "RSP:OK\n";
    }

    // GET:
    if (strncmp(req, "GET:", 4) == 0) {
        const char *p = req + 4;
        pthread_mutex_lock(&g_mu);
        std::string body;
        if (strstr(p, "牌库")) {
            body = "RSP:" + (g_pool.empty() ? std::string("") : g_pool) + "\n";
        } else if (strstr(p, "日志")) {
            body = "RSP:" + g_log_buf + "\n";
        } else if (strstr(p, "海克斯品质")) {
            body = "RSP:" + g_hex + "\n";
        } else if (strstr(p, "三星预警")) {
            body = "RSP:" + g_warn + "\n";
        } else if (strstr(p, "头像位置")) {
            body = "RSP:" + g_pos + "\n";
        } else if (strstr(p, "对手预测")) {
            body = "RSP:" + g_pred + "\n";
        } else if (strstr(p, "STATUS") || strstr(p, "status")) {
            body = "RSP:" + g_status + "\n";
        } else if (strstr(p, "转区")) {
            body = "RSP:0\n"; // 不用转区
        } else {
            body = "RSP:EMPTY\n";
        }
        pthread_mutex_unlock(&g_mu);
        return body;
    }

    return "RSP:ERR unknown\n";
}

static void *server_thread(void *) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_status("FAIL: socket");
        return nullptr;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(CTRL_PORT);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        set_status("FAIL: bind 31338");
        close(fd);
        return nullptr;
    }
    listen(fd, 8);
    set_status("server: 127.0.0.1:31338 full-protocol");

    while (g_running.load()) {
        int c = accept(fd, nullptr, nullptr);
        if (c < 0) continue;
        char req[1024]{};
        ssize_t n = recv(c, req, sizeof(req) - 1, 0);
        if (n > 0) {
            // 一条连接可多行
            char *line = req;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                auto body = handle_request(line);
                send(c, body.data(), body.size(), 0);
                if (!nl) break;
                line = nl + 1;
            }
        }
        close(c);
    }
    close(fd);
    return nullptr;
}

static void *worker(void *) {
    for (int i = 0; i < 90; i++) {
        if (scan_cardpool_table()) break;
        char st[64];
        snprintf(st, sizeof(st), "wait tables... %ds", (i + 1) * 2);
        set_status(st);
        sleep(2);
    }
    while (g_running.load()) {
        sleep(3);
        refresh_hex_quality();
        refresh_battle_info();
        try_auto_buy();
        // 刷新牌库 remaining 展示
        pthread_mutex_lock(&g_mu);
        if (!g_heroes.empty()) {
            g_pool = build_pool_payload();
        }
        pthread_mutex_unlock(&g_mu);
        // 每 2 分钟重扫表（热更）
        static int tick = 0;
        if ((++tick % 40) == 0) {
            scan_cardpool_table();
        }
    }
    return nullptr;
}

// 供外部/后续 hook 报告商店/场上棋子占用
extern "C" void jcc_report_owned(int hero_id, int count) {
    if (hero_id <= 0) return;
    pthread_mutex_lock(&g_mu);
    g_owned[hero_id] = count;
    if (!g_heroes.empty()) g_pool = build_pool_payload();
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
    g_running.store(true);
    set_status("full_protocol_start " JCC_SEASON_TAG);

    if (il2cpp_domain_get && il2cpp_thread_attach) {
        auto domain = il2cpp_domain_get();
        if (domain) {
            il2cpp_thread_attach(domain);
            set_status("il2cpp_thread_attach ok");
        }
    }

    pthread_t t1{}, t2{};
    pthread_create(&t1, nullptr, server_thread, nullptr);
    pthread_detach(t1);
    pthread_create(&t2, nullptr, worker, nullptr);
    pthread_detach(t2);
}
