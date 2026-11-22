# Handover — implementation of WP-PAL (Lazar Zubović)

**Written:** 2026-11-22, at the end of the specification phase (specification version 1.1).
**For:** Lazar Zubović and their coding agent. Lazar implements **WP-PAL**: the platform layer (DTV SDK and platform services), the HAL (software tuner, demultiplexer and descrambler) and the operator side except the packaging tool (head-end, OTT MW REST server, FFmpeg content generator) — all native code in C++17.
**Read first:** this file completely, then `SPECIFICATION.md` (version 1.1) completely. The specification is the source of truth; this file only says who does what, in which order and which details must not be missed. `HANDOVER.md` describes the specification phase and the two review rounds that were merged into 1.1 (yours included). The other two members have their own files: `HANDOVER-WP-AL-Ivan.md` and `HANDOVER-WP-APP-Smilja.md`. Sections 2 – 5 and 10 are the same in all three files.

---

## 1. How to start (for the agent)

Suggested first prompt (after Ivan has pushed the skeleton and the branch `pal`, I-1 of §4):

> Read `HANDOVER-WP-PAL-Lazar.md` completely, then `SPECIFICATION.md` (version 1.1) completely. I implement work package WP-PAL on branch `pal`. The specification is frozen: never change it, except Section 5.9 and a contract change that I explicitly ask for (rule 2 of Section 2.2.7). When something in it is missing, contradictory or not implementable, stop and tell me instead of guessing. Run the consistency check of §10, confirm the state and propose step 1 of §7. Talk to me in Serbian (Latin script); write all code, comments, commit messages and documents in English. Work in small steps: one increment with its unit tests per step, then a short summary; wait for my confirmation before the next step.

---

## 2. Project and team

**Task M15 (group VII):** Broadband managed IPTV receiver with protected rewind — Linux + Web variant, defense **2026-12-03**. A Web application in Chromium (kiosk) talks to a native C++ DTV middleware over a WebSocket and plays its HLS output with hls.js; the middleware receives MPEG-TS over RTP from the team's head-end, parses PAT/PMT, decrypts with the key typed by the viewer (AES-128-CBC, D-04), validates it (D-11) and packages HLS; service list and EPG come from the team's OTT MW REST server. Archive = server-side catch-up on a looped 10-minute file (D-01, D-08). Repository: `git@github.com:bereniich/broadband-managed-iptv-receiver.git`.

| Member | Work package | Layer | Branch | Directories | Also responsible for |
|---|---|---|---|---|---|
| Ivan Berenić | **WP-AL** | AL — DTV middleware (`dtvcore`, `dtv-middleware`) | `al` | `middleware/core/`, `middleware/main/` | Repository skeleton and shared files on `main` (`middleware/CMakeLists.txt`, `middleware/include/pal/platform.hpp`, `third_party/`, `README.md`, `.gitignore`); `tests/tools/trace_check.py`, `contract_check.py`, `ws_client.py`, `hls_check.py`; integration tests of CFG-MW |
| Lazar Zubović | **WP-PAL** | PAL (DTV SDK and platform services), HAL, operator side except SRV-PKG (C++17) | `pal` | `middleware/pal/`, `middleware/hal/`, `server/` (except `server/package/`), `content/` | `pal_dump`; `tests/tools/rtp_capture.py`, `mw_stub.py`, `headend_stub.py`, RTP test sender, JSON / random-byte sender; integration tests of CFG-OP and CFG-RXP; generation of the clear content for the team |
| Smilja Krulj | **WP-APP** | Application (Web UI) and the offline packaging tool SRV-PKG (C++17) | `app` | `webui/`, `server/package/` | packaging of the protected content for the team; `scripts/start.sh`, `scripts/stop.sh`; measurement and analysis scripts in `tests/acceptance/`; integration tests of CFG-MOCK and CFG-ALL |

The work packages, modules and directories are those of Sections 2.2.4 and 2.2.7 of the specification (SRV-PKG belongs to WP-APP since version 1.1, so that the two implementations of D-04 come from different members, Section 5.1.7, rule 6); only the names and the extra responsibilities in the last column are decided here. All native code — receiver and operator side — is C++17 (HLR-19); Python is used only for the test tools. `tests/` and the shared files are changed only on `main`, through pull requests (rule 5 of Section 2.2.7: integration tests run on `main`).

---

## 3. Rules of the implementation phase

1. **`SPECIFICATION.md` 1.1 is the contract.** Implement against it, not against another member's code (rule 1 of Section 2.2.7). Where this file and the specification differ, the specification wins — tell the team.
2. **Never change `SPECIFICATION.md` on a work-package branch.** If something is missing, contradictory or not implementable: stop, describe it, open a GitHub issue with the label `spec`. A change is made only on `main`, with the agreement of the owners of both sides of the interface (rule 2 of Section 2.2.7), with a new line `Document version` (1.2, 1.3, …), followed by the consistency check of §10 and, if a test table changed, regenerated Section 5.7 (`tests/tools/trace_check.py`); then everybody merges `main` immediately. Only Section 5.9 (test results) changes without a new version.
3. **Daily:** `git fetch && git merge origin/main` into your branch. A finished, tested increment goes to `main` through a pull request reviewed by the default reviewer (table below); all unit tests must pass before the merge (Section 5.1.1). A successful build is not evidence of anything (Section 5.1.7).
4. **Tests:** every test function carries its test ID (GoogleTest `TEST(Tst131, …)`, also for the operator side `TEST(Tst301, …)`; Node.js `test("TST-401 …")`; Section 5.3.1). Expected values come from the specification, the standards or `ffprobe`, never from the output of your own code (Section 5.1.7, rule 5). Tests written with an LLM are reviewed by a member of another work package, who checks the expected values against the specification (rule 7). A test that needs content files **fails** with "content missing: run content/generate.sh"; it never skips.
5. **No invented scope.** Implement exactly the requirements of your layer (column "Layer" of Section 2.3); extensions (Section 2.5) only after everything mandatory is verified. No "TBD" in code or documents.
6. **Keys:** never written to a file or a log; a log line shows at most the first 4 characters and `…` (REQ-55, NFR-08). Test keys (Section 5.2.2) appear only in test code and test scripts.
7. **Code quality (NFR-11):** C++17, `-Wall -Wextra` without warnings; the Python test tools only the standard library; Web application plain ES2020 modules, no build step.
8. **Results:** after a merge, record the result rows of your tests in Section 5.9 on `main` (Section 5.9.1); evidence in `tests/evidence/<TST-id>/` with the date and the commit in the file name; packet captures are never committed.
9. **Language:** talk Serbian (Latin script); code, comments, commit messages and documents in English; use "they" or role names for people.
10. **Defense:** each member presents their own layer (Section 2.2.7, "Defense presentation"); 5 slides in total, 5 minutes of demo (Section 5.8).

**Default reviewers** (can be changed by agreement):

| Pull requests of | Reviewed by |
|---|---|
| WP-AL (Ivan) | Smilja |
| WP-PAL (Lazar) | Ivan |
| WP-APP (Smilja) | Ivan |
| Shared files and `tests/` | a member who did not write the change |

---

## 4. Timeline and deliveries between the work packages

Today is 2026-11-22. The milestones I-2 – I-5 are those of Section 2.2.7; I-1 (contracts frozen) moves to 2026-11-23, because the skeleton is made only now.

| Target date | Milestone or delivery | From → to |
|---|---|---|
| 2026-11-23 | **I-1:** skeleton on `main` (specification 1.1 and the handovers are already there); branches `app`, `al`, `pal` pushed | Ivan → all |
| 2026-11-23 | `content/generate.sh`, `content/services.json`; every member generates the clear content on their own machine (Section 5.2.1) | Lazar → all |
| 2026-11-24 | `mw_server` and `headend` (C++, Section 4.7) runnable with `--config` | Lazar → Ivan, Smilja |
| 2026-11-24 | Mock middleware `webui/mock/` | Smilja (own stand-in) |
| 2026-11-25 | Libraries `pal` and `hal` for the clear path: tuner, demux, PAL-HTTP, PAL-WS, PAL-FS, PAL-CLK, PAL-LOG, `create_platform` | Lazar → Ivan |
| 2026-11-25 | `package_ts` (SRV-PKG); every member packages the protected content `svc-3.enc.ts`, `svc-4.enc.ts`, `test/svc-4.altpid.enc.ts` (Section 4.7.3) | Smilja → all |
| 2026-11-25 | **I-2:** a clear service travels head-end → PAL → AL → browser and plays; service list shown | all |
| 2026-11-26 | `scripts/start.sh`, `scripts/stop.sh` | Smilja → all |
| 2026-11-26 | `tests/tools/contract_check.py`, `ws_client.py`, `hls_check.py` | Ivan → all |
| 2026-11-27 | HAL-DSC and `IDemux::set_key`; `pal_dump`; stubs and senders in `tests/tools/` | Lazar → Ivan, all |
| 2026-11-28 | **I-3:** protected rewind end to end (archive at T, correct key plays, incorrect key reported, resume from T); acceptance subset TST-613, TST-616 – TST-618 | all |
| 2026-11-29 | Measurement and analysis scripts in `tests/acceptance/` | Smilja → all |
| 2026-12-01 | **I-4:** all mandatory requirements implemented; all tests executed, Section 5.9 filled in | all |
| 2026-12-02 | **I-5:** demo rehearsal TST-626 on the defense laptop with the projector | all |
| 2026-12-03 | Defense | all |

If a delivery slips, say so the same day: the others continue with their stand-ins (rule 4 of Section 2.2.7), but the milestone of the next row moves. If WP-PAL (the largest package) falls behind, the team can move SRV-MW to another work package; that is a change of Sections 2.2.4 / 2.2.7 (new version).

---

## 5. Shared implementation conventions

These are **implementation conventions, not part of the specification**. They are fixed here so that the three branches fit together; whoever wants to change one tells the other two first (the log keywords are parsed by the acceptance scripts).

**Build and run** (everything from the repository root, Section 4.1.1):

```bash
cmake -S middleware -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure
node --test webui/tests/*.test.js
```

- Executables in `build/bin/` (Section 4.1.1): `dtv-middleware`, `pal_dump`, `headend`, `mw_server`, `package_ts`. `ctest` runs the native tests of the receiver and of the operator side (NFR-10).
- Processes: `build/bin/dtv-middleware --config middleware/main/middleware.json`; `build/bin/mw_server --config server/config.json`; `build/bin/headend --config server/config.json`; the test set with `--config server/config.test.json` (Section 5.2.1). Packaging: `build/bin/package_ts --in <clear.ts> --out <enc.ts>`, key on standard input (Section 4.7.3).
- Vendored third-party code in `third_party/` (shared file, Section 2.2.7; not team code in the sense of rule R5): `third_party/nlohmann/json.hpp` (3.12.0; Ubuntu 24.04 only packages 3.11.3) and `third_party/mongoose/` (`mongoose.c`, `mongoose.h` of release 7.23, GPLv2). OpenSSL, GoogleTest and the rest come from Ubuntu packages (Section 2.2.8).
- CMake (`cmake -S middleware -B build`; `middleware/CMakeLists.txt` adds `server/` with `add_subdirectory(../server server)`, Section 4.7): receiver targets `dtvcore` (links nothing but the C++ standard library; includes only `middleware/include/` and `third_party/nlohmann/`), `hal` (OpenSSL::Crypto, Threads), `pal` (`hal`, Mongoose), `dtv-middleware` (`dtvcore`, `pal`, `hal`); operator-side targets `srvcommon` (`server/common/`), `headend`, `mw_server`, `package_ts` (OpenSSL::Crypto), which link nothing of `dtvcore`, `pal` or `hal` (R5); test executables `dtvcore_tests`, `hal_tests`, `pal_tests`, `server_tests` (TST-3xx, sources in `server/tests/`) registered with `gtest_discover_tests`. Flags `-Wall -Wextra`.

**Log keywords.** Line format of NFR-13 (`<ISO local time with ms> <LEVEL> [<module>] <message>`); the message starts with these words:

| What | Module tag | Message |
|---|---|---|
| Command received | `AL-API` | `recv command id=<id> name=<name> <params as key=value>`, e.g. `recv command id=7 name=play service_id=svc-1 start=live` |
| Event sent | `AL-API` | `send event session_state session=<n> state=<state> reason=<reason> start=<start> start_time=<t>`; `send event stream_info session=<n> program=<p> pmt_pid=<pid> pcr_pid=<pid> video=<pid> audio=<pid>` |
| Tuner status | `AL-SES` | `tuner Locked session=<n> generation=<g>`; `tuner Unlocked session=<n> generation=<g>` |
| Head-end session | `AL-HEC` | `start ok session=<n> headend_session=<hs-…> ssrc=<ssrc> start_time=<t>`; `stop headend_session=<hs-…>` |
| PSI | `AL-PSI` | `PAT program=<p> pmt_pid=<pid>`; `PMT pcr_pid=<pid> video=<pid>/0x1B audio=<pid>/0x0F` |
| Protection and key | `AL-DSC` | `protection clear`; `protection protected`; `key accepted service=<id> key=3A7F…`; `key rejected service=<id> key=0011…` |
| Segments | `AL-OUT` | `segment seg-<k>.ts duration=<d>`; `#EXT-X-DISCONTINUITY before seg-<k>.ts` (additional evidence; TST-706 counts the tags with `hls_check.py`) |
| Statistics, every 10 s (Section 4.8.1) | `AL-SES` | `stats rtp_packets=… rtp_lost=… rtp_invalid=… ts_packets=… sync_errors=… cc_errors=… dropped_tasks=…` |
| Head-end requests | `SRV-HEC` | `start session_id=<hs-…> service=<id> start=<start> client=<host>:<port> ssrc=<ssrc>`; `keepalive session_id=<hs-…>`; `stop session_id=<hs-…>`; `session expired session_id=<hs-…>` |
| Pacing skip (Section 4.7.5, step 4) | `SRV-RTP` | `late by <x> s, skipping to content time <c>` |
| Packaging (to standard error) | `SRV-PKG` | `pid=<pid> encrypted=<n> clear=<n>` per A/V PID; `key=3A7F…` |

**Web application:** the WebSocket URL is built from the page origin (`ws://${location.host}/ws/control`) and the HLS URLs are relative (`/hls/<n>/index.m3u8`), so that the port change of TST-718 needs no code change. `DIAG` lines exactly as in Section 5.1.8 (`DIAG <epoch_ms> <mark> <details>`).

**Start script options** (proposal of WP-APP): `scripts/start.sh --diag` opens the page with `?diag=1` and writes the standard error of Chromium to `logs/chromium.log`; `scripts/start.sh --test-set` starts P3 and P4 with `server/config.test.json`.

**Acceptance tests — who executes them** (rule 8 of Section 5.1.7: not the member who implemented the layer of the main requirement):

| Executor | Tests |
|---|---|
| Ivan | TST-604, 605, 606, 609, 611, 613, 615, 622, 625, 704, 705, 709, 713, 714, 716 |
| Smilja | TST-603, 607, 616, 617, 618, 621, 702, 703, 706, 707, 708, 710, 711, 712, 715, 717, 718 |
| Lazar | TST-601, 602, 608, 610, 612, 614, 619, 620, 623, 624, 626, 701, 719, 720 (TST-626 is the demo rehearsal: Lazar operates the keyboard, as at the defense) |

**Integration tests — who writes and runs them, who reviews** (Section 5.1.1: both owners of the interface):

| Tests | Writes and runs | Reviews |
|---|---|---|
| TST-501 – 504 (CFG-OP), TST-511 – 512 (CFG-RXP) | Lazar | Ivan |
| TST-521 – 531 (CFG-MW), TST-533 | Ivan | Lazar |
| TST-532 (mock against real middleware) | Ivan (real side), Smilja (mock side) | both |
| TST-541 – 543 (CFG-MOCK, CFG-ALL) | Smilja | Ivan |

---

## 6. Your work package: WP-PAL

**Modules** (Sections 2.2.4, 4.1.3, 4.7): PAL-SDK (DTV SDK: `ITuner`, `IDemux`), PAL-HTTP (`IHttpClient`), PAL-WS (HTTP / WebSocket server, `IUiChannel`, serving of IF-INT-02 and IF-INT-03), PAL-FS (`IHlsStore`), PAL-CLK (`IClock`), PAL-LOG (`ILogger`); HAL-TUN (IP tuner), HAL-DMX (software demultiplexer), HAL-DSC (software descrambler); SRV-GEN (content generator), SRV-HEC (head-end control API), SRV-ARC (archive timeline), SRV-RTP (RTP sender), SRV-MW (OTT MW REST server). **SRV-PKG (packaging tool) is not yours** since version 1.1: Smilja writes it, so that D-04 is implemented by two different members (Section 5.1.7, rule 6). Do not look at or share code with `server/package/`, and do not let Smilja's tool be the source of HAL-DSC's expected values.

**Your requirements:** REQ-13, 15, 19 (HAL part), 42, 50, 55 (PAL and HAL part), 58 (PAL and operator part), 62, 64 – 66, 68; NFR-08 and NFR-10 for your components, NFR-11, NFR-13 and NFR-16 for your components, NFR-17 (`content/SHA256SUMS`); the PAL / HAL / operator share of NFR-01 – NFR-07 and NFR-15.

**Files you own:** `middleware/pal/` (incl. `tests/`, `tools/`, `CMakeLists.txt`), `middleware/hal/` (incl. `tests/`, `CMakeLists.txt`), `server/` except `server/package/` (`common/`, `headend/`, `mw/`, `tests/` except the tests of SRV-PKG, `CMakeLists.txt`, `config.json`, `config.test.json`), `content/` (`generate.sh`, `services.json`, `test/services.test.json`, `SHA256SUMS`). Do not change `middleware/core/`, `middleware/main/`, `webui/`, `server/package/`, and never `middleware/include/pal/platform.hpp` (frozen contract with WP-AL) or `third_party/` (shared).

**Interfaces:**

| Interface | Your side | Other side | Section |
|---|---|---|---|
| IF-INT-04 DTV SDK and platform | provider (all PAL modules over the HAL) | Ivan (AL) | 3.9 — implement `platform.hpp` of 3.9.2 exactly, including every comment |
| IF-INT-01 transport | PAL-WS carries the frames (`IUiChannel`) | Ivan (semantics), Smilja (client) | 3.6, 3.9.2 |
| IF-INT-02 storage and serving | PAL-FS stores, PAL-WS serves the files written by AL-OUT | Ivan, Smilja | 3.7, 3.9.2 |
| IF-INT-03 Web application files | provider (PAL-WS) | Smilja | 3.8 |
| IF-EXT-03 OTT MW REST | provider (SRV-MW) | Ivan (AL-SVC) | 3.2 |
| IF-EXT-04 stream control | provider (SRV-HEC) | Ivan (AL-HEC) | 3.3 — error body `{"error": {…}}` as in 3.2 |
| IF-EXT-05 RTP media stream | provider (SRV-RTP) and consumer (HAL-TUN); TS content rules are a contract with WP-AL | Ivan | 3.4 |
| IF-INT-05 content configuration and files | `services.json`, the clear files; you stream the encrypted files that SRV-PKG writes | Smilja (SRV-PKG) | 3.5, 4.7.1 |

The internal HAL API (`hal::IpTuner`, `hal::Demux`, `hal::Descrambler`, Section 4.1.4) is yours and not frozen.

**Your stand-ins** (Section 2.2.7): small test programs in `middleware/pal/tools/` (the first one is `pal_dump` of Section 5.4.2); until the own head-end works, RTP from FFmpeg with a fixed SSRC, so that the tuner accepts it:

```bash
ffmpeg -re -i content/svc-1.ts -c copy -f rtp_mpegts -ssrc 1180331842 rtp://127.0.0.1:5004
```

The server side is checked with `curl`, `ffprobe` and `ffplay`. Until Smilja's `package_ts` exists, test the protected path of the head-end with the clear files (the head-end does not decrypt or look at the payload).

---

## 7. Step plan

Each step with its unit tests (the TST IDs are those of Sections 5.3.3 and 5.3.4). **Clear content first:** everybody's tests need it.

| Step | Content | Tests | Target |
|---|---|---|---|
| 1 | `content/generate.sh` for the reference set and the test set (Sections 4.7.2, 5.2.1, `-pat_period 0.09`); `content/services.json` and `content/test/services.test.json` (Sections 3.5, 5.2.3). Generate on your machine and send the exact command sequence to the team (also into `README.md` through a pull request to `main`). | TST-301 – 303 | 2026-11-23 |
| 2 | `server/common/` (configuration, logging in the NFR-13 format, HTTP and JSON helpers over Mongoose and nlohmann/json, injected clock); `server/config.json`, `server/config.test.json` (Section 4.7.1); SRV-MW `mw_server` (Section 4.7.7) | TST-319, 320, 318 (MW part) | 2026-11-23 / 24 |
| 3 | `headend`: SRV-ARC (Section 4.7.4), SRV-RTP (Section 4.7.5, with the skip-forward rule of step 4), SRV-HEC with the watchdog (Section 4.7.6) | TST-311 – 317, 318 | 2026-11-24 |
| 4 | HAL-TUN, HAL-DMX (`hal::IpTuner`, `hal::Demux`) | TST-201 – 206, 211 – 214 | 2026-11-25 |
| 5 | PAL-CLK, PAL-LOG, PAL-FS, PAL-HTTP, PAL-WS (Mongoose event loop, command queue, `mg_wakeup`), PAL-SDK (`ITuner`, `IDemux` without descrambler), `create_platform` — replaces the placeholder of the skeleton | TST-221 (without the key part), 222 – 226 | 2026-11-25 |
| 6 | **I-2** with Ivan and Smilja: clear `svc-1` live in the browser | TST-522 (Ivan runs it) | 2026-11-25 |
| 7 | HAL-DSC (OpenSSL EVP, AES-128-CBC without padding; an internal function with the IV as a parameter, called with 16 zero bytes per D-04 and with the NIST vectors in TST-215), `IDemux::set_key`, `IDemux::clear_keys` | TST-215 – 217, 221 (key part) | 2026-11-27 |
| 8 | `pal_dump` (Section 5.4.2) | TST-511, 512 | 2026-11-27 |
| 9 | Tools in `tests/tools/` (pull request to `main`; Python standard library only): `rtp_capture.py`, `mw_stub.py`, `headend_stub.py`, an RTP test sender for IN-08 – IN-10 and a JSON / random-byte sender for IN-01 – IN-04 and IN-07 (suggested names `rtp_send.py`, `junk_send.py`) | TST-501 – 504 | 2026-11-27 |
| 10 | **I-3** with Ivan and Smilja (protected rewind) | — | 2026-11-28 |
| 11 | Acceptance tests you execute (§5), fixes, Section 5.9; on the defense laptop: generate (you) and package (Smilja) the reference set and commit `content/SHA256SUMS` (Section 5.2.1) | — | 2026-12-01 |

---

## 8. Design points you must not miss

**Receiver side (`pal`, `hal`):**

- **`platform.hpp` is the contract** (Section 3.9.2): `set_status_handler` / `set_handlers` before `Platform::start()`; `tune()` returns `false` if the port cannot be opened, status `Unlocked` after `tune()`, `Locked` at the first accepted packet, `Unlocked` after 3 s without accepted packets; `open_filter` creates a **stopped** filter and returns 0 on failure; section filters deliver `table_id … CRC_32` without checking the CRC; several filters on one PID each get their own delivery; `IHttpClient` calls its callback exactly once unless cancelled; `write_file` is atomic; removing a missing file is not an error; `Platform::start()` empties `hls_dir`.
- **Rules of Section 3.9.1:** callbacks run on the PAL / HAL threads; all methods thread-safe; data passed to a method is copied before it returns; **no callback after** `untune()`, `close_filter()`, `cancel()`, `cancel_timer()` or `Platform::stop()` has returned; no exceptions across the interface.
- **Threads** (Section 4.5.2): one event-loop thread (`mg_mgr_poll` every 10 ms) — Mongoose is single-threaded, so methods called from other threads go through a mutex-protected command queue and `mg_wakeup`; one tuner receive thread (`recv` with a 100 ms timeout, also used for the 3 s `Unlocked` check) on which demux, descrambler and filter callbacks run; the filter table of HAL-DMX is protected by one mutex, held while one packet is delivered, so that `close_filter` returning guarantees "no callback after return".
- **Listening addresses** (Section 4.2.5, new in 1.1): PAL-WS listens on `127.0.0.1:<ui_http_port>`, HAL-TUN binds `127.0.0.1:<rtp_port>`; fixed in the PAL / HAL, `PlatformConfig` unchanged (TST-625).
- **Buffers** (Section 4.5.4): B1 — request `SO_RCVBUF` 1 MiB, log the effective value, do not change `net.core.rmem_max`; B2 — one 2 048-byte datagram buffer, reused, the descrambler works in place; B3 — 4 096 bytes per section filter, a longer section is discarded and counted, assembly restarts at the next `payload_unit_start_indicator = 1`.
- **HAL-TUN** (REQ-13): accept only `V = 2`, `PT = 33`, the tuned SSRC and a payload of 1 – 7 whole TS packets; count `rtp_invalid` and `rtp_lost` (sequence wrap 65 535 → 0 is not a gap).
- **HAL-DMX** (REQ-15, REQ-19): drop packets without `0x47` (`sync_errors`); count continuity-counter discontinuities per PID and continue; section assembly with `pointer_field`, over several packets and several sections in one packet (TST-213).
- **HAL-DSC** (REQ-50): a packet with `transport_scrambling_control = '10'` of a PID with a key is decrypted (payload after header and adaptation field, largest multiple of 16 bytes, AES-128-CBC, IV = 16 zero bytes per packet), its bits set to `'00'`, delivered with `scrambled = true`; `'10'` without a key → unchanged with `scrambled = true`; `'00'` → unchanged with `scrambled = false`.
- **PAL-WS** (Sections 3.7, 3.8): `/` → `index.html` of `webui_dir`; `/hls/<session>/…` from `hls_dir` with `application/vnd.apple.mpegurl` + `Cache-Control: no-cache` for the playlist and `video/mp2t` for segments; unknown → `404`; WebSocket on `/ws/control` with text frames; a binary frame is ignored without closing the process (TST-223).
- **PAL-FS:** `<hls_dir>/<session>/`; atomic write = temporary name + rename. Use a temporary name that does not match `seg-*.ts` (for example `.tmp-seg-17.ts`), so that the file counts of TST-522 and TST-706 see only complete segments.
- **PAL-LOG:** the line format of NFR-13 with local time and milliseconds, to the console and `logs/middleware.log`, thread-safe; the log keywords of §5.
- **R4 / R5** (Section 4.1.4): only PAL-SDK calls the HAL; the operator side links nothing of `dtvcore`, `pal` or `hal`, and HAL-DSC shares no code with SRV-PKG.

**Operator side (C++17, `server/`):**

- Targets `headend`, `mw_server` and the library `srvcommon`, built by `middleware/CMakeLists.txt` through `add_subdirectory(../server server)`; unit tests in `server_tests` (Section 4.7). HTTP with Mongoose (one event-loop thread per server, Section 4.5.2), JSON with nlohmann/json, both from `third_party/`. Both servers take `--config <file>` (Section 4.1.1), bind only to `127.0.0.1` (Section 4.7.1), log to `logs/headend.log` and `logs/mw_server.log` in the NFR-13 format, and survive any input (REQ-58).
- Every module reads the time only through clock functions passed at construction (defaults `std::chrono::system_clock` and `std::chrono::steady_clock`, Section 4.7), so that the unit tests use a fixed or advancing clock.
- **SRV-GEN** (Section 4.7.2): the FFmpeg settings of the table exactly (testsrc2 1280×720@25, drawtext with the DejaVu font and the `enable` expressions, sine `220·(n+1)` Hz, x264 High with closed GOP 25, AAC 128 k, `-muxrate 3500k -pat_period 0.09 -t 600`; 0.09 because FFmpeg inserts PAT/PMT only after the period has passed); the test set with `-vn` and the PID options of Section 5.2.1. Check the resulting PIDs with `ffprobe` (TST-301); if FFmpeg 6.1.1 does not produce 4096 / 256 / 257, report it.
- **SRV-ARC** (Section 4.7.4): PCR index (byte offset, `PCR_base`) built at start; copy of the first PAT and PMT; duration from `services.json`, refuse to start if the measured duration differs by more than 0.1 s (ER-38); start entry = largest `c ≤ T mod D`; `start_time = T`.
- **SRV-RTP** (Section 4.7.5): first datagram = cached PAT + PMT (2 packets); then 7 packets per datagram, the last block of a pass may have 1 – 6; pacing by interpolated content time with `steady_clock` and `sleep_until`; **if more than 1 s late, skip forward to the content of the current time and log it** (keeps the D-08 timeline that the UI position relies on); RTP timestamp `base + round(C · 90 000) mod 2³²`; random initial sequence number; stop flag checked before every block.
- **SRV-HEC** (Section 4.7.6): validation order `INVALID_REQUEST` → `SERVICE_NOT_FOUND` → `OUT_OF_ARCHIVE` / `FUTURE_TIME` (window `now − 7200 ≤ T < now`); stop the session of the same `client_host:client_port` and join its sender thread **under the lock** before creating the new one; ids `hs-` + 6 digits from `000001` per head-end start; watchdog every 1 s with `now − last_seen ≥ 15 s` (expiry 15 – 16 s); `POST /stop` answers only after the sender thread has ended; error bodies `{"error": {"code": …, "message": …}}`.
- **SRV-MW** (Section 4.7.7): base `/api/v1`; EPG cycle formula with `k` from `⌊from / D⌋ − 1` to `⌊to / D⌋`; events overlapping `[from, to)`; `event_id = <service_id>-<start>`; `in_archive = server_time − 7200 ≤ start ≤ server_time` with `server_time` read once per request; `INVALID_RANGE` and `SERVICE_NOT_FOUND` as in Section 3.2; stateless after loading.

---

## 9. Tests

| Kind | Tests |
|---|---|
| Unit tests you write | TST-201 – 206, 211 – 217, 221 – 226 (Section 5.3.3); TST-301 – 303, 311 – 320 (Section 5.3.4; TST-304 – 306 of SRV-PKG are Smilja's) |
| Integration tests you write and run | TST-501 – 504 (CFG-OP), TST-511, 512 (CFG-RXP) |
| Integration tests you review | TST-521 – 531, 533 (Ivan) |
| Acceptance tests you execute | TST-601, 602, 608, 610, 612, 614, 619, 620, 623, 624, 626, 701, 719, 720 |
| Your pull requests are reviewed by | Ivan |

---

## 10. Environment and checks

**Every developer machine and the demo laptop:** Ubuntu 24.04 LTS with the versions of Section 2.2.8.

```bash
sudo apt install build-essential cmake libssl-dev libgtest-dev python3 ffmpeg fonts-dejavu-core tcpdump xdotool curl nodejs
```

```bash
sudo snap install chromium
```

- On Ubuntu 24.04 Chromium is a snap (`chromium`); `nodejs` of Ubuntu is 18.19. A newer Node.js (for example through nvm) also works with the test command of §5.
- The demo laptop should run the **"Ubuntu on Xorg"** session: `xdotool` (TST-719) does not work reliably under Wayland.
- `fonts-dejavu-core` provides `/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf` for the burned-in text (Section 4.7.2).

**Consistency check** of `SPECIFICATION.md` (run from the repository root before and after any change of the specification):

```bash
python3 - <<'EOF'
import re
s = open('SPECIFICATION.md').read()
for p in ['REQ', 'NFR', 'HLR', 'US', 'D', 'EXT', 'T', 'ER', 'TST']:
    defined = (re.findall(rf'^\*\*({p}-\d+) ', s, re.M) if p == 'US'
               else re.findall(rf'^\| ({p}-\d+) \|', s, re.M))
    used = set(re.findall(rf'\b{p}-\d+\b', s))
    dup = sorted({x for x in defined if defined.count(x) > 1})
    print(p, 'defined:', len(set(defined)), 'duplicates:', dup, 'dangling:', sorted(used - set(defined)))
print('code fences balanced:', s.count('```') % 2 == 0)
EOF
```

Expected for version 1.1: REQ 70, NFR 17, HLR 25, US 22, D 14, EXT 11, T 33, ER 40, TST 168 defined; no duplicates; no dangling references; code fences balanced. Section 5.7 equals the output of `tests/tools/trace_check.py` (`missing: []`). All 16 Mermaid diagrams were rendered successfully with Mermaid 10.9.1.

---

## 11. Risks to watch

- **WP-PAL is the largest work package.** Steps 4 and 5 are needed on the same day as I-2; if they slip, say so on 2026-11-24 so that the team can decide (for example SRV-MW to another work package, a change of Sections 2.2.4 / 2.2.7).
- **Clear content is a prerequisite of most tests** of all three members: step 1 first, and help the others to generate it on their machines.
- **Mongoose is single-threaded** in the receiver and in each server: every call from another thread must go through a command queue; a direct call is a data race that tests will not always catch.
- **"No callback after return"** (rule 4 of Section 3.9.1) is what lets the AL discard old sessions; TST-205, TST-214 and TST-226 must pass reliably, run them many times.
- **Pacing:** about 380 datagrams per second; sleep until the computed time (`sleep_until`), never a fixed sleep per datagram (average bitrate within ±5 % over 10 s, REQ-66).
- **Loopback receive buffer** (B1, ≈ 0.8 s at 4 Mbit/s with the default `rmem_max`): a stalled tuner thread loses data; keep the receive thread free of anything but delivery.
