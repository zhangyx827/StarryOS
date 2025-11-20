use alloc::sync::Arc;
use alloc::vec::Vec;
use axfs_ng_vfs::{Filesystem, VfsError};
use axmm::backend::{current_ano_policy, modify_ano_policy};
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
                                let s = current_ano_policy();
                                Ok(Some(s.into_bytes()))
                            }
                            SimpleFileOperation::Write(data) => {
                                // Parse user input and update global THP mode
                                let s = core::str::from_utf8(data)
                                    .map_err(|_| VfsError::InvalidInput)?
                                    .trim();
                                modify_ano_policy(s)?;
                                Ok(None)
                            }
                        }),
                    ),
                );

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

            graphics.add("fb0", {
                let mut fb0 = DirMapping::new();

                fb0.add(
                    "enabled",
                    SimpleFile::new_regular(
                        fs.clone(),
                        RwFile::new(move |req| match req {
                            SimpleFileOperation::Read => Ok(Some(Vec::new())),
                            SimpleFileOperation::Write(data) => {
                                // Parse user input and update global THP mode
                                Ok(None)
                            }
                        }),
                    ),
                );

                SimpleDir::new_maker(fs.clone(), Arc::new(fb0))
            });

            SimpleDir::new_maker(fs.clone(), Arc::new(graphics))
        });

        SimpleDir::new_maker(fs.clone(), Arc::new(class))
    });

    SimpleDir::new_maker(fs, Arc::new(root))
}
