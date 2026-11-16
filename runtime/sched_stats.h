#ifndef __SCHED_STATS_HEADER__
#define __SCHED_STATS_HEADER__

#include "rts-config.h"
#include <cilk/sched_stats.h>
#include <stdbool.h>

struct global_state;
struct __cilkrts_worker;
struct cilk_snapshot_pool;

#define SCHED_STATS CILK_STATS

enum timing_type {
    INTERVAL_WORK = CILK_STATS_WORKING,
    INTERVAL_SCHED = CILK_STATS_SCHEDULING,
    INTERVAL_IDLE = CILK_STATS_IDLING,
    INTERVAL_SLEEP = CILK_STATS_SLEEP,
    INTERVAL_SLEEP_UNCILK = CILK_STATS_SLEEP_UNCILK,
    INTERVAL_CILKIFY_ENTER = CILK_STATS_CILKIFY_ENTER,
    INTERVAL_CILKIFY_EXIT = CILK_STATS_CILKIFY_EXIT,
    INTERVAL_PENDING_SEARCH = CILK_STATS_PENDING_SEARCH
};

enum cilk_snapshot_counter {
    SNAPSHOT_STEALS, SNAPSHOT_REPOSSESSIONS,
    SNAPSHOT_REENGAGEMENT_REQUESTS, SNAPSHOT_ONE_SENTINEL_REQUESTS
};

#if SCHED_STATS
CHEETAH_INTERNAL void cilk_snapshot_pool_init(struct global_state *g);
CHEETAH_INTERNAL void cilk_snapshot_pool_resize(struct global_state *g, unsigned n);
CHEETAH_INTERNAL void cilk_snapshot_pool_stop(struct global_state *g);
CHEETAH_INTERNAL void cilk_snapshot_write_exit(void);
CHEETAH_INTERNAL void cilk_snapshot_start(struct __cilkrts_worker *w, int category);
CHEETAH_INTERNAL void cilk_snapshot_stop(struct __cilkrts_worker *w, int category);
CHEETAH_INTERNAL void cilk_snapshot_search_finish(struct __cilkrts_worker *w, bool found);
CHEETAH_INTERNAL void cilk_snapshot_add(struct __cilkrts_worker *w, enum cilk_snapshot_counter c, uint64_t n);
CHEETAH_INTERNAL void cilk_snapshot_boss_start(struct global_state *g);
CHEETAH_INTERNAL void cilk_snapshot_boss_stop(struct global_state *g);
CHEETAH_INTERNAL void cilk_snapshot_worker_exit(struct global_state *g);

#define WHEN_SCHED_STATS(ex) ex
#define CILK_START_TIMING(w, t) cilk_snapshot_start(w, t)
#define CILK_STOP_TIMING(w, t) cilk_snapshot_stop(w, t)
// Starting a different state closes the previous state at the same timestamp.
#define CILK_SWITCH_TIMING(w, from, to) cilk_snapshot_start(w, to)
#define CILK_START_SEARCH(w) cilk_snapshot_start(w, INTERVAL_PENDING_SEARCH)
#define CILK_FINISH_SEARCH(w, found) cilk_snapshot_search_finish(w, found)
#define CILK_ADD_STATS(w, counter, n) cilk_snapshot_add(w, counter, n)
#define CILK_BOSS_START_TIMING(g) cilk_snapshot_boss_start(g)
#define CILK_BOSS_STOP_TIMING(g) cilk_snapshot_boss_stop(g)
#define CILK_EXIT_WORKER_TIMING(g) cilk_snapshot_worker_exit(g)
#else
#define WHEN_SCHED_STATS(ex)
#define CILK_START_TIMING(w, t)
#define CILK_STOP_TIMING(w, t)
#define CILK_SWITCH_TIMING(w, from, to)
#define CILK_START_SEARCH(w)
#define CILK_FINISH_SEARCH(w, found)
#define CILK_ADD_STATS(w, counter, n)
#define CILK_BOSS_START_TIMING(g)
#define CILK_BOSS_STOP_TIMING(g)
#define CILK_EXIT_WORKER_TIMING(g)
#endif

#endif
