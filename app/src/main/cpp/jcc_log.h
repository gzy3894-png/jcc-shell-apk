#pragma once
// 强制落盘日志 — 断线可恢复，避免写崩
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

class JccFileLog {
public:
    static JccFileLog &I() {
        static JccFileLog inst;
        return inst;
    }

    void init(const char *game_data_dir) {
        std::lock_guard<std::mutex> lk(mu_);
        if (game_data_dir && game_data_dir[0]) {
            char p[640];
            snprintf(p, sizeof(p), "%s/files/jcc_full.log", game_data_dir);
            path_ = p;
            snprintf(p, sizeof(p), "%s/files/jcc_shell_status.txt", game_data_dir);
            status_path_ = p;
        } else {
            path_ = "/data/local/tmp/jcc_full.log";
            status_path_ = "/data/local/tmp/jcc_shell_status.txt";
        }
        // also mirror to sdcard if writable
        FILE *f = fopen(path_.c_str(), "a");
        if (f) {
            fprintf(f, "\n==== session %ld ====\n", (long)time(nullptr));
            fflush(f);
            fclose(f);
        }
        FILE *s = fopen("/sdcard/Download/jcc-scan/jcc_full.log", "a");
        if (s) {
            fprintf(s, "\n==== session %ld path=%s ====\n", (long)time(nullptr), path_.c_str());
            fflush(s);
            fclose(s);
        }
    }

    void log(const char *level, const char *msg) {
        std::lock_guard<std::mutex> lk(mu_);
        char line[1024];
        snprintf(line, sizeof(line), "[%ld][%s] %s\n", (long)time(nullptr), level, msg ? msg : "");
        write_path(path_.c_str(), line);
        write_path("/sdcard/Download/jcc-scan/jcc_full.log", line);
        write_path("/data/local/tmp/jcc_full.log", line);
        if (status_path_.size()) {
            FILE *f = fopen(status_path_.c_str(), "a");
            if (f) {
                fputs(line, f);
                fflush(f);
                fclose(f);
            }
        }
    }

    void checkpoint(const char *name) {
        char b[256];
        snprintf(b, sizeof(b), "CHECKPOINT %s", name ? name : "?");
        log("CKPT", b);
        // durable marker file
        FILE *f = fopen("/data/local/tmp/jcc_ckpt.txt", "w");
        if (f) {
            fprintf(f, "%s %ld\n", name ? name : "?", (long)time(nullptr));
            fflush(f);
            fclose(f);
        }
        f = fopen("/sdcard/Download/jcc-scan/jcc_ckpt.txt", "w");
        if (f) {
            fprintf(f, "%s %ld\n", name ? name : "?", (long)time(nullptr));
            fflush(f);
            fclose(f);
        }
    }

private:
    std::mutex mu_;
    std::string path_;
    std::string status_path_;

    static void write_path(const char *path, const char *line) {
        if (!path || !line) return;
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

#define JCKPT(name) JccFileLog::I().checkpoint(name)
