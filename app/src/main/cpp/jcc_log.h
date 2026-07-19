#pragma once
// 尽量写进游戏能写的路径；同时业务侧用 GET:日志（内存）不依赖文件
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

class JccFileLog {
public:
    static JccFileLog &I() {
        static JccFileLog inst;
        return inst;
    }

    void init(const char * /*ignored*/) {
        std::lock_guard<std::mutex> lk(mu_);
        if (inited_) return;
        const char *cands[] = {
            "/data/user/0/com.tencent.jkchess/files/jcc.log",
            "/data/data/com.tencent.jkchess/files/jcc.log",
            "/sdcard/Android/data/com.tencent.jkchess/files/jcc.log",
            "/storage/emulated/0/Android/data/com.tencent.jkchess/files/jcc.log",
            "/data/local/tmp/jcc.log",
            nullptr,
        };
        primary_.clear();
        secondary_.clear();
        for (int i = 0; cands[i]; i++) {
            ensure_parent(cands[i]);
            if (!try_write(cands[i], "\n==== BOOT ====\n")) continue;
            if (primary_.empty())
                primary_ = cands[i];
            else if (secondary_.empty())
                secondary_ = cands[i];
        }
        inited_ = true;
    }

    void log(const char *level, const char *msg) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!inited_) {
            // 允许未 init 时直接尝试
            inited_ = true;
        }
        char line[900];
        snprintf(line, sizeof(line), "[%ld][%s] %s\n", (long)time(nullptr), level ? level : "?",
                 msg ? msg : "");
        if (primary_.empty()) {
            // 每次失败再试一遍路径
            const char *cands[] = {
                "/data/user/0/com.tencent.jkchess/files/jcc.log",
                "/data/data/com.tencent.jkchess/files/jcc.log",
                "/sdcard/Android/data/com.tencent.jkchess/files/jcc.log",
                "/data/local/tmp/jcc.log",
                nullptr,
            };
            for (int i = 0; cands[i]; i++) {
                ensure_parent(cands[i]);
                if (try_write(cands[i], line)) {
                    primary_ = cands[i];
                    break;
                }
            }
        } else {
            try_write(primary_.c_str(), line);
            if (!secondary_.empty()) try_write(secondary_.c_str(), line);
        }
    }

    void checkpoint(const char *name) {
        char b[128];
        snprintf(b, sizeof(b), "CKPT %s", name ? name : "?");
        log("C", b);
    }

    const char *path() const {
        return primary_.empty() ? "none" : primary_.c_str();
    }

private:
    std::mutex mu_;
    std::string primary_;
    std::string secondary_;
    bool inited_{false};

    static void ensure_parent(const char *path) {
        if (!path) return;
        std::string p = path;
        auto slash = p.rfind('/');
        if (slash == std::string::npos) return;
        std::string dir = p.substr(0, slash);
        // 逐级 mkdir
        for (size_t i = 1; i < dir.size(); i++) {
            if (dir[i] == '/') {
                dir[i] = 0;
                mkdir(dir.c_str(), 0777);
                dir[i] = '/';
            }
        }
        mkdir(dir.c_str(), 0777);
    }

    static bool try_write(const char *path, const char *line) {
        if (!path || !line) return false;
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0) {
            // fallback FILE*
            FILE *f = fopen(path, "a");
            if (!f) return false;
            fputs(line, f);
            fflush(f);
            fclose(f);
            return true;
        }
        size_t n = strlen(line);
        (void)!write(fd, line, n);
        fsync(fd);
        close(fd);
        return true;
    }
};

#define JLOGI(...)                                                                                 \
    do {                                                                                           \
        char _jcc_buf[768];                                                                        \
        snprintf(_jcc_buf, sizeof(_jcc_buf), __VA_ARGS__);                                         \
        JccFileLog::I().log("I", _jcc_buf);                                                        \
        LOGI("%s", _jcc_buf);                                                                      \
    } while (0)

#define JLOGE(...)                                                                                 \
    do {                                                                                           \
        char _jcc_buf[768];                                                                        \
        snprintf(_jcc_buf, sizeof(_jcc_buf), __VA_ARGS__);                                         \
        JccFileLog::I().log("E", _jcc_buf);                                                        \
        LOGE("%s", _jcc_buf);                                                                      \
    } while (0)

#define JERRF(feat, ...)                                                                           \
    do {                                                                                           \
        char _jcc_buf[768];                                                                        \
        snprintf(_jcc_buf, sizeof(_jcc_buf), "ERR[%s] " __VA_ARGS__, feat);                        \
        JccFileLog::I().log("E", _jcc_buf);                                                        \
        LOGE("%s", _jcc_buf);                                                                      \
    } while (0)

#define JOKF(feat, ...)                                                                            \
    do {                                                                                           \
        char _jcc_buf[768];                                                                        \
        snprintf(_jcc_buf, sizeof(_jcc_buf), "OK[%s] " __VA_ARGS__, feat);                         \
        JccFileLog::I().log("I", _jcc_buf);                                                        \
        LOGI("%s", _jcc_buf);                                                                      \
    } while (0)

#define JCKPT(name) JccFileLog::I().checkpoint(name)
