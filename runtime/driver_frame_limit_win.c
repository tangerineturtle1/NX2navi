// NVIDIA's Max Frame Rate, read (never written) from the driver's settings through NVAPI's settings session
// (driver_frame_limit.h). nvapi64.dll ships with NVIDIA's driver; without it the limit is 0.
#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "driver_frame_limit.h"
#include "host.h"

#pragma pack(push, 4)
typedef struct {   // NVDRS_SETTING version 1
    uint32_t version;
    uint16_t name[2048];
    uint32_t id, type, location, is_current_predefined, is_predefined_valid;
    uint8_t predefined[4100];
    uint8_t current[4100];
} NvSetting;
typedef struct {   // NVDRS_APPLICATION, large enough for its versions 1 to 4
    uint32_t version;
    uint8_t rest[4 + 5 * 4096];
} NvApp;
#pragma pack(pop)

typedef void *(*nv_query)(uint32_t);

int driver_frame_limit(void) {
    static int limit = -1;
    if (limit >= 0) return limit;
    limit = 0;
    HMODULE m = LoadLibraryW(L"nvapi64.dll");
    nv_query qi = m ? (nv_query)(void *)GetProcAddress(m, "nvapi_QueryInterface") : NULL;
    if (!qi) return 0;
    int (*init)(void) = (int (*)(void))qi(0x0150E828);
    int (*create)(void **) = (int (*)(void **))qi(0x0694D52E);
    int (*load)(void *) = (int (*)(void *))qi(0x375DBD6B);
    int (*global)(void *, void **) = (int (*)(void *, void **))qi(0x617BFF9F);
    int (*find)(void *, const uint16_t *, void **, void *) = (int (*)(void *, const uint16_t *, void **, void *))qi(0xEEE566B2);
    int (*get)(void *, void *, uint32_t, void *) = (int (*)(void *, void *, uint32_t, void *))qi(0x73BF8338);
    int (*destroy)(void *) = (int (*)(void *))qi(0xDAD9CFF8);
    void *s = NULL, *p = NULL;
    if (!init || !create || !load || !global || !get || init() || create(&s) || !s) return 0;
    static NvSetting st;
    const uint32_t FRL = 0x10835002;   // FRL_FPS: Max Frame Rate
    int found = 0;
    if (!load(s)) {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
        const wchar_t *name = path;
        for (DWORD i = 0; i < n; i++)
            if (path[i] == 92 || path[i] == L'/') name = path + i + 1;   // 92: the backslash
        static NvApp app;
        static const uint32_t ver[4][2] = {{4 + 4 + 5 * 4096, 4}, {4 + 4 + 4 * 4096, 3}, {4 + 4 + 4 * 4096, 2}, {4 + 4 + 3 * 4096, 1}};
        int r = -1;
        for (int i = 0; i < 4 && r && find && n; i++) {
            memset(&app, 0, sizeof app);
            app.version = ver[i][0] | (ver[i][1] << 16);
            r = find(s, (const uint16_t *)name, &p, &app);
        }
        memset(&st, 0, sizeof st);
        st.version = (uint32_t)sizeof st | (1u << 16);
        if (!r && p && !get(s, p, FRL, &st)) found = 1;
        if (!found && !global(s, &p)) {
            memset(&st, 0, sizeof st);
            st.version = (uint32_t)sizeof st | (1u << 16);
            if (!get(s, p, FRL, &st)) found = 2;
        }
    }
    if (destroy) destroy(s);
    uint32_t v = 0;
    if (found) memcpy(&v, st.current, sizeof v);
    limit = v > 0 && v < 1000 ? (int)v : 0;
    if (limit)
        rt_log("frame limit: NVIDIA's driver holds this game to %d frames a second (Max Frame Rate, %s profile)", limit,
               found == 1 ? "the game's" : "the global");
    return limit;
}
