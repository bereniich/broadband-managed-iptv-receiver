# Handover — implementation of WP-AL (Ivan Berenić)

**Written:** 2026-11-22, at the end of the specification phase (specification version 1.1).
**For:** Ivan Berenić and their coding agent. Ivan implements **WP-AL** (application layer — DTV middleware, C++17) and, before the parallel work starts, the **repository skeleton on `main`**.
**Read first:** this file completely, then `SPECIFICATION.md` (version 1.1) completely. The specification is the source of truth; this file only says who does what, in which order and which details must not be missed. `HANDOVER.md` describes the specification phase (written for version 1.0) and is background only. The other two members have their own files: `HANDOVER-WP-PAL-Lazar.md` and `HANDOVER-WP-APP-Smilja.md`. Sections 2 – 5 and 10 are the same in all three files.

---

## 1. How to start (for the agent)

Suggested first prompt:

> Read `HANDOVER-WP-AL-Ivan.md` completely, then `SPECIFICATION.md` (version 1.1) completely. I implement work package WP-AL on branch `al`, and first the repository skeleton on `main` (§7, step 0). The specification is frozen: never change it, except Section 5.9 and a contract change that I explicitly ask for (rule 2 of Section 2.2.7). When something in it is missing, contradictory or not implementable, stop and tell me instead of guessing. Run the consistency check of §10, confirm the state and propose step 0. Talk to me in Serbian (Latin script); write all code, comments, commit messages and documents in English. Work in small steps: one increment with its unit tests per step, then a short summary; wait for my confirmation before the next step.

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

## 6. Your work package: WP-AL

**Modules** (Sections 2.2.4, 4.1.3, 4.2): AL-API (UI API facade, owns the semantics of IF-INT-01), AL-SVC (service list, service-info cache, EPG pass-through), AL-SES (session state machine), AL-HEC (head-end client), AL-PSI (PAT/PMT), AL-DSC (protection decision, key store, key validation, hold), AL-OUT (HLS packager), AL-MAIN (configuration, composition root, orderly stop).

**Your requirements:** REQ-01, 02, 05, 12, 14, 16 – 19, 20 (AL part), 21 (AL part), 22 – 24, 32, 41, 44, 45, 49, 51 – 56, 57 (AL part), 58 (AL part), 59, 67, 69 (AL part), 70; NFR-09, NFR-15, NFR-16 (middleware part); the AL share of NFR-01 – NFR-07.

**Files you own:** `middleware/core/` (incl. `tests/` and `tests/fakes/`), `middleware/main/` (incl. `middleware.json`), and the shared files of step 0. Do not change `middleware/pal/`, `middleware/hal/`, `server/`, `content/`, `webui/`.

**Interfaces:**

| Interface | Your side | Other side | Section |
|---|---|---|---|
| IF-INT-01 UI control API | provider of the semantics (AL-API) | Smilja (APP-MWC) | 3.6 |
| IF-INT-02 HLS | provider (AL-OUT writes through `IHlsStore`) | Smilja (hls.js) | 3.7 |
| IF-INT-04 DTV SDK and platform | consumer | Lazar (PAL) | 3.9 — `platform.hpp` is frozen, byte for byte as in 3.9.2 |
| IF-EXT-03 OTT MW REST | consumer (AL-SVC) | Lazar (SRV-MW) | 3.2 |
| IF-EXT-04 stream control | consumer (AL-HEC) | Lazar (SRV-HEC) | 3.3 |
| IF-EXT-05 TS content rules | consumer (AL-PSI, AL-DSC, AL-OUT) | Lazar (SRV-RTP), Smilja (SRV-PKG, encryption of D-04) | 3.4 |

**Your stand-ins** (Section 2.2.7): fakes in `middleware/core/tests/fakes/` — fake tuner (status on request), fake demux (filters fed from byte arrays or a content file; descrambling by a reference implementation of D-04 written in the test code from the text of D-04, not copied from HAL-DSC or `package_ts`), fake HTTP client (the JSON examples of Section 3 per URL), fake UI channel (records frames), HLS store on a temporary directory, controllable clock (`advance(ms)`), fake logger; the task queue in test mode (`run_pending()`, rule T5).

---

## 7. Step plan

**Step 0 — repository skeleton on `main` (before anything else, target 2026-11-23).** Do this on `main`, show Ivan the result before pushing.

1. Check that `SPECIFICATION.md` 1.1 and the three `HANDOVER-WP-*.md` files are on `main` (committed and pushed on 2026-11-22 together with this file) and that the consistency check of §10 passes.
2. Create the directories of Sections 2.2.7, 4.4.1, 4.7, 5.3.1 and 5.4.2 (empty ones with `.gitkeep`): `webui/{js,css,tests,mock}`, `middleware/{include/pal,core/tests/fakes,main,pal/tests,pal/tools,hal/tests}`, `server/{common,headend,mw,package,tests}`, `third_party/{nlohmann,mongoose}`, `content/test`, `scripts`, `tests/{tools,integration,acceptance,evidence}`.
3. `middleware/include/pal/platform.hpp` **exactly** as in Section 3.9.2 (copy the code block; no edits, no extra includes).
4. Third-party sources (§5): `third_party/nlohmann/json.hpp` from the release v3.12.0 of nlohmann/json, and `mongoose.c`, `mongoose.h` from the release 7.23 of Mongoose into `third_party/mongoose/`; files unchanged, versions noted in `README.md`.
5. CMake: `middleware/CMakeLists.txt` (C++17, `-Wall -Wextra`, `enable_testing()`, `find_package(GTest REQUIRED)`, `find_package(OpenSSL REQUIRED)`, `find_package(Threads REQUIRED)`, output directory `build/bin`, a `mongoose` static library from `third_party/mongoose/`, `add_subdirectory` of `core`, `hal`, `pal`, `main` and `add_subdirectory(../server server)`), one `CMakeLists.txt` per subdirectory with the targets of §5, and `server/CMakeLists.txt` with the operator-side targets. Each target gets one placeholder source so that everything builds; the placeholder `create_platform()` in `middleware/pal/` returns `nullptr` (replaced by Lazar), the placeholder mains of `headend`, `mw_server` (Lazar) and `package_ts` (Smilja) only print their usage. `dtvcore` must not link `hal`, `pal`, OpenSSL or Mongoose (rule R3, TST-159); the operator-side targets must not link `dtvcore`, `pal` or `hal` (rule R5).
6. `middleware/main/middleware.json` with every key and default of Section 4.2.5.
7. `.gitignore`: `build/`, `logs/`, `content/*.ts`, `content/test/*.ts`, `*.pcap`, `__pycache__/`.
8. `README.md`: purpose (one paragraph), the work-package table of §2, prerequisites (§10), build / test / run commands of §5, "content: run `content/generate.sh` (filled in by WP-PAL) and `build/bin/package_ts` (filled in by WP-APP)", test data → Section 5.2 of the specification (NFR-17). Each work package adds its own run notes later through a pull request.
9. `tests/tools/trace_check.py`: the generator of Section 5.7 from `HANDOVER.md` §9, with a `--check` option that exits with 1 if the generated tables differ from Section 5.7 or something is missing.
10. Build once (`cmake … && ctest …`), commit `Repository skeleton`, push `main` (after Ivan's confirmation), create and push the branches `app`, `al`, `pal` from it, and tell Lazar and Smilja.

**Then on branch `al`** (each step with its unit tests; the TST IDs are those of Section 5.3.2):

| Step | Content | Tests | Target |
|---|---|---|---|
| 1 | `TaskQueue` (control / data tasks, bound 8 192, drop counter, warning ≤ 1/s, test mode) | TST-148 | 11-23 |
| 2 | Free functions: MPEG-2 CRC-32, `parse_pat`, `parse_pmt`, PES start check, PCR and `random_access_indicator` extraction, playlist text; the TS / section test helper (check value `0x0376E6E7`) | TST-101 – 104 | 11-23 |
| 3 | Fakes of §6 | — (used by all later tests) | 11-24 |
| 4 | AL-PSI module | TST-105, 106 (content) | 11-24 |
| 5 | AL-DSC | TST-111 – 116 (TST-113, 114 with the packaged content of `package_ts`, from 11-25) | 11-24 / 25 |
| 6 | AL-OUT | TST-121 – 127 | 11-24 |
| 7 | AL-SES, clear path: T-01, T-03, T-06, T-09, T-11, T-12, T-18, T-19, T-26, T-27; AL-HEC; AL-SVC; AL-API; AL-MAIN | TST-131, 132, 135, 151 – 158 | 11-25 |
| 8 | **I-2:** link with Lazar's `pal` / `hal`, play `svc-1` live from the real head-end in Chromium with Smilja's application | TST-522 (first run) | 11-25 |
| 9 | AL-SES errors and no-signal: T-04, T-05, T-07, T-08, T-10, T-13, T-20 – T-23, T-28 – T-30, T-32 | TST-133, 134, 136 – 138, 145 | 11-26 |
| 10 | Tools `contract_check.py`, `ws_client.py`, `hls_check.py` (Python standard library, Section 5.4.2) | used by TST-521 – 533 | 11-26 |
| 11 | AL-SES keys and concurrency: T-14 – T-17, T-24, T-25, T-33, T-02, T-31 | TST-139 – 144, 146, 147 | 11-27 |
| 12 | Portability check | TST-159 | 11-27 |
| 13 | **I-3:** protected rewind with Lazar's descrambler and the content packaged by Smilja's `package_ts`; integration tests of CFG-MW | TST-521 – 531, 533; TST-532 with Smilja | 11-28 |
| 14 | Acceptance tests you execute (§5), fixes, Section 5.9 | — | 12-01 |

---

## 8. Design points you must not miss

- **One AL worker thread** (Section 4.2.1): PAL callbacks only copy their arguments into a task and post it (T1); the AL calls the PAL only from the worker (T2); one queue for all filter data keeps the received order (T3); no blocking on the worker (T4).
- **Generation, not session number,** separates stream attempts (Section 4.2.2): incremented on every `play` and on every restart after `set_key` (T-24, T-33); every registered lambda captures it; the tuner status uses the atomic `tuned_generation`, set in the order `untune()` → `tuned_generation = g` → `tune()`.
- **Steps S1 – S12** (Section 4.2.4): the PAT filter and the HLS directory are opened **before** `POST /start` (S3), because the head-end sends PAT and PMT first.
- **The transition table T-01 – T-33 is normative** (Section 4.3.4); the state diagram leaves out the transitions without a change of state. Rule 3 (data arriving before the `/start` response) was rewritten in 1.1: in `starting` only the data-path actions are executed; state changes, RELEASE and discarding a rejected key are applied by T-06, and only the resulting state is sent. The stream deadline is cancelled as soon as its decisions are made.
- **RELEASE order** (rule 4): timers and HTTP requests → filters and `clear_keys` → `untune` → AL-OUT `close` (`close_session`) → `POST /stop` if a head-end session exists. It keeps the session number and the start point (D-14).
- **Keepalive loss = three consecutive failed keepalives** (REQ-69, T-27, T-29; new in 1.1); `SESSION_NOT_FOUND` releases at once without `/stop` (T-28).
- **Protection** is decided on the first video packet with at least 16 bytes of payload (REQ-44); `protected-key` means that a key was set on the descrambler at T-12 of this attempt (EV-VIDEO1). The key is set before the TS filters are started (REQ-49). Validation: 3 PES starts received with `scrambled = true` must begin with `00 00 01` (REQ-51); output held until accepted (REQ-52, B6 ≤ 4 segments).
- **AL-OUT** (Section 4.2.4, REQ-22 – REQ-24, Section 3.7): cut at `payload_unit_start_indicator = 1` with `random_access_indicator = 1` once ≥ 1 s of PCR has passed; every segment starts with the last PAT and PMT; `#EXTINF` with 3 decimals; `#EXT-X-TARGETDURATION:2`; at most 6 listed; **remove the old segment before writing the new one** (at most 8 segment files, new in 1.1); discontinuity when the PCR decreases or jumps forward by more than 1 s; `#EXT-X-DISCONTINUITY-SEQUENCE` kept correct; a segment without a key frame within 2 s is discarded (B5).
- **`session_state`** carries `playing_since` (UNIX time of the first `playing`, new in 1.1): the UI uses it only to request a running session again from its position after a reconnect; the shown position is computed by the UI from the first picture (`firstPictureAt`, Section 4.4.2). `stream_info` is sent once per session, not again after a restart, and has no `scrambled` field (Section 3.6.3).
- **Clamping** (REQ-41): `max(T, now − 7195)`, `"live"` if `T ≥ now`; `FUTURE_TIME` from the head-end → one retry as live (T-07).
- **AL-API:** one response per command within 1 s; the response to `play` before the first event of the new session; `get_status` returns the last `session_state` data or `{"state": "idle"}`; `mw_status` to every new client; events by `broadcast`.
- **AL-SVC:** retry every 5 s; `mw_status false` after the first failure, `true` after the first success; validation of REQ-02 and REQ-32; service-info cache, no EPG cache; timeouts 800 ms (MW) and 2000 ms (head-end).
- **AL-MAIN:** start-up order and orderly stop of Section 4.2.5 (≤ 3 s, `/stop` before exit, key bytes zeroed).
- **Portability** (NFR-09, R3): `dtvcore` includes only `pal/platform.hpp`, the standard library and `nlohmann/json.hpp`; no socket, OpenSSL, Mongoose or HAL code.

---

## 9. Tests

| Kind | Tests |
|---|---|
| Unit tests you write | TST-101 – 106, 111 – 116, 121 – 127, 131 – 148, 151 – 159 (Section 5.3.2) |
| Integration tests you write and run | TST-521 – 531, 533; TST-532 with Smilja |
| Integration tests you review | TST-501 – 504, 511, 512 (Lazar), TST-541 – 543 (Smilja) |
| Acceptance tests you execute | TST-604, 605, 606, 609, 611, 613, 615, 622, 625, 704, 705, 709, 713, 714, 716 |
| Pull requests you review | WP-PAL and WP-APP (§3) |

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

- **I-2 on 2026-11-25 depends on three deliveries** (content, head-end, PAL clear path). Until they arrive, work only with the fakes; do not wait.
- **HLS start position:** the player must start at the first listed segment (`startPosition: 0`), otherwise rewinds do not start at T (NFR-04, D-14). Check it together with Smilja at I-2 (TST-542).
- **Loop boundary** (REQ-24): PCR / PTS / DTS jump back by 600 s; test early with TST-124 and TST-531.
- **Rule 3 timing** (data before the `/start` response) is easy to get wrong; TST-132 covers both variants.
- **Content files are not committed** (≈ 262 MB each); every machine generates them, `content/SHA256SUMS` identifies the reference set of the defense laptop (Section 5.2.1).
