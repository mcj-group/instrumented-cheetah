#ifndef _CILK_PUBLIC_SCHED_STATS_H
#define _CILK_PUBLIC_SCHED_STATS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum __cilkrts_sched_stats_category {
    CILK_STATS_WORKING,
    CILK_STATS_SCHEDULING,
    CILK_STATS_IDLING,
    CILK_STATS_SLEEP,
    CILK_STATS_SLEEP_UNCILK,
    CILK_STATS_CILKIFY_ENTER,
    CILK_STATS_CILKIFY_EXIT,
    CILK_STATS_PENDING_SEARCH,
    CILK_STATS_CATEGORY_COUNT
};

typedef struct __cilkrts_sched_stats_worker_snapshot {
    uint64_t worker_id;
    uint64_t times_ns[CILK_STATS_CATEGORY_COUNT];
    uint64_t counts[CILK_STATS_CATEGORY_COUNT];
    uint64_t steals;
    uint64_t repossessions;
    uint64_t reengagement_requests;
    uint64_t one_sentinel_requests;
    uint64_t observed_ns;
    uint8_t active;
} __cilkrts_sched_stats_worker_snapshot;

/* Boss waiting is separate from worker totals. Pending waiting has not yet
 * reached the exiting-worker timestamp used by the boss-wait metric. */
typedef struct __cilkrts_sched_stats_boss_snapshot {
    uint64_t waiting_ns;
    uint64_t wait_count;
    uint64_t pending_wait_ns;
    uint8_t active;
} __cilkrts_sched_stats_boss_snapshot;

typedef struct __cilkrts_sched_stats_pool_snapshot {
    uint64_t pool_id;
    size_t worker_count;
    __cilkrts_sched_stats_worker_snapshot *workers;
    __cilkrts_sched_stats_boss_snapshot boss;
} __cilkrts_sched_stats_pool_snapshot;

typedef struct __cilkrts_sched_stats_snapshot {
    size_t pool_count;
    __cilkrts_sched_stats_pool_snapshot *pools;
} __cilkrts_sched_stats_snapshot;

/* Return 0 on success, otherwise an errno value (not -1). On failure *out is
 * NULL. Requires CILK_STATS=1; otherwise returns ENOTSUP without recording.
 * Only the process's main thread, outside a Cilk region, may reset (EPERM).
 * The caller must finish its parallel work first. Capture includes open
 * intervals and restarts them at each worker's capture timestamp. Counts are
 * interval fragments, not necessarily distinct scheduler events. Unresolved
 * steal searches remain pending in the returned window; only the later part
 * can be classified by the eventual search outcome. Resets affect all pools.
 *
 * This is the runtime's sole scheduler accounting system. Reset clears live
 * measurements but never the history already copied by record_snapshots().
 * Caller owns the result. Snapshotting does not record it or write a file. */
int __cilkrts_sched_stats_snapshot_and_reset(__cilkrts_sched_stats_snapshot **out);

/* Copy supplied snapshots into the JSON history under a UTF-8 string ID.
 * Does not capture or reset live counters, and performs no file I/O. May be
 * called from any thread. Repeated IDs are retained. The entire call is added
 * or none of it; returns EINVAL, ENOMEM, EOVERFLOW, ENOTSUP or EPIPE on error.
 * After a successful call the input may be modified or freed immediately.
 *
 * At normal runtime shutdown, append a final {"kind":"exit"} snapshot and
 * write cheetah_stats_YYYYMMDD_HHMMSS_ffffff.json (UTC, exclusive creation).
 * Directory: CHEETAH_STATS_DIR, then compiled CHEETAH_STATS_DIR_DEFAULT, then
 * initial working directory. Resolve once at first pool creation (normally
 * before main); set the runtime environment before launching the process.
 * Missing directories are created. Exit I/O failures are reported to stderr;
 * buffered records are lost on abort, _exit or fatal signals. */
int __cilkrts_sched_stats_record_snapshots(
    const __cilkrts_sched_stats_snapshot *snapshots, const char *id);

/* Accepts NULL. Only free objects returned by snapshot_and_reset(). */
void __cilkrts_sched_stats_free_snapshot(__cilkrts_sched_stats_snapshot *snapshots);

#ifdef __cplusplus
}
#endif
#endif
