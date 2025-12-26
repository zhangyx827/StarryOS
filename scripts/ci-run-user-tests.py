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
    parser.add_argument("tests", nargs="+", help="Test program names under /bin")
    parser.add_argument("--timeout", type=int, default=120, help="Total timeout seconds")
    args = parser.parse_args()

    p = subprocess.Popen(
        [
            "make",
            "ARCH=" + args.arch,
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

    start = datetime.datetime.now()
    try:
        if not ready.wait(timeout=10):
            raise RuntimeError("QEMU did not start in time")
        if p.poll() is not None:
            raise RuntimeError("QEMU exited prematurely")

        prompt = "starry:~#"
        s = socket.create_connection(("localhost", 4444), timeout=5)
        s.settimeout(0.5)

        buffer = ""
        sent = False
        failures: list[str] = []
        missing: list[str] = []

        script_lines = ["set +e", "cd /", "echo __TESTS_BEGIN__"]
        for t in args.tests:
            script_lines += [
                f"echo __RUN__{t}__",
                f"if [ ! -x /bin/{t} ]; then echo __MISSING__{t}__; exit 127; fi",
                f"/bin/{t}",
                f"rc=$?",
                f"echo __RC__{t}__${{rc}}",
            ]
        script_lines += ["echo __TESTS_OK__", "exit"]
        script = ("\r\n".join(script_lines) + "\r\n").encode("utf-8")

        while True:
            if datetime.datetime.now() - start > datetime.timedelta(seconds=args.timeout):
                raise RuntimeError("Timeout waiting for tests to finish")

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

        for t in args.tests:
            if f"__MISSING__{t}__" in buffer:
                missing.append(t)
                continue
            if f"__RUN__{t}__" not in buffer:
                failures.append(f"{t}: not executed")
                continue
            if f"__RC__{t}__0" in buffer:
                continue
            if f"__RC__{t}__77" in buffer:
                continue
            failures.append(f"{t}: non-zero exit (see log)")

        if missing:
            failures.append(f"missing: {', '.join(missing)}")

        if "__TESTS_OK__" not in buffer:
            failures.append("suite: did not complete")

        if failures:
            print("\n\x1b[31m❌ user-tests failed\x1b[0m", file=sys.stderr)
            for f in failures:
                print(f"  - {f}", file=sys.stderr)
            return 1

        print("\n\x1b[32m✔ user-tests passed\x1b[0m")
        return 0
    finally:
        try:
            p.wait(1)
        except subprocess.TimeoutExpired:
            p.terminate()
            p.wait()


if __name__ == "__main__":
    raise SystemExit(main())
