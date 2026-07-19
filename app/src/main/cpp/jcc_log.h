#pragma once
// 强制多路径落盘 — 真机必能捞到报错（游戏 files + Download + tmp）
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

class JccFileLog {
public:
    static JccFileLog &I() {
        static JccFileLog inst;
        return inst;
    }

    void init(const char *game_data_dir) {
        std::lock_guard<std::mutex> lk(mu_);
        paths_.clear();
        last_err_paths_.clear();

        if (game_data_dir && game_data_dir[0]) {
            char p[640];
            snprintf(p, sizeof(p), "%s/files", game_data_dir);
            mkdir(p, 0777);
            snprintf(p, sizeof(p), "%s/files/jcc_full.log", game_data_dir);
            paths_.push_back(p);
            primary_ = p;
            snprintf(p, sizeof(p), "%s/files/jcc_last_error.txt", game_data_dir);
            last_err_paths_.push_back(p);
            snprintf(p, sizeof(p), "%s/files/jcc_shell_status.txt", game_data_dir);
            status_path_ = p;
        }

        mkdir("/sdcard/Download/jcc-scan", 0777);
        paths_.push_back("/sdcard/Download/jcc-scan/jcc_full.log");
        last_err_paths_.push_back("/sdcard/Download/jcc-scan/jcc_last_error.txt");
        paths_.push_back("/data/local/tmp/jcc_full.log");
        last_err_paths_.push_back("/data/local/tmp/jcc_last_error.txt");
        if (primary_.empty()) primary_ = paths_[0];

        char head[256];
        snprintf(head, sizeof(head),
                 "\n==== session %ld primary=%s ====\n", (long)time(nullptr), primary_.c_str());
        for (auto &p : paths_) append_raw(p.c_str(), head);
        // 启动即写一条，证明日志通道活着
        log("BOOT", "jcc_file_log_alive multi-path");
    }

    void log(const char *level, const char *msg) {
        std::lock_guard<std::mutex> lk(mu_);
        char line[1024];
        snprintf(line, sizeof(line), "[%ld][%s] %s\n", (long)time(nullptr), level ? level : "?",
                 msg ? msg : "");
        for (auto &p : paths_) append_raw(p.c_str(), line);
        if (status_path_.size()) append_raw(status_path_.c_str(), line);

        // 错误单独落 last_error，方便只 adb pull 一个小文件
        if (level && (level[0] == 'E' || level[0] == 'F' ||
                      (level[0] == 'W' && level[1] == 'A'))) {
            for (auto &p : last_err_paths_) {
                FILE *f = fopen(p.c_str(), "w"); // overwrite = 最新一条错误
                if (!f) continue;
                fputs(line, f);
                fflush(f);
                fclose(f);
            }
            // 错误也追加到 error 滚动文件
            append_raw("/sdcard/Download/jcc-scan/jcc_errors.log", line);
            append_raw("/data/local/tmp/jcc_errors.log", line);
            if (primary_.size()) {
                std::string ep = primary_;
                auto pos = ep.rfind('/');
                if (pos != std::string::npos) {
                    ep = ep.substr(0, pos) + "/jcc_errors.log";
                    append_raw(ep.c_str(), line);
                }
            }
        }
    }

    void checkpoint(const char *name) {
        char b[256];
        snprintf(b, sizeof(b), "CHECKPOINT %s", name ? name : "?");
        log("CKPT", b);
        const char *marks[] = {"/data/local/tmp/jcc_ckpt.txt",
                               "/sdcard/Download/jcc-scan/jcc_ckpt.txt", nullptr};
        for (int i = 0; marks[i]; i++) {
            FILE *f = fopen(marks[i], "w");
            if (!f) continue;
            fprintf(f, "%s %ld\n", name ? name : "?", (long)time(nullptr));
            fflush(f);
            fclose(f);
        }
    }

    const char *primary() const { return primary_.c_str(); }

private:
    std::mutex mu_;
    std::vector<std::string> paths_;
    std::vector<std::string> last_err_paths_;
    std::string primary_;
    std::string status_path_;

    static void append_raw(const char *path, const char *line) {
        if (!path || !line) return;
        FILE *f = fopen(path, "a");
        if (!f) return;
        fputs(line, f);
        fflush(f);
        fclose(f);
    }
};

// 普通信息 → jcc_full.log
#define JLOGI(...)                                                                                 \
    do {                                                                                           \
        char _jcc_buf[768];                                                                        \
        snprintf(_jcc_buf, sizeof(_jcc_buf), __VA_ARGS__);                                         \
        JccFileLog::I().log("I", _jcc_buf);                                                        \
        LOGI("%s", _jcc_buf);                                                                      \
    } while (0)

// 错误 → jcc_full.log + jcc_last_error.txt + jcc_errors.log
#define JLOGE(...)                                                                                 \
    do {                                                                                           \
        char _jcc_buf[768];                                                                        \
        snprintf(_jcc_buf, sizeof(_jcc_buf), __VA_ARGS__);                                         \
        JccFileLog::I().log("E", _jcc_buf);                                                        \
        LOGE("%s", _jcc_buf);                                                                      \
    } while (0)

// 功能失败：带功能码，必落盘
#define JERRF(feat, ...)                                                                           \
    do {                                                                                           \
        char _jcc_buf[768];                                                                        \
        snprintf(_jcc_buf, sizeof(_jcc_buf), "ERR[%s] " __VA_ARGS__, feat);                        \
        JccFileLog::I().log("E", _jcc_buf);                                                        \
        LOGE("%s", _jcc_buf);                                                                      \
    } while (0)

// 功能成功（节流外可每分钟打）
#define JOKF(feat, ...)                                                                            \
    do {                                                                                           \
        char _jcc_buf[768];                                                                        \
        snprintf(_jcc_buf, sizeof(_jcc_buf), "OK[%s] " __VA_ARGS__, feat);                         \
        JccFileLog::I().log("I", _jcc_buf);                                                        \
        LOGI("%s", _jcc_buf);                                                                      \
    } while (0)

#define JCKPT(name) JccFileLog::I().checkpoint(name)
