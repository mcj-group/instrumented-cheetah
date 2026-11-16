/* Deterministic boundary tests against the production implementation. */
#define _GNU_SOURCE
#define CILK_STATS 1
#define CILK_DEBUG 0
#include <assert.h>
#include <stdint.h>
#include <time.h>

static uint64_t monotonic_ns = 1;
static uint64_t realtime_ns = UINT64_C(1794672000000000000);
static int test_clock_gettime(clockid_t clock, struct timespec *out) {
    uint64_t value = clock == CLOCK_MONOTONIC ? monotonic_ns : realtime_ns;
    if (clock == CLOCK_REALTIME) realtime_ns += 1000;
    out->tv_sec = value / UINT64_C(1000000000);
    out->tv_nsec = value % UINT64_C(1000000000);
    return 0;
}

#define clock_gettime test_clock_gettime
#include "../runtime/sched_stats.c"

static int on_workers;
int __cilkrts_running_on_workers(void) { return on_workers; }

static snapshots *capture(void) {
    snapshots *s = NULL;
    assert(__cilkrts_sched_stats_snapshot_and_reset(&s) == 0 && s);
    for (size_t p = 0; p < s->pool_count; ++p) {
        for (size_t i = 0; i < s->pools[p].worker_count; ++i) {
            worker_snapshot *w = &s->pools[p].workers[i];
            uint64_t sum = 0;
            for (int j = 0; j < CAT_COUNT; ++j) sum += w->times_ns[j];
            assert(sum == w->observed_ns);
        }
    }
    return s;
}

static void save(snapshots *s, const char *id) {
    assert(__cilkrts_sched_stats_record_snapshots(s, id) == 0);
    __cilkrts_sched_stats_free_snapshot(s);
}

static void *rejected_thread(void *unused) {
    (void)unused;
    snapshots *s = (snapshots *)1;
    assert(__cilkrts_sched_stats_snapshot_and_reset(&s) == EPERM && !s);
    return NULL;
}

int main(void) {
    snapshots *s = capture();
    assert(s->pool_count == 0);
    __cilkrts_sched_stats_free_snapshot(s);
    global_state g = {0};
    __cilkrts_worker worker = {.self = 0, .g = &g};
    g.nworkers = 1;
    cilk_snapshot_pool_init(&g);

    monotonic_ns = 10;
    CILK_START_TIMING(&worker, INTERVAL_SLEEP_UNCILK);
    monotonic_ns = 30;
    s = capture();
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_SLEEP_UNCILK] == 20);
    assert(s->pools[0].workers[0].counts[CILK_STATS_SLEEP_UNCILK] == 1);
    assert(s->pools[0].workers[0].active);
    assert(__cilkrts_sched_stats_record_snapshots(s, "copy-☃\"\\\n") == 0);
    assert(__cilkrts_sched_stats_record_snapshots(s, "exit") == 0);
    assert(__cilkrts_sched_stats_record_snapshots(s, "exit") == 0);
    s->pools[0].workers[0].times_ns[CILK_STATS_SLEEP_UNCILK] = 999;
    __cilkrts_sched_stats_free_snapshot(s); /* history owns its copies */

    monotonic_ns = 60;
    s = capture();
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_SLEEP_UNCILK] == 30);
    save(s, "sleep-middle");
    monotonic_ns = 90;
    CILK_STOP_TIMING(&worker, INTERVAL_SLEEP_UNCILK);
    monotonic_ns = 100;
    s = capture();
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_SLEEP_UNCILK] == 30);
    assert(!s->pools[0].workers[0].active);
    save(s, "sleep-end");

    monotonic_ns = 110;
    CILK_START_SEARCH(&worker);
    monotonic_ns = 120;
    CILK_START_SEARCH(&worker); // retries must not restart the pending interval
    monotonic_ns = 130;
    s = capture();
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_PENDING_SEARCH] == 20);
    assert(s->pools[0].workers[0].counts[CILK_STATS_PENDING_SEARCH] == 1);
    save(s, "pending-success");
    monotonic_ns = 150;
    CILK_ADD_STATS(&worker, SNAPSHOT_STEALS, 1);
    CILK_FINISH_SEARCH(&worker, true);
    s = capture();
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_SCHEDULING] == 20);
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_IDLING] == 0);
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_PENDING_SEARCH] == 0);
    assert(s->pools[0].workers[0].steals == 1);
    save(s, "resolved-success");

    monotonic_ns = 160;
    CILK_START_SEARCH(&worker);
    monotonic_ns = 180;
    s = capture();
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_PENDING_SEARCH] == 20);
    save(s, "pending-failure");
    monotonic_ns = 200;
    CILK_FINISH_SEARCH(&worker, false);
    s = capture();
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_IDLING] == 20);
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_SCHEDULING] == 0);
    save(s, "resolved-failure");

    monotonic_ns = 201;
    CILK_START_SEARCH(&worker);
    monotonic_ns = 203;
    s = capture();
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_PENDING_SEARCH] == 2);
    save(s, "abandoned-prefix");
    monotonic_ns = 205;
    // Leaving a search without an outcome must retain its pending tail.
    CILK_START_TIMING(&worker, INTERVAL_SCHED);
    monotonic_ns = 207;
    CILK_STOP_TIMING(&worker, INTERVAL_SCHED);
    s = capture();
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_PENDING_SEARCH] == 2);
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_SCHEDULING] == 2);
    save(s, "abandoned-tail");

    monotonic_ns = 210;
    CILK_START_TIMING(&worker, INTERVAL_WORK);
    monotonic_ns = 230;
    s = capture();
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_WORKING] == 20);
    __cilkrts_sched_stats_free_snapshot(s);
    monotonic_ns = 250;
    CILK_SWITCH_TIMING(&worker, INTERVAL_WORK, INTERVAL_SCHED);
    monotonic_ns = 270;
    CILK_STOP_TIMING(&worker, INTERVAL_SCHED);
    s = capture();
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_WORKING] == 20);
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_SCHEDULING] == 20);
    save(s, "switch");

    CILK_ADD_STATS(&worker, SNAPSHOT_REPOSSESSIONS, 2);
    CILK_ADD_STATS(&worker, SNAPSHOT_REENGAGEMENT_REQUESTS, 3);
    CILK_ADD_STATS(&worker, SNAPSHOT_ONE_SENTINEL_REQUESTS, 4);
    s = capture();
    assert(s->pools[0].workers[0].repossessions == 2);
    assert(s->pools[0].workers[0].reengagement_requests == 3);
    assert(s->pools[0].workers[0].one_sentinel_requests == 4);
    save(s, "counters");
    s = capture();
    assert(s->pools[0].workers[0].repossessions == 0);
    assert(s->pools[0].workers[0].observed_ns == 0);
    __cilkrts_sched_stats_free_snapshot(s);

    worker_snapshot excessive[2] = {{.observed_ns = UINT64_MAX}, {.observed_ns = 1}};
    pool_snapshot mixed[2] = {{.pool_id = 0}, {.pool_id = 999, .worker_count = 2, .workers = excessive}};
    snapshots invalid = {.pool_count = 2, .pools = mixed};
    assert(__cilkrts_sched_stats_record_snapshots(&invalid, "must-not-appear") == EOVERFLOW);
    assert(__cilkrts_sched_stats_record_snapshots(&invalid, "\xc0\x80") == EINVAL);

    monotonic_ns = 1000;
    CILK_START_TIMING(&worker, INTERVAL_WORK);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, rejected_thread, NULL));
    assert(!pthread_join(thread, NULL));
    on_workers = 1;
    assert(__cilkrts_sched_stats_snapshot_and_reset(&s) == EPERM && !s);
    on_workers = 0;
    monotonic_ns = 1200;
    s = capture();
    assert(s->pools[0].workers[0].times_ns[CILK_STATS_WORKING] == 200);
    save(s, "invalid-callers-did-not-reset");
    CILK_STOP_TIMING(&worker, INTERVAL_WORK);

    global_state second = {0};
    __cilkrts_worker other = {.self = 0, .g = &second};
    second.nworkers = 1;
    cilk_snapshot_pool_init(&second);
    monotonic_ns = 2000;
    CILK_START_TIMING(&other, INTERVAL_WORK);
    monotonic_ns = 2100;
    CILK_STOP_TIMING(&other, INTERVAL_WORK);
    cilk_snapshot_pool_stop(&second);
    memset(&second, 0, sizeof(second)); /* snapshots must retain retired pools */
    s = capture();
    assert(s->pool_count == 2 && s->pools[1].pool_id == 1);
    assert(s->pools[1].workers[0].times_ns[CILK_STATS_WORKING] == 100);
    save(s, "two-pools");

    monotonic_ns = 2200;
    CILK_BOSS_START_TIMING(&g);
    monotonic_ns = 2250;
    s = capture();
    assert(s->pools[0].boss.pending_wait_ns == 50);
    assert(s->pools[0].boss.waiting_ns == 0);
    save(s, "boss-pending");
    monotonic_ns = 2300;
    CILK_EXIT_WORKER_TIMING(&g);
    monotonic_ns = 2350;
    s = capture();
    assert(s->pools[0].boss.waiting_ns == 50);
    save(s, "boss-waiting");
    monotonic_ns = 2400;
    CILK_BOSS_STOP_TIMING(&g);
    s = capture();
    assert(s->pools[0].boss.waiting_ns == 50);
    assert(s->pools[0].boss.wait_count == 1);
    save(s, "boss-end");

    /* Precreate the next UTC filename: the writer must retry, not overwrite. */
    struct tm utc;
    time_t seconds = realtime_ns / UINT64_C(1000000000);
    assert(gmtime_r(&seconds, &utc));
    char stamp[32];
    assert(strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &utc));
    assert(!mkdir_all(directory));
    char *collision;
    assert(asprintf(&collision, "%s/cheetah_stats_%s_%06" PRIu64 ".json", directory,
                    stamp, (realtime_ns % UINT64_C(1000000000)) / 1000) >= 0);
    FILE *fp = fopen(collision, "w");
    assert(fp); fputs("sentinel", fp); assert(!fclose(fp)); free(collision);

    monotonic_ns = 2500;
    CILK_START_TIMING(&worker, INTERVAL_WORK);
    monotonic_ns = 2600;
    cilk_snapshot_pool_stop(&g);
    cilk_snapshot_write_exit();
    assert(__cilkrts_sched_stats_snapshot_and_reset(&s) == EPIPE && !s);
    snapshots empty = {0};
    assert(__cilkrts_sched_stats_record_snapshots(&empty, "late") == EPIPE);
    return 0;
}
