# Repository Guidelines

This document gives a focused overview for contributors (humans and tools) working on StarryOS. For full process details, see `CONTRIBUTING.md` and `README.md`.

## Project Structure & Module Organization

- `src/`: top-level StarryOS crate (boot and init path).
- `core/`: core OS logic (memory management, tasks, VFS).
- `api/`: user-space facing APIs (syscalls, file, socket, task, time).
- `arceos/`: upstream OS framework; treat as a vendored dependency.
- `scripts/`: helper tools (`scripts/ci-test.py`, `scripts/flash.sh`).
- `docs/`: additional documentation (for example `docs/x11.md`).

## Build, Test, and Development Commands

- Build kernel image: `make build` (use `ARCH=riscv64` or `ARCH=loongarch64` if needed).
- Prepare root filesystem: `make img ARCH=riscv64`.
- Run in QEMU: `make run ARCH=riscv64` or shortcuts `make rv` / `make la`.
- Fast Rust checks: `cargo check -p starry-core`, `cargo check -p starry-api`.
- Lint and format: `cargo clippy --all-targets --all-features -- -D warnings` and `cargo fmt`.

## Coding Style & Naming Conventions

- Rust 2024 edition; always run `cargo fmt` (uses repo `rustfmt.toml`).
- Follow idiomatic Rust style: snake_case for functions/variables, PascalCase for types.
- Keep modules cohesive (for example, core kernel changes under `core/src`, syscalls under `api/src`).

## Testing Guidelines

- Smoke test boot: `python3 scripts/ci-test.py riscv64` (boots under QEMU and checks for the BusyBox shell prompt).
- When adding nontrivial logic, add unit tests with `#[cfg(test)]` where feasible and/or describe manual test steps in the PR.
- Prefer tests that can run without hardware-specific tweaks; keep QEMU assumptions explicit.

## Commit & Pull Request Guidelines

- Use Conventional Commits, e.g. `feat(core): add copy-on-write fork` or `fix(api): handle closed socket`.
- Ensure `cargo clippy` and `cargo fmt` pass before pushing.
- PRs should be narrow in scope, reference related issues, and clearly describe motivation, design, and test strategy.

## Agent-Specific Instructions

- Avoid modifying `arceos/` and generated artifacts (such as disk images) unless explicitly requested.
- Prefer small, incremental changes isolated to `api/`, `core/`, and `src/`, with commands and assumptions documented in comments or the PR description.

