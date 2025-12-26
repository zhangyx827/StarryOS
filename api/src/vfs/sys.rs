use alloc::{sync::Arc, vec::Vec};
use core::sync::atomic::{AtomicBool, Ordering};

use axfs_ng_vfs::{Filesystem, VfsError};
use axmm::backend::{
    current_shmem_thp_policy, current_thp_policy, set_shmem_thp_policy, set_thp_policy,
};
use starry_core::vfs::{
    DirMaker, DirMapping, RwFile, SimpleDir, SimpleFile, SimpleFileOperation, SimpleFs,
};

enum SysfsWrite<'a> {
    Clear,
    Value(&'a str),
}

fn parse_sysfs_write(data: &[u8]) -> Result<SysfsWrite<'_>, VfsError> {
    let s = core::str::from_utf8(data)
        .map_err(|_| VfsError::InvalidInput)?
        .trim();
    if s.is_empty() {
        return Ok(SysfsWrite::Clear);
    }

    // Some write paths might provide extra trailing bytes (e.g., due to the VFS
    // write implementation composing a new buffer from the previous contents).
    // sysfs-style attributes only care about the first token.
    let Some(tok) = s.split_whitespace().next() else {
        return Ok(SysfsWrite::Clear);
    };
    Ok(SysfsWrite::Value(tok))
}

pub fn new_sysfs() -> Filesystem {
    SimpleFs::new_with("sys".into(), 0x62656572, builder)
}

fn builder(fs: Arc<SimpleFs>) -> DirMaker {
    let mut root = DirMapping::new();
    root.add("kernel", {
        let mut kernel = DirMapping::new();

        kernel.add("mm", {
            let mut mm = DirMapping::new();

            mm.add("transparent_hugepage", {
                let mut thp = DirMapping::new();

                thp.add("defrag", {
                    let cleared = Arc::new(AtomicBool::new(false));
                    SimpleFile::new_regular(
                        fs.clone(),
                        RwFile::new(move |req| match req {
                            SimpleFileOperation::Read => {
                                if cleared.load(Ordering::Relaxed) {
                                    return Ok(Some(Vec::new()));
                                }
                                let val = starry_core::khuge::thp_defrag_policy();
                                Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                            }
                            SimpleFileOperation::Write(data) => match parse_sysfs_write(data)? {
                                SysfsWrite::Clear => {
                                    cleared.store(true, Ordering::Relaxed);
                                    Ok(None)
                                }
                                SysfsWrite::Value(s) => {
                                    cleared.store(false, Ordering::Relaxed);
                                    if !starry_core::khuge::set_thp_defrag_policy(s) {
                                        return Err(VfsError::InvalidInput);
                                    }
                                    Ok(None)
                                }
                            },
                        }),
                    )
                });

                thp.add("enabled", {
                    let cleared = Arc::new(AtomicBool::new(false));
                    SimpleFile::new_regular(
                        fs.clone(),
                        RwFile::new(move |req| match req {
                            SimpleFileOperation::Read => {
                                if cleared.load(Ordering::Relaxed) {
                                    return Ok(Some(Vec::new()));
                                }
                                // Read global THP mode and render it
                                let s = current_thp_policy();
                                Ok(Some(alloc::format!("{}\n", s).into_bytes()))
                            }
                            SimpleFileOperation::Write(data) => match parse_sysfs_write(data)? {
                                SysfsWrite::Clear => {
                                    cleared.store(true, Ordering::Relaxed);
                                    Ok(None)
                                }
                                SysfsWrite::Value(s) => {
                                    cleared.store(false, Ordering::Relaxed);
                                    set_thp_policy(s)?;
                                    Ok(None)
                                }
                            },
                        }),
                    )
                });

                thp.add("shmem_enabled", {
                    let cleared = Arc::new(AtomicBool::new(false));
                    SimpleFile::new_regular(
                        fs.clone(),
                        RwFile::new(move |req| match req {
                            SimpleFileOperation::Read => {
                                if cleared.load(Ordering::Relaxed) {
                                    return Ok(Some(Vec::new()));
                                }
                                // Read shmem/tmpfs THP mode and render it
                                let s = current_shmem_thp_policy();
                                Ok(Some(alloc::format!("{}\n", s).into_bytes()))
                            }
                            SimpleFileOperation::Write(data) => match parse_sysfs_write(data)? {
                                SysfsWrite::Clear => {
                                    cleared.store(true, Ordering::Relaxed);
                                    Ok(None)
                                }
                                SysfsWrite::Value(s) => {
                                    cleared.store(false, Ordering::Relaxed);
                                    set_shmem_thp_policy(s)?;
                                    Ok(None)
                                }
                            },
                        }),
                    )
                });
                thp.add("khugepaged", {
                    let mut khugepaged = DirMapping::new();

                    // max_ptes_none
                    khugepaged.add("max_ptes_none", {
                        let cleared = Arc::new(AtomicBool::new(false));
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    if cleared.load(Ordering::Relaxed) {
                                        return Ok(Some(Vec::new()));
                                    }
                                    let val = starry_core::khuge::max_ptes_none();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(data) => {
                                    match parse_sysfs_write(data)? {
                                        SysfsWrite::Clear => {
                                            cleared.store(true, Ordering::Relaxed);
                                            Ok(None)
                                        }
                                        SysfsWrite::Value(s) => {
                                            cleared.store(false, Ordering::Relaxed);
                                            let val: usize =
                                                s.parse().map_err(|_| VfsError::InvalidInput)?;
                                            starry_core::khuge::modify_max_ptes_none(val);
                                            Ok(None)
                                        }
                                    }
                                }
                            }),
                        )
                    });

                    // full_scans
                    khugepaged.add(
                        "full_scans",
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    let val = starry_core::khuge::full_scans();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(_data) => Ok(None),
                            }),
                        ),
                    );

                    // pages_to_scan
                    khugepaged.add("pages_to_scan", {
                        let cleared = Arc::new(AtomicBool::new(false));
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    if cleared.load(Ordering::Relaxed) {
                                        return Ok(Some(Vec::new()));
                                    }
                                    let val = starry_core::khuge::pages_to_scan();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(data) => {
                                    match parse_sysfs_write(data)? {
                                        SysfsWrite::Clear => {
                                            cleared.store(true, Ordering::Relaxed);
                                            Ok(None)
                                        }
                                        SysfsWrite::Value(s) => {
                                            cleared.store(false, Ordering::Relaxed);
                                            let val: usize =
                                                s.parse().map_err(|_| VfsError::InvalidInput)?;
                                            starry_core::khuge::modify_pages_to_scan(val);
                                            Ok(None)
                                        }
                                    }
                                }
                            }),
                        )
                    });

                    // pages_collapsed (read-only stat)
                    khugepaged.add(
                        "pages_collapsed",
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    let val = starry_core::khuge::pages_collapsed();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(_) => Ok(None),
                            }),
                        ),
                    );

                    // max_ptes_swap
                    khugepaged.add("max_ptes_swap", {
                        let cleared = Arc::new(AtomicBool::new(false));
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    if cleared.load(Ordering::Relaxed) {
                                        return Ok(Some(Vec::new()));
                                    }
                                    let val = starry_core::khuge::max_ptes_swap();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(data) => {
                                    match parse_sysfs_write(data)? {
                                        SysfsWrite::Clear => {
                                            cleared.store(true, Ordering::Relaxed);
                                            Ok(None)
                                        }
                                        SysfsWrite::Value(s) => {
                                            cleared.store(false, Ordering::Relaxed);
                                            let val: usize =
                                                s.parse().map_err(|_| VfsError::InvalidInput)?;
                                            starry_core::khuge::modify_max_ptes_swap(val);
                                            Ok(None)
                                        }
                                    }
                                }
                            }),
                        )
                    });

                    // max_ptes_shared
                    khugepaged.add("max_ptes_shared", {
                        let cleared = Arc::new(AtomicBool::new(false));
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    if cleared.load(Ordering::Relaxed) {
                                        return Ok(Some(Vec::new()));
                                    }
                                    let val = starry_core::khuge::max_ptes_shared();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(data) => {
                                    match parse_sysfs_write(data)? {
                                        SysfsWrite::Clear => {
                                            cleared.store(true, Ordering::Relaxed);
                                            Ok(None)
                                        }
                                        SysfsWrite::Value(s) => {
                                            cleared.store(false, Ordering::Relaxed);
                                            let val: usize =
                                                s.parse().map_err(|_| VfsError::InvalidInput)?;
                                            starry_core::khuge::modify_max_ptes_shared(val);
                                            Ok(None)
                                        }
                                    }
                                }
                            }),
                        )
                    });

                    // defrag
                    khugepaged.add("defrag", {
                        let cleared = Arc::new(AtomicBool::new(false));
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    if cleared.load(Ordering::Relaxed) {
                                        return Ok(Some(Vec::new()));
                                    }
                                    let val = starry_core::khuge::khugepaged_defrag_policy();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(data) => {
                                    match parse_sysfs_write(data)? {
                                        SysfsWrite::Clear => {
                                            cleared.store(true, Ordering::Relaxed);
                                            Ok(None)
                                        }
                                        SysfsWrite::Value(s) => {
                                            cleared.store(false, Ordering::Relaxed);
                                            if !starry_core::khuge::set_khugepaged_defrag_policy(s)
                                            {
                                                return Err(VfsError::InvalidInput);
                                            }
                                            Ok(None)
                                        }
                                    }
                                }
                            }),
                        )
                    });

                    // scan_sleep_millisecs
                    khugepaged.add("scan_sleep_millisecs", {
                        let cleared = Arc::new(AtomicBool::new(false));
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    if cleared.load(Ordering::Relaxed) {
                                        return Ok(Some(Vec::new()));
                                    }
                                    let val = starry_core::khuge::scan_sleep_millisecs();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(data) => {
                                    match parse_sysfs_write(data)? {
                                        SysfsWrite::Clear => {
                                            cleared.store(true, Ordering::Relaxed);
                                            Ok(None)
                                        }
                                        SysfsWrite::Value(s) => {
                                            cleared.store(false, Ordering::Relaxed);
                                            let val: u64 =
                                                s.parse().map_err(|_| VfsError::InvalidInput)?;
                                            starry_core::khuge::modify_scan_sleep_millisecs(val);
                                            Ok(None)
                                        }
                                    }
                                }
                            }),
                        )
                    });

                    // alloc_sleep_millisecs
                    khugepaged.add("alloc_sleep_millisecs", {
                        let cleared = Arc::new(AtomicBool::new(false));
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    if cleared.load(Ordering::Relaxed) {
                                        return Ok(Some(Vec::new()));
                                    }
                                    let val = starry_core::khuge::alloc_sleep_millisecs();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(data) => {
                                    match parse_sysfs_write(data)? {
                                        SysfsWrite::Clear => {
                                            cleared.store(true, Ordering::Relaxed);
                                            Ok(None)
                                        }
                                        SysfsWrite::Value(s) => {
                                            cleared.store(false, Ordering::Relaxed);
                                            let val: u64 =
                                                s.parse().map_err(|_| VfsError::InvalidInput)?;
                                            starry_core::khuge::modify_alloc_sleep_millisecs(val);
                                            Ok(None)
                                        }
                                    }
                                }
                            }),
                        )
                    });
                    SimpleDir::new_maker(fs.clone(), Arc::new(khugepaged))
                });

                SimpleDir::new_maker(fs.clone(), Arc::new(thp))
            });

            SimpleDir::new_maker(fs.clone(), Arc::new(mm))
        });

        SimpleDir::new_maker(fs.clone(), Arc::new(kernel))
    });

    root.add("class", {
        let mut class = DirMapping::new();

        class.add("graphics", {
            let mut graphics = DirMapping::new();

            graphics.add(
                "fb0",
                SimpleFile::new_regular(
                    fs.clone(),
                    RwFile::new(move |req| match req {
                        SimpleFileOperation::Read => Ok(Some(Vec::new())),
                        SimpleFileOperation::Write(_data) => Ok(None),
                    }),
                ),
            );
            SimpleDir::new_maker(fs.clone(), Arc::new(graphics))
        });

        SimpleDir::new_maker(fs.clone(), Arc::new(class))
    });

    SimpleDir::new_maker(fs, Arc::new(root))
}
