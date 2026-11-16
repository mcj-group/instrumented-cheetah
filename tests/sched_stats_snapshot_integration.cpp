#include <cilk/cilk.h>
#include <cilk/cilk_api.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using Snapshot = __cilkrts_sched_stats_snapshot;
static volatile unsigned problem_size = 20; // prevent cross-iteration CSE

__attribute__((noinline)) static unsigned fib(unsigned n) {
    if (n < 2) return n;
    unsigned a = cilk_spawn fib(n - 1);
    unsigned b = fib(n - 2);
    cilk_sync;
    return a + b;
}

static void reject_reset() {
    Snapshot *s = reinterpret_cast<Snapshot *>(1);
    assert(__cilkrts_sched_stats_snapshot_and_reset(&s) == EPERM && !s);
}

__attribute__((noinline)) static void reject_in_region() {
    cilk_spawn reject_reset();
    reject_reset();
    cilk_sync;
}

static Snapshot *capture() {
    Snapshot *s = nullptr;
    assert(__cilkrts_sched_stats_snapshot_and_reset(&s) == 0 && s);
    assert(s->pool_count == 1);
    assert(s->pools[0].worker_count == __cilkrts_get_nworkers());
    for (size_t i = 0; i < s->pools[0].worker_count; ++i) {
        const auto &w = s->pools[0].workers[i];
        uint64_t total = 0;
        for (auto time : w.times_ns) total += time;
        assert(total == w.observed_ns);
    }
    return s;
}

int main(int argc, char **argv) {
    std::string mode = argc > 1 ? argv[1] : "normal";
    if (mode == "disabled") {
        Snapshot *s = reinterpret_cast<Snapshot *>(1);
        assert(__cilkrts_sched_stats_snapshot_and_reset(&s) == ENOTSUP && !s);
        assert(__cilkrts_sched_stats_record_snapshots(nullptr, "disabled") == ENOTSUP);
        assert(fib(problem_size) == 6765);
        return 0;
    }
    if (mode == "directory-change") {
        assert(argc == 3 && chdir(argv[2]) == 0);
        assert(setenv("CHEETAH_STATS_DIR", "must-not-be-used", 1) == 0);
    }
    if (mode == "no-work") return 0;
    assert(fib(problem_size) == 6765); // excluded setup/warmup
    if (mode == "exit-only") {
        return 0;
    }
    std::thread(reject_reset).join();
    reject_in_region();
    usleep(10000);
    for (unsigned i = 1; i <= 3; ++i) {
        __cilkrts_sched_stats_free_snapshot(capture());
        assert(fib(problem_size) == 6765);
        Snapshot *s = capture();
        std::string id = "iteration/" + std::to_string(i);
        assert(__cilkrts_sched_stats_record_snapshots(s, id.c_str()) == 0);
        if (mode == "abort") abort();
        if (mode == "quick-exit") _exit(0);
        if (i == 3) {
            assert(__cilkrts_sched_stats_record_snapshots(s, "exit") == 0);
            assert(__cilkrts_sched_stats_record_snapshots(s, "exit") == 0);
            char mutable_id[] = "quote-\"\\\n-☃";
            assert(__cilkrts_sched_stats_record_snapshots(s, mutable_id) == 0);
            memset(mutable_id, 'x', sizeof(mutable_id) - 1);
            std::vector<std::thread> threads;
            for (unsigned thread = 0; thread < 4; ++thread) {
                threads.emplace_back([s, thread] {
                    std::string id = "thread/" + std::to_string(thread);
                    for (unsigned j = 0; j < 2; ++j)
                        assert(__cilkrts_sched_stats_record_snapshots(s, id.c_str()) == 0);
                });
            }
            for (auto &thread : threads) thread.join();
        }
        // Later mutation and freeing the input must not change saved history.
        memset(s->pools[0].workers, 0,
               s->pools[0].worker_count * sizeof(*s->pools[0].workers));
        __cilkrts_sched_stats_free_snapshot(s);
        usleep(10000);
    }
    usleep(30000); // must appear in exit tail, not earlier user snapshots
    return 0;
}
