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
// stutter in play. An area load is the game's (stutter_area) or the texture pool's (stutter_load: the pool makes an
// area's textures, vk_exec_draw.c pool_loading), which comes without the game's when it streams the next area in.
// Consecutive long frames are one episode, a hitch counted once with their summed time: a change of pace is a run of
// more than SM_EPISODE_MAX long frames none of which stands out from the ones after it, and an episode ends at its
// last frame that stood out once more than SM_EPISODE_MAX such frames follow that one. Three frames of 87, 91 and
// 96 ms after an area load (2026-11-13) each counted as a change of pace before, as each was followed by one as long.
//
// What a hitch went to comes from the frame-time summary's split (perf.c ft_frame: pipeline, translation, texture,
// fence, pacing, hand-off, replay, census, report, unattributed), with the draws that met a pipeline or a shader
// missing between the previous present and this one (stutter_miss, from the replay thread). Each hitch gets a
// "stutter:" line, at most SM_LINES_PER_S a second and SM_LINES_MAX in all; the counts go on past that. Every
// SM_REPORT_NS of play, when the window loses the focus in play (perf.c session_report) and at exit, "stutter map"
// lines rank the areas by their hitches and the pipelines by the time draws lost to them, each with whether this
// install had recorded it before (the pipeline list or an earlier run) and whether one of its shaders was translated
// in play rather than prepared.
//
// Threads: stutter_area runs on the game's loading threads, stutter_miss on the replay thread, stutter_ready on the
// pipeline workers (under vk_pipeline.c's P.lock; sm_lock is taken inside it and never takes it), stutter_frame and
// the periodic report on the present thread. The area table's play counts belong to the present thread; the rest is
// under sm_lock. A report copies the map under sm_lock and sorts and logs the copy without it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "stutter_map.h"
#include "platform/platform.h"

int stutter_map_on;

#define SM_AREAS 128                       // areas kept by name; later ones share the last slot
#define SM_CAUSES STUTTER_CAUSES           // perf.c's FT_C_N, which perf.c asserts
#define SM_LOAD_NS 3000000000ull           // after an area load, frames count as the load's for this long
#define SM_HITCH_MIN_NS 12000000.0         // a hitch is at least this long...
#define SM_HITCH_ANY_NS 25000000ull        // ...and any frame over this is one
#define SM_HITCH_FACTOR 2.5                // times the median of the frames before it
#define SM_HITCH_SURE_NS 100000000ull      // a frame this long is a hitch whatever follows it
#define SM_MEDIAN_N 32                     // frames the median is taken over
#define SM_AFTER 2                         // frames after a candidate that must be shorter...
#define SM_AFTER_RATIO 1.5                 // ...by this factor
#define SM_EPISODE_MAX 8                   // a run of long frames up to this long is an episode, longer a new pace
#define SM_PIPES 4096                      // pipelines kept for the ranking (open addressing)
#define SM_RING 256                        // draws that met something missing, between two presents
#define SM_LINES_MAX 4000
#define SM_LINES_PER_S 5
#define SM_REPORT_NS 300000000000ull       // the map every 5 minutes of play
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
    uint64_t frame, d, after_max, med;
    double t_s;            // seconds since the first frame
    int area, load, cause, after;
    int tail;              // where the line's text after its head ("stutter: frame ... in <area>[, after its load]:") starts
} Cand;
static Cand cand[SM_AFTER + 1];
static int n_cand;
// The episode being gathered: decided candidates of consecutive frames, the first one's line and area (cand_decide).
static struct {
    Cand first;
    uint64_t last, total;   // the last frame in it; the frames' summed intervals
    int n, hitch, open;     // frames in it; one of them stood out from the frames after it (or was over the sure line)
    uint64_t hit_last, hit_total;   // the episode up to its last frame that stood out: that frame, the summed intervals
    int hit_n;                      // and the frames to it (0: none stood out)
} ep;
static int pace_run;        // more than SM_EPISODE_MAX long frames, none standing out: a new pace, until it ends
static uint64_t pace_last;

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

void stutter_load(void) {
    if (stutter_map_on) __atomic_store_n(&load_at, kern_now_ns(), __ATOMIC_RELAXED);
}

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

// The episode ends: a hitch with its frames' summed time, counted in its first frame's area and charged to that frame's
// cause, and its line logged; a single frame that did not stand out from the ones after it is a change of pace.
static void episode_close(uint64_t now) {
    ep.open = 0;
    const Cand *c = &ep.first;
    if (!ep.hitch && ep.n < 2) {
        paced_out++;
        return;
    }
    Area *a = &areas[c->area];
    if (c->load) a->load_hitch++;
    else {
        a->hitch++;
        if (c->cause >= 0 && c->cause < SM_CAUSES) a->by_cause[c->cause]++, a->cause_ns[c->cause] += ep.total;
        if (ep.total > a->worst_ns) a->worst_ns = ep.total, a->worst_frame = c->frame;
    }
    uint64_t sec = (now - t_first) / 1000000000ull;
    if (sec != line_sec) line_sec = sec, lines_in_sec = 0;
    if (lines >= SM_LINES_MAX || lines_in_sec >= SM_LINES_PER_S) return;
    if (ep.n == 1) rt_log("%s", c->line);
    else   // the head with the episode's time; the parts and the draws are its first frame's
        rt_log("stutter: frame %llu at %.1f s, %.1f ms (frames about %.1f ms) in %s%s:%s; an episode of %d frames to frame "
               "%llu, %.1f ms the first, the parts above the first frame's", (unsigned long long)c->frame, c->t_s,
               ep.total / 1e6, c->med / 1e6, areas[c->area].name, c->load ? ", after its load" : "", c->line + c->tail,
               ep.n, (unsigned long long)ep.last, c->d / 1e6);
    lines++, lines_in_sec++;
    if (lines == SM_LINES_MAX) rt_log("stutter: the last such line of this run; the map's counts go on");
}

// A candidate the SM_AFTER frames after it have been seen for: it stands out (a hitch) or the frames after it are as
// long (paced). It joins the episode of the frame before it, or starts one. More than SM_EPISODE_MAX paced frames
// after the episode's last frame that stood out (or from its start, when none did) are a change of pace: they and the
// paced frames right after them are left out, and an episode with a frame that stood out ends at that frame and is
// counted. At 40 fps or less every frame is over SM_HITCH_ANY_NS and so a candidate: without the cut at the last frame
// that stood out, the first hitch took in every frame after it until the rate rose, and a 30 fps session counted one
// hitch seconds long in place of its five.
static void cand_decide(const Cand *c, uint64_t now) {
    int paced = c->d <= SM_HITCH_SURE_NS && (double)c->after_max * SM_AFTER_RATIO >= (double)c->d;
    if (pace_run) {
        if (paced && c->frame == pace_last + 1) {
            paced_out++, pace_last = c->frame;
            return;
        }
        pace_run = 0;
    }
    if (ep.open && c->frame == ep.last + 1) {
        ep.last = c->frame, ep.total += c->d, ep.n++;
        if (!paced) ep.hitch = 1, ep.hit_n = ep.n, ep.hit_last = c->frame, ep.hit_total = ep.total;
        if (ep.n - ep.hit_n > SM_EPISODE_MAX) {
            paced_out += (uint64_t)(ep.n - ep.hit_n);
            if (ep.hitch) ep.n = ep.hit_n, ep.last = ep.hit_last, ep.total = ep.hit_total, episode_close(now);
            else ep.open = 0;
            pace_run = 1, pace_last = c->frame;
        }
        return;
    }
    if (ep.open) episode_close(now);
    ep.first = *c, ep.last = c->frame, ep.total = c->d, ep.n = 1, ep.hitch = !paced, ep.open = 1;
    ep.hit_n = !paced, ep.hit_last = c->frame, ep.hit_total = paced ? 0 : c->d;
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
    if (ep.open && frame > ep.last + SM_AFTER) episode_close(now);   // the frame after its last was no candidate
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
        c->med = med, c->t_s = (now - t_first) / 1e9;
        int o = snprintf(c->line, sizeof c->line, "stutter: frame %llu at %.1f s, %.1f ms (frames about %.1f ms) in %s%s:",
                         (unsigned long long)frame, c->t_s, d / 1e6, med / 1e6, a->name, c->load ? ", after its load" : "");
        c->tail = o < (int)sizeof c->line ? o : (int)sizeof c->line - 1;
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

// In the order of perf.c's causes (FT_C_*); "hand-off" is the wait for the present thread to take the previous picture,
// "census" NX2NAVI_MEM_CENSUS's walk of the address space, "report" the memory report on the presenting thread.
static const char *const cause_short[SM_CAUSES] = {"pipeline", "translation", "texture", "fence", "pacing", "hand-off",
                                                   "replay", "census", "report", "unattributed"};

static int area_cmp(const void *x, const void *y) {
    const Area *a = *(const Area *const *)x, *b = *(const Area *const *)y;
    uint64_t ka = a->hitch + a->load_hitch, kb = b->hitch + b->load_hitch;
    if (ka != kb) return ka < kb ? 1 : -1;
    return a->ns < b->ns ? 1 : a->ns > b->ns ? -1 : 0;
}

static int pipe_cmp(const void *x, const void *y) {
    const Pipe *a = *(const Pipe *const *)x, *b = *(const Pipe *const *)y;
    double ka = a->wait_ns / 1e6 + a->ready_max, kb = b->wait_ns / 1e6 + b->ready_max;
    return ka < kb ? 1 : ka > kb ? -1 : 0;
}

// At the exit: the hitches in the run's last frames, which cand_decide and episode_close would have counted had the
// run gone on: the open episode, and the candidates still waiting for their next frames (the last two frames, three
// in a run of long ones). A waiting candidate counts as one that did not stand out unless it is over SM_HITCH_SURE_NS
// (a hitch whatever follows), and joins the episode of the frame before it as cand_decide joins it. Added to the
// exit's printed totals only, and only read: the exit's report runs on another thread (perf.c perf_summary_flush, from
// the window's close or the game's exit) while the presenting thread, which owns this state, may still be in
// stutter_frame; a race there miscounts these last frames at worst. Other reports leave them to the next one.
static void exit_tail(uint64_t *hitch, uint64_t *load, uint64_t *cause) {
    int n = ep.open ? ep.n : 0, hit = ep.hitch, paced = pace_run, at_load = ep.first.load, why = ep.first.cause;
    int nc = n_cand < SM_AFTER + 1 ? n_cand : SM_AFTER + 1;
    uint64_t last = ep.open ? ep.last : pace_last;
    for (int i = 0; i <= nc; i++) {
        const Cand *c = i < nc ? &cand[i] : NULL;
        int sure = c && c->d > SM_HITCH_SURE_NS;
        if (c && c->frame == last + 1) {
            last = c->frame;
            if (n) {   // it joins the episode
                n++, hit |= sure;
                if (!hit && n > SM_EPISODE_MAX) n = 0, paced = 1;   // a change of pace
                continue;
            }
            if (paced && !sure) continue;   // the change of pace goes on
        }
        if (n && (hit || n >= 2)) {
            if (at_load) ++*load;
            else {
                ++*hitch;
                if (why >= 0 && why < SM_CAUSES) cause[why]++;
            }
        }
        n = 0, paced = 0;
        if (c) n = 1, hit = sure, at_load = c->load, why = c->cause, last = c->frame;
    }
}

// The map as a report reads it: copied under sm_lock, then sorted and logged without it. The replay thread
// (stutter_miss) and the pipeline workers (stutter_ready, inside vk_pipeline.c's P.lock, which the draw path takes too)
// wait for sm_lock, and a report sorts up to SM_PIPES entries and logs about 85 lines.
typedef struct {
    Area areas[SM_AREAS];
    Pipe pipes[SM_PIPES];
    const Area *order[SM_AREAS];
    const Pipe *list[SM_PIPES];
} MapCopy;

// try_only: sm_lock is only tried (a fatal error's summary, perf.c perf_summary_flush_try, on a thread that may
// hold it); 0 when it was held and nothing was written.
int stutter_report_as(const char *tag, int try_only) {
    if (!stutter_map_on) return 1;
    MapCopy *m = malloc(sizeof *m);
    if (!m) {
        rt_log("stutter map (%s): no memory for the report", tag);
        return 1;
    }
    if (!try_only) plat_mutex_lock(&sm_lock);
    else if (!plat_mutex_trylock(&sm_lock)) {
        free(m);
        return 0;
    }
    int na = n_areas;
    memcpy(m->areas, areas, (size_t)na * sizeof *areas);
    uint32_t np = 0, lost = pipes_lost;
    for (uint32_t i = 0; i < SM_PIPES; i++)
        if (pipes[i].used) m->pipes[np++] = pipes[i];
    uint64_t rlost = ring_lost;
    plat_mutex_unlock(&sm_lock);
    uint64_t hitch = 0, load = 0, ns = 0, cause[SM_CAUSES] = {0};
    for (int i = 0; i < na; i++) {
        m->order[i] = &m->areas[i];
        hitch += m->areas[i].hitch, load += m->areas[i].load_hitch, ns += m->areas[i].ns;
        for (int c = 0; c < SM_CAUSES; c++) cause[c] += m->areas[i].by_cause[c];
    }
    if (!strcmp(tag, "exit")) exit_tail(&hitch, &load, cause);
    char line[1400];
    int o = snprintf(line, sizeof line, "stutter map (%s): %.1f min of play in %d areas; %llu hitches in play and %llu "
                     "after area loads; %llu long frames were a change of pace (the frames after them as long), not "
                     "hitches; in play by cause:", tag, ns / 6e10, na, (unsigned long long)hitch, (unsigned long long)load,
                     (unsigned long long)paced_out);
    for (int c = 0; c < SM_CAUSES; c++)
        if (cause[c]) o += snprintf(line + o, sizeof line - (size_t)o, " %s %llu", cause_short[c], (unsigned long long)cause[c]);
    rt_log("%s", line);
    qsort(m->order, (size_t)na, sizeof m->order[0], area_cmp);
    for (int k = 0; k < na && k < SM_TOP_AREAS; k++) {
        const Area *a = m->order[k];
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
    uint32_t nrec = 0, nnew = 0, nfresh = 0, nunknown = 0;
    for (uint32_t i = 0; i < np; i++) {
        const Pipe *p = m->list[i] = &m->pipes[i];
        if (!p->key) nunknown++;
        else if (p->ready_n) nrec += p->recorded, nnew += !p->recorded;
        nfresh += p->fresh;
    }
    qsort(m->list, np, sizeof m->list[0], pipe_cmp);
    rt_log("stutter map: %u pipelines draws met missing: %u that this install had recorded before (its prebuild had not "
           "come to them), %u new to it, %u with a shader translated in play, %u still translating when met; %u not "
           "kept (table full), %llu draws not seen by a present (ring full); the %d that cost the most:",
           np, nrec, nnew, nfresh, nunknown, lost, (unsigned long long)rlost, SM_TOP_PIPES);
    for (uint32_t k = 0; k < np && k < SM_TOP_PIPES; k++) {
        const Pipe *p = m->list[k];
        char v[9], f[9];
        hex8(v, (uint32_t)(p->key >> 32), p->key >> 32 != 0);
        hex8(f, (uint32_t)p->key, 1);
        rt_log("stutter map:   %s %s/%s first met in %s: %llu draws met it missing, %llu its shader, %.1f ms on the "
               "replay thread; usable after %.0f ms at most (%llu times); %s%s",
               p->mesh ? "mesh" : "vertex", v, (uint32_t)p->key ? f : "none", m->areas[p->area].name,   // copied: area < na
               (unsigned long long)p->misses, (unsigned long long)p->shader_misses, p->wait_ns / 1e6, p->ready_max,
               (unsigned long long)p->ready_n,
               !p->key ? "its shaders still translating" : !p->ready_n ? "not seen ready" : p->recorded ? "recorded before"
                                                                                                        : "new to this install",
               p->fresh ? ", a shader translated in play" : "");
    }
    free(m);
    return 1;
}

void stutter_report(const char *tag) { stutter_report_as(tag, 0); }
