// The stutter map (NX2NAVI_STUTTER_MAP=1): what a play session's hitches were, area by area, and which pipelines and
// shaders the draws met missing, so a long session says where the remaining stutters are and what the next pipeline
// list (tools/pipeline_list.py merge) and shader preparation must take in. Off by default; while it is off every hook
// returns at its first test.
//
// A hitch is a frame that stands out from the frames around it: its interval is over 2.5 times the median of the 32
// frames before it and at least 12 ms (or over 25 ms), and both of the next two frames are under two thirds of it
// (a frame over SM_HITCH_SURE_NS is one whatever follows). At 140 fps that is a frame over 17.5 ms followed by shorter
// ones. The look at the next two frames keeps a change of pace out: the game drawing its menus or a scene at a steady
// 60 or 30 fps after 140 is a slower rate, not a run of hitches, and the median takes the new rate within 16 frames.
// The decision waits two presents; the line and the counts carry what the frame itself waited for. Frames within
// SM_LOAD_NS of an area's load count apart, as the load's: an area load shows as a few long frames that are not a
// stutter in play.
//
// What a hitch went to comes from the frame-time summary's split (perf.c ft_frame: pipeline, translation, texture,
// fence, pacing, replay, unattributed), with the draws that met a pipeline or a shader missing between the previous
// present and this one (stutter_miss, from the replay thread). Each hitch gets a "stutter:" line, at most
// SM_LINES_PER_S a second and SM_LINES_MAX in all; the counts go on past that. Every SM_REPORT_NS of play and at exit,
// "stutter map" lines rank the areas by their hitches and the pipelines by the time draws lost to them, each with
// whether this install had recorded it before (the pipeline list or an earlier run) and whether one of its shaders was
// translated in play rather than prepared.
//
// Threads: stutter_area runs on the game's loading threads, stutter_miss on the replay thread, stutter_ready on the
// pipeline workers (under vk_pipeline.c's P.lock; sm_lock is taken inside it and never takes it), stutter_frame and
// the periodic report on the present thread. The area table's play counts belong to the present thread; the rest is
// under sm_lock.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "stutter_map.h"
#include "platform/platform.h"

int stutter_map_on;

#define SM_AREAS 128                       // areas kept by name; later ones share the last slot
#define SM_CAUSES 8
#define SM_LOAD_NS 3000000000ull           // after an area load, frames count as the load's for this long
#define SM_HITCH_MIN_NS 12000000.0         // a hitch is at least this long...
#define SM_HITCH_ANY_NS 25000000ull        // ...and any frame over this is one
#define SM_HITCH_FACTOR 2.5                // times the median of the frames before it
#define SM_HITCH_SURE_NS 100000000ull      // a frame this long is a hitch whatever follows it
#define SM_MEDIAN_N 32                     // frames the median is taken over
#define SM_AFTER 2                         // frames after a candidate that must be shorter...
#define SM_AFTER_RATIO 1.5                 // ...by this factor
#define SM_PIPES 4096                      // pipelines kept for the ranking (open addressing)
#define SM_RING 256                        // draws that met something missing, between two presents
#define SM_LINES_MAX 4000
#define SM_LINES_PER_S 5
#define SM_REPORT_NS 600000000000ull       // the map every 10 minutes of play
#define SM_TOP_AREAS 40
#define SM_TOP_PIPES 40

typedef struct {
    char name[64];
    uint64_t ns, frames, over25;                        // present thread
    uint64_t hitch, load_hitch, by_cause[SM_CAUSES], cause_ns[SM_CAUSES], worst_ns, worst_frame;
    uint64_t misses, shader_misses, miss_ns;            // draws that met a pipeline / a shader missing
} Area;

typedef struct {
    uint64_t key;          // vertex (or mesh) and fragment input ids' first four bytes each; 0: not known
    uint8_t used, mesh, recorded, fresh;
    uint16_t area;         // where draws first met it
    uint64_t misses, shader_misses, wait_ns, ready_n;
    double ready_max, ready_sum;
} Pipe;

typedef struct {
    uint64_t key, ns;
    uint8_t shader, mesh, skipped, has_v, has_f;
} Miss;

static PlatMutex sm_lock = PLAT_MUTEX_INIT;
static Area areas[SM_AREAS];
static int n_areas = 1;              // areas[0]: from the start to the first area load
static volatile int cur_area;
static volatile uint64_t load_at;    // kern_now_ns of the last area load
static Pipe pipes[SM_PIPES];
static uint32_t n_pipes, pipes_lost;
static Miss ring[SM_RING];
static volatile uint64_t ring_head;  // sm_lock: draws noted so far
static uint64_t ring_tail;           // present thread: draws taken so far
static uint64_t ring_lost;

// present thread
static uint64_t recent[SM_MEDIAN_N];   // the last frames' intervals, every frame (hitches too, so a new pace is learnt)
static uint64_t t_first, t_report, line_sec, lines_in_sec, lines, frames_seen, paced_out;
// A frame that may be a hitch, waiting for the SM_AFTER frames after it: its line (made when it happened, with the
// draws that met something missing then), its area and what it is charged to.
typedef struct {
    char line[1400];
    uint64_t frame, d, after_max;
    int area, load, cause, after;
} Cand;
static Cand cand[SM_AFTER + 1];
static int n_cand;

void stutter_map_init(void) {
    stutter_map_on = cfg_bool("NX2NAVI_STUTTER_MAP");
    if (!stutter_map_on) return;
    snprintf(areas[0].name, sizeof areas[0].name, "%s", "(before the first area)");
    rt_log("stutter map: on (NX2NAVI_STUTTER_MAP): a hitch is a frame over %.1f times the median of the %d before it "
           "and at least %.0f ms (or over %llu ms) whose next %d frames are under 1/%.1f of it (or one over %llu ms); "
           "frames within %llu s of an area load count as the load's; the map every %llu minutes and at exit",
           SM_HITCH_FACTOR, SM_MEDIAN_N, SM_HITCH_MIN_NS / 1e6, SM_HITCH_ANY_NS / 1000000ull, SM_AFTER, SM_AFTER_RATIO,
           SM_HITCH_SURE_NS / 1000000ull, SM_LOAD_NS / 1000000000ull, SM_REPORT_NS / 60000000000ull);
}

static uint32_t id4(const uint8_t *id) {
    return id ? (uint32_t)id[0] << 24 | (uint32_t)id[1] << 16 | (uint32_t)id[2] << 8 | id[3] : 0;
}

static uint64_t key_of(const uint8_t *vi, const uint8_t *fi) { return (uint64_t)id4(vi) << 32 | id4(fi); }

void stutter_area(const char *name) {
    if (!stutter_map_on || !name || !*name) return;
    plat_mutex_lock(&sm_lock);
    int i = 0;
    while (i < n_areas && strcmp(areas[i].name, name)) i++;
    if (i == n_areas) {
        if (n_areas < SM_AREAS) snprintf(areas[n_areas++].name, sizeof areas[0].name, "%s", name);
        else {
            i = SM_AREAS - 1;
            snprintf(areas[i].name, sizeof areas[i].name, "%s", "(more areas)");
        }
    }
    __atomic_store_n(&cur_area, i, __ATOMIC_RELAXED);
    __atomic_store_n(&load_at, kern_now_ns(), __ATOMIC_RELAXED);
    plat_mutex_unlock(&sm_lock);
}

// sm_lock held. The entry for key (mesh apart), made on first use; NULL once the table is full.
static Pipe *pipe_at(uint64_t key, int mesh) {
    uint64_t h = (key ^ (uint64_t)mesh) * 0x9E3779B97F4A7C15ull;
    for (uint32_t k = 0; k < 64; k++) {
        Pipe *p = &pipes[((h >> 52) + k) & (SM_PIPES - 1)];
        if (p->used && p->key == key && p->mesh == mesh) return p;
        if (!p->used) {
            if (n_pipes >= SM_PIPES * 3 / 4) break;
            memset(p, 0, sizeof *p);
            p->used = 1, p->key = key, p->mesh = (uint8_t)mesh;
            p->area = (uint16_t)__atomic_load_n(&cur_area, __ATOMIC_RELAXED);
            n_pipes++;
            return p;
        }
    }
    pipes_lost++;
    return NULL;
}

void stutter_miss(int shader, int mesh, const uint8_t *vi, const uint8_t *fi, uint64_t ns, int skipped) {
    if (!stutter_map_on) return;
    uint64_t key = key_of(vi, fi);
    plat_mutex_lock(&sm_lock);
    uint64_t h = ring_head;
    if (h - ring_tail >= SM_RING) ring_lost++;   // the present has not taken the oldest yet: it is overwritten
    Miss *m = &ring[h % SM_RING];
    m->key = key, m->ns = ns, m->shader = (uint8_t)(shader != 0), m->mesh = (uint8_t)(mesh != 0);
    m->skipped = (uint8_t)(skipped != 0), m->has_v = vi != NULL, m->has_f = fi != NULL;
    __atomic_store_n(&ring_head, h + 1, __ATOMIC_RELEASE);
    Pipe *p = pipe_at(key, mesh != 0);
    if (p) {
        if (shader) p->shader_misses++;
        else p->misses++;
        p->wait_ns += ns;
    }
    plat_mutex_unlock(&sm_lock);
}

void stutter_ready(int mesh, const uint8_t *vi, const uint8_t *fi, double ms, int recorded, int fresh, int fast) {
    (void)fast;
    if (!stutter_map_on) return;
    plat_mutex_lock(&sm_lock);
    Pipe *p = pipe_at(key_of(vi, fi), mesh != 0);
    if (p) {
        p->ready_n++, p->ready_sum += ms;
        if (ms > p->ready_max) p->ready_max = ms;
        p->recorded |= (uint8_t)(recorded != 0), p->fresh |= (uint8_t)(fresh != 0);
    }
    plat_mutex_unlock(&sm_lock);
}

static void hex8(char *b, uint32_t v, int known) {
    if (known) snprintf(b, 9, "%08x", v);
    else snprintf(b, 9, "%s", "?");
}

// The draws noted since the previous present: counted for the area, and the first few named in line at *o.
static void take_misses(Area *a, char *line, int *o, size_t cap) {
    uint64_t head = __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE);
    if (head == ring_tail) return;
    plat_mutex_lock(&sm_lock);
    head = ring_head;
    if (head - ring_tail > SM_RING) ring_tail = head - SM_RING;
    int named = 0;
    uint64_t more = 0;
    for (; ring_tail < head; ring_tail++) {
        const Miss *m = &ring[ring_tail % SM_RING];
        if (m->shader) a->shader_misses++;
        else a->misses++;
        a->miss_ns += m->ns;
        if (!line) continue;
        if (named >= 4 || *o >= (int)cap - 120) { more++; continue; }
        char v[9], f[9];
        hex8(v, (uint32_t)(m->key >> 32), m->has_v);
        hex8(f, (uint32_t)m->key, m->has_f);
        *o += snprintf(line + *o, cap - (size_t)*o, "%s %s %s %s/%s %.1f ms%s", named ? "," : "; draws that met",
                       m->shader ? "a shader missing:" : "a pipeline missing:", m->mesh ? "mesh" : "vertex", v,
                       m->has_f ? f : "none", m->ns / 1e6, m->skipped ? " (skipped)" : "");
        named++;
    }
    if (more && line && *o < (int)cap - 32) *o += snprintf(line + *o, cap - (size_t)*o, " and %llu more", (unsigned long long)more);
    plat_mutex_unlock(&sm_lock);
}

// The median of the last SM_MEDIAN_N frames' intervals (fewer at the start).
static uint64_t median_recent(void) {
    uint64_t v[SM_MEDIAN_N], n = frames_seen < SM_MEDIAN_N ? frames_seen : SM_MEDIAN_N;
    if (!n) return 0;
    memcpy(v, recent, (size_t)n * sizeof v[0]);
    for (uint64_t i = 1; i < n; i++)
        for (uint64_t k = i; k > 0 && v[k - 1] > v[k]; k--) {
            uint64_t t = v[k];
            v[k] = v[k - 1], v[k - 1] = t;
        }
    return v[n / 2];
}

// A candidate the SM_AFTER frames after it have been seen for: a hitch, counted and its line logged, or a change of
// pace (the frames after it as long), left out.
static void cand_decide(const Cand *c, uint64_t now) {
    if (c->d <= SM_HITCH_SURE_NS && (double)c->after_max * SM_AFTER_RATIO >= (double)c->d) {
        paced_out++;
        return;
    }
    Area *a = &areas[c->area];
    if (c->load) a->load_hitch++;
    else {
        a->hitch++;
        if (c->cause >= 0 && c->cause < SM_CAUSES) a->by_cause[c->cause]++, a->cause_ns[c->cause] += c->d;
        if (c->d > a->worst_ns) a->worst_ns = c->d, a->worst_frame = c->frame;
    }
    uint64_t sec = (now - t_first) / 1000000000ull;
    if (sec != line_sec) line_sec = sec, lines_in_sec = 0;
    if (lines >= SM_LINES_MAX || lines_in_sec >= SM_LINES_PER_S) return;
    rt_log("%s", c->line);
    lines++, lines_in_sec++;
    if (lines == SM_LINES_MAX) rt_log("stutter: the last such line of this run; the map's counts go on");
}

void stutter_frame(uint64_t frame, uint64_t now, uint64_t d, const uint64_t *part, const char *const *names, int n,
                   int cause) {
    if (!stutter_map_on || !d) return;
    if (!t_first) t_first = t_report = now;
    int ai = __atomic_load_n(&cur_area, __ATOMIC_RELAXED);
    Area *a = &areas[ai];
    a->ns += d, a->frames++;
    a->over25 += d > SM_HITCH_ANY_NS;
    // this frame is one of the frames after each waiting candidate
    for (int i = 0; i < n_cand;) {
        Cand *c = &cand[i];
        if (d > c->after_max) c->after_max = d;
        if (++c->after < SM_AFTER) {
            i++;
            continue;
        }
        cand_decide(c, now);
        memmove(&cand[i], &cand[i + 1], (size_t)(n_cand - i - 1) * sizeof cand[0]);   // at most two others
        n_cand--;
    }
    uint64_t med = median_recent();
    recent[frames_seen % SM_MEDIAN_N] = d;
    frames_seen++;
    double lim = (double)med * SM_HITCH_FACTOR;
    if (lim < SM_HITCH_MIN_NS) lim = SM_HITCH_MIN_NS;
    int maybe = (frames_seen > SM_MEDIAN_N && (double)d > lim) || d > SM_HITCH_ANY_NS;
    if (!maybe || n_cand >= SM_AFTER + 1) {
        take_misses(a, NULL, NULL, 0);
    } else {
        Cand *c = &cand[n_cand++];
        c->frame = frame, c->d = d, c->after_max = 0, c->after = 0, c->area = ai, c->cause = cause;
        c->load = now - __atomic_load_n(&load_at, __ATOMIC_RELAXED) < SM_LOAD_NS;
        int o = snprintf(c->line, sizeof c->line, "stutter: frame %llu at %.1f s, %.1f ms (frames about %.1f ms) in %s%s:",
                         (unsigned long long)frame, (now - t_first) / 1e9, d / 1e6, med / 1e6, a->name,
                         c->load ? ", after its load" : "");
        for (int k = 0; k < n && o < (int)sizeof c->line - 40; k++)
            if (part[k] >= 500000) o += snprintf(c->line + o, sizeof c->line - (size_t)o, " %s %.1f", names[k], part[k] / 1e6);
        if (cause >= 0 && cause < n && o < (int)sizeof c->line - 40)
            o += snprintf(c->line + o, sizeof c->line - (size_t)o, " (charged to %s)", names[cause]);
        take_misses(a, c->line, &o, sizeof c->line);
    }
    if (now - t_report >= SM_REPORT_NS) {
        t_report = now;
        char tag[32];
        snprintf(tag, sizeof tag, "%.0f min", (now - t_first) / 6e10);
        stutter_report(tag);
    }
}

static const char *const cause_short[SM_CAUSES] = {"pipeline", "translation", "texture", "fence", "pacing", "replay",
                                                   "unattributed", "?"};

static int area_cmp(const void *x, const void *y) {
    const Area *a = &areas[*(const int *)x], *b = &areas[*(const int *)y];
    uint64_t ka = a->hitch + a->load_hitch, kb = b->hitch + b->load_hitch;
    if (ka != kb) return ka < kb ? 1 : -1;
    return a->ns < b->ns ? 1 : a->ns > b->ns ? -1 : 0;
}

static int pipe_cmp(const void *x, const void *y) {
    const Pipe *a = *(Pipe *const *)x, *b = *(Pipe *const *)y;
    double ka = a->wait_ns / 1e6 + a->ready_max, kb = b->wait_ns / 1e6 + b->ready_max;
    return ka < kb ? 1 : ka > kb ? -1 : 0;
}

void stutter_report(const char *tag) {
    if (!stutter_map_on) return;
    plat_mutex_lock(&sm_lock);
    uint64_t hitch = 0, load = 0, ns = 0, cause[SM_CAUSES] = {0};
    int order[SM_AREAS], na = n_areas;
    for (int i = 0; i < na; i++) {
        order[i] = i;
        hitch += areas[i].hitch, load += areas[i].load_hitch, ns += areas[i].ns;
        for (int c = 0; c < SM_CAUSES; c++) cause[c] += areas[i].by_cause[c];
    }
    char line[1400];
    int o = snprintf(line, sizeof line, "stutter map (%s): %.1f min of play in %d areas; %llu hitches in play and %llu "
                     "after area loads; %llu long frames were a change of pace (the frames after them as long), not "
                     "hitches; in play by cause:", tag, ns / 6e10, na, (unsigned long long)hitch, (unsigned long long)load,
                     (unsigned long long)paced_out);
    for (int c = 0; c < SM_CAUSES; c++)
        if (cause[c]) o += snprintf(line + o, sizeof line - (size_t)o, " %s %llu", cause_short[c], (unsigned long long)cause[c]);
    rt_log("%s", line);
    qsort(order, (size_t)na, sizeof order[0], area_cmp);
    for (int k = 0; k < na && k < SM_TOP_AREAS; k++) {
        const Area *a = &areas[order[k]];
        if (!a->frames) continue;
        o = snprintf(line, sizeof line, "stutter map:   %s: %.1f min at %.0f fps; hitches %llu (%.2f a minute",
                     a->name, a->ns / 6e10, a->ns ? a->frames * 1e9 / a->ns : 0.0, (unsigned long long)a->hitch,
                     a->ns ? a->hitch * 6e10 / a->ns : 0.0);
        for (int c = 0; c < SM_CAUSES; c++)
            if (a->by_cause[c])
                o += snprintf(line + o, sizeof line - (size_t)o, "; %s %llu, %.1f ms avg", cause_short[c],
                              (unsigned long long)a->by_cause[c], a->cause_ns[c] / 1e6 / a->by_cause[c]);
        o += snprintf(line + o, sizeof line - (size_t)o, "), worst %.1f ms (frame %llu); over 25 ms %llu; after its "
                      "loads %llu; draws that met a pipeline missing %llu, a shader %llu, %.1f ms on the replay thread",
                      a->worst_ns / 1e6, (unsigned long long)a->worst_frame, (unsigned long long)a->over25,
                      (unsigned long long)a->load_hitch, (unsigned long long)a->misses,
                      (unsigned long long)a->shader_misses, a->miss_ns / 1e6);
        rt_log("%s", line);
    }
    static Pipe *list[SM_PIPES];
    uint32_t np = 0, nrec = 0, nnew = 0, nfresh = 0, nunknown = 0;
    for (uint32_t i = 0; i < SM_PIPES; i++)
        if (pipes[i].used) {
            list[np++] = &pipes[i];
            if (!pipes[i].key) nunknown++;
            else if (pipes[i].ready_n) nrec += pipes[i].recorded, nnew += !pipes[i].recorded;
            nfresh += pipes[i].fresh;
        }
    qsort(list, np, sizeof list[0], pipe_cmp);
    rt_log("stutter map: %u pipelines draws met missing: %u that this install had recorded before (its prebuild had not "
           "come to them), %u new to it, %u with a shader translated in play, %u still translating when met; %u not "
           "kept (table full), %llu draws not seen by a present (ring full); the %d that cost the most:",
           np, nrec, nnew, nfresh, nunknown, pipes_lost, (unsigned long long)ring_lost, SM_TOP_PIPES);
    for (uint32_t k = 0; k < np && k < SM_TOP_PIPES; k++) {
        const Pipe *p = list[k];
        char v[9], f[9];
        hex8(v, (uint32_t)(p->key >> 32), p->key >> 32 != 0);
        hex8(f, (uint32_t)p->key, 1);
        rt_log("stutter map:   %s %s/%s first met in %s: %llu draws met it missing, %llu its shader, %.1f ms on the "
               "replay thread; usable after %.0f ms at most (%llu times); %s%s",
               p->mesh ? "mesh" : "vertex", v, (uint32_t)p->key ? f : "none", areas[p->area].name,
               (unsigned long long)p->misses, (unsigned long long)p->shader_misses, p->wait_ns / 1e6, p->ready_max,
               (unsigned long long)p->ready_n,
               !p->key ? "its shaders still translating" : !p->ready_n ? "not seen ready" : p->recorded ? "recorded before"
                                                                                                        : "new to this install",
               p->fresh ? ", a shader translated in play" : "");
    }
    plat_mutex_unlock(&sm_lock);
}
