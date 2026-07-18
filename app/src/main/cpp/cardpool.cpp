// JCC Shell 2.5.1 — 当前赛季牌库（机扫新数据）
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
#include <string>
#include <vector>

// 函数指针在 il2cpp_dump.cpp 里定义，这里只引用
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
static std::string g_pool; // id:name:cost:rem:total:icon,...
static std::string g_status = "init";

static void set_status(const char *msg) {
    pthread_mutex_lock(&g_mu);
    g_status = msg ? msg : "";
    pthread_mutex_unlock(&g_mu);
    LOGI("status: %s", msg);
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
    int32_t n = il2cpp_string_length(s);
    return utf16_to_utf8(il2cpp_string_chars(s), n);
}

static Il2CppClass *find_class(const char *ns, const char *name) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_assembly_get_image ||
        !il2cpp_class_from_name) {
        return nullptr;
    }
    size_t n = 0;
    auto domain = il2cpp_domain_get();
    if (!domain) return nullptr;
    auto asms = il2cpp_domain_get_assemblies(domain, &n);
    if (!asms) return nullptr;
    for (size_t i = 0; i < n; i++) {
        auto image = il2cpp_assembly_get_image(asms[i]);
        if (!image) continue;
        auto klass = il2cpp_class_from_name(image, ns, name);
        if (klass) return klass;
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
    if (exc) {
        LOGW("invoke exception on method");
        return nullptr;
    }
    return ret;
}

static Il2CppObject *get_db_instance(Il2CppClass *dbKlass) {
    // Singleton`1: get_Instance()
    auto m = find_method(dbKlass, "get_Instance", 0);
    if (!m) m = find_method(dbKlass, "get_instance", 0);
    if (!m) return nullptr;
    return invoke(m, nullptr, nullptr);
}

static void sanitize_field(std::string &s) {
    for (char &c : s) {
        if (c == ':' || c == ',' || c == '\n' || c == '\r') c = '_';
    }
}

static std::string scan_cardpool() {
    set_status("scan: resolve DataBaseManager (" JCC_SEASON_TAG ")");
    Il2CppClass *db = find_class(JCC_NS_DB, JCC_CLS_DB);
    if (!db) db = find_class("ZGame", "DataBaseManager");
    if (!db) db = find_class("", "DataBaseManager");
    if (!db) {
        set_status("FAIL: DataBaseManager class not found");
        return {};
    }

    auto mSearch = find_method(db, "SearchACGHero2", 1);
    if (!mSearch) mSearch = find_method(db, "SearchACGHero", 1);
    if (!mSearch) mSearch = find_method(db, "SearchACGHero2", 2);
    if (!mSearch) {
        set_status("FAIL: SearchACGHero method not found");
        return {};
    }

    auto inst = get_db_instance(db);
    if (!inst) {
        set_status("FAIL: DataBaseManager.get_Instance null (进大厅后再等)");
        return {};
    }

    set_status("scan: probing hero ids...");
    std::string out;
    out.reserve(64 * 1024);
    int hit = 0;
    int cost_ok = 0;

    for (int id = 1; id <= MAX_HERO_ID; id++) {
        void *params[1];
        int32_t hid = id;
        params[0] = &hid;
        auto hero = invoke(mSearch, inst, params);
        if (!hero) continue;

        int32_t iid = *reinterpret_cast<int32_t *>(reinterpret_cast<char *>(hero) + OFF_IID);
        int32_t cost = *reinterpret_cast<int32_t *>(reinterpret_cast<char *>(hero) + OFF_ICOST);
        auto *nameObj = *reinterpret_cast<Il2CppString **>(reinterpret_cast<char *>(hero) + OFF_SNAME);
        auto *iconObj =
                *reinterpret_cast<Il2CppString **>(reinterpret_cast<char *>(hero) + OFF_PAINT_SMALL);

        if (iid <= 0) iid = id;
        // 费用 1–5 视为有效棋子；其它可能是召唤物/占位
        if (cost < 1 || cost > 5) continue;

        std::string name = il2cpp_str(nameObj);
        std::string icon = il2cpp_str(iconObj);
        if (name.empty()) continue;
        sanitize_field(name);
        sanitize_field(icon);

        // 协议: id:name:cost:remaining:total[:icon]
        // 静态表无剩余量：用 0/0 占位，对局 hook 后再补
        char buf[512];
        if (!icon.empty()) {
            snprintf(buf, sizeof(buf), "%d:%s:%d:0:0:%s", iid, name.c_str(), cost, icon.c_str());
        } else {
            snprintf(buf, sizeof(buf), "%d:%s:%d:0:0", iid, name.c_str(), cost);
        }
        if (!out.empty()) out.push_back(',');
        out.append(buf);
        hit++;
        cost_ok++;
        if ((hit % 50) == 0) {
            char st[80];
            snprintf(st, sizeof(st), "scan: hit=%d last_id=%d", hit, id);
            set_status(st);
        }
    }

    char st[96];
    snprintf(st, sizeof(st), "scan done: heroes=%d", hit);
    set_status(st);
    LOGI("cardpool heroes=%d bytes=%zu", hit, out.size());
    return out;
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
    listen(fd, 4);
    set_status("server: 127.0.0.1:31338 listening");

    while (g_running.load()) {
        int c = accept(fd, nullptr, nullptr);
        if (c < 0) continue;
        char req[512]{};
        ssize_t n = recv(c, req, sizeof(req) - 1, 0);
        if (n > 0) {
            // 兼容原 JCC: GET:牌库 / GET:日志
            std::string body;
            pthread_mutex_lock(&g_mu);
            if (strstr(req, "GET:") && (strstr(req, "牌库") || strstr(req, "\xe7\x89\x8c\xe5\xba\x93"))) {
                body = "RSP:" + g_pool + "\n";
            } else if (strstr(req, "GET:") && (strstr(req, "日志") || strstr(req, "log"))) {
                body = "RSP:" + g_status + "\n";
            } else if (strstr(req, "GET:STATUS") || strstr(req, "GET:status")) {
                body = "RSP:" + g_status + "\n";
            } else if (strstr(req, "DO:RESCAN") || strstr(req, "DO:刷新")) {
                pthread_mutex_unlock(&g_mu);
                auto pool = scan_cardpool();
                pthread_mutex_lock(&g_mu);
                if (!pool.empty()) {
                    g_pool = pool;
                    write_pool_file(g_pool);
                }
                body = "RSP:OK " + g_status + "\n";
            } else {
                body = "RSP:ERR unknown cmd (use GET:牌库)\n";
            }
            pthread_mutex_unlock(&g_mu);
            send(c, body.data(), body.size(), 0);
        }
        close(c);
    }
    close(fd);
    return nullptr;
}

static void *worker(void *) {
    // 等 DataBase 表加载：进大厅后更稳
    for (int i = 0; i < 60; i++) {
        sleep(2);
        auto pool = scan_cardpool();
        if (!pool.empty()) {
            pthread_mutex_lock(&g_mu);
            g_pool = pool;
            pthread_mutex_unlock(&g_mu);
            write_pool_file(pool);
            break;
        }
        char st[64];
        snprintf(st, sizeof(st), "wait tables... %ds", (i + 1) * 2);
        set_status(st);
    }

    // 周期轻量刷新（表热更 / 赛季内补丁）
    while (g_running.load()) {
        sleep(120);
        auto pool = scan_cardpool();
        if (!pool.empty()) {
            pthread_mutex_lock(&g_mu);
            g_pool = pool;
            pthread_mutex_unlock(&g_mu);
            write_pool_file(pool);
        }
    }
    return nullptr;
}

void cardpool_start(const char *game_data_dir) {
    if (!game_data_dir) return;
    strncpy(g_data_dir, game_data_dir, sizeof(g_data_dir) - 1);
    g_running.store(true);
    set_status("cardpool_start");

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
