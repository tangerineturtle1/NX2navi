// nx2navi.exe --probe-meshfb: the mesh shader fallback (mesh_fb.c, vk_mesh_fb.c, vk_exec_draw.c "mesh fallback").
//
// Draw part, no game files: two mesh programs of the runtime's own in the shape the translator gives one
// (shaders/meshfb_probe_*.mesh, .task), one without a task stage and one fed by a task program, drawn through the
// replay into an RGBA8 target with direct and with indirect workgroup counts, and both in one pass after each other.
// Each picture is drawn twice, through VK_EXT_mesh_shader and through the fallback, and the two are compared texel by
// texel; without mesh shaders on the device only the fallback draws, and the probe says that nothing was compared.
//
// Module part, with NX2NAVI_PROBE_MESHFB_DIR=<dir>: every "<name>" of <dir>/list.txt names <dir>/<name>.in (a stored
// shader input, gfx_shader.c in_store: control section and code) and <dir>/<name>.spv (its translation). The compute
// module and, for a mesh program, a vertex module are written to <dir>/out/<name>.cs.spv and .vs.spv for spirv-val
// (work the caller does; tests of game shaders stay outside the repository). Where <dir>/<name>.res.json (the
// translation's reflection) is there too, the compute pipeline of the module is built on this device, as its first
// draw would build it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "mesh_fb.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_mesh_fb.h"
#include "platform/platform.h"

#include "meshfb_probe_f_spv.h"
#include "meshfb_probe_m_spv.h"
#include "meshfb_probe_t_spv.h"
#include "meshfb_probe_tm_spv.h"

#define TEX_W 256
#define TEX_H 144
#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("probe-meshfb: FAILED: ");                                                                          \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

void vk_exec_probe_shader(const uint8_t *ctl, const GfxShader *s);   // vk_exec_draw.c
void vk_exec_draw_shutdown(void);
void vk_exec_xfer_shutdown(void);

// ---- module part

static uint8_t *read_all(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    if (b) b[n] = 0, *size = (size_t)n;
    return b;
}

static int write_all(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

static int modules_part(const char *dir) {
    char path[1024];
    size_t list_n = 0;
    snprintf(path, sizeof path, "%s/list.txt", dir);
    char *list = (char *)read_all(path, &list_n);
    CHECK(list, "no %s", path);
    int n_task = 0, n_mesh = 0, n_fail = 0, n_pipe = 0, n_pipe_fail = 0;
    double pipe_ms = 0, pipe_worst = 0;
    uint64_t pipe_worst_words = 0;
    const GfxBackend *b = &gfx_vulkan_backend;
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22};
    int device = 0;
    for (char *name = strtok(list, "\r\n"); name; name = strtok(NULL, "\r\n")) {
        size_t in_n = 0, spv_n = 0;
        snprintf(path, sizeof path, "%s/%s.in", dir, name);
        uint8_t *in = read_all(path, &in_n);
        snprintf(path, sizeof path, "%s/%s.spv", dir, name);
        uint8_t *spv = read_all(path, &spv_n);
        uint32_t h[6];   // magic, version, stage, code length, c1 length, size of the answers (gfx_shader.c InHeader)
        if (!in || !spv || in_n < sizeof h) {
            printf("probe-meshfb: %s: input or translation missing\n", name), n_fail++;
            free(in), free(spv);
            continue;
        }
        memcpy(h, in, sizeof h);
        const uint8_t *control = in + sizeof h + h[5], *code = control + 0x880;
        int stage = (int)h[2];   // GFX_STAGE_TS / GFX_STAGE_MS are the NVN stage numbers 6 and 7
        char err[200] = "";
        MeshFbInfo m;
        uint64_t on = 0, vn = 0;
        uint32_t *cs = NULL, *vs = NULL;
        if (in_n < sizeof h + h[5] + 0x880 + 0x80 || (stage != 6 && stage != 7)) snprintf(err, sizeof err, "not a task or mesh input");
        else if (meshfb_info(control, code, stage, &m, err, sizeof err)) {
            uint32_t top = 0;
            const uint32_t *w = (const uint32_t *)spv;
            for (size_t i = 5; i < spv_n / 4;) {
                uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
                if (!len) break;
                if (op == 71 && len == 4 && w[i + 2] == 33 && w[i + 3] > top) top = w[i + 3];
                i += len;
            }
            cs = stage == 6 ? meshfb_task_cs(w, spv_n / 4, &m, top + 1, &on, err, sizeof err)
                            : meshfb_mesh_cs(w, spv_n / 4, &m, top + 1, m.payload_bytes / 4, &on, err, sizeof err);
            if (cs && stage == 7) {
                uint8_t type[32] = {0};
                uint32_t mask = 0;
                for (uint32_t l = 0; l < m.nlines; l++)
                    if (m.lines[l] >= 0x80) mask |= 1u << (m.lines[l] - 0x80) / 16;
                for (int g = 0; g < 32; g++) type[g] = (uint8_t)(g % 3);   // every output type gets made somewhere
                vs = meshfb_vs(&m, mask, type, &vn);
                if (!vs) snprintf(err, sizeof err, "no vertex module");
            }
        }
        if (cs && (stage == 6 || vs)) {
            snprintf(path, sizeof path, "%s/out/%s.cs.spv", dir, name);
            int ok = write_all(path, cs, on * 4);
            if (vs) snprintf(path, sizeof path, "%s/out/%s.vs.spv", dir, name), ok = ok && write_all(path, vs, vn * 4);
            if (!ok) printf("probe-meshfb: %s: cannot write under %s/out\n", name, dir), n_fail++;
            else if (stage == 6) n_task++;
            else n_mesh++;
            size_t res_n = 0;
            snprintf(path, sizeof path, "%s/%s.res.json", dir, name);
            char *res = (char *)read_all(path, &res_n);
            if (res) {   // the pipeline, with the layout the reflection gives
                if (!device) {   // the first pool brings the device up
                    uint64_t heap = 0;
                    CHECK(mem_set_heap_size(16ull << 20, &heap) == 0 && heap, "SetHeapSize 16 MiB");
                    pool.mem = pool.game_mem = (uint8_t *)(uintptr_t)heap + 0x100000, pool.size = 0x100000;
                    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool");
                    device = 1;
                }
                uint32_t top = 0;
                const uint32_t *w = (const uint32_t *)spv;
                for (size_t i = 5; i < spv_n / 4;) {
                    uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
                    if (!len) break;
                    if (op == 71 && len == 4 && w[i + 2] == 33 && w[i + 3] > top) top = w[i + 3];
                    i += len;
                }
                static const int task_slots[1] = {MESHFB_CB_TASK}, mesh_slots[3] = {MESHFB_CB_TASK, MESHFB_CB_MESH, MESHFB_CB_LIST};
                char *cres = stage == 6 ? meshfb_res_json(res, top + 1, task_slots, 1)
                           : m.has_task ? meshfb_res_json(res, top + 1, mesh_slots, 3) : meshfb_res_json(res, top + 2, mesh_slots + 1, 1);
                const GfxShader *sh = cres ? gfx_shader_own(cs, on, cres) : NULL;
                const VkPipeInfo *info = NULL;
                uint64_t t0 = plat_now_ns();
                VkPipeline p = sh ? vk_pipeline_compute(sh, &info) : VK_NULL_HANDLE;
                double ms = (double)(plat_now_ns() - t0) / 1e6;
                if (p && info) {
                    n_pipe++, pipe_ms += ms;
                    if (ms > pipe_worst) pipe_worst = ms, pipe_worst_words = on;
                } else {
                    n_pipe_fail++;
                    printf("probe-meshfb: %s (stage %d, %llu words): %s\n", name, stage, (unsigned long long)on,
                           sh ? "its compute pipeline was not built" : "its reflection could not be made");
                }
                fflush(stdout);
                free(cres), free(res);
            }
        } else {
            printf("probe-meshfb: %s (stage %d): %s\n", name, stage, err), n_fail++;
        }
        free(cs), free(vs), free(in), free(spv);
    }
    free(list);
    printf("probe-meshfb: module part: %d task and %d mesh programs converted, %d failed\n", n_task, n_mesh, n_fail);
    if (device) {
        printf("probe-meshfb: module part: %d compute pipelines built on this device (%.0f ms each, the longest %.0f ms for %llu "
               "words), %d not built\n", n_pipe, n_pipe ? pipe_ms / n_pipe : 0.0, pipe_worst, (unsigned long long)pipe_worst_words,
               n_pipe_fail);
        b->pool_fini(&pool);
        b->submit(NULL, 0);   // the pool's buffers go when a submission after their release has completed
        b->finish();
        vk_pipeline_shutdown();
        vk_exec_draw_shutdown();
        vk_exec_xfer_shutdown();
        vk_backend_shutdown();
    }
    return n_fail != 0 || n_pipe_fail != 0;
}

// ---- draw part

static uint8_t *put(uint8_t *p, uint32_t op, const void *payload, uint32_t n) {
    op_hdr h = {op, (uint32_t)((sizeof h + n + 7) & ~7u)};
    memset(p, 0, h.size);
    memcpy(p, &h, sizeof h);
    if (n) memcpy(p + sizeof h, payload, n);
    return p + h.size;
}

// An NVN2 control section of a task (6), mesh (7) or fragment (1) stage as vk_exec_draw.c and mesh_fb.c read it, and
// a code block whose header maps the outputs: position and generic vector 0.
static void fake_stage(NVNshaderData *sd, uint32_t stage, int fed_by_task) {
    uint8_t *ctl = calloc(1, 0x880), *code = calloc(1, 0x100);
    uint32_t magic = 0x98761234u, len = 0x100, st = stage << 16;
    uint32_t w58 = 32u | 1u << 24;                           // 32 threads, a payload of one 256-byte unit
    uint32_t w5c = 3u << 24 | (fed_by_task ? 1u << 16 : 0);   // triangles
    uint32_t w60 = 64u | 32u << 16;                           // 64 vertices, 32 primitives
    memcpy(ctl, &magic, 4), memcpy(ctl + 0x38, &len, 4), memcpy(ctl + 0x54, &st, 4);
    if (stage >= 6) memcpy(ctl + 0x58, &w58, 4);
    if (stage == 7) memcpy(ctl + 0x5c, &w5c, 4), memcpy(ctl + 0x60, &w60, 4), code[0x35] = 0xf0, code[0x36] = 0x0f;
    sd->control = ctl, sd->data = (uint64_t)(uintptr_t)code;
}

static const char RES_MS[] = "{\"key\":\"probe\",\"stage\":\"ms\",\"notes\":[],\"cbufs\":[{\"binding\":1,\"slot\":3}],"
                             "\"sbufs\":[],\"textures\":[],\"images\":[]}";
static const char RES_TS[] = "{\"key\":\"probe\",\"stage\":\"ts\",\"notes\":[],\"cbufs\":[{\"binding\":1,\"slot\":3}],"
                             "\"sbufs\":[],\"textures\":[],\"images\":[]}";
static const char RES_FS[] = "{\"key\":\"probe\",\"stage\":\"ps\",\"notes\":[],\"cbufs\":[],\"sbufs\":[],\"textures\":[],"
                             "\"images\":[]}";

// The program of a mesh stage, its task stage if any, and the fragment stage.
static prog_t *make_prog(const GfxShader *ms, const GfxShader *ts, const GfxShader *fs) {
    static uint64_t serial = 0x9000;
    prog_t *p = calloc(1, sizeof *p);
    p->magic = NVN_PROG_MAGIC, p->serial = ++serial;
    fake_stage(&p->sd[p->nstage], 7, ts != NULL), vk_exec_probe_shader(p->sd[p->nstage++].control, ms);
    if (ts) fake_stage(&p->sd[p->nstage], 6, 0), vk_exec_probe_shader(p->sd[p->nstage++].control, ts);
    fake_stage(&p->sd[p->nstage], 1, 0), vk_exec_probe_shader(p->sd[p->nstage++].control, fs);
    return p;
}

typedef struct { prog_t *prog; uint32_t count; int indirect; uint32_t grid[2]; float tint[4]; } MeshDraw;

// Uniform data in the pool, 0x1000 apart per draw: the mesh stage's block at 0x90000, the task stage's at 0x98000; the
// indirect records at 0xa0000.
static int draw_picture(const GfxBackend *b, pool_t *pool, tex_t *color, const MeshDraw *d, int n, uint32_t *px) {
    static _Alignas(8) uint8_t stream[16384];
    uint8_t *q = put(stream, OP_BEGIN, NULL, 0);
    p_set_rt rt;
    memset(&rt, 0, sizeof rt);
    rt.n = 1, rt.color[0] = color;
    q = put(q, OP_SET_RT, &rt, sizeof rt);
    const p_clear_color c0 = {0, 0xf, {0.2f, 0.4f, 0.6f, 1.0f}};
    q = put(q, OP_CLEAR_COLOR, &c0, sizeof c0);
    p_viewports vp;
    memset(&vp, 0, sizeof vp);
    vp.n = 1, vp.v[0][2] = TEX_W, vp.v[0][3] = TEX_H, vp.v[0][5] = 1.0f;
    q = put(q, OP_VIEWPORTS, &vp, sizeof vp);
    const p_scissor full = {0, 0, 1 << 20, 1 << 20};
    q = put(q, OP_SCISSOR, &full, sizeof full);
    for (int i = 0; i < n; i++) {
        uint8_t *mu = pool->mem + 0x90000 + 0x1000 * i, *tu = pool->mem + 0x98000 + 0x1000 * i;
        uint32_t *rec = (uint32_t *)(pool->mem + 0xa0000 + 0x100 * i);
        uint32_t grid[4] = {d[i].grid[0], d[i].grid[1], 0, 0}, shift[4] = {1, 0, 0, 0};
        memcpy(mu, d[i].tint, 16), memcpy(mu + 16, grid, 16), memcpy(tu, shift, 16);
        rec[0] = d[i].count, rec[1] = 0, rec[2] = 0;
        const p_bind_program bp = {d[i].prog, 0xc2};
        q = put(q, OP_BIND_PROGRAM, &bp, sizeof bp);
        const p_bind_buf bm = {7, 0, (uint64_t)(uintptr_t)mu, 256}, bt = {6, 0, (uint64_t)(uintptr_t)tu, 256};
        q = put(q, OP_BIND_UBO, &bm, sizeof bm);
        q = put(q, OP_BIND_UBO, &bt, sizeof bt);
        if (d[i].indirect) {
            const p_draw_mesh_ind di = {(uint64_t)(uintptr_t)rec, 0, 0, 0, 0};
            q = put(q, OP_DRAW_MESH_IND, &di, sizeof di);
        } else {
            const p_draw_mesh dm = {0, d[i].count, 0};
            q = put(q, OP_DRAW_MESH, &dm, sizeof dm);
        }
    }
    q = put(q, OP_END, NULL, 0);
    CHECK(q <= stream + sizeof stream, "op stream overflow");
    uint64_t handle = (uint64_t)(uintptr_t)stream;
    uint64_t f = b->submit(&handle, 1);
    CHECK(b->fence_wait(f, 60000000000ull), "fence_wait of a mesh draw submission timed out");
    NVNcopyRegion r = {0, 0, 0, TEX_W, TEX_H, 1};
    b->read_texels(color, NULL, &r, (uint8_t *)px, TEX_W * 4, TEX_W * TEX_H * 4);
    return 0;
}

static int drawn_texels(const uint32_t *px) {
    int n = 0;
    for (int i = 0; i < TEX_W * TEX_H; i++) n += px[i] != px[0];   // texel 0 is in the margin no cell covers
    return n;
}

// Texels of a and b that differ, and the largest difference of a channel.
static int differing(const uint32_t *a, const uint32_t *b, int *worst) {
    int n = 0;
    *worst = 0;
    for (int i = 0; i < TEX_W * TEX_H; i++) {
        if (a[i] == b[i]) continue;
        n++;
        for (int c = 0; c < 4; c++) {
            int d = (int)(a[i] >> 8 * c & 0xff) - (int)(b[i] >> 8 * c & 0xff);
            if (d < 0) d = -d;
            if (d > *worst) *worst = d;
        }
    }
    return n;
}

static int draw_part(const GfxBackend *b, uint8_t *guest) {
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0x100000, .size = 0xC0000,
                   .game_mem = guest + 0x100000};
    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool");
    VkCtx *c = vk_ctx();
    CHECK(c, "no Vulkan device");
    static tex_t color;
    memset(&color, 0, sizeof color);
    color.magic = NVN_TEX_MAGIC;
    color.d.target = NVN_TEXTURE_TARGET_2D, color.d.format = NVN_FORMAT_RGBA8, color.d.width = TEX_W, color.d.height = TEX_H;
    color.d.depth = 1, color.d.levels = 1;
    nvn_tex_layout(&color.d, &color.L);
    CHECK(color.L.size <= 0x80000, "the target exceeds its pool slot");
    color.pool = &pool, color.pool_flags = (uint16_t)pool.flags, color.off = 0, color.mem = pool.mem, color.serial = 4000;

    const GfxShader *fs = gfx_shader_own(meshfb_probe_f_spv, sizeof meshfb_probe_f_spv / 4, RES_FS);
    const GfxShader *ms = gfx_shader_own(meshfb_probe_m_spv, sizeof meshfb_probe_m_spv / 4, RES_MS);
    const GfxShader *ts = gfx_shader_own(meshfb_probe_t_spv, sizeof meshfb_probe_t_spv / 4, RES_TS);
    const GfxShader *tm = gfx_shader_own(meshfb_probe_tm_spv, sizeof meshfb_probe_tm_spv / 4, RES_MS);
    CHECK(fs && ms && ts && tm, "a probe shader is malformed");
    prog_t *plain = make_prog(ms, NULL, fs), *tasked = make_prog(tm, ts, fs);

    // 24 cells of a 6 x 4 grid by 24 mesh workgroups; 40 cells of an 8 x 5 grid by 10 task workgroups that start 0 to
    // 4 mesh workgroups each
    const MeshDraw cases[][2] = {
        {{plain, 24, 0, {6, 4}, {1, 1, 1, 1}}},
        {{plain, 24, 1, {6, 4}, {1, 1, 1, 1}}},
        {{tasked, 10, 0, {8, 5}, {1, 1, 1, 1}}},
        {{tasked, 10, 1, {8, 5}, {1, 1, 1, 1}}},
        {{plain, 24, 1, {6, 4}, {1, 1, 1, 1}}, {tasked, 10, 1, {8, 5}, {0.5f, 1, 0.25f, 1}}},
        {{tasked, 10, 0, {8, 5}, {1, 1, 1, 1}}, {plain, 24, 0, {6, 4}, {1, 0.5f, 0.5f, 1}}},
    };
    static const char *const names[] = {"mesh, direct", "mesh, indirect", "task + mesh, direct", "task + mesh, indirect",
                                        "mesh then task + mesh in one pass, indirect", "task + mesh then mesh in one pass, direct"};
    enum { CASES = sizeof cases / sizeof cases[0] };
    static uint32_t px[2][CASES][TEX_W * TEX_H];
    int native = c->has_mesh_shader, was = c->mesh_fallback;
    for (int way = native ? 0 : 1; way < 2; way++) {
        c->mesh_fallback = way;
        for (int k = 0; k < CASES; k++)
            if (draw_picture(b, &pool, &color, cases[k], cases[k][1].prog ? 2 : 1, px[way][k])) return 1;
    }
    c->mesh_fallback = was;
    for (int k = 0; k < CASES; k++) {
        int fb = drawn_texels(px[1][k]);
        CHECK(fb > TEX_W * TEX_H / 20, "%s: the fallback drew %d texels", names[k], fb);
        if (!native) {
            printf("probe-meshfb: %s: the fallback drew %d of %d texels (no mesh shaders here: not compared)\n", names[k], fb,
                   TEX_W * TEX_H);
            continue;
        }
        int worst, diff = differing(px[0][k], px[1][k], &worst), nat = drawn_texels(px[0][k]);
        CHECK(nat > TEX_W * TEX_H / 20, "%s: the mesh pipeline drew %d texels", names[k], nat);
        CHECK(worst <= 1, "%s: %d texels differ between the mesh pipeline and the fallback, by up to %d of 255 (the mesh "
              "pipeline drew %d texels, the fallback %d)", names[k], diff, worst, nat, fb);
        printf("probe-meshfb: %s: %d texels drawn by the mesh pipeline, %d by the fallback, %d differ (by %d of 255 at most)\n",
               names[k], nat, fb, diff, worst);
    }
    int w01, w23;
    CHECK(!differing(px[1][0], px[1][1], &w01) && !differing(px[1][2], px[1][3], &w23),
          "the fallback's direct and indirect pictures differ");
    CHECK(!vk_meshfb_short(), "%llu of the draws above did not fit the fallback's buffers", (unsigned long long)vk_meshfb_short());
    // More workgroups than the mesh buffer has slots for: what fits is drawn, the rest is dropped and counted. The
    // cells past the grid are outside the picture, so it is the 24-cell picture again.
    uint32_t slots = (uint32_t)(vk_meshfb_mesh_words() / 540), over = slots + 1000;
    const MeshDraw big = {plain, over, 1, {6, 4}, {1, 1, 1, 1}};
    static uint32_t bpx[TEX_W * TEX_H];
    c->mesh_fallback = 1;
    int bad = draw_picture(b, &pool, &color, &big, 1, bpx);
    c->mesh_fallback = was;
    if (bad) return 1;
    b->finish();
    int wb, db = differing(bpx, px[1][0], &wb);
    CHECK(!db, "a draw of %u workgroups, %u more than the mesh buffer holds: %d texels differ from the 24-cell picture", over,
          over - slots, db);
    CHECK(vk_meshfb_short() == 1, "a draw of %u workgroups with room for %u was counted as %llu draws that did not fit", over, slots,
          (unsigned long long)vk_meshfb_short());
    printf("probe-meshfb: a draw of %u workgroups with room for %u: the picture of the first 24 is whole, the draw was counted\n",
           over, slots);
    // As the game runs it: pipelines built in the background, and a program's modules made on the fallback's own
    // thread. New shader objects of the same programs are drawn until the picture is whole: the draws before that are
    // skipped, and none may leave a partial picture behind for good.
    const GfxShader *fs2 = gfx_shader_own(meshfb_probe_f_spv, sizeof meshfb_probe_f_spv / 4, RES_FS);
    const GfxShader *ms2 = gfx_shader_own(meshfb_probe_m_spv, sizeof meshfb_probe_m_spv / 4, RES_MS);
    const GfxShader *ts2 = gfx_shader_own(meshfb_probe_t_spv, sizeof meshfb_probe_t_spv / 4, RES_TS);
    const GfxShader *tm2 = gfx_shader_own(meshfb_probe_tm_spv, sizeof meshfb_probe_tm_spv / 4, RES_MS);
    CHECK(fs2 && ms2 && ts2 && tm2, "a probe shader is malformed");
    prog_t *plain2 = make_prog(ms2, NULL, fs2), *tasked2 = make_prog(tm2, ts2, fs2);
    const MeshDraw again[2] = {{tasked2, 10, 0, {8, 5}, {1, 1, 1, 1}}, {plain2, 24, 0, {6, 4}, {1, 0.5f, 0.5f, 1}}};
    vk_pipeline_async_enable();
    c->mesh_fallback = 1;
    int tries = 0, left = -1, wa;
    while (left && tries < 500) {
        if (tries++) plat_sleep_ms(10);
        if (draw_picture(b, &pool, &color, again, 2, bpx)) return 1;
        left = differing(bpx, px[1][5], &wa);
    }
    c->mesh_fallback = was;
    CHECK(!left, "with background builds the picture still differs in %d texels after %d submissions", left, tries);
    printf("probe-meshfb: with background builds the picture was whole at submission %d (the earlier ones skipped their draws)\n",
           tries);
    b->texture_release(&color);
    b->pool_fini(&pool);
    return 0;
}

int probe_meshfb(void) {
    const char *dir = cfg_get("NX2NAVI_PROBE_MESHFB_DIR");
    if (dir && *dir) return modules_part(dir);
    vk_hdr_off();
    const GfxBackend *b = &gfx_vulkan_backend;
    uint64_t heap = 0;
    CHECK(mem_set_heap_size(16ull << 20, &heap) == 0 && heap, "SetHeapSize 16 MiB");
    int bad = draw_part(b, (uint8_t *)(uintptr_t)heap);
    b->finish();
    vk_pipeline_shutdown();
    vk_exec_draw_shutdown();
    vk_exec_xfer_shutdown();
    vk_backend_shutdown();
    long errors = vk_validation_errors_total();
    if (bad || errors) {
        printf("probe-meshfb: FAILED (%s, %ld validation errors)\n", bad ? "a draw check failed" : "checks passed", errors);
        return 1;
    }
    printf("probe-meshfb: passed (0 validation errors)\n");
    return 0;
}
