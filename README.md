# `tsh`: A Thread-Safe, Zero-Allocation POSIX Shell

![Build Status](https://github.com/Twisper/tsh/actions/workflows/ci.yml/badge.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-blue)
![Arch](https://img.shields.io/badge/arch-x86__64%20%7C%20arm64-blueviolet)
![License](https://img.shields.io/badge/license-MIT-green)
![Memory Policy](https://img.shields.io/badge/memory-Zero%20Heap-orange)

`tsh` is a simple, custom shell implementation written in C. I created this project to understand how UNIX processes, signals, and pipes work at a low level.

The main goal was to build a stable shell without using dynamic memory allocation (`malloc`). This makes the code safer and prevents memory leaks. It supports standard shell features like pipelines (`|`), background jobs (`&`), and interruptions (`Ctrl+C/Z`).

## Key Technical Features

### Zero-Heap Architecture (No Malloc)
Unlike typical shell implementations, `tsh` follows a strict **no-dynamic-allocation policy**.
* **Safety:** Immune to Heap Overflows, Double Free, and Use-After-Free vulnerabilities.
* **Stability:** Deterministic memory footprint; no runtime overhead for memory management.
* **Performance:** Utilizes fast stack allocation for command parsing.
* **Command Parsing** Parses every command in pipeline in-place.

### Security-First Design
* **Signal Handling:** Correctly handles `SIGINT` (Ctrl+C), `SIGTSTP` (Ctrl+Z), and `SIGCHLD` to prevent zombies.
* **Resource Management:** Enforces `FD_CLOEXEC` on pipes to prevent file descriptor leaks.
* **Verified:** Tested with **AddressSanitizer (ASan)** and **UndefinedBehaviorSanitizer (UBSan)**.

### Cross-Platform & Multi-Arch
* **Portable:** POSIX-compliant code runs natively on **Linux** and **macOS**.
* **Multiple Architectures Support:** Verified on **ARM64 (Apple Silicon)** and **x86_64** (via Docker).

## Functionality

* **Job Control:** Support for foreground (`fg`) and background (`bg`, `&`) execution.
* **Pipelining:** Fixed depth pipelines: `cmd1 | cmd2 | ... | cmd32` (Maximum depth is 32).
* **I/O Redirection:** Standard redirection operators: `>`, `<`, `>>`.
* **Signal Handling:** Interrupt and suspend running jobs without killing the shell.
* **Builtin Commands:** Support for commands like `cd`, `pwd`, `kill` (with Process Group ID and %Job ID), `jobs`, `history`, `export`, `unset`, `exit`.

## Build & Install

The project uses a smart Makefile that auto-detects the OS (Linux/macOS).

```bash
# Clone the repository
git clone https://github.com/Twisper/tsh.git
cd tsh

# Build the release version
make

# Build with Security Sanitizers (Debug mode)
make security
