#!/usr/bin/env python3

import argparse
import datetime
import socket
import subprocess
import sys
import threading


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("arch")
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()

    arch = args.arch

    p = subprocess.Popen(
        [
            "make",
            "ARCH=" + arch,
            "ACCEL=n",
            "justrun",
            "QEMU_ARGS=-monitor none -serial tcp::4444,server=on",
        ],
        stderr=subprocess.PIPE,
        text=True,
    )

    ready = threading.Event()

    def worker() -> None:
        assert p.stderr is not None
        for line in p.stderr:
            print(line, file=sys.stderr, end="")
            if "QEMU waiting for connection" in line:
                ready.set()
        ready.set()

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()

    try:
        if not ready.wait(timeout=10):
            raise RuntimeError("QEMU did not start in time")
        if p.poll() is not None:
            raise RuntimeError("QEMU exited prematurely")

        prompt = "starry:~#"
        s = socket.create_connection(("localhost", 4444), timeout=5)
        s.settimeout(5)
        buffer = ""
        sent = False
        start = datetime.datetime.now()

        script = "\r\n".join(
            [
                "set -e",
                "echo __THP_TEST_BEGIN__",
                "cat /sys/kernel/mm/transparent_hugepage/enabled",
                "echo always > /sys/kernel/mm/transparent_hugepage/enabled",
                "cat /sys/kernel/mm/transparent_hugepage/enabled",
                "echo never > /sys/kernel/mm/transparent_hugepage/enabled",
                "cat /sys/kernel/mm/transparent_hugepage/enabled",
                "if echo bad > /sys/kernel/mm/transparent_hugepage/enabled; then echo __UNEXPECTED_OK__; fi",
                "if printf '\\n' > /sys/kernel/mm/transparent_hugepage/enabled; then echo __UNEXPECTED_EMPTY_OK__; fi",
                "cat /sys/kernel/mm/transparent_hugepage/enabled",
                "cat /sys/kernel/mm/transparent_hugepage/shmem_enabled",
                "echo always > /sys/kernel/mm/transparent_hugepage/shmem_enabled",
                "cat /sys/kernel/mm/transparent_hugepage/shmem_enabled",
                "echo advise > /sys/kernel/mm/transparent_hugepage/shmem_enabled",
                "cat /sys/kernel/mm/transparent_hugepage/shmem_enabled",
                "cat /sys/kernel/mm/transparent_hugepage/khugepaged/max_ptes_none",
                "echo 123 > /sys/kernel/mm/transparent_hugepage/khugepaged/max_ptes_none",
                "cat /sys/kernel/mm/transparent_hugepage/khugepaged/max_ptes_none",
                "cat /sys/kernel/mm/transparent_hugepage/khugepaged/full_scans",
                "sleep 1",
                "cat /sys/kernel/mm/transparent_hugepage/khugepaged/full_scans",
                "echo __THP_TEST_OK__",
                "exit",
                "",
            ]
        ).encode("utf-8")

        while True:
            if datetime.datetime.now() - start > datetime.timedelta(seconds=args.timeout):
                raise RuntimeError("Timeout waiting for THP test to finish")

            try:
                b = s.recv(4096).decode("utf-8", errors="ignore")
            except socket.timeout:
                continue
            except ConnectionError as e:
                print(e)
                break

            if not b:
                break

            print(b, end="")
            buffer += b

            if (prompt in buffer) and not sent:
                s.sendall(script)
                sent = True

        if prompt not in buffer:
            raise RuntimeError("Did not reach BusyBox shell prompt")

        if "__THP_TEST_OK__" not in buffer:
            raise RuntimeError("THP sysfs smoke test did not complete")

        if "__UNEXPECTED_OK__" in buffer:
            raise RuntimeError("Invalid THP policy write unexpectedly succeeded")
        if "__UNEXPECTED_EMPTY_OK__" in buffer:
            raise RuntimeError("Empty THP policy write unexpectedly succeeded")

        print()
        print("\x1b[32m✔ THP sysfs smoke test passed\x1b[0m")
        return 0
    finally:
        try:
            p.wait(1)
        except subprocess.TimeoutExpired:
            p.terminate()
            p.wait()


if __name__ == "__main__":
    raise SystemExit(main())
