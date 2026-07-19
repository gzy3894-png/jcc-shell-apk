#pragma once
// 只写 1 个日志文件，禁止往金铲铲 data 目录塞文件
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>

class JccFileLog {
public:
    static JccFileLog &I() {
        static JccFileLog inst;
        return inst;
    }

    // game_data_dir 忽略：绝不写游戏 files/
    void init(const char * /*game_data_dir*/) {
        std::lock_guard<std::mutex> lk(mu_);
        if (inited_) return;
        mkdir("/sdcard/Download/jcc-scan", 0777);
        // 优先公共目录（用户可直接文件管理器看到）
        path_ = "/sdcard/Download/jcc-scan/jcc.log";
        FILE *t = fopen(path_.c_str(), "a");
        if (!t) {
            path_ = "/data/local/tmp/jcc.log";
            t = fopen(path_.c_str(), "a");
        }
        if (t) {
            fprintf(t, "\n==== %ld path=%s ====\n", (long)time(nullptr), path_.c_str());
            fflush(t);
            fclose(t);
        }
        inited_ = true;
    }

    void log(const char *level, const char *msg) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!inited_) init(nullptr);
        if (path_.empty()) return;
        FILE *f = fopen(path_.c_str(), "a");
        if (!f) return;
        fprintf(f, "[%ld][%s] %s\n", (long)time(nullptr), level ? level : "?", msg ? msg : "");
        fflush(f);
        fclose(f);
    }

    void checkpoint(const char *name) {
        char b[128];
        snprintf(b, sizeof(b), "CKPT %s", name ? name : "?");
        log("C", b);
    }

    const char *path() const { return path_.c_str(); }

private:
    std::mutex mu_;
    std::string path_;
    bool inited_{false};
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
