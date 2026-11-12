# tclock

A colorful, terminal-sized system dashboard built with C and ncurses. `tclock` puts a large live clock and date beside lightweight CPU and memory meters—so you can keep an eye on your system without leaving the terminal.

```text
┌ tclock ───────────────────────────────────────────────────────────┐
│                    ██ ██  ███     ██ ██  ███                     │
│                    ██ ██     █    ██ ██     █                    │
│                    ██ ██  ███     ██ ██  ███                     │
│                    ██ ██  █          █  █                        │
│                    ██ ██  ████     ██ ██  ████                   │
│                                                                  │
│                   Wednesday, November 12, 2026                  │
│                 day 316 of 2026 · week 46 · UTC                  │
│                                                                  │
│   ───────────────────── CPU · 8 threads ──────────────────────  │
│       ALL  [████████░░░░░░░░░░░░] 23.4%                          │
│       T0   [██████░░░░] 41%       T1   [███░░░░░] 22%             │
│   ───────────────────────── memory ───────────────────────────  │
│       MEM  [█████████░░░░░░░] 9.4 / 31.3 GiB · 30%              │
│                 load 0.42 0.38 0.31 · up 3d 02:14               │
└──────────────────────────────────────────────────────────────────┘
```

*The display adapts to your terminal size; the values above are illustrative.*

## Features

- A large, custom-drawn `HH:MM:SS` clock with blinking colons
- Localized date, day of year, ISO week number, and time-zone label
- Aggregate CPU activity and per-CPU meters, sampled live
- Memory usage meter, with a peak marker; Linux also reports swap when available
- Load averages and uptime
- Color when supported, with terminal-friendly character fallbacks
- Responsive layout that rearranges and trims optional rows for smaller terminals
- Peak markers that can be reset while the program is running

`tclock` refreshes twice per second. It is designed to fit an ordinary terminal; a terminal of at least 46 columns by 12 rows is required for the dashboard layout.

## Platforms

- **Linux:** CPU, memory, swap, load average, and uptime are read from Linux system interfaces, including `/proc`.
- **FreeBSD:** CPU, memory, load average, and uptime use native system interfaces. The optional Linux-style process counts and swap meter are not currently provided.
- **Other Unix-like systems:** the clock and calendar display work; system-stat panels may be unavailable.

CPU and memory values are operating-system metrics, not performance benchmarks. The dashboard displays up to 16 per-CPU meters so it remains readable on systems with many CPUs.

## Build

You need a C compiler, ncurses development headers, and GNU Make. The Makefile uses GNU Make conditionals, so on FreeBSD run `gmake` rather than the system `make`.

### Linux

Install your distribution's C build tools and ncurses development package, then run:

```sh
make
```

For example, on Debian or Ubuntu:

```sh
sudo apt install build-essential pkg-config libncurses-dev
gmake
```

### FreeBSD

Make sure GNU Make and the ncurses development files are installed, then run:

```sh
gmake
```

On FreeBSD, the Makefile generates `tclock-freebsd.c` from `tclock.c` and `freebsd-stats.inc` using `freebsd-build.awk`, then compiles the generated source. You normally do not need to run the generator yourself.

The Makefile prefers `ncursesw` or `ncurses` via `pkg-config`, and falls back to linking `ncursesw` if neither is detected. If your ncurses installation is in a nonstandard location, set the relevant compiler and linker flags for your environment.

## Run

```sh
./tclock
```

| Key | Action |
| --- | --- |
| `q` or `Q` | Quit |
| `Esc` | Quit |
| `r` or `R` | Reset CPU and memory peak markers |

Show the short usage message with:

```sh
./tclock --help
```

## Configuration notes

The clock uses your system's local time zone and locale. For the best date and character rendering, run with a UTF-8 locale and a terminal font that includes block characters. If those characters are unavailable, `tclock` falls back to ncurses drawing characters where possible.

Color support depends on the terminal. The dashboard remains usable without color.

## Clean generated files

```sh
gmake clean
```

This removes the executable and the generated FreeBSD source file.

## License

No license has been specified yet. Please check with the repository owner before redistributing or reusing this project.
