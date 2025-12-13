use alloc::{sync::Arc, vec::Vec};

use axfs_ng_vfs::{Filesystem, VfsError};
use axmm::backend::{
    current_shmem_thp_policy, current_thp_policy, set_shmem_thp_policy, set_thp_policy,
};
use starry_core::vfs::{
    DirMaker, DirMapping, RwFile, SimpleDir, SimpleFile, SimpleFileOperation, SimpleFs,
};

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

                thp.add(
                    "enabled",
                    SimpleFile::new_regular(
                        fs.clone(),
                        RwFile::new(move |req| match req {
                            SimpleFileOperation::Read => {
                                // Read global THP mode and render it
                                let s = current_thp_policy();
                                Ok(Some(alloc::format!("{}\n", s).into_bytes()))
                            }
                            SimpleFileOperation::Write(data) => {
                                // Parse user input and update global THP mode
                                let s = core::str::from_utf8(data)
                                    .map_err(|_| VfsError::InvalidInput)?
                                    .trim();
                                set_thp_policy(s)?;
                                Ok(None)
                            }
                        }),
                    ),
                );

                thp.add(
                    "shmem_enabled",
                    SimpleFile::new_regular(
                        fs.clone(),
                        RwFile::new(move |req| match req {
                            SimpleFileOperation::Read => {
                                // Read shmem/tmpfs THP mode and render it
                                let s = current_shmem_thp_policy();
                                Ok(Some(alloc::format!("{}\n", s).into_bytes()))
                            }
                            SimpleFileOperation::Write(data) => {
                                // Parse user input and update shmem/tmpfs THP mode
                                let s = core::str::from_utf8(data)
                                    .map_err(|_| VfsError::InvalidInput)?
                                    .trim();
                                set_shmem_thp_policy(s)?;
                                Ok(None)
                            }
                        }),
                    ),
                );
                thp.add("khugepaged", {
                    let mut khugepaged = DirMapping::new();

                    // max_ptes_none
                    khugepaged.add(
                        "max_ptes_none",
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    let val = starry_core::khuge::max_ptes_none();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(data) => {
                                    let s = core::str::from_utf8(data)
                                        .map_err(|_| VfsError::InvalidInput)?
                                        .trim();
                                    let val: usize =
                                        s.parse().map_err(|_| VfsError::InvalidInput)?;
                                    starry_core::khuge::modify_max_ptes_none(val);
                                    Ok(None)
                                }
                            }),
                        ),
                    );

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
                    khugepaged.add(
                        "pages_to_scan",
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    let val = starry_core::khuge::pages_to_scan();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(data) => {
                                    let s = core::str::from_utf8(data)
                                        .map_err(|_| VfsError::InvalidInput)?
                                        .trim();
                                    let val: usize =
                                        s.parse().map_err(|_| VfsError::InvalidInput)?;
                                    starry_core::khuge::modify_pages_to_scan(val);
                                    Ok(None)
                                }
                            }),
                        ),
                    );

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
                    khugepaged.add(
                        "max_ptes_swap",
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    let val = starry_core::khuge::max_ptes_swap();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(data) => {
                                    let s = core::str::from_utf8(data)
                                        .map_err(|_| VfsError::InvalidInput)?
                                        .trim();
                                    let val: usize =
                                        s.parse().map_err(|_| VfsError::InvalidInput)?;
                                    starry_core::khuge::modify_max_ptes_swap(val);
                                    Ok(None)
                                }
                            }),
                        ),
                    );

                    // max_ptes_shared
                    khugepaged.add(
                        "max_ptes_shared",
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    let val = starry_core::khuge::max_ptes_shared();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(data) => {
                                    let s = core::str::from_utf8(data)
                                        .map_err(|_| VfsError::InvalidInput)?
                                        .trim();
                                    let val: usize =
                                        s.parse().map_err(|_| VfsError::InvalidInput)?;
                                    starry_core::khuge::modify_max_ptes_shared(val);
                                    Ok(None)
                                }
                            }),
                        ),
                    );

                    // defrag
                    khugepaged.add(
                        "defrag",
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    let val = starry_core::khuge::defrag();
                                    Ok(Some(
                                        alloc::format!("{}\n", if val { 1 } else { 0 })
                                            .into_bytes(),
                                    ))
                                }
                                SimpleFileOperation::Write(data) => {
                                    let s = core::str::from_utf8(data)
                                        .map_err(|_| VfsError::InvalidInput)?
                                        .trim();
                                    let val: bool = match s {
                                        "0" => false,
                                        _ => true,
                                    };
                                    starry_core::khuge::modify_defrag(val);
                                    Ok(None)
                                }
                            }),
                        ),
                    );

                    // scan_sleep_millisecs
                    khugepaged.add(
                        "scan_sleep_millisecs",
                        SimpleFile::new_regular(
                            fs.clone(),
                            RwFile::new(move |req| match req {
                                SimpleFileOperation::Read => {
                                    let val = starry_core::khuge::scan_sleep_millisecs();
                                    Ok(Some(alloc::format!("{}\n", val).into_bytes()))
                                }
                                SimpleFileOperation::Write(data) => {
                                    let s = core::str::from_utf8(data)
                                        .map_err(|_| VfsError::InvalidInput)?
                                        .trim();
                                    let val: u64 = s.parse().map_err(|_| VfsError::InvalidInput)?;
                                    starry_core::khuge::modify_scan_sleep_millisecs(val);
                                    Ok(None)
                                }
                            }),
                        ),
                    );
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
