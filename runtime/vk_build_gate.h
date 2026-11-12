// The memory gate of the background pipeline builds (vk_pipeline.c gate_enter): whether a build may start, given the
// system's commit and what the builds already under way were promised. Own header so tests/build_gate_check.c checks
// it without Vulkan.
//
// A pipeline build runs the driver's shader compiler in this process, and what the compiler takes while it works is
// committed memory like any other. A gate that only reads the commit in use sees none of it for the builds that have
// just started: sixteen builders that each find "enough room" start together and take sixteen shares of the same
// room. So every build that starts reserves an estimate of what its compile takes, the gate counts the reservations
// as if they were in use already, and a build that does not fit is refused: it does not start, and its caller leaves
// it to a later moment or to the draw that first needs the pipeline. A machine at its commit limit fails allocations
// (Windows) or pages until the driver gives the device up; a pipeline built later costs one hitch.
#pragma once

typedef struct {
    double free_mb;       // commit headroom in MB that stays free of background builds; 0 or less: nothing is refused
    double reserved_mb;   // the estimates of the builds under way
} BuildGate;

// 1 when a build estimated at est_mb may start with used_mb of limit_mb of the system's commit in use. A limit below
// 0 is an unknown one (platform.h plat_commit_info): nothing is refused.
static inline int build_gate_admits(const BuildGate *g, double used_mb, double limit_mb, double est_mb) {
    if (g->free_mb <= 0 || limit_mb < 0) return 1;
    return limit_mb - used_mb - g->reserved_mb >= g->free_mb + est_mb;
}

// Reserves est_mb for a build that starts now; 0 when it is refused, and then nothing is reserved.
static inline int build_gate_enter(BuildGate *g, double used_mb, double limit_mb, double est_mb) {
    if (!build_gate_admits(g, used_mb, limit_mb, est_mb)) return 0;
    g->reserved_mb += est_mb;
    return 1;
}

// The build that reserved est_mb is over.
static inline void build_gate_leave(BuildGate *g, double est_mb) {
    g->reserved_mb -= est_mb;
    if (g->reserved_mb < 0) g->reserved_mb = 0;
}
