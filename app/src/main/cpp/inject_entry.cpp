

#include "cardpool.h"
#include "il2cpp_dump.h"
#include "jcc_log.h"
#include "log.h"
#include "xdl.h"

#include <pthread.h>
#include <unistd.h>

static void *boot_thread(void *) {
    JLOGI("boot_thread v1.1.4 path=%s", JccFileLog::I().path());

    
    cardpool_start_server_only();

    
    sleep(12);

    for (int i = 0; i < 150; i++) {
        void *handle = xdl_open("libil2cpp.so", 0);
        if (handle) {
            JLOGI("libil2cpp +%ds", i + 12);
            il2cpp_api_init(handle);
            sleep(8); 
            cardpool_start_worker();
            JLOGI("worker_started v1.1.4");
            return nullptr;
        }
        if ((i % 10) == 0) JLOGI("wait_il2cpp %d", i);
        sleep(1);
    }
    JLOGE("FAIL libil2cpp timeout — inject ok but game runtime not ready");
    return nullptr;
}

__attribute__((constructor)) static void on_load() {
    
    JccFileLog::I().init(nullptr);
    JLOGI("BOOT so_loaded log=%s", JccFileLog::I().path());

    pthread_t t;
    pthread_create(&t, nullptr, boot_thread, nullptr);
    pthread_detach(t);
}
