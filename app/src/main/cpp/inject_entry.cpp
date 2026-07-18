// 被 JCC.sh / ptrace dlopen 进游戏进程后自动跑
#include "cardpool.h"
#include "il2cpp_dump.h"
#include "log.h"
#include "xdl.h"

#include <pthread.h>
#include <unistd.h>
#include <cstdio>

static const char *kDataDir = "/data/user/0/com.tencent.jkchess";

static void *boot_thread(void *) {
    LOGI("inject_entry boot_thread");
    char status[512];
    snprintf(status, sizeof(status), "%s/files/jcc_shell_status.txt", kDataDir);
    FILE *f = fopen(status, "w");
    if (f) {
        fprintf(f, "inject_entry: waiting libil2cpp\n");
        fclose(f);
    }

    for (int i = 0; i < 180; i++) {
        void *handle = xdl_open("libil2cpp.so", 0);
        if (handle) {
            LOGI("libil2cpp +%ds", i);
            il2cpp_api_init(handle);
            cardpool_start(kDataDir);
            return nullptr;
        }
        if (i % 10 == 0) LOGI("wait il2cpp %d", i);
        sleep(1);
    }
    f = fopen(status, "a");
    if (f) {
        fprintf(f, "FAIL: libil2cpp not found\n");
        fclose(f);
    }
    return nullptr;
}

__attribute__((constructor))
static void on_load() {
    LOGI("libJCC.so constructor (jcc-shell-apk)");
    pthread_t t;
    pthread_create(&t, nullptr, boot_thread, nullptr);
    pthread_detach(t);
}
