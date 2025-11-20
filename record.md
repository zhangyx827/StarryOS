The THP behaviour is controlled via sysfs interface and using madvise(2) and prctl(2) system calls.

- 代码里没有：
    - 统一的 LRU 链表、kswapd 线程、watermark、水位触发回收；
    - 也没有 swap 设备；Anonymous 页不会被写出到磁盘。
- 物理内存用完时：
    - alloc_pages 直接失败 → 返回错误，一路上传成 AxError::NoMemory / ENOMEM；
    - 不会自动去扫描其它进程的页、回收“冷页”来满足新分配。