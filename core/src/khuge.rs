use alloc::{string::String, vec::Vec};
use core::{
    sync::atomic::{AtomicUsize, Ordering},
    time::Duration,
};

use axfs_ng::shrink_page_cache;
use axlog::info;
use axsync::Mutex;
use lazy_static::lazy_static;

use crate::task::{AsThread, tasks};

static TASK_CURSOR: AtomicUsize = AtomicUsize::new(0);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DefragPolicy {
    Always,
    Defer,
    DeferMadvise,
    Madvise,
    Never,
}

impl DefragPolicy {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Always => "always",
            Self::Defer => "defer",
            Self::DeferMadvise => "defer+madvise",
            Self::Madvise => "madvise",
            Self::Never => "never",
        }
    }

    pub fn parse(s: &str) -> Option<Self> {
        match s.trim() {
            "always" => Some(Self::Always),
            "defer" => Some(Self::Defer),
            "defer+madvise" => Some(Self::DeferMadvise),
            "madvise" => Some(Self::Madvise),
            "never" => Some(Self::Never),
            _ => None,
        }
    }
}

/// Configuration and statistics for the khugepaged scanner.
pub struct KhugePagedControl {
    max_ptes_none: usize,
    full_scans: usize,
    pages_collapsed: usize,
    max_ptes_swap: usize,
    max_ptes_shared: usize,
    pages_to_scan: usize,
    alloc_sleep_millisecs: u64,
    scan_sleep_millisecs: u64,
    khugepaged_defrag: DefragPolicy,
}

impl KhugePagedControl {
    /// Creates a new `KhugePagedControl` with default settings.
    pub fn new() -> Self {
        Self {
            max_ptes_none: 1024,
            full_scans: 0,
            pages_collapsed: 0,
            max_ptes_swap: 1024,
            max_ptes_shared: 512,
            pages_to_scan: 4096,
            alloc_sleep_millisecs: 1000,
            scan_sleep_millisecs: 10000,
            khugepaged_defrag: DefragPolicy::Never,
        }
    }

    /// Updates the number of pages to scan in each pass.
    pub fn modify_pages_to_scan(&mut self, new_pages_to_scan: usize) {
        self.pages_to_scan = new_pages_to_scan;
    }

    /// Updates the maximum number of PTE holes allowed when collapsing.
    pub fn modify_max_ptes_none(&mut self, new_max_ptes_none: usize) {
        self.max_ptes_none = new_max_ptes_none;
    }

    /// Updates the total full scan count.
    pub fn modify_full_scans(&mut self, new_full_scans: usize) {
        self.full_scans = new_full_scans;
    }

    /// Updates the maximum swap PTEs threshold.
    pub fn modify_max_ptes_swap(&mut self, new_max_ptes_swap: usize) {
        self.max_ptes_swap = new_max_ptes_swap;
    }

    /// Updates the maximum shared PTEs threshold.
    pub fn modify_max_ptes_shared(&mut self, new_max_ptes_shared: usize) {
        self.max_ptes_shared = new_max_ptes_shared;
    }

    /// Updates defragmentation policy.
    pub fn modify_defrag(&mut self, new_defrag: DefragPolicy) {
        self.khugepaged_defrag = new_defrag;
    }

    /// Updates the sleep interval between scans in milliseconds.
    pub fn modify_scan_sleep_millisecs(&mut self, new_scan_sleep_millisecs: u64) {
        self.scan_sleep_millisecs = new_scan_sleep_millisecs;
    }

    /// Updates the sleep interval after allocation failures in milliseconds.
    pub fn modify_alloc_sleep_millisecs(&mut self, new_alloc_sleep_millisecs: u64) {
        self.alloc_sleep_millisecs = new_alloc_sleep_millisecs;
    }
}

lazy_static! {
    /// Global `khugepaged` control structure.
    pub static ref KHUGEPAGED_CONTROL: Mutex<KhugePagedControl> = Mutex::new(KhugePagedControl::new());
    /// Global THP defragmentation policy (Linux `/transparent_hugepage/defrag`).
    pub static ref THP_DEFRAG_POLICY: Mutex<DefragPolicy> = Mutex::new(DefragPolicy::Madvise);
}

/// Modifies the maximum number of PTE holes allowed.
pub fn modify_max_ptes_none(new_max_ptes_none: usize) {
    KHUGEPAGED_CONTROL.lock().max_ptes_none = new_max_ptes_none;
}

/// Modifies the full scan counter.
pub fn modify_full_scans(new_full_scans: usize) {
    KHUGEPAGED_CONTROL.lock().full_scans = new_full_scans;
}

/// Increments the global collapsed pages counter.
pub fn modify_pages_collapsed(new_pages_collapsed: usize) {
    KHUGEPAGED_CONTROL.lock().pages_collapsed = new_pages_collapsed;
}

/// Modifies the maximum swap PTEs threshold.
pub fn modify_max_ptes_swap(new_max_ptes_swap: usize) {
    KHUGEPAGED_CONTROL.lock().max_ptes_swap = new_max_ptes_swap;
}

/// Modifies the maximum shared PTEs threshold.
pub fn modify_max_ptes_shared(new_max_ptes_shared: usize) {
    KHUGEPAGED_CONTROL.lock().max_ptes_shared = new_max_ptes_shared;
}

/// Updates khugepaged defragmentation policy globally.
pub fn modify_khugepaged_defrag_policy(new_defrag: DefragPolicy) {
    KHUGEPAGED_CONTROL.lock().khugepaged_defrag = new_defrag;
}

pub fn set_khugepaged_defrag_policy(s: &str) -> bool {
    let trimmed = s.trim();
    if trimmed.is_empty() {
        return true;
    }

    let Some(policy) = DefragPolicy::parse(trimmed) else {
        return false;
    };
    modify_khugepaged_defrag_policy(policy);
    true
}

pub fn khugepaged_defrag_policy() -> String {
    KHUGEPAGED_CONTROL.lock().khugepaged_defrag.as_str().into()
}

pub fn set_thp_defrag_policy(s: &str) -> bool {
    let trimmed = s.trim();
    if trimmed.is_empty() {
        return true;
    }

    let Some(policy) = DefragPolicy::parse(trimmed) else {
        return false;
    };
    *THP_DEFRAG_POLICY.lock() = policy;
    true
}

pub fn thp_defrag_policy() -> String {
    THP_DEFRAG_POLICY.lock().as_str().into()
}

/// Modifies the sleep interval between scans in milliseconds.
pub fn modify_scan_sleep_millisecs(new_scan_sleep_millisecs: u64) {
    KHUGEPAGED_CONTROL.lock().scan_sleep_millisecs = new_scan_sleep_millisecs;
}

/// Modifies the sleep interval after allocation failures in milliseconds.
pub fn modify_alloc_sleep_millisecs(new_alloc_sleep_millisecs: u64) {
    KHUGEPAGED_CONTROL.lock().alloc_sleep_millisecs = new_alloc_sleep_millisecs;
}

/// Modifies the number of pages scanned per pass.
pub fn modify_pages_to_scan(new_pages_to_scan: usize) {
    KHUGEPAGED_CONTROL.lock().pages_to_scan = new_pages_to_scan;
}

/// Returns the maximum number of PTE holes allowed.
pub fn max_ptes_none() -> usize {
    KHUGEPAGED_CONTROL.lock().max_ptes_none
}

/// Returns the number of full scans performed by `khugepaged`.
pub fn full_scans() -> usize {
    KHUGEPAGED_CONTROL.lock().full_scans
}

/// Returns the total number of collapsed pages.
pub fn pages_collapsed() -> usize {
    KHUGEPAGED_CONTROL.lock().pages_collapsed
}

/// Returns the maximum swap PTEs threshold.
pub fn max_ptes_swap() -> usize {
    KHUGEPAGED_CONTROL.lock().max_ptes_swap
}

/// Returns the maximum shared PTEs threshold.
pub fn max_ptes_shared() -> usize {
    KHUGEPAGED_CONTROL.lock().max_ptes_shared
}

/// Returns whether defragmentation is enabled.
pub fn defrag() -> bool {
    KHUGEPAGED_CONTROL.lock().khugepaged_defrag != DefragPolicy::Never
}

/// Returns the sleep interval between scans in milliseconds.
pub fn scan_sleep_millisecs() -> u64 {
    KHUGEPAGED_CONTROL.lock().scan_sleep_millisecs
}

/// Returns the sleep interval after allocation failures in milliseconds.
pub fn alloc_sleep_millisecs() -> u64 {
    KHUGEPAGED_CONTROL.lock().alloc_sleep_millisecs
}
/// Returns the number of pages to scan in each pass.
pub fn pages_to_scan() -> usize {
    KHUGEPAGED_CONTROL.lock().pages_to_scan
}

/// Sleeps for the given number of milliseconds.
fn sleep_ms(millis: u64) {
    axtask::sleep(Duration::from_millis(millis));
}

/// Main loop of the `khugepaged` background scanner.
fn khuge_entry() {
    loop {
        let (max_ptes_none, max_ptes_shared, pages_to_scan, alloc_sleep, scan_sleep, defrag) = {
            let control = KHUGEPAGED_CONTROL.lock();
            (
                control.max_ptes_none,
                control.max_ptes_shared,
                control.pages_to_scan,
                control.alloc_sleep_millisecs,
                control.scan_sleep_millisecs,
                control.khugepaged_defrag,
            )
        };

        let cursor = TASK_CURSOR.fetch_add(1, Ordering::Relaxed);
        let user_tasks: Vec<_> = tasks()
            .into_iter()
            .filter(|t| t.try_as_thread().is_some())
            .collect();
        if user_tasks.is_empty() {
            info!("khugepaged tick - no user tasks available");
        } else {
            let idx = (cursor % user_tasks.len()) as usize;
            let task = &user_tasks[idx];
            let thread = task.try_as_thread().unwrap();
            let proc_data = thread.proc_data.clone();
            let pid = proc_data.proc.pid();
            let tid = task.id().as_u64();

            let mut base = 0usize;
            let mut size = 0usize;
            let mut pt_root = 0usize;
            let mut full_scan_finished = false;
            let mut need_alloc_sleep = false;

            {
                let mut aspace = proc_data.aspace.lock();
                base = aspace.base().as_usize();
                size = aspace.size();
                pt_root = aspace.page_table_root().as_usize();

                let mut pages_scanned = 0;
                let mut pages_collapsed = pages_collapsed();
                let mut alloc_fail_retries = 0usize;
                // Page scanning framework: walk PT for collapse opportunities
                while pages_scanned < pages_to_scan {
                    match aspace.try_collapse_page(
                        &mut pages_scanned,
                        max_ptes_none,
                        pages_to_scan,
                        max_ptes_shared,
                        &mut pages_collapsed,
                    ) {
                        axmm::ScanResult::ScanFinished => {
                            full_scan_finished = true;
                            break;
                        }
                        axmm::ScanResult::ScanAllocFailed => {
                            let direct_reclaim = matches!(
                                defrag,
                                DefragPolicy::Always
                                    | DefragPolicy::Madvise
                                    | DefragPolicy::DeferMadvise
                            );
                            if direct_reclaim && alloc_fail_retries < 1 {
                                // Best-effort reclaim to increase the chance of finding a free
                                // 2MiB-contiguous block next time.
                                //
                                // Note: this only evicts unmapped clean page cache pages, so it
                                // is safe to call while holding the address space lock.
                                let _freed = shrink_page_cache(4096);
                                alloc_fail_retries += 1;
                                continue;
                            }
                            need_alloc_sleep = true;
                            break;
                        }
                        _ => {}
                    }
                }
                modify_pages_collapsed(pages_collapsed);
            }

            if need_alloc_sleep {
                sleep_ms(alloc_sleep);
                continue;
            }

            if full_scan_finished {
                modify_full_scans(crate::khuge::full_scans() + 1);
            }

            info!(
                "khugepaged tick - scanning task tid={} pid={:?} aspace=[{:x?}, {:x?}) size={} \
                 pt_root={:x?}",
                tid,
                pid,
                base,
                base + size,
                size,
                pt_root
            );
        }
        sleep_ms(scan_sleep);
    }
}

/// Spawn a khugepaged thread
pub fn spawn_khugepaged() {
    let _handle = axtask::spawn(khuge_entry);
    info!("khugepaged kernel thread spawned");
}
