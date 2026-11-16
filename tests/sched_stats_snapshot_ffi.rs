#![feature(cilk)]
use std::ffi::{c_char, c_int, CString};

#[repr(C)]
struct Boss { waiting_ns: u64, wait_count: u64, pending_wait_ns: u64, active: u8 }
#[repr(C)]
struct Worker {
    worker_id: u64, times_ns: [u64; 8], counts: [u64; 8], steals: u64,
    repossessions: u64, reengagement_requests: u64, one_sentinel_requests: u64,
    observed_ns: u64, active: u8,
}
#[repr(C)]
struct Pool { pool_id: u64, worker_count: usize, workers: *mut Worker, boss: Boss }
#[repr(C)]
struct Snapshot { pool_count: usize, pools: *mut Pool }
unsafe extern "C" {
    fn __cilkrts_sched_stats_snapshot_and_reset(out: *mut *mut Snapshot) -> c_int;
    fn __cilkrts_sched_stats_record_snapshots(s: *const Snapshot, id: *const c_char) -> c_int;
    fn __cilkrts_sched_stats_free_snapshot(s: *mut Snapshot);
}

#[inline(never)]
fn fib(n: u32) -> u64 {
    if n < 2 { return n as u64; }
    let a = cilk_spawn { fib(n - 1) };
    let b = fib(n - 2);
    cilk_sync;
    a + b
}

fn main() {
    unsafe {
        let mut s = std::ptr::null_mut();
        assert_eq!(__cilkrts_sched_stats_snapshot_and_reset(&mut s), 0);
        __cilkrts_sched_stats_free_snapshot(s);
        assert_eq!(fib(std::hint::black_box(20)), 6765);
        assert_eq!(__cilkrts_sched_stats_snapshot_and_reset(&mut s), 0);
        assert!(!s.is_null());
        assert_eq!((*s).pool_count, 1);
        let pool = &*(*s).pools;
        assert_eq!(pool.pool_id, 0);
        assert_eq!(pool.worker_count, 4);
        for worker in std::slice::from_raw_parts(pool.workers, pool.worker_count) {
            assert_eq!(worker.times_ns.iter().sum::<u64>(), worker.observed_ns);
        }
        let id = CString::new("rust-ffi/iteration/1").unwrap();
        assert_eq!(__cilkrts_sched_stats_record_snapshots(s, id.as_ptr()), 0);
        __cilkrts_sched_stats_free_snapshot(s);
    }
}
