// jcc_inject: ptrace remote dlopen — 只加载 SO，不装任何游戏 Hook
// usage: jcc_inject <pid> <absolute_so_path>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>

#ifndef PTRACE_GETREGSET
#define PTRACE_GETREGSET 0x4204
#define PTRACE_SETREGSET 0x4205
#endif
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

struct pt_regs_arm64 {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

static void die(const char *m) {
    fprintf(stderr, "[-] %s errno=%d\n", m, errno);
    _exit(1);
}

static void poke(pid_t pid, uint64_t addr, const void *buf, size_t n) {
    auto *p = (const uint8_t *)buf;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(uintptr_t)(addr + i), (void *)(uintptr_t)v) < 0)
            die("POKE");
    }
    if (i < n) {
        errno = 0;
        long old = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)(addr + i), nullptr);
        if (old == -1 && errno) die("PEEK");
        uint64_t v = (uint64_t)old;
        memcpy(&v, p + i, n - i);
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(uintptr_t)(addr + i), (void *)(uintptr_t)v) < 0)
            die("POKE2");
    }
}

static bool maps_base(pid_t pid, const char *needle, uint64_t *base) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, needle)) continue;
        unsigned long s = 0;
        if (sscanf(line, "%lx-", &s) == 1) {
            *base = s;
            fclose(f);
            return true;
        }
    }
    fclose(f);
    return false;
}

static uint64_t remote_sym(pid_t pid, void *local_fn, const char *mod_hint) {
    Dl_info info{};
    if (!dladdr(local_fn, &info) || !info.dli_fbase) return 0;
    uint64_t off = (uint64_t)local_fn - (uint64_t)info.dli_fbase;
    uint64_t base = 0;
    if (mod_hint && maps_base(pid, mod_hint, &base)) return base + off;
    const char *name = info.dli_fname ? strrchr(info.dli_fname, '/') : nullptr;
    name = name ? name + 1 : info.dli_fname;
    if (name && maps_base(pid, name, &base)) return base + off;
    // try common
    const char *cands[] = {"libdl.so", "linker64", "libc.so", nullptr};
    for (int i = 0; cands[i]; i++) {
        if (maps_base(pid, cands[i], &base)) {
            // only valid if same module family - try libdl first for dlopen
            if (strstr(cands[i], "dl") || strstr(cands[i], "linker")) return base + off;
        }
    }
    return 0;
}

static void getregs(pid_t pid, pt_regs_arm64 *r) {
    iovec io = {r, sizeof(*r)};
    if (ptrace(PTRACE_GETREGSET, pid, (void *)(uintptr_t)NT_PRSTATUS, &io) < 0) die("GETREGSET");
}
static void setregs(pid_t pid, pt_regs_arm64 *r) {
    iovec io = {r, sizeof(*r)};
    if (ptrace(PTRACE_SETREGSET, pid, (void *)(uintptr_t)NT_PRSTATUS, &io) < 0) die("SETREGSET");
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <pid> </abs/path/libJCC.so>\n", argv[0]);
        return 2;
    }
    pid_t pid = (pid_t)atoi(argv[1]);
    const char *so = argv[2];
    if (pid <= 1 || !so || so[0] != '/') {
        fprintf(stderr, "bad args\n");
        return 2;
    }

    uint64_t dlopen_r = remote_sym(pid, (void *)dlopen, "libdl.so");
    if (!dlopen_r) dlopen_r = remote_sym(pid, (void *)dlopen, "linker64");
    if (!dlopen_r) die("resolve remote dlopen");

    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) < 0) die("ATTACH");
    int st = 0;
    waitpid(pid, &st, 0);

    pt_regs_arm64 reg{}, bak{};
    getregs(pid, &reg);
    bak = reg;

    size_t sl = strlen(so) + 1;
    uint64_t sp = (reg.sp - 0x200) & ~0xFULL;
    poke(pid, sp, so, sl);

    // x0 = path, x1 = RTLD_NOW (2), lr = 0, pc = dlopen
    reg.regs[0] = sp;
    reg.regs[1] = 2;
    reg.regs[30] = 0;
    reg.sp = sp;
    reg.pc = dlopen_r;
    setregs(pid, &reg);

    if (ptrace(PTRACE_CONT, pid, nullptr, nullptr) < 0) die("CONT");
    waitpid(pid, &st, 0);

    getregs(pid, &reg);
    uint64_t handle = reg.regs[0];

    setregs(pid, &bak);
    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);

    if (!handle) {
        fprintf(stderr, "[-] dlopen NULL\n");
        return 1;
    }
    printf("[+] injected handle=0x%llx pid=%d\n", (unsigned long long)handle, (int)pid);
    return 0;
}
