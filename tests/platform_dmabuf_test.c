// Guest memory as a memfd's (runtime/platform/linux.c, NX2NAVI_DMABUF): a dma-buf of a watched range maps the very
// pages the CPU writes; a decommit inside it zeroes them in place (the dma-buf still sees the CPU's pages), one outside
// it reads zero when committed again; the write watch keeps working. No GPU, no game files. Build and run from the
// repository root:
//   clang -O2 -ffunction-sections -fdata-sections -Iruntime -c runtime/platform/linux.c -o /tmp/plat.o
//   clang -O2 -Wall -Iruntime tests/platform_dmabuf_test.c /tmp/plat.o -Wl,--gc-sections -lpthread -ldl -o /tmp/dmabuf
//   /tmp/dmabuf      (exit 77: no /dev/udmabuf or no write watch here)
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform/platform.h"

#define MiB (1ull << 20)

static uint64_t memfd_bytes(void) {   // the guest memfd's allocated bytes
    DIR *dir = opendir("/proc/self/fd");
    struct dirent *e;
    uint64_t got = 0;
    while (dir && (e = readdir(dir))) {
        char path[64], link[128];
        snprintf(path, sizeof path, "/proc/self/fd/%s", e->d_name);
        ssize_t k = readlink(path, link, sizeof link - 1);
        struct stat st;
        if (k > 0 && (link[k] = 0, strstr(link, "memfd:nx2navi-guest")) && !stat(path, &st)) got = (uint64_t)st.st_blocks * 512;
    }
    if (dir) closedir(dir);
    return got;
}

int main(void) {
    const uint64_t a = 0x6100000000ull, n = 64 * MiB;
    uint64_t pages[16], limit = 0, any = 0;
    const char *why = "";
    setenv("NX2NAVI_DMABUF", "1", 0);
    assert(plat_mem_reserve(a, n, 1));
    if (!plat_mem_dmabuf_ok(&limit, &any, &why)) { printf("SKIP: guest memory is not a memfd's here: %s\n", why); return 77; }
    assert(any == a);
    assert(plat_mem_commit(a, 16 * MiB));
    memset((void *)(uintptr_t)a, 0x11, 16 * MiB);
    for (uint64_t k = 16; k == 16;)   // read and reset until a call has fewer than it could take
        if ((k = plat_mem_written(a, n, pages, 16, 1)) == UINT64_MAX) { puts("SKIP: asynchronous write watch unavailable"); return 77; }
    assert(plat_mem_written(a, n, pages, 16, 0) == 0);

    // a dma-buf of the first 8 MiB maps the guest's own pages
    int fd = plat_mem_dmabuf(a, 8 * MiB);
    assert(fd >= 0);
    volatile uint8_t *d = mmap(NULL, 8 * MiB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(d != MAP_FAILED);
    assert(d[0] == 0x11 && d[8 * MiB - 1] == 0x11);
    ((volatile uint8_t *)(uintptr_t)a)[4096 + 5] = 0x22;
    assert(d[4096 + 5] == 0x22);
    d[3 * 4096] = 0x33;   // as a GPU write: the guest sees it, the write watch does not
    assert(((volatile uint8_t *)(uintptr_t)a)[3 * 4096] == 0x33);
    assert(plat_mem_written(a, n, pages, 16, 1) == 1 && pages[0] == a + 4096);

    // protection changes (a freeze, an armed lazy range) leave the dma-buf alone
    assert(plat_mem_protect(a, 2 * MiB, PLAT_PROT_READ));
    assert(d[4096 + 5] == 0x22);
    assert(plat_mem_protect(a, 2 * MiB, PLAT_PROT_RW));

    // a decommit inside the dma-buf: zero in place, the same pages for both
    assert(plat_mem_decommit(a + 4096, 4096));
    assert(plat_mem_commit(a + 4096, 4096));
    assert(((volatile uint8_t *)(uintptr_t)a)[4096 + 5] == 0 && d[4096 + 5] == 0);
    ((volatile uint8_t *)(uintptr_t)a)[4096 + 6] = 0x44;
    assert(d[4096 + 6] == 0x44);

    // a decommit that straddles the dma-buf's end: zero both sides when committed again
    assert(plat_mem_decommit(a + 8 * MiB - 4096, 2 * 4096));
    assert(plat_mem_commit(a + 8 * MiB - 4096, 2 * 4096));
    assert(((volatile uint8_t *)(uintptr_t)(a + 8 * MiB - 1))[0] == 0 && d[8 * MiB - 1] == 0);
    assert(((volatile uint8_t *)(uintptr_t)(a + 8 * MiB))[0] == 0);

    // a decommit outside any dma-buf: the pages leave the memfd and read zero
    assert(plat_mem_decommit(a + 12 * MiB, MiB));
    assert(plat_mem_commit(a + 12 * MiB, MiB));
    assert(((volatile uint8_t *)(uintptr_t)(a + 12 * MiB))[77] == 0);

    // a dma-buf over pages nothing committed yet allocates them; when it is gone they go back, the committed ones stay
    uint64_t before = memfd_bytes();
    int fd2 = plat_mem_dmabuf(a + 32 * MiB, 8 * MiB);
    assert(fd2 >= 0);
    uint64_t pinned = memfd_bytes();
    close(fd2);
    plat_mem_dmabuf_done(a + 32 * MiB, 8 * MiB);
    uint64_t after = memfd_bytes();
    printf("memfd: %.1f MiB, %.1f MiB with an 8 MiB dma-buf over uncommitted pages, %.1f MiB after it\n", before / 1048576.0,
           pinned / 1048576.0, after / 1048576.0);
    assert(pinned >= before + 8 * MiB && after == before);
    assert(plat_mem_commit(a + 32 * MiB, 4096));
    assert(((volatile uint8_t *)(uintptr_t)(a + 32 * MiB))[9] == 0);

    munmap((void *)d, 8 * MiB);
    close(fd);
    plat_mem_dmabuf_done(a, 8 * MiB);
    assert(((volatile uint8_t *)(uintptr_t)a)[4096 + 6] == 0x44 && ((volatile uint8_t *)(uintptr_t)a)[2 * MiB] == 0x11);

    // outside the reservation, or not whole pages: no dma-buf
    assert(plat_mem_dmabuf(a + n - 4096, 2 * 4096) < 0);
    assert(plat_mem_dmabuf(a + 1, 4096) < 0);
    *(volatile uint8_t *)(uintptr_t)(a + 5 * MiB) = 1;
    assert(plat_mem_written(a, n, pages, 16, 1) >= 1);
    puts("dma-buf of guest memory: same pages both ways, decommits zero in place inside and punch outside, write watch kept");
    return 0;
}
