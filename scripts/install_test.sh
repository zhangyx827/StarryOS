#!/bin/sh
# Helper script to build and install user tests into the RISC-V rootfs.
# Usage (from repo root):
#   scripts/install_test.sh thp_sysfs_test
#   scripts/install_test.sh thp_sysfs_test.c thp_madvise_test thp_collapse_test.c
#   scripts/install_test.sh ./user-tests/thp_sysfs_test.c
# Run this on the host from the repo root, with riscv64-linux-gnu-gcc installed.

set -e

ROOTFS_IMG="./arceos/disk.img"
MOUNT_POINT="/mnt/starry-rootfs"
CC="${CC:-riscv64-linux-gnu-gcc}"

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 TEST_NAME[.c] ..." >&2
    exit 1
fi

echo "[*] Building user tests with ${CC}..."

# Build all requested tests.
for name in "$@"; do
    raw="$name"
    src="$raw"

    # Ensure we have a .c extension.
    case "$src" in
        *.c)
            ;;
        *)
            src="${src}.c"
            ;;
    esac

    # Determine source path: if caller gave a path, use it; otherwise assume user-tests/.
    case "$src" in
        */*)
            src_path="${src}"
            ;;
        *)
            src_path="user-tests/${src}"
            ;;
    esac

    # Program name = basename without .c.
    prog="${src_path##*/}"
    prog="${prog%.c}"

    # Put the built binary under user-tests/ to match existing layout.
    out_path="user-tests/${prog}"

    if [ ! -f "${src_path}" ]; then
        echo "[-] ${src_path} not found." >&2
        exit 1
    fi

    echo "[*] Building ${src_path} -> ${out_path}..."
    ${CC} -g -static "${src_path}" -o "${out_path}"
done

if [ ! -f "${ROOTFS_IMG}" ]; then
    echo "[-] ${ROOTFS_IMG} not found. Run 'make img' first." >&2
    exit 1
fi

echo "[*] Mounting ${ROOTFS_IMG} at ${MOUNT_POINT}..."
sudo mkdir -p "${MOUNT_POINT}"
sudo mount -o loop "${ROOTFS_IMG}" "${MOUNT_POINT}"

echo "[*] Installing tests into /bin inside rootfs..."
for name in "$@"; do
    raw="$name"
    src="$raw"

    # Ensure we have a .c extension.
    case "$src" in
        *.c)
            ;;
        *)
            src="${src}.c"
            ;;
    esac

    # Determine source path: if caller gave a path, use it; otherwise assume user-tests/.
    case "$src" in
        */*)
            src_path="${src}"
            ;;
        *)
            src_path="user-tests/${src}"
            ;;
    esac

    # Program name = basename without .c.
    prog="${src_path##*/}"
    prog="${prog%.c}"

    bin_src="user-tests/${prog}"
    bin_dst="${MOUNT_POINT}/bin/${prog}"

    if [ ! -f "${bin_src}" ]; then
        echo "[-] ${bin_src} not found (build step failed?)." >&2
        sudo umount "${MOUNT_POINT}" || true
        exit 1
    fi

    echo "    ${bin_src} -> ${bin_dst}"
    sudo cp "${bin_src}" "${bin_dst}"
    sudo chmod +x "${bin_dst}"
done

echo "[*] Unmounting..."
sudo umount "${MOUNT_POINT}"

echo "[*] Done. Rebuild/run StarryOS and run tests from /bin inside the guest."
