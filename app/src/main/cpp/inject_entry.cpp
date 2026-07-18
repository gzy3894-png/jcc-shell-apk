// 被 JCC.sh / ptrace dlopen 进游戏进程后自动跑
#include "cardpool.h"
#include "il2cpp_dump.h"
#include "jcc_log.h"
#include "log.h"
#include "xdl.h"

#include <cstdio>
#include <pthread.h>
#include <unistd.h>

static const char *kDataDir = "/data/user/0/com.tencent.jkchess";

static void *boot_thread(void *) {
    JccFileLog::I().init(kDataDir);
    JCKPT("inject_boot_start");
    JLOGI("inject_entry boot_thread");

    for (int i = 0; i < 180; i++) {
        void *handle = xdl_open("libil2cpp.so", 0);
        if (handle) {
            JLOGI("libil2cpp +%ds", i);
            JCKPT("libil2cpp_found");
            il2cpp_api_init(handle);
            JCKPT("il2cpp_api_init_done");
            cardpool_start(kDataDir);
            JCKPT("cardpool_start_returned");
            return nullptr;
        }
        if (i % 10 == 0) JLOGI("wait il2cpp %d", i);
        sleep(1);
    }
    JLOGE("FAIL: libil2cpp not found");
    JCKPT("libil2cpp_timeout");
    return nullptr;
}

__attribute__((constructor)) static void on_load() {
    // 最早落盘，防断线丢状态
    JccFileLog::I().init(kDataDir);
    JCKPT("constructor");
    JLOGI("libJCC.so constructor full-kernel");
    pthread_t t;
    pthread_create(&t, nullptr, boot_thread, nullptr);
    pthread_detach(t);
}
