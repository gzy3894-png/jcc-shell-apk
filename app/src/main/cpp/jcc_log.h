#pragma once
// 游戏进程可写路径（Download 经常写不进，别用）
// 只维护最多 2 个文件：files/jcc.log + /data/local/tmp/jcc.log
#include <cstdio>
#include <ctime>
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
        // 候选：游戏 files（进程一定能写）+ tmp
        const char *cands[] = {
            "/data/user/0/com.tencent.jkchess/files/jcc.log",
            "/data/data/com.tencent.jkchess/files/jcc.log",
            "/data/local/tmp/jcc.log",
            nullptr,
        };
        primary_.clear();
        secondary_.clear();
        for (int i = 0; cands[i]; i++) {
            // 确保 files 目录存在
            std::string p = cands[i];
            auto slash = p.rfind('/');
            if (slash != std::string::npos) {
                std::string dir = p.substr(0, slash);
                mkdir(dir.c_str(), 0777);
            }
            FILE *f = fopen(cands[i], "a");
            if (!f) continue;
            fprintf(f, "\n==== BOOT %ld pid=%d ====\n", (long)time(nullptr), (int)getpid());
            fflush(f);
            fclose(f);
            if (primary_.empty())
                primary_ = cands[i];
            else if (secondary_.empty() && primary_ != cands[i])
                secondary_ = cands[i];
        }
        inited_ = true;
    }

    void log(const char *level, const char *msg) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!inited_) init(nullptr);
        char line[900];
        snprintf(line, sizeof(line), "[%ld][%s] %s\n", (long)time(nullptr), level ? level : "?",
                 msg ? msg : "");
        write_one(primary_.c_str(), line);
        if (!secondary_.empty()) write_one(secondary_.c_str(), line);
    }

    void checkpoint(const char *name) {
        char b[128];
        snprintf(b, sizeof(b), "CKPT %s", name ? name : "?");
        log("C", b);
    }

    const char *path() const {
        return primary_.empty() ? "(no-writable-log-path)" : primary_.c_str();
    }

private:
    std::mutex mu_;
    std::string primary_;
    std::string secondary_;
    bool inited_{false};

    static void write_one(const char *path, const char *line) {
        if (!path || !path[0] || !line) return;
        FILE *f = fopen(path, "a");
        if (!f) return;
        fputs(line, f);
        fflush(f);
        fclose(f);
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
