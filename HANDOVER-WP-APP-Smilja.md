# Handover — implementation of WP-APP (Smilja Krulj)

**Written:** 2026-11-22, at the end of the specification phase (specification version 1.1).
**For:** Smilja Krulj and their coding agent. Smilja implements **WP-APP**: the Web application (HTML, CSS, ES2020 JavaScript, Model-View-Presenter, hls.js) with its mock middleware, the offline packaging tool SRV-PKG of the operator side (C++17, since version 1.1), and in addition the start / stop scripts and the measurement scripts of the acceptance tests.
**Read first:** this file completely, then `SPECIFICATION.md` (version 1.1) completely. The specification is the source of truth; this file only says who does what, in which order and which details must not be missed. `HANDOVER.md` describes the specification phase and the two review rounds that were merged into 1.1; background only. The other two members have their own files: `HANDOVER-WP-AL-Ivan.md` and `HANDOVER-WP-PAL-Lazar.md`. Sections 2 – 5 and 10 are the same in all three files.

---

## 1. How to start (for the agent)

Suggested first prompt (after Ivan has pushed the skeleton and the branch `app`, I-1 of §4):

> Read `HANDOVER-WP-APP-Smilja.md` completely, then `SPECIFICATION.md` (version 1.1) completely. I implement work package WP-APP on branch `app` — the Web application in `webui/` and the packaging tool in `server/package/` — plus the start / stop scripts and the acceptance measurement scripts on `main`. The specification is frozen: never change it, except Section 5.9 and a contract change that I explicitly ask for (rule 2 of Section 2.2.7). When something in it is missing, contradictory or not implementable, stop and tell me instead of guessing. Run the consistency check of §10, confirm the state and propose step 1 of §7. Talk to me in Serbian (Latin script); write all code, comments, commit messages and documents in English. Work in small steps: one increment with its unit tests per step, then a short summary; wait for my confirmation before the next step.

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

## 6. Your work package: WP-APP

**Modules** (Sections 2.2.4, 4.1.3, 4.4, 4.7.3): APP-MWC (middleware client), APP-MOD (application model), APP-KEY (key input router), APP-PRS (presenters), APP-VIEW (views, `<video>` with hls.js); integration of hls.js 1.7.4 (reused, Section 2.2.3); **SRV-PKG**, the packaging tool `package_ts` that encrypts the protected content according to D-04. SRV-PKG is yours so that D-04 is implemented by a different member than HAL-DSC (Lazar): never look at or share code with `middleware/hal/` (Section 5.1.7, rule 6; rule R5).

**Your requirements:** REQ-03, 04, 06 – 11, 20 (APP part), 21 (APP part), 25 – 31, 33 – 40, 43, 46 – 48, 57 (APP part), 60, 61, 63 (SRV-PKG), 69 (APP part); NFR-05, NFR-10 (SRV-PKG round trip), NFR-11 (Web and SRV-PKG), NFR-12 (start / stop scripts), NFR-14; the APP share of NFR-01, NFR-02 and NFR-04 (player start).

**Files you own:** everything in `webui/` (`index.html`, `package.json`, `css/`, `js/`, `js/presenters/`, `js/views/`, `tests/`, `mock/`, Section 4.4.1); `server/package/` (with its `CMakeLists.txt`) and the test file of TST-304 – 306 in `server/tests/` (for example `server/tests/test_package.cpp`, in the target `server_tests`); on `main`, through pull requests: `scripts/start.sh`, `scripts/stop.sh`, and the scripts of `tests/acceptance/` (Section 5.6.1). Do not change `middleware/`, the rest of `server/`, `content/` or `third_party/`. You may use the helpers of `server/common/` (logging, configuration; Lazar's), which contain no D-04 code.

**Interfaces:**

| Interface | Your side | Other side | Section |
|---|---|---|---|
| IF-INT-01 UI control API (WebSocket, JSON) | consumer (APP-MWC) | Ivan (AL-API), transport by Lazar (PAL-WS) | 3.6 — the JSON examples are normative |
| IF-INT-02 HLS | consumer (hls.js in APP-VIEW) | Ivan (AL-OUT), served by Lazar (PAL-WS) | 3.7 |
| IF-INT-03 Web application files | your files, served by PAL-WS from `webui/` | Lazar | 3.8 |
| IF-EXT-01 keyboard, IF-EXT-02 screen | yours | viewer | 1.5.1, 1.6.1 |
| D-04 encryption, IF-INT-05 files | producer of `svc-3.enc.ts`, `svc-4.enc.ts`, `test/svc-4.altpid.enc.ts` (SRV-PKG) | Lazar (head-end streams them), Ivan / Lazar (AL-DSC, HAL-DSC decrypt them) | 1.8 (D-04), 3.4, 3.5, 4.7.3 |

**Your stand-in** (Section 2.2.7): the **mock middleware** in `webui/mock/` — a Python 3 standard-library program (HTTP plus a minimal RFC 6455 WebSocket server, the same technique as `tests/tools/ws_client.py`) on `127.0.0.1:8090` that

- serves the files of `webui/` and `/hls/<n>/…`;
- answers every IF-INT-01 command with the JSON of Section 3 (services of 3.2.1 / 5.2.3; EPG generated with the formula of Section 4.7.7 from the programs of Section 5.2.3; `play` → `{"session": n}` and the session numbers increasing);
- sends the event sequences of the state machine (Section 4.3) for the scripted cases: clear service `starting` → `waiting_stream` → `playing` (with `playing_since`); protected service without a key → `key_required` `no_key`; `set_key` with KX → `starting` … `validating_key` → `key_required` `key_rejected`; with K3 / K4 → … `playing`; plus `no_signal` and `error` on request;
- serves an HLS live stream made from a clear content file, as in Section 2.2.7:

```bash
ffmpeg -re -i content/svc-1.ts -c copy -f hls -hls_time 1 -hls_list_size 6 -hls_flags delete_segments /tmp/m15-mock-hls/1/index.m3u8
```

TST-532 runs the same `ws_client.py` script against the mock and against the real middleware and expects the same sequence of message types and `state` values, so the mock must follow Section 4.3, not a simplified flow. Until Lazar's content files exist, a temporary clear TS can be made with the FFmpeg settings of Section 4.7.2.

---

## 7. Step plan

Each step with its unit tests (`node --test webui/tests/*.test.js`; the TST IDs are those of Section 5.3.5).

| Step | Content | Tests | Target |
|---|---|---|---|
| 1 | `webui/package.json` (only `{"type": "module"}`), `index.html` skeleton, `css/style.css` skeleton; APP-MOD `model.js` (Section 4.4.2, with `playerSession`, `firstPictureAt` and `session.playingSince` of 1.1); APP-KEY `keys.js`; fake clock and fake middleware client for the tests | TST-401 | 2026-11-23 |
| 2 | Presenters (Section 4.4.3 – 4.4.5): `playback.js` (zap debounce 300 ms, seek accumulation 500 ms, `B`, `I`, `K`, volume / mute), `number_entry.js`, `service_list.js`, `epg.js`, `key_dialog.js`, `session.js` (reactions of Section 4.4.4, including "Adopting a session", new in 1.1) | TST-402 – 407, 409 – 417 | 2026-11-24 |
| 3 | APP-MWC `mwc.js`: request ids, `call(name, params) → Promise`, event dispatch, reconnect every 2 s | TST-408 | 2026-11-24 |
| 4 | Views and `player.js` (hls.js from the CDN URL of Section 3.8, `startPosition: 0`, `video.play()` on `MANIFEST_PARSED`, error policy ER-35), CSS for NFR-14, diagnostics mode `?diag=1` (Section 5.1.8) | TST-418, 419 | 2026-11-24 |
| 5 | Mock middleware (§6) | TST-541 | 2026-11-24 |
| 6 | **I-2** with the real middleware of Ivan and Lazar | TST-542 (first run) | 2026-11-25 |
| 6a | SRV-PKG `package_ts` (Section 4.7.3) in `server/package/`; package the three protected files on your machine and send the command sequence to the team (and into `README.md` through a pull request to `main`) | TST-304 – 306 | 2026-11-25 |
| 7 | `scripts/start.sh`, `scripts/stop.sh` (pull request to `main`) | TST-714 (Ivan executes it) | 2026-11-26 |
| 8 | TST-532 with Ivan (mock side); TST-543 | — | 2026-11-27 |
| 9 | `tests/acceptance/` (pull request to `main`): log analysis for `t_play`, `t_picture`, `t_reject`, `t_osd`, `t_gap` (Section 5.6.1, one CSV row per repetition and a final row with the limit check), `sample_proc.py` (memory and CPU of P2), `random_keys.txt` (300 keys of TST-719, pseudo-random with seed 15, committed) and the `xdotool` runner | — | 2026-11-29 |
| 10 | Acceptance tests you execute (§5), fixes, Section 5.9 | — | 2026-12-01 |

---

## 8. Design points you must not miss

- **MVP rules** (Section 4.4.1): presenters never touch the DOM (they run in Node.js in the tests); views only render the model and format times; timers only through the injected clock object (`now()`, `setTimeout`, `clearTimeout`).
- **The key table of Section 4.4.3 is normative**, together with Section 1.5.1; TST-402 is table-driven against them. Mapped keys call `preventDefault()`; auto-repeat counts as presses; letters are converted to upper case; `+` / `-` also from the numeric keypad; unmapped keys are ignored. In the key dialog all letter and digit keys are text input (`E`, `B` are hexadecimal characters, `L`, `G` are ignored).
- **Session numbers** (REQ-26, Section 3.6.2): store the id of the last `play`; take the session number only from the response to that id; ignore every event of another session. AL-API sends the response before the first event of the new session.
- **Player life cycle** (REQ-25, Section 4.4.4 with 1.1): create a new hls.js player with `startPosition: 0` at the **first** `playing` of a session (`playerSession ≠ data.session`; `firstPictureAt = null` until its first picture), keep it when the same session returns from `no_signal`, destroy it on `key_required`, `error`, `stopped`. Volume and mute are applied to the `<video>` element and kept across player re-creation (REQ-31). Only errors of the current session's player are handled (ER-35: `recoverMediaError()` once, re-create once, third fatal error → "No signal" until the next `play`).
- **Derived values** (Section 4.4.2, updated in 1.1): mode `ARCHIVE` if `session.start` is an integer; position `startTime + (now − firstPictureAt)` (`startTime` while `firstPictureAt` is `null`), where `firstPictureAt` is set at the first `playing` event of the `<video>` element of the new player, because the picture appears 1 – 2 s after `session_state` `playing`; protection status `Clear` / `Protected – key OK` (`playing`, `no_signal`) / `Protected – key required` (`key_required`), otherwise no protection line; the persistent status message by the priority list of Section 4.4.2.
- **Reconnect** (REQ-57, "Adopting a session" in Section 4.4.4, new in 1.1): after `get_status`, keep an existing player of that session; otherwise, if the session has already been playing, send `play` from the current position (`"live"`, or `start_time + (now − playing_since)`); otherwise react as to a `session_state`; `idle` → service number 1.
- **Timers** (Section 4.4.5): zap 300 ms, seek 500 ms, number commit 2 s, info dialog 5 s (an update by REQ-28 does not restart it), volume indicator 2 s, toast 3 s, position update 1 s, EPG refresh 300 s, reconnect 2 s.
- **EPG** (REQ-33 – REQ-38): `get_epg` for now ± 2 h after the service list is loaded, every 300 s and when the EPG opens; "available" = `in_archive: true`, past with `false` = "not available", future = no mark; `ENTER` on a future event does nothing; "No information" for a service without events and for all services when `get_epg` fails.
- **Zapping and lists:** `↑` / `↓` in full-screen playback wrap around (REQ-06); the highlight in the service list does not (Section 4.4.3); number entry at most 3 digits, commit by `ENTER` or 2 s, invalid number → toast 3 s and no `play`.
- **Seek** (REQ-39): `←` from LIVE starts at `now − 30`; `→` in LIVE only while a target set by `←` is pending; one `play` after 500 ms without `←` / `→`, `"live"` if the target is not earlier than now; `B` only in ARCHIVE.
- **Key dialog** (REQ-46 – REQ-48): titles "Enter the decryption key" (`no_key`, `K`) and "Incorrect decryption key" (`key_rejected`); masked `•`, counter `n/32`, `ENTER` only with 32 characters; `ESC` in `key_required` → "Protected service – press K to enter the key".
- **Texts** exactly as in Section 1.6.1 (English). **OSD** at least 28 px at 1920 × 1080, light text on a dark semi-transparent background (NFR-14); `cursor: none` on the whole page (REQ-61).
- **Same origin** (Section 3.8, §5): WebSocket URL from `location.host`, HLS URLs relative; hls.js only from `https://cdn.jsdelivr.net/npm/hls.js@1.7.4/dist/hls.min.js` (HLR-21).
- **DIAG marks** (Section 5.1.8): `key`, `osd` (next `requestAnimationFrame`), `frame` (first `requestVideoFrameCallback` of a new player), `frag <sn>` (`FRAG_CHANGED`), `wait` / `resume` (`waiting` / `playing` of the `<video>`), only with `?diag=1`.
- **SRV-PKG `package_ts`** (Section 4.7.3, REQ-63): `build/bin/package_ts --in <clear.ts> --out <enc.ts>`; key of 32 hexadecimal characters from standard input **without echo** (switch the terminal echo off with `termios`); own PAT/PMT parser with CRC-32 check; A/V PIDs = stream types `0x1B` and `0x0F`; for every A/V packet with a payload: offset `o = 4` (`'01'`) or `5 + adaptation_field_length` (`'11'`), `n = ⌊(188 − o) / 16⌋ · 16`, `n = 0` → unchanged, otherwise AES-128-CBC with OpenSSL EVP, IV = 16 zero bytes, a new context per packet, no padding, bytes after `o + n` unchanged and the scrambling bits set to `'10'`; adaptation-field-only packets and all other PIDs unchanged; output of exactly the input size; log the counts per PID and only the first 4 key characters (REQ-55); ER-39 → exit code 1 and no output file (write to a temporary name, rename at the end).
- **`scripts/start.sh`** (NFR-12, ER-37, ER-40, Section 4.1.1): start order P4 → P3 → P2 → P1 from the repository root with the commands of §5; after each process check that its port is open (8080, 8081, 8090 TCP; 5004 UDP), stop with a message naming the failed component and stop the ones already started; check `GET /` of `127.0.0.1:8090` before Chromium; everything within 15 s; create `logs/`. Chromium (snap): `chromium --kiosk --no-first-run --autoplay-policy=no-user-gesture-required http://127.0.0.1:8090/`, with `--diag` also `--enable-logging=stderr` → `logs/chromium.log` and the page `?diag=1`. **`scripts/stop.sh`** stops all four processes; afterwards `pgrep -f "dtv-middleware|headend.py|mw_server.py|chromium"` finds nothing (TST-623, TST-714).

---

## 9. Tests

| Kind | Tests |
|---|---|
| Unit tests you write | TST-401 – 419 (Section 5.3.5); TST-304 – 306 (SRV-PKG, Section 5.3.4) |
| Integration tests you write and run | TST-541 – 543; the mock side of TST-532 |
| Integration tests you review | TST-532 (with Ivan) |
| Acceptance tests you execute | TST-603, 607, 616, 617, 618, 621, 702, 703, 706, 707, 708, 710, 711, 712, 715, 717, 718 |
| Pull requests you review | WP-AL (Ivan) — check the expected values of the WP-AL tests against Sections 3.6 and 4.3 |
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

- **SRV-PKG is C++ in a JavaScript work package:** keep it small (one source file plus a PAT/PMT parser), follow Section 4.7.3 literally and let its tests take their expected values only from the FIPS-197 vector (TST-305) and from the test's own OpenSSL calls (TST-304), never from HAL-DSC. Ivan and Lazar need the encrypted files from 2026-11-25 (AL tests, I-3).
- **TST-711** (you execute it): the frames from the browser to `/ws/control` are masked (RFC 6455); unmask them before searching the capture, as the test says.
- **H.264 in Chromium:** hls.js plays through MSE, which needs H.264 High and AAC (`MediaSource.isTypeSupported('video/mp4; codecs="avc1.64001f,mp4a.40.2"')`). Check it in the Chromium snap of every machine and of the defense laptop on day one.
- **Start at the first segment** (`startPosition: 0`) is what makes a rewind start at T (NFR-04, D-14); verify it at I-2 with TST-542 (the first `frag` mark must be 0).
- **Autoplay with sound** needs `--autoplay-policy=no-user-gesture-required`; without it the picture stays black after a zap.
- **Chromium snap:** its private `/tmp` and confinement can surprise (profile directory, logging); keep everything over HTTP from `127.0.0.1:8090` and test `start.sh` early.
- **`xdotool` and Wayland:** TST-719 needs the "Ubuntu on Xorg" session on the laptop where it runs.
- **Node.js version:** Ubuntu's Node.js 18.19 is the reference; with a newer Node.js use exactly the command of §5 (a directory argument does not work there).
