/* Unified scheduler accounting, snapshots, and JSON history. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cilk-internal.h"
#include "global.h"
#include "sched_stats.h"
#include <cilk/sched_stats.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef __cilkrts_sched_stats_worker_snapshot worker_snapshot;
typedef __cilkrts_sched_stats_pool_snapshot pool_snapshot;
typedef __cilkrts_sched_stats_snapshot snapshots;
typedef __cilkrts_sched_stats_boss_snapshot boss_snapshot;

void __cilkrts_sched_stats_free_snapshot(snapshots *s) {
    if (!s) return;
    for (size_t i = 0; i < s->pool_count; ++i) free(s->pools[i].workers);
    free(s->pools);
    free(s);
}

#if !CILK_STATS
int __cilkrts_sched_stats_snapshot_and_reset(snapshots **out) {
    if (!out) return EINVAL;
    *out = NULL;
    return ENOTSUP;
}
int __cilkrts_sched_stats_record_snapshots(const snapshots *s, const char *id) {
    (void)s; (void)id;
    return ENOTSUP;
}
#else
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#define CAT_COUNT CILK_STATS_CATEGORY_COUNT

struct timeline {
    pthread_mutex_t lock;
    worker_snapshot value;
    uint64_t since, captured_at;
    int state, error;
};

struct boss_timeline {
    pthread_mutex_t lock;
    boss_snapshot value;
    uint64_t since, exit_at, captured_at;
    bool active;
    int error;
};

struct cilk_snapshot_pool {
    uint64_t id;
    size_t worker_count, capacity;
    struct timeline *rows;
    struct boss_timeline boss;
    struct cilk_snapshot_pool *next;
};

struct saved_snapshot {
    pool_snapshot value;
    worker_snapshot total;
    char *id; /* NULL is the automatic exit ID; user "exit" stays distinct. */
    struct saved_snapshot *next;
};

struct saved_pool {
    uint64_t id;
    struct saved_snapshot *head, *tail;
    struct saved_pool *next;
};

/* Lock order: registry, worker rows, boss row. Runtime event hooks take only
 * their row lock; they never acquire registry or call user code while locked. */
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static struct cilk_snapshot_pool *pools, **pool_tail = &pools;
static struct saved_pool *history;
static size_t pool_count;
static int tracking_error, directory_error;
static bool configured, closed;
static char *directory;

static uint64_t tick(int *error) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        *error = errno; return 0;
    }
    if (ts.tv_sec < 0 || (uint64_t)ts.tv_sec > (UINT64_MAX - ts.tv_nsec) / UINT64_C(1000000000)) {
        *error = EOVERFLOW; return 0;
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static int add(uint64_t *a, uint64_t b) {
    if (b > UINT64_MAX - *a) return EOVERFLOW;
    *a += b;
    return 0;
}

static int interval(worker_snapshot *s, int category, uint64_t since, uint64_t now) {
    if (category < 0) return 0;
    if (now < since) return EOVERFLOW;
    int error;
    if ((error = add(&s->times_ns[category], now - since)) ||
        (error = add(&s->counts[category], 1)) ||
        (error = add(&s->observed_ns, now - since))) return error;
    return 0;
}

static void close_interval(struct timeline *r, uint64_t now, int category) {
    int error = interval(&r->value, category, r->since, now);
    if (error) r->error = error;
    r->since = now;
}

static struct timeline *worker_row(__cilkrts_worker *w) {
    if (!w || !w->g->stats) return NULL;
    return &w->g->stats->rows[w->self];
}

void cilk_snapshot_start(__cilkrts_worker *w, int t) {
    struct timeline *r = worker_row(w);
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    /* Retrying a pending search continues the same interval. Changing states
     * closes the previous interval, including an abandoned pending search. */
    if (r->state != t) {
        uint64_t now = tick(&r->error);
        close_interval(r, now, r->state);
        r->state = t;
    }
    pthread_mutex_unlock(&r->lock);
}

void cilk_snapshot_stop(__cilkrts_worker *w, int t) {
    struct timeline *r = worker_row(w);
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    if (r->state == t) {
        uint64_t now = tick(&r->error);
        close_interval(r, now, t);
        r->state = -1;
    }
    pthread_mutex_unlock(&r->lock);
}

void cilk_snapshot_search_finish(__cilkrts_worker *w, bool found) {
    struct timeline *r = worker_row(w);
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    if (r->state == INTERVAL_PENDING_SEARCH) {
        uint64_t now = tick(&r->error);
        // Only the post-reset portion is classified; saved prefixes stay pending.
        close_interval(r, now, found ? INTERVAL_SCHED : INTERVAL_IDLE);
        r->state = -1;
    }
    pthread_mutex_unlock(&r->lock);
}

void cilk_snapshot_add(__cilkrts_worker *w, enum cilk_snapshot_counter c, uint64_t n) {
    struct timeline *r = worker_row(w);
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    uint64_t *target = NULL;
    switch (c) {
    case SNAPSHOT_STEALS: target = &r->value.steals; break;
    case SNAPSHOT_REPOSSESSIONS: target = &r->value.repossessions; break;
    case SNAPSHOT_REENGAGEMENT_REQUESTS: target = &r->value.reengagement_requests; break;
    case SNAPSHOT_ONE_SENTINEL_REQUESTS: target = &r->value.one_sentinel_requests; break;
    }
    int error = target ? add(target, n) : EINVAL;
    if (error) r->error = error;
    pthread_mutex_unlock(&r->lock);
}

void cilk_snapshot_boss_start(global_state *g) {
    if (!g->stats) return;
    struct boss_timeline *b = &g->stats->boss;
    pthread_mutex_lock(&b->lock);
    b->since = tick(&b->error);
    b->active = true;
    pthread_mutex_unlock(&b->lock);
}

void cilk_snapshot_worker_exit(global_state *g) {
    if (!g->stats) return;
    struct boss_timeline *b = &g->stats->boss;
    pthread_mutex_lock(&b->lock);
    b->exit_at = tick(&b->error);
    pthread_mutex_unlock(&b->lock);
}

void cilk_snapshot_boss_stop(global_state *g) {
    if (!g->stats) return;
    struct boss_timeline *b = &g->stats->boss;
    pthread_mutex_lock(&b->lock);
    if (b->active) {
        uint64_t now = tick(&b->error);
        uint64_t start = b->since > b->exit_at ? b->since : b->exit_at;
        int error = now < start ? EOVERFLOW : add(&b->value.waiting_ns, now - start);
        if (!error) error = add(&b->value.wait_count, 1);
        if (error) b->error = error;
    }
    b->active = false;
    b->exit_at = 0;
    pthread_mutex_unlock(&b->lock);
}

static void configure_directory(void) {
    if (configured) return;
    configured = true;
    const char *value = getenv("CHEETAH_STATS_DIR");
#ifdef CHEETAH_STATS_DIR_DEFAULT
    if (!value) value = CHEETAH_STATS_DIR_DEFAULT;
#endif
    if (!value) value = ".";
    if (!*value) { directory_error = EINVAL; return; }
    if (*value == '/') {
        directory = strdup(value);
    } else {
        char *cwd = getcwd(NULL, 0);
        if (!cwd) { directory_error = errno; return; }
        if (asprintf(&directory, "%s/%s", cwd, value) < 0) directory = NULL;
        free(cwd);
    }
    if (!directory) directory_error = ENOMEM;
}

void cilk_snapshot_pool_init(global_state *g) {
    pthread_mutex_lock(&registry);
    configure_directory();
    struct cilk_snapshot_pool *p = calloc(1, sizeof(*p));
    if (!p) { tracking_error = ENOMEM; goto done; }
    p->capacity = p->worker_count = g->nworkers;
    p->rows = calloc(p->capacity, sizeof(*p->rows));
    if (!p->rows) { free(p); tracking_error = ENOMEM; goto done; }
    size_t initialized = 0;
    int error = 0;
    for (; initialized < p->capacity; ++initialized) {
        error = pthread_mutex_init(&p->rows[initialized].lock, NULL);
        if (error) break;
        p->rows[initialized].state = -1;
    }
    if (!error) error = pthread_mutex_init(&p->boss.lock, NULL);
    if (error) {
        while (initialized) pthread_mutex_destroy(&p->rows[--initialized].lock);
        free(p->rows); free(p); tracking_error = error; goto done;
    }
    p->id = pool_count++;
    *pool_tail = p;
    pool_tail = &p->next;
    g->stats = p;
done:
    pthread_mutex_unlock(&registry);
}

void cilk_snapshot_pool_resize(global_state *g, unsigned n) {
    pthread_mutex_lock(&registry);
    if (g->stats && n <= g->stats->capacity)
        g->stats->worker_count = n;
    pthread_mutex_unlock(&registry);
}

void cilk_snapshot_pool_stop(global_state *g) {
    struct cilk_snapshot_pool *p = g->stats;
    if (!p) return;
    pthread_mutex_lock(&registry);
    for (size_t i = 0; i < p->worker_count; ++i) {
        struct timeline *r = &p->rows[i];
        pthread_mutex_lock(&r->lock);
        close_interval(r, tick(&r->error), r->state);
        r->state = -1;
        pthread_mutex_unlock(&r->lock);
    }
    pthread_mutex_unlock(&registry);
    /* Retain the rows after global_state and workers are freed. */
}

static int capture_boss(struct boss_timeline *b, boss_snapshot *s) {
    *s = b->value;
    s->active = b->active;
    b->captured_at = tick(&b->error);
    if (b->error) return b->error;
    if (!b->active) return 0;
    if (b->captured_at < b->since) return EOVERFLOW;
    if (!b->exit_at) {
        s->pending_wait_ns = b->captured_at - b->since;
        return 0;
    }
    uint64_t start = b->since > b->exit_at ? b->since : b->exit_at;
    if (b->captured_at < start) return EOVERFLOW;
    int error = add(&s->waiting_ns, b->captured_at - start);
    return error ? error : add(&s->wait_count, 1);
}

/* registry is held. Allocate and validate everything before resetting any
 * row. Locking rows prevents data races with lazy worker startup and searches.
 * Timestamps remain per-row, not a simultaneous scheduler barrier. */
static int collect(bool reset, snapshots **out) {
    *out = NULL;
    if (tracking_error) return tracking_error;
    snapshots *s = calloc(1, sizeof(*s));
    if (!s) return ENOMEM;
    s->pool_count = pool_count;
    s->pools = calloc(pool_count ? pool_count : 1, sizeof(*s->pools));
    if (!s->pools) { free(s); return ENOMEM; }
    size_t index = 0;
    for (struct cilk_snapshot_pool *p = pools; p; p = p->next, ++index) {
        s->pools[index].pool_id = p->id;
        s->pools[index].worker_count = p->worker_count;
        s->pools[index].workers = calloc(p->worker_count ? p->worker_count : 1, sizeof(worker_snapshot));
        if (!s->pools[index].workers) { __cilkrts_sched_stats_free_snapshot(s); return ENOMEM; }
    }
    for (struct cilk_snapshot_pool *p = pools; p; p = p->next) {
        for (size_t i = 0; i < p->worker_count; ++i) pthread_mutex_lock(&p->rows[i].lock);
        pthread_mutex_lock(&p->boss.lock);
    }
    int error = 0;
    index = 0;
    for (struct cilk_snapshot_pool *p = pools; p && !error; p = p->next, ++index) {
        for (size_t i = 0; i < p->worker_count; ++i) {
            struct timeline *r = &p->rows[i];
            worker_snapshot *w = &s->pools[index].workers[i];
            *w = r->value;
            w->worker_id = i;
            w->active = r->state >= 0;
            r->captured_at = tick(&r->error);
            error = r->error ? r->error : interval(w, r->state, r->since, r->captured_at);
            if (error) break;
        }
        if (!error) error = capture_boss(&p->boss, &s->pools[index].boss);
    }
    for (struct cilk_snapshot_pool *p = pools; p; p = p->next) {
        for (size_t i = 0; i < p->worker_count; ++i) {
            struct timeline *r = &p->rows[i];
            if (!error && reset) {
                memset(&r->value, 0, sizeof(r->value));
                r->since = r->captured_at;
            }
            pthread_mutex_unlock(&r->lock);
        }
        if (!error && reset) {
            memset(&p->boss.value, 0, sizeof(p->boss.value));
            if (p->boss.active) p->boss.since = p->boss.captured_at;
        }
        pthread_mutex_unlock(&p->boss.lock);
    }
    if (error) __cilkrts_sched_stats_free_snapshot(s);
    else *out = s;
    return error;
}

static int allowed_caller(void) {
#if defined(__linux__) && defined(SYS_gettid)
    if (syscall(SYS_gettid) != getpid()) return EPERM;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
    if (!pthread_main_np()) return EPERM;
#else
    return ENOTSUP;
#endif
    return __cilkrts_running_on_workers() ? EPERM : 0;
}

int __cilkrts_sched_stats_snapshot_and_reset(snapshots **out) {
    if (!out) return EINVAL;
    *out = NULL;
    int error = allowed_caller();
    if (error) return error;
    pthread_mutex_lock(&registry);
    error = closed ? EPIPE : collect(true, out);
    pthread_mutex_unlock(&registry);
    return error;
}

static bool valid_utf8(const unsigned char *s) {
    while (*s) {
        unsigned c = *s++, n, lower = 0x80, upper = 0xbf;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) n = 1;
        else if (c >= 0xe0 && c <= 0xef) {
            n = 2; if (c == 0xe0) lower = 0xa0; if (c == 0xed) upper = 0x9f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            n = 3; if (c == 0xf0) lower = 0x90; if (c == 0xf4) upper = 0x8f;
        } else return false;
        if (*s < lower || *s > upper) return false;
        ++s;
        while (--n) { if (*s < 0x80 || *s > 0xbf) return false; ++s; }
    }
    return true;
}

static int add_worker(worker_snapshot *a, const worker_snapshot *b) {
    int error;
    for (int i = 0; i < CAT_COUNT; ++i) {
        if ((error = add(&a->times_ns[i], b->times_ns[i])) ||
            (error = add(&a->counts[i], b->counts[i]))) return error;
    }
    if ((error = add(&a->steals, b->steals)) ||
        (error = add(&a->repossessions, b->repossessions)) ||
        (error = add(&a->reengagement_requests, b->reengagement_requests)) ||
        (error = add(&a->one_sentinel_requests, b->one_sentinel_requests)) ||
        (error = add(&a->observed_ns, b->observed_ns))) return error;
    a->active |= b->active;
    return 0;
}

static void free_history(struct saved_pool *groups) {
    while (groups) {
        struct saved_pool *next = groups->next;
        struct saved_snapshot *s = groups->head;
        while (s) {
            struct saved_snapshot *after = s->next;
            free(s->value.workers); free(s->id); free(s); s = after;
        }
        free(groups); groups = next;
    }
}

/* Prepare allocations and validate totals before taking the recorder lock. */
static int prepare_records(const snapshots *input, const char *id, struct saved_pool **out) {
    *out = NULL;
    if (!input || (input->pool_count && !input->pools)) return EINVAL;
    struct saved_pool **tail = out;
    int error = ENOMEM;
    for (size_t i = 0; i < input->pool_count; ++i) {
        const pool_snapshot *p = &input->pools[i];
        if (p->worker_count && !p->workers) { error = EINVAL; goto fail; }
        if (p->worker_count > SIZE_MAX / sizeof(worker_snapshot)) { error = EOVERFLOW; goto fail; }
        struct saved_pool *group = calloc(1, sizeof(*group));
        if (!group) goto fail;
        *tail = group; tail = &group->next; group->id = p->pool_id;
        struct saved_snapshot *s = calloc(1, sizeof(*s));
        if (!s) goto fail;
        group->head = group->tail = s;
        s->value = *p;
        s->value.workers = calloc(p->worker_count ? p->worker_count : 1, sizeof(worker_snapshot));
        if (!s->value.workers) goto fail;
        if (id && !(s->id = strdup(id))) goto fail;
        if (p->worker_count) memcpy(s->value.workers, p->workers, p->worker_count * sizeof(worker_snapshot));
        for (size_t j = 0; j < p->worker_count; ++j) {
            int overflow = add_worker(&s->total, &p->workers[j]);
            if (overflow) { error = overflow; goto fail; }
        }
    }
    return 0;
fail:
    free_history(*out); *out = NULL; return error;
}

static void append_records(struct saved_pool *ready) {
    while (ready) {
        struct saved_pool *next = ready->next;
        struct saved_pool **slot = &history;
        while (*slot && (*slot)->id < ready->id) slot = &(*slot)->next;
        if (*slot && (*slot)->id == ready->id) {
            (*slot)->tail->next = ready->head;
            (*slot)->tail = ready->tail;
            free(ready);
        } else {
            ready->next = *slot; *slot = ready;
        }
        ready = next;
    }
}

int __cilkrts_sched_stats_record_snapshots(const snapshots *s, const char *id) {
    if (!id || !valid_utf8((const unsigned char *)id)) return EINVAL;
    struct saved_pool *ready;
    int error = prepare_records(s, id, &ready);
    if (error) return error;
    pthread_mutex_lock(&registry);
    configure_directory();
    if (closed) error = EPIPE;
    else append_records(ready);
    pthread_mutex_unlock(&registry);
    if (error) free_history(ready);
    return error;
}

static const char *names[CAT_COUNT] = {
    "working", "scheduling", "idling", "sleep", "sleep_uncilk",
    "cilkify_enter", "cilkify_exit", "pending_search"
};

static void json_string(FILE *fp, const char *s) {
    fputc('"', fp);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') { fputc('\\', fp); fputc(*p, fp); }
        else if (*p < 0x20) fprintf(fp, "\\u%04x", *p);
        else fputc(*p, fp);
    }
    fputc('"', fp);
}

static void json_metrics(FILE *fp, const worker_snapshot *s) {
    fputs("\"times_ns\":{", fp);
    for (int i = 0; i < CAT_COUNT; ++i)
        fprintf(fp, "%s\"%s\":%" PRIu64, i ? "," : "", names[i], s->times_ns[i]);
    fputs("},\"counts\":{", fp);
    for (int i = 0; i < CAT_COUNT; ++i)
        fprintf(fp, "%s\"%s\":%" PRIu64, i ? "," : "", names[i], s->counts[i]);
    fprintf(fp, "},\"steals\":%" PRIu64 ",\"repossessions\":%" PRIu64
            ",\"reengagement_requests\":%" PRIu64 ",\"one_sentinel_requests\":%" PRIu64
            ",\"observed_ns\":%" PRIu64, s->steals, s->repossessions,
            s->reengagement_requests, s->one_sentinel_requests, s->observed_ns);
}

static void json_report(FILE *fp, const struct saved_pool *groups) {
    fputs("[\n", fp);
    for (const struct saved_pool *g = groups; g; g = g->next) {
        fprintf(fp, "  {\"pool_id\":%" PRIu64 ",\"snapshots\":[\n", g->id);
        for (const struct saved_snapshot *s = g->head; s; s = s->next) {
            fputs("    {\"id\":{\"kind\":", fp);
            if (s->id) { fputs("\"user\",\"value\":", fp); json_string(fp, s->id); }
            else fputs("\"exit\"", fp);
            fputs("},\"workers\":[", fp);
            for (size_t i = 0; i < s->value.worker_count; ++i) {
                const worker_snapshot *w = &s->value.workers[i];
                fprintf(fp, "%s{\"worker_id\":%" PRIu64 ",\"stats\":{", i ? "," : "", w->worker_id);
                json_metrics(fp, w);
                fprintf(fp, ",\"active\":%s}}", w->active ? "true" : "false");
            }
            fputs("],\"total\":{", fp); json_metrics(fp, &s->total);
            fprintf(fp, ",\"worker_count\":%zu,\"any_worker_active\":%s},",
                    s->value.worker_count, s->total.active ? "true" : "false");
            const boss_snapshot *b = &s->value.boss;
            fprintf(fp, "\"boss\":{\"waiting_ns\":%" PRIu64 ",\"wait_count\":%" PRIu64
                    ",\"pending_wait_ns\":%" PRIu64 ",\"active\":%s}}%s\n",
                    b->waiting_ns, b->wait_count, b->pending_wait_ns,
                    b->active ? "true" : "false", s->next ? "," : "");
        }
        fprintf(fp, "  ]}%s\n", g->next ? "," : "");
    }
    fputs("]\n", fp);
}

static int mkdir_one(const char *path) {
    if (mkdir(path, 0777) == 0) return 0;
    if (errno != EEXIST) return errno;
    struct stat st;
    if (stat(path, &st)) return errno;
    return S_ISDIR(st.st_mode) ? 0 : ENOTDIR;
}

static int mkdir_all(const char *path) {
    char *copy = strdup(path);
    if (!copy) return ENOMEM;
    int error = 0;
    for (char *p = copy + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0; error = mkdir_one(copy); *p = '/';
        if (error) break;
    }
    if (!error) error = mkdir_one(copy);
    free(copy);
    return error;
}

static int write_report(const struct saved_pool *groups) {
    int error = directory_error;
    if (!error) error = mkdir_all(directory);
    if (error) return error;
    char *path = NULL;
    int fd;
    for (;;) {
        struct timespec now;
        struct tm utc;
        if (clock_gettime(CLOCK_REALTIME, &now)) return errno;
        if (!gmtime_r(&now.tv_sec, &utc)) return EOVERFLOW;
        char stamp[32];
        if (!strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &utc)) return EOVERFLOW;
        if (asprintf(&path, "%s/cheetah_stats_%s_%06ld.json", directory, stamp, now.tv_nsec / 1000) < 0)
            return ENOMEM;
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) { error = 0; break; }
        error = errno; free(path); path = NULL;
        if (error != EEXIST) return error;
        sched_yield();
    }
    FILE *fp = fdopen(fd, "w");
    if (!fp) { error = errno; close(fd); unlink(path); free(path); return error; }
    errno = 0;
    json_report(fp, groups);
    if (ferror(fp)) error = errno ? errno : EIO;
    if (fclose(fp) && !error) error = errno;
    if (error) unlink(path);
    free(path);
    return error;
}

void cilk_snapshot_write_exit(void) {
    pthread_mutex_lock(&registry);
    if (closed) { pthread_mutex_unlock(&registry); return; }
    closed = true;
    configure_directory();
    snapshots *s = NULL;
    struct saved_pool *ready = NULL;
    int error = collect(false, &s);
    if (!error) error = prepare_records(s, NULL, &ready);
    if (!error) append_records(ready);
    __cilkrts_sched_stats_free_snapshot(s);
    struct saved_pool *saved = history;
    history = NULL;
    pthread_mutex_unlock(&registry);
    if (!error) error = write_report(saved);
    if (error) fprintf(stderr, "Cheetah sched-stats: failed to write JSON report in %s: %s\n",
                       directory ? directory : "(unavailable directory)", strerror(error));
    free_history(saved);
}
#endif /* CILK_STATS */
