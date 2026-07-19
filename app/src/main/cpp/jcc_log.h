#pragma once

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#ifndef JCC_LOG_TAG
#define JCC_LOG_TAG "v1.1.2"
#endif

class JccFileLog {
public:
    static JccFileLog &I() {
        static JccFileLog inst;
        return inst;
    }

    void init(const char * ) {
        std::lock_guard<std::mutex> lk(mu_);
        if (inited_) return;
        
        const char *cands[] = {
            "/data/user/0/com.tencent.jkchess/files/log.txt",
            "/data/data/com.tencent.jkchess/files/log.txt",
            "/data/local/tmp/jcc_full.log",
            nullptr,
        };
        path_.clear();
        for (int i = 0; cands[i]; i++) {
            ensure_parent(cands[i]);
            char boot[256];
            snprintf(boot, sizeof(boot),
                     "\n==== [%s] BOOT pid=%d path=%s ====\n", JCC_LOG_TAG, (int)getpid(),
                     cands[i]);
            if (try_write(cands[i], boot)) {
                path_ = cands[i];
                break;
            }
        }
        inited_ = true;
    }

    void log(const char *level, const char *msg) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!inited_) init(nullptr);
        char line[960];
        snprintf(line, sizeof(line), "[%ld][%s][%s] %s\n", (long)time(nullptr), JCC_LOG_TAG,
                 level ? level : "?", msg ? msg : "");
        if (!path_.empty()) {
            try_write(path_.c_str(), line);
            return;
        }
        
        const char *cands[] = {
            "/data/user/0/com.tencent.jkchess/files/log.txt",
            "/data/data/com.tencent.jkchess/files/log.txt",
            "/data/local/tmp/jcc_full.log",
            nullptr,
        };
        for (int i = 0; cands[i]; i++) {
            ensure_parent(cands[i]);
            if (try_write(cands[i], line)) {
                path_ = cands[i];
                break;
            }
        }
    }

    void checkpoint(const char *name) {
        char b[128];
        snprintf(b, sizeof(b), "CKPT %s", name ? name : "?");
        log("C", b);
    }

    const char *path() const { return path_.empty() ? "none" : path_.c_str(); }

private:
    std::mutex mu_;
    std::string path_;
    bool inited_{false};

    static void ensure_parent(const char *path) {
        if (!path) return;
        std::string p = path;
        auto slash = p.rfind('/');
        if (slash == std::string::npos) return;
        std::string dir = p.substr(0, slash);
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
        if (fd >= 0) {
            size_t n = strlen(line);
            (void)!write(fd, line, n);
            fsync(fd);
            close(fd);
            return true;
        }
        FILE *f = fopen(path, "a");
        if (!f) return false;
        fputs(line, f);
        fflush(f);
        fclose(f);
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
