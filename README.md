# SATORI SAT Solver — WebAssembly Edition

A high-performance CDCL SAT solver specialized for SAT-encoded planning problems, now compiled to **WebAssembly** for in-browser execution with zero server dependencies.

## Overview

**SATORI** is a lightweight, single-file C++17 SAT solver featuring:

- **CDCL (Conflict-Driven Clause Learning)** with 1-UIP backtracking
- **VSIDS variable selection** with PDDL-aware activity boosting
- **Planning pattern detection** for faster solving on planning instances
- **Failed Literal Probing (FLP)** for aggressive unit propagation
- **SCC preprocessing** to simplify formulas
- **Luby restart strategy** for dynamic search behavior
- **Arena-based memory management** (~4 MB peak RSS)

### Performance

On planning benchmarks (SAT-encoded PDDL):
- **13/13** instances faster than Kissat (avg **3×**, peak **25×** speedup)
- **12/13** instances faster than Glucose (avg **1.67×** speedup)
- Optimized for binary-clause-heavy formulas (≥85% binary clauses)
- Scales efficiently to 10K+ clauses

## Files

- **`index.html`** — Complete single-page website (~490 KB, WASM embedded)
  - Online solver with live terminal output
  - File upload (CNF format)
  - Real-time clause/variable parsing
  - DIMACS-compliant result download
  - Full technical documentation
  - Terms & Conditions, Privacy Policy

- **`satori.wasm`** — Compiled SATORI engine (285 KB stripped)
  - WASI snapshot_preview1 compatible
  - Runs on any modern browser or server

- **`satori.cpp`** — Source code (1479 lines, MIT license)
  - Single-file, zero external dependencies
  - Compiles with any C++17 compiler

- **`favicon.ico`** — Multi-resolution favicon (16/32/48 px)

## Getting Started

### Run Locally

1. Clone or download the repository
2. Serve the files (any HTTP server works, or use `file://` protocol):
   ```bash
   python3 -m http.server 8000
   ```
3. Open `http://localhost:8000` in your browser
4. Upload a CNF file or use the demo
5. Download results in DIMACS format

### Deploy Online

Upload all files to any static hosting (GitHub Pages, Vercel, Netlify, etc.). No special configuration needed — WASM is embedded as base64, works with any MIME type.

## Usage

### Web Interface

1. **Load CNF** — Drag & drop or click to upload DIMACS format files
2. **Run Solver** — Click "Run SATORI" to execute
3. **Monitor** — Watch real-time output in the terminal
4. **Download** — Get result file with model assignments (if SAT)

### Input Format (CNF/DIMACS)

```
c Comment line
p cnf 3 2
1 -3 0
2 3 -1 0
```

- `p cnf <vars> <clauses>` — Header
- One clause per line, space-separated literals
- Positive integer = variable true, negative = variable false
- Zero terminates each clause

### Output Format (DIMACS)

```
c SATORI-CDCL (1-UIP + VSIDS + Luby restart + watched literals)
c Input: /input.cnf
c Variables: 2547  Clauses: 32525
c Parse Time: 15.000 ms
c Solve Time: 101.000 ms
c Total Time: 117.000 ms
c Decisions: 22442
c Conflicts: 5715
c Propagations: 1291397
c [... solver statistics ...]
s SATISFIABLE
c Valid: True
v 1 -2 3 -4 5 ... 0
v 6 -7 8 ... 0
```

- `s SATISFIABLE` or `s UNSATISFIABLE` — Result
- `v ...` lines — Variable assignments (one variable per integer, 10 per line)
  - Positive = variable is true
  - Negative = variable is false
  - Terminates with `0`

## Features

### Online Solver
- ✓ Drag-and-drop file upload
- ✓ Live terminal output with syntax highlighting
- ✓ Real-time clause/variable count
- ✓ 60-second timeout with graceful handling
- ✓ DIMACS result download
- ✓ Statistics: decisions, conflicts, propagations, etc.

### Technical
- ✓ Single-file deployment (no CDN, no npm dependencies)
- ✓ WebAssembly via WASI (standard snapshot_preview1)
- ✓ Virtual filesystem for input/output
- ✓ 285 KB compressed binary
- ✓ ~4 MB memory footprint
- ✓ Responsive design (mobile-friendly)

### Privacy & Security
- ✓ All computation runs locally in your browser
- ✓ No data transmitted to any server
- ✓ No tracking, no analytics
- ✓ Open-source and auditable (MIT license)

## Technical Stack

| Component | Technology |
|-----------|-----------|
| **Solver Engine** | C++17, clang-18 |
| **Compilation Target** | WebAssembly (WASM) |
| **System Interface** | WASI snapshot_preview1 |
| **Web Interface** | Vanilla HTML/CSS/JavaScript |
| **WASI Runtime** | @bjorn3/browser_wasi_shim (inlined) |
| **Deployment** | Static files (any server) |

## Compilation

### Native C++ Compilation (Linux/macOS/Windows with clang)

```bash
clang++-18 -std=c++17 -O3 -o satori satori.cpp
./satori < input.cnf
```

**Output:**
```
c SATORI-CDCL ...
s SATISFIABLE
v 1 -2 3 -4 ... 0
```

### WebAssembly Compilation (WASI Target)

#### Prerequisites (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y \
  clang-18 \
  lld \
  libc++-dev-wasm32 \
  libclang-rt-18-dev-wasm32 \
  wasi-libc \
  llvm-18
```

#### Compile to WASM

```bash
clang-18 --target=wasm32-wasi \
  -I/usr/lib/llvm-18/include/wasm32-wasi/c++/v1 \
  -I/usr/include/wasm32-wasi \
  -stdlib=libc++ -std=c++17 -O3 -fno-exceptions \
  -fuse-ld=lld -Wl,--allow-undefined \
  -L/usr/lib/wasm32-wasi \
  -o satori.wasm satori.cpp \
  /usr/lib/wasm32-wasi/libc++.a \
  /usr/lib/wasm32-wasi/libc++abi.a \
  /usr/lib/wasm32-wasi/libc.a \
  /usr/lib/llvm-18/lib/clang/18/lib/wasi/libclang_rt.builtins-wasm32.a
```

**Output:** `satori.wasm` (unstripped ~1.6 MB)

#### Strip Binary

```bash
llvm-strip-18 satori.wasm
ls -lh satori.wasm  # → 285 KB
```

The `llvm-strip-18` command removes all symbol tables and debug information, reducing file size by ~82% with no impact on runtime performance.

**What gets stripped:**
- Debug symbols (`.debug_*` sections)
- Symbol name tables (`.symtab`, `.strtab`)
- Relocation information for rebuilding
- Size reduction: **1.6 MB → 285 KB**
- Performance impact: **None** (already optimized at -O3)

#### Verify Build

```bash
file satori.wasm
# Output: satori.wasm: WebAssembly (wasm) binary module, version 0x1

wasm-objdump -h satori.wasm 2>/dev/null || echo "(Install wabt for full inspection)"
```

### Build Website

1. **Embed WASM as base64** (already done in `index.html`):
   ```bash
   python3 << 'EOF'
   import base64
   with open('satori.wasm', 'rb') as f:
       b64 = base64.b64encode(f.read()).decode()
   print(f"const SATORI_WASM_B64 = \"{b64}\";")
   EOF
   ```

2. **Update `index.html`** with the base64 string (or keep using the provided version)

3. **Serve all files** in the same directory:
   ```bash
   python3 -m http.server 8000
   ```

4. **Deploy** to any static hosting (GitHub Pages, Vercel, Netlify, etc.)

### Compilation Flags Explained

| Flag | Purpose |
|------|---------|
| `--target=wasm32-wasi` | Target WebAssembly 32-bit WASI |
| `-stdlib=libc++` | Use LLVM's libc++ (C++17 features) |
| `-std=c++17` | Enable C++17 standard |
| `-O3` | Maximum optimization (performance critical) |
| `-fno-exceptions` | Disable C++ exceptions (reduce binary size) |
| `--allow-undefined` | Allow WASI imports without builtin implementations |
| `-Wl,--allow-undefined` | Linker flag for undefined symbols (WASI calls) |

### Build Troubleshooting

**"clang-18: command not found"**
```bash
# Install via LLVM apt repository or use system package manager
sudo apt-get install clang-18
```

**"cannot find -lc++"**
```bash
# Ensure WASI libc++ is installed
sudo apt-get install libc++-dev-wasm32
```

**WASM too large (>1 MB unstripped)**
- Verify `-O3` optimization is enabled
- Check `-fno-exceptions` is set
- Use `llvm-strip-18` to remove debug symbols

**Runtime errors in browser (WASI imports)**
- Ensure WASI shim is properly inlined in HTML
- Check browser console for specific `fd_*` errors
- Verify virtual filesystem is correctly initialized

## Algorithm Details

### Clause Learning (1-UIP)

SATORI learns one Unique Implication Point (1-UIP) clause per conflict, enabling effective constraint propagation and decision backtracking.

### VSIDS (Variable State Independent Decaying Sum)

Dynamically ranks variables by activity; frequently involved in conflicts are chosen first. Planning-specific boost prioritizes action/state variables.

### Failed Literal Probing (FLP)

Aggressively probes unassigned variables to detect forced assignments before entering the main search, reducing search space.

### Strongly Connected Components (SCC)

Preprocessing identifies variable equivalences and chains, simplifying the formula before solving begins.

### Planning Pattern Detection

Recognizes common SAT-encoding patterns from PDDL (planning domains), enabling specialized heuristics for faster convergence on planning instances.

## Performance Tips

- **Binary-heavy formulas** — SATORI excels on instances with ≥85% binary clauses
- **Planning problems** — Best suited for SAT-encoded planning (PDDL)
- **Large instances** — Efficient memory use scales to 10K+ clauses
- **Timeout handling** — 60-second timeout prevents hung browser tabs

## License

**MIT License** — See source code header in `satori.cpp` for full license text.

```
Copyright (c) 2024 Massimo Di Gruso
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions...
```

## References

- **SAT Problem** — Boolean satisfiability (https://en.wikipedia.org/wiki/Boolean_satisfiability_problem)
- **CDCL Solvers** — Conflict-driven clause learning (https://en.wikipedia.org/wiki/Conflict-driven_clause_learning)
- **DIMACS Format** — Standard SAT benchmark format (https://www.satcompetition.org/format-benchmarks2023.html)
- **WebAssembly** — https://webassembly.org/
- **WASI** — WebAssembly System Interface (https://wasi.dev/)

## Browser Compatibility

- ✓ Chrome/Chromium 57+
- ✓ Firefox 52+
- ✓ Safari 11+
- ✓ Edge 79+
- ✓ Opera 44+

Requires WebAssembly support. No polyfills needed.

## Hardware & System Requirements

### Recommended Hardware

| Component | Recommended | Minimum |
|-----------|-------------|---------|
| **CPU** | Intel i5/AMD Ryzen 5 (2016+) or better | Any modern processor |
| **RAM** | 8 GB | 2 GB |
| **Storage** | 50 MB free | 10 MB (for files) |
| **Network** | Broadband (for initial load) | 1 Mbps (25 KB download) |

### Browser Requirements

- **WebAssembly (WASM)** support — standard in all modern browsers since 2017
- **JavaScript ES6+** support — standard since 2015
- **Local file access** — for drag-and-drop CNF uploads
- **No plugins** required (Flash, Java, ActiveX, etc.)

### Performance Characteristics

**Browser-based Solving:**
- **Single-threaded** execution (JavaScript event loop)
- **Timeout protection** — 60 seconds per instance (configurable)
- **Memory limit** — ~4 MB peak RSS (instance dependent)
- **CPU usage** — Full single core during solving

**Recommended Instance Size:**
- **Small** (< 500 clauses, < 100 vars) — instant (< 100 ms)
- **Medium** (500-5K clauses) — seconds (100 ms - 10 s)
- **Large** (5K-50K clauses) — minutes (10 s - 60 s)
- **Very Large** (> 50K clauses) — may timeout (use native binary)

### System Requirements (for Compilation)

**Build WASM:**
- **OS:** Linux (Ubuntu 22.04+, Debian 12+), macOS 12+, Windows (WSL2 or native)
- **Compiler:** LLVM 18 with clang
- **Toolchain:** wasi-libc, lld linker
- **Storage:** ~500 MB for build environment
- **Time:** ~2 minutes for first build

**Build Native C++:**
- **OS:** Any (Linux, macOS, Windows, BSD)
- **Compiler:** GCC 11+, Clang 12+, MSVC 2019+
- **C++ Standard:** C++17 or later
- **Storage:** ~10 MB source, ~50 MB object files
- **Time:** < 1 minute

### Network Performance

| Action | Bandwidth | Time | Device |
|--------|-----------|------|--------|
| Load website | 25 KB | < 1 s | Modern router |
| Download result | 10-100 KB | < 1 s | Modern router |
| Upload CNF (5 MB) | 5 MB | 5-10 s | 5 Mbps connection |

**Offline Support:**
- Once loaded, website works fully offline
- No server communication required during solving
- Results stay local in browser memory

### Mobile Compatibility

**Supported:**
- ✓ iOS 11+ (Safari, Chrome mobile)
- ✓ Android 5+ (Chrome, Firefox mobile)
- ✓ Large-screen tablets (optimal for file upload)

**Limitations:**
- Small instances only (keyboard/touch input constraints)
- Portrait mode recommended for terminal output
- File upload via camera/gallery on mobile devices

### Accessibility

- ✓ WCAG 2.1 Level AA (keyboard navigation, screen reader support)
- ✓ High contrast color scheme (red `#b83232`, green `#2e7d32`)
- ✓ Monospace font (JetBrains Mono) for code clarity
- ✓ Focus indicators on all interactive elements
- ✓ Semantic HTML structure

## Contributing

Bug reports, feature suggestions, and pull requests are welcome. Please open an issue on GitHub.

## Author

**Massimo Di Gruso** — SAT solver design and C++ implementation

**WebAssembly Edition** — Browser adaptation, WASI integration, web interface

---

**Live Demo:** https://www.satori-sat.com/tory:** https://github.com/[your-username]/satori-wasm

**Issues & Support:** Open an issue on GitHub
