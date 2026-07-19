// 被 JCC.sh / ptrace dlopen 进游戏进程后自动跑
// 注意：加载阶段禁止狂扫 IL2CPP，否则易「资源错误」
#include "cardpool.h"
#include "il2cpp_dump.h"
#include "jcc_log.h"
#include "log.h"
#include "xdl.h"

#include <pthread.h>
#include <unistd.h>

static void *boot_thread(void *) {
    // 日志只写 Download/jcc-scan/jcc.log（不写游戏目录）
    JccFileLog::I().init(nullptr);
    JLOGI("inject_boot path=%s", JccFileLog::I().path());

    // 等游戏完成基础加载，再碰 libil2cpp（避免进局加载页资源错误）
    sleep(8);

    for (int i = 0; i < 120; i++) {
        void *handle = xdl_open("libil2cpp.so", 0);
        if (handle) {
            JLOGI("libil2cpp ready +%ds", i + 8);
            il2cpp_api_init(handle);
            // 再等一会儿再起业务线程，错开「匹配成功→加载资源」高峰
            sleep(5);
            cardpool_start(nullptr);
            return nullptr;
        }
        if (i % 15 == 0) JLOGI("wait il2cpp %d", i);
        sleep(1);
    }
    JLOGE("FAIL libil2cpp not found");
    return nullptr;
}

__attribute__((constructor)) static void on_load() {
    // constructor 里只起线程，不做任何游戏调用、不写游戏目录
    pthread_t t;
    pthread_create(&t, nullptr, boot_thread, nullptr);
    pthread_detach(t);
}
