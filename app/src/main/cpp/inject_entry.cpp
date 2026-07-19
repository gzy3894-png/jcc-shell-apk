// dlopen 进游戏后自动跑
// 1) 构造函数立刻写 jcc.log（证明注入成功）
// 2) 立刻起 31338（Controller 能连上）
// 3) 延后碰 il2cpp / 业务，避免加载页资源错误
#include "cardpool.h"
#include "il2cpp_dump.h"
#include "jcc_log.h"
#include "log.h"
#include "xdl.h"

#include <pthread.h>
#include <unistd.h>

static void *boot_thread(void *) {
    JLOGI("boot_thread full-1.0.3 path=%s", JccFileLog::I().path());

    // TCP 先起：不依赖 il2cpp，Controller 连上就能看到版本日志
    cardpool_start_server_only();

    // 等加载高峰过后再碰 libil2cpp
    sleep(12);

    for (int i = 0; i < 150; i++) {
        void *handle = xdl_open("libil2cpp.so", 0);
        if (handle) {
            JLOGI("libil2cpp +%ds", i + 12);
            il2cpp_api_init(handle);
            sleep(8); // 再错开进局资源加载
            cardpool_start_worker();
            JLOGI("worker_started full-1.0.3");
            return nullptr;
        }
        if ((i % 10) == 0) JLOGI("wait_il2cpp %d", i);
        sleep(1);
    }
    JLOGE("FAIL libil2cpp timeout — inject ok but game runtime not ready");
    return nullptr;
}

__attribute__((constructor)) static void on_load() {
    // 最早、最硬：只要 so 被加载，就必须留下字
    JccFileLog::I().init(nullptr);
    JLOGI("BOOT full-1.0.3 so_loaded path=%s", JccFileLog::I().path());

    pthread_t t;
    pthread_create(&t, nullptr, boot_thread, nullptr);
    pthread_detach(t);
}
