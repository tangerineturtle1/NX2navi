// runtime/platform: the census of the process's handles by kind (platform.h plat_handle_census) sees the handles it is
// asked about. A census, then 20 events and 5 sections made, a census again (Event +20, Section +5, its total within 2
// of the process's handle count), the handles closed and a third census back at the first figures. Windows only: the
// Linux census groups file descriptors, which this check does not make. Built and run by tests/handle_census_check.py.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "platform/platform.h"

void rt_log(const char *fmt, ...) {   // the platform layer's lines
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#ifdef _WIN32
#include <windows.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t of(const PlatHandleKind *k, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(k[i].name, name)) return k[i].n;
    return 0;
}

static uint32_t total(const PlatHandleKind *k, int n) {
    uint32_t t = 0;
    for (int i = 0; i < n; i++) t += k[i].n;
    return t;
}

int main(void) {
    static PlatHandleKind a[64], b[64], c[64];
    int na = plat_handle_census(a, 64);
    CHECK(na > 0, "the first census found %d kinds", na);
    if (na <= 0) return 1;
    uint32_t count = plat_handle_count();
    CHECK(total(a, na) + 2 >= count && total(a, na) <= count + 2, "the census counted %u handles, the process holds %u",
          total(a, na), count);
    CHECK(of(a, na, "Event") > 0, "no Event handles named in the first census");
    HANDLE ev[20], sec[5];
    for (int i = 0; i < 20; i++) ev[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (int i = 0; i < 5; i++) sec[i] = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, NULL);
    int nb = plat_handle_census(b, 64);
    count = plat_handle_count();
    CHECK(nb > 0, "the second census found %d kinds", nb);
    CHECK(of(b, nb, "Event") == of(a, na, "Event") + 20, "Event %u after 20 made, %u before", of(b, nb, "Event"),
          of(a, na, "Event"));
    CHECK(of(b, nb, "Section") == of(a, na, "Section") + 5, "Section %u after 5 made, %u before", of(b, nb, "Section"),
          of(a, na, "Section"));
    CHECK(total(b, nb) + 2 >= count && total(b, nb) <= count + 2, "the census counted %u handles, the process holds %u",
          total(b, nb), count);
    for (int i = 0; i < 20; i++) CloseHandle(ev[i]);
    for (int i = 0; i < 5; i++) CloseHandle(sec[i]);
    int nc = plat_handle_census(c, 64);
    CHECK(of(c, nc, "Event") == of(a, na, "Event") && of(c, nc, "Section") == of(a, na, "Section"),
          "after closing: Event %u, Section %u; at first %u and %u", of(c, nc, "Event"), of(c, nc, "Section"),
          of(a, na, "Event"), of(a, na, "Section"));
    PlatHandleKind few[2];
    int nf = plat_handle_census(few, 2);
    CHECK(nf == 2 && !strcmp(few[1].name, "(more kinds)") && few[0].n + few[1].n == total(c, nc),
          "a census into 2 entries: %d kinds, the second '%s', %u of %u handles", nf, nf == 2 ? few[1].name : "",
          nf == 2 ? few[0].n + few[1].n : 0, total(c, nc));
    if (fails) return 1;
    printf("handle_census_check: all checks passed (%d kinds, %u handles)\n", nc, total(c, nc));
    return 0;
}
#else
int main(void) {
    printf("handle_census_check: skipped (Windows only)\n");
    return 0;
}
#endif
