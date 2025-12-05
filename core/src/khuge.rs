use alloc::vec::Vec;
use core::sync::atomic::{AtomicUsize, Ordering};

use axhal::time::monotonic_time_nanos;
use axlog::info;
use axsync::Mutex;
use axtask::yield_now;
use lazy_static::lazy_static;

use crate::task::{AsThread, tasks};

static TASK_CURSOR: AtomicUsize = AtomicUsize::new(0);

/// Configuration and statistics for the khugepaged scanner.
pub struct KhugePagedControl {
    max_ptes_none: usize,
    full_scans: usize,
    pages_collapsed: usize,
    max_ptes_swap: usize,
    max_ptes_shared: usize,
    pages_to_scan: usize,
    scan_sleep_millisecs: u64,
    defrag: bool,
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
            scan_sleep_millisecs: 10000,
            defrag: false,
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

    /// Increments the number of collapsed pages.
    pub fn modify_pages_collapsed(&mut self) {
        self.pages_collapsed += 1;
    }

    /// Updates the maximum swap PTEs threshold.
    pub fn modify_max_ptes_swap(&mut self, new_max_ptes_swap: usize) {
        self.max_ptes_swap = new_max_ptes_swap;
    }

    /// Updates the maximum shared PTEs threshold.
    pub fn modify_max_ptes_shared(&mut self, new_max_ptes_shared: usize) {
        self.max_ptes_shared = new_max_ptes_shared;
    }

    /// Enables or disables defragmentation.
    pub fn modify_defrag(&mut self, new_defrag: bool) {
        self.defrag = new_defrag;
    }

    /// Updates the sleep interval between scans in milliseconds.
    pub fn modify_scan_sleep_millisecs(&mut self, new_scan_sleep_millisecs: u64) {
        self.scan_sleep_millisecs = new_scan_sleep_millisecs;
    }
}

lazy_static! {
    /// Global `khugepaged` control structure.
    pub static ref KHUGEPAGED_CONTROL: Mutex<KhugePagedControl> = Mutex::new(KhugePagedControl::new());
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

/// Enables or disables defragmentation globally.
pub fn modify_defrag(new_defrag: bool) {
    KHUGEPAGED_CONTROL.lock().defrag = new_defrag;
}

/// Modifies the sleep interval between scans in milliseconds.
pub fn modify_scan_sleep_millisecs(new_scan_sleep_millisecs: u64) {
    KHUGEPAGED_CONTROL.lock().scan_sleep_millisecs = new_scan_sleep_millisecs;
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
    KHUGEPAGED_CONTROL.lock().defrag
}

/// Returns the sleep interval between scans in milliseconds.
pub fn scan_sleep_millisecs() -> u64 {
    KHUGEPAGED_CONTROL.lock().scan_sleep_millisecs
}
/// Returns the number of pages to scan in each pass.
pub fn pages_to_scan() -> usize {
    KHUGEPAGED_CONTROL.lock().pages_to_scan
}

/// Busy-waits for the given number of milliseconds.
fn sleep_ms(millis: u64) {
    let target_nanos = monotonic_time_nanos() + (millis * 1_000_000);
    while monotonic_time_nanos() < target_nanos {
        yield_now();
    }
}

/// Main loop of the `khugepaged` background scanner.
fn khuge_entry() {
    loop {
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
            let mut aspace = proc_data.aspace.lock();

            let base = aspace.base();
            let size = aspace.size();
            let pt_root = aspace.page_table_root();
            let mut page_scanned = 0;
            let mut pages_collapsed = pages_collapsed();
            // Page scanning framework: walk PT for collapse opportunities
            while page_scanned < pages_to_scan() {
                match aspace.try_collapse_page(
                    &mut page_scanned,
                    max_ptes_none(),
                    pages_to_scan(),
                    max_ptes_shared(),
                    &mut pages_collapsed,
                ) {
                    axmm::ScanResult::ScanFinished | axmm::ScanResult::ScanMmExit => {
                        break;
                    }
                    _ => {}
                }
                modify_pages_collapsed(pages_collapsed);
            }
            // Increment full_scans for this tick's scan
            modify_full_scans(crate::khuge::full_scans() + 1);

            drop(aspace);
            info!(
                "khugepaged tick - scanning task tid={} pid={:?} aspace=[{:x?}, {:x?}) size={} \
                 pt_root={:x?}",
                tid,
                pid,
                base.as_usize(),
                base.as_usize() + size,
                size,
                pt_root.as_usize()
            );
        }
        sleep_ms(scan_sleep_millisecs());
    }
}

/// Spawn a khugepaged thread
pub fn spawn_khugepaged() {
    let _handle = axtask::spawn(khuge_entry);
    info!("khugepaged kernel thread spawned");
}
