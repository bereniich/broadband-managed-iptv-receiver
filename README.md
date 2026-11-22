# M15 — Broadband Managed IPTV Receiver: Protected Rewind

Course project "Software in Digital Television", group VII, task M15 (Linux + Web variant). A Web application in Chromium (kiosk mode) is the user interface of a managed IPTV receiver; it talks to a native C++17 DTV middleware over a WebSocket and plays the middleware's HLS output with hls.js. The middleware requests MPEG-TS over RTP from the team's head-end, parses PAT/PMT, detects protection, decrypts protected services with the key typed by the viewer (AES-128-CBC), rejects an incorrect key and packages the decrypted stream as HLS. Service list and EPG come from the team's OTT MW REST server; the archive is a server-side catch-up on a looped 10-minute file, so a protected program can be played again from a selected time in the past. The specification is [`SPECIFICATION.md`](SPECIFICATION.md).

## Team and work packages

| Member | Work package | Layer | Branch | Directories |
|---|---|---|---|---|
| Ivan Berenić | WP-AL | AL — DTV middleware (`dtvcore`, `dtv-middleware`) | `al` | `middleware/core/`, `middleware/main/` |
| Lazar Zubović | WP-PAL | PAL (DTV SDK and platform services), HAL, operator side except SRV-PKG | `pal` | `middleware/pal/`, `middleware/hal/`, `server/` (except `server/package/`), `content/` |
| Smilja Krulj | WP-APP | Application (Web UI) and the packaging tool SRV-PKG | `app` | `webui/`, `server/package/` |

Shared files are changed only on `main`, through pull requests: `SPECIFICATION.md`, `middleware/include/pal/` (IF-INT-04), `middleware/CMakeLists.txt`, `third_party/`, `scripts/`, `README.md`, `.gitignore` and `tests/` (Section 2.2.7 of the specification).

## Repository layout

| Directory | Content |
|---|---|
| `middleware/include/pal/` | `platform.hpp` — DTV SDK and platform interfaces (IF-INT-04), frozen |
| `middleware/core/` | AL library `dtvcore` and its unit tests (`tests/`, fakes in `tests/fakes/`) |
| `middleware/main/` | `dtv-middleware` entry point and its configuration `middleware.json` |
| `middleware/pal/`, `middleware/hal/` | PAL and HAL libraries, their unit tests, test program `pal_dump` (`pal/tools/`) |
| `server/` | Operator side: `common/`, `headend/`, `mw/`, `package/`, unit tests in `tests/` |
| `webui/` | Web application (plain ES2020 modules), unit tests, mock middleware |
| `content/` | Content generator and content configuration; generated `.ts` files are not committed |
| `third_party/` | Reused third-party sources (below) |
| `scripts/` | `start.sh`, `stop.sh` |
| `tests/` | `tools/`, `integration/`, `acceptance/`, `evidence/` (Section 5 of the specification) |

## Prerequisites

Every developer machine and the demo laptop: Ubuntu 24.04 LTS with the versions of Section 2.2.8 of the specification.

```bash
sudo apt install build-essential cmake libssl-dev libgtest-dev python3 ffmpeg fonts-dejavu-core tcpdump xdotool curl nodejs
```

```bash
sudo snap install chromium
```

- On Ubuntu 24.04 Chromium is a snap (`chromium`); `nodejs` of Ubuntu is 18.19. A newer Node.js also works with the test command below.
- The demo laptop runs the "Ubuntu on Xorg" session: `xdotool` does not work reliably under Wayland.
- `fonts-dejavu-core` provides `/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf` for the burned-in text of the content.

## Build and test

All commands run from the repository root.

```bash
cmake -S middleware -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

```bash
ctest --test-dir build --output-on-failure
```

```bash
node --test webui/tests/*.test.js
```

The executables are built into `build/bin/`: `dtv-middleware`, `pal_dump`, `headend`, `mw_server`, `package_ts`. `ctest` runs the native unit tests of the receiver and of the operator side (`dtvcore_tests`, `hal_tests`, `pal_tests`, `server_tests`).

## Content

The content files are generated on every machine and are not committed (about 262 MB each). Generate the clear content with `content/generate.sh` (WP-PAL) and package the protected content with `build/bin/package_ts --in <clear.ts> --out <enc.ts>`, which reads the key from standard input (WP-APP): `svc-3.enc.ts`, `svc-4.enc.ts` and `test/svc-4.altpid.enc.ts` (Section 4.7.3). `content/SHA256SUMS` identifies the reference set of the defense laptop (Section 5.2.1).

**Test data** — content files, checksums, the keys of the protected services, the incorrect key and the expected services and EPG — is documented in Section 5.2 of the specification (NFR-17); this file does not repeat it.

## Run

The whole system is started and stopped with `scripts/start.sh` and `scripts/stop.sh` (WP-APP). The processes, each started from the repository root:

```bash
build/bin/mw_server --config server/config.json
```

```bash
build/bin/headend --config server/config.json
```

```bash
build/bin/dtv-middleware --config middleware/main/middleware.json
```

The test set of Section 5.2.1 is served by `mw_server` and `headend` started with `--config server/config.test.json`. The Web application is opened at `http://127.0.0.1:8090/`. Each work package adds its own run notes to this file through a pull request.

## Specification checks

`SPECIFICATION.md` 1.1 is frozen: changes are made only on `main` with the agreement of the owners concerned and a new document version; only Section 5.9 (test results) changes without a new version. After a change of a test table or of Section 5.9.3:

```bash
python3 tests/tools/trace_check.py --check
```

`--check` exits with 1 if Section 5.7 (traceability) or Section 5.9.2 (summary of the results) differs from what the script generates, or if a requirement, user story, transition, error or buffer has no test; `--write` replaces both sections with the generated text.

## Third-party sources

Vendored unchanged in `third_party/` (reused code, not team code in the sense of rule R5 of the specification). OpenSSL, GoogleTest and the other tools come from the Ubuntu packages above.

| Component | Version | Files | Source | License | SHA-256 |
|---|---|---|---|---|---|
| nlohmann/json | 3.12.0 | `third_party/nlohmann/json.hpp` | `https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp` | MIT | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |
| Mongoose | 7.23 | `third_party/mongoose/mongoose.c` | `https://raw.githubusercontent.com/cesanta/mongoose/7.23/mongoose.c` | GPLv2 | `651d514ee52761c26af774d74e91346bf150c915120d967ec4332a69ea1600f5` |
| Mongoose | 7.23 | `third_party/mongoose/mongoose.h` | `https://raw.githubusercontent.com/cesanta/mongoose/7.23/mongoose.h` | GPLv2 | `0ee23157099964b7ca1c7bf3e02b1b8250cbd7838a6938fed63726e720f70b0b` |

The Web application loads hls.js 1.7.4 from `https://cdn.jsdelivr.net/npm/hls.js@1.7.4/dist/hls.min.js` (HLR-21).
