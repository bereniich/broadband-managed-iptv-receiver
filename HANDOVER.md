# Handover — M15 specification work

**Written:** 2026-11-19, end of the first specification session (Ivan Berenić with Claude Code). **Updated:** 2026-11-20 — browser playback switched to HLS (D-13). 2026-11-21 — layering aligned with the professor's lecture drawing (DTV SDK in PAL; tuner, SW demux, descrambler in HAL) and head-end keepalive (spec 0.11); professor's lecture notes and open questions added (§5a); server-side archive confirmed by course staff (spec 0.12). 2026-11-22 — consistency fixes and the complete Section 4 (design) written (spec 0.21). 2026-11-22 (second session) — 15 consistency fixes, the complete Section 5 (verification plan, 5.1 – 5.9) written and confirmed by Ivan; **the whole specification frozen as version 1.0** on Ivan's decision. Next: implementation, only when the team says so (§8).
**For:** the next team member (Lazar or Smilja) and their coding agent or Ivan's agent, to continue the project from the frozen `SPECIFICATION.md` without access to the previous conversations.
**Read first:** this file completely, then `SPECIFICATION.md` completely. The specification is the source of truth. This file explains how it was made, which rules apply and what comes next.

---

## 1. How to start (for the agent)

Suggested first prompt for the agent:

> Read `HANDOVER.md` and then `SPECIFICATION.md` in full. The specification is frozen (version 1.0): do not change it unless I ask, and then only following rule 2 of Section 2.2.7. Run the consistency check of HANDOVER.md §9, confirm the current state and wait for my instructions. Talk to me in Serbian (Latin script); write all project documents and code comments in English.

Before editing, the agent should run the consistency check of §9 to confirm the file is in the expected state.

---

## 2. The project

**Course:** Softver u digitalnoj televiziji (Software in Digital Television), 4th year, module Računarska tehnika i računarske komunikacije.
**Team (group VII):** Lazar Zubović (RA 13/2023), Smilja Krulj (RA 145/2023), Ivan Berenić (RA 59/2023).
**Task code:** M15, group project (3 students).
**Repository:** `git@github.com:bereniich/broadband-managed-iptv-receiver.git`, branch `main`.

Original task text (Serbian):

> **Grupa VII — M15 — Broadband Managed IPTV prijemnik: zaštićeni rewind**
> Zadatak: Omogućiti vremenski izbor ranije emitovanog zaštićenog programa i reprodukciju uz ključ za dekripciju. Demonstrirati rad sa ispravnim i neispravnim ključem.
> Zajednički zahtevi — OBAVEZNO ZA SVE: Live TV zapper (izbor sledećeg/prethodnog servisa, direktan unos broja i lista servisa), info dijalog pri promeni servisa, regulacija i utišavanje zvuka, EPG/TV vodič i upravljanje tastaturom kao daljinskim (strelice, ENTER, brojevi; slova za dijaloge).
> Tip prenosa — BROADBAND MANAGED: MPEG-TS preko RTP-a na zahtev klijenta (serverska production strana). PAT/PMT parsirati iz primljenog TS-a, a servisne informacije i EPG dobavljati od OTT MW REST servera.
> Obim projekta: Grupa predaje detaljnu tehničku specifikaciju i funkcionalnu implementaciju. Grupa samostalno bira implementaciono okruženje: 1. Linux Monolit; 2. Linux + Web; 3. Android TV sa TIF.
> Očekivani rezultati: Specifikacija treba da sadrži funkcionalne i nefunkcionalne zahteve, komponente i interfejse, tokove podataka, ponašanje aplikacije, obradu grešaka i plan verifikacije. Priložiti izvorni kod, uputstvo za pokretanje i demonstraciju. Na odbrani 3.12.2026. svaki član prezentuje svoj deo; tim ukupno ima 5 minuta prezentacije (5 slajdova: uvod, 3 sadržajna slajda, zaključak) i 5 minuta demonstracije. Svaki član odgovara na individualno pitanje; pripremiti demonstraciju uz HDMI i projektor.
> Dozvoljena je upotreba LLM alata, uz obavezu da studenti razumeju i mogu da obrazlože sve odluke i rezultate.

**Chosen variant:** Linux + Web.
**Defense:** 2026-12-03.

---

## 3. Course material the specification relies on

The lecture notes are seven PDFs ("Module 1 – 7 – Writeup.pdf"). Every team member has them. The parts used in the specification:

| Lecture | Content used |
|---|---|
| 1 | Production → distribution → consumption; broadcast vs broadband (managed / non-managed); data vs metadata paths; CAS vs DRM. |
| 2 | Head-end pipeline; receiver blocks (front-end, demux, CPU command & control, decoders, GPU composition of video + OSD layers); Internet TV consumption path (Stream RX → DRM → SW demux → decoders); Project 1 style: numbered dataflow diagrams plus UML sequence diagrams from the CPU's point of view. |
| 3 | MPEG-TS (188 B, sync `0x47`, 13-bit PID), PES, PCR/PTS/DTS, PSI (PAT on PID 0 → PMT → ES PIDs, stream types), SDT/EIT/CAT, scrambling model (headers and PSI in the clear, payload scrambled), CAS key chain (user key → EMM → service key → ECM → control word). |
| 4 | Managed IPTV: IP multicast/IGMP, UDP, RTP/RTSP, SNMP/CWMP, MPEG-TS over RTP, REST metadata (GET SERVICES, GET SERVICE INFO, LOGIN); OTT: HTTP, HLS/DASH, CMAF, CENC + DRM. |
| 5 | **Host software stack — mandatory layering:** Applications / Middleware = AL (application runtime, DTV middleware, media player, UI frameworks) / Platform = PAL (OS, common libraries, DTV SDK, A/V SDK, CA SDK) / Drivers = HAL. HAL answers "what can the hardware do", PAL "what does the programmer want", AL "what does the user want". Applications only talk to the user. |
| 6 | PAL in detail: Broadcom Nexus objects (InputBand → ParserBand → PidChannel → decoders → VideoWindow → Display) as a filter graph; Android Tuner SDK / TRM / Tuner HAL; table-parsing flow (open filter → configure PID → start → event → read → parse); why wrappers exist. |
| 7 | AL in detail: iWedia Comedia middleware (API → MW core with Service Control, Scan, Player, Xserv routing, Table module, service database → CHAL glue layer = platform adaptation). The course's Project 2 deliverable is an architecture block diagram over **HAL, PAL and AL** plus UML sequence diagrams close to implementation level for the application and middleware parts. |

## 4. General project guidelines from the course staff ("Instrukcije za izradu projekta")

These rules shape the whole specification:

- **Process:** use case → requirements → design specification → implementation iterations → verification.
- **Requirements:** describe user, goal, inputs, outputs and system boundaries; detailed user stories. High-level requirements come from (1) user goals, (2) system boundaries, (3) technical constraints. Then detailed functional requirements with IDs (`REQ-01` …), each with a verification method; mandatory separated from extensions. Concrete and measurable, never "fast and reliable". Cover a missing input file, invalid data and the user changing a choice before the previous operation has finished. Also define technical postulates: formats and standards, signal path type, layering, key interfaces, modules, design patterns.
- **The LLM may point out ambiguities but must not invent missing requirements and present them as part of the task.**
- **Design specification:** maps requirements to blocks, interfaces, states and signal paths. Principles: layered architecture (mark what is implemented, what is reused from libraries, what is simulated); signal path and producer–consumer (media vs metadata, who owns buffers and resources); asynchronous commands and events (accepting a request ≠ completing it; callbacks, errors, cancellation, timeouts); platform adaptation through interfaces. Architecture diagram plus UML sequence diagrams of the key operations; **state diagram or transition table for state-dependent operations**. Naming a design pattern is not enough: explain which problem it solves and where it appears. Diagrams must match real modules and calls.
- **Verification plan (inside the design specification):** for each test: test ID → requirement → preconditions and input → steps → measurable expected result → actual result and evidence. Unit, integration and acceptance tests. Negative cases, repeated selection changes, a new request arriving before the previous asynchronous one completes. Prefer independent checks (a test written by the LLM together with the code may share its wrong assumption). Status values: passed / failed / not executed / not verifiable. A successful build is not proof of correctness; a shown channel list is not proof of playback; one file playing is not proof that all services are discovered.
- **A ready-made player does not prove the team's own implementation** of parsing, scanning or service management.
- After each larger iteration, produce a handover (this file).

## 5. Answers from the course staff (already built into the specification)

| Question | Answer | Where in the spec |
|---|---|---|
| Does the viewer type the key, or does the receiver get it from the OTT MW server? | Either is acceptable → the team chose: **the viewer types it** in a dialog. | D-03 |
| One fixed key, or rotating control words via ECM/EMM? | **The simplest variant: one fixed key.** | D-04 |
| After an incorrect key followed by a correct one, where does playback start? | **It continues from where it was started** (the originally selected archive time T; live stays live). | D-14, REQ-49, REQ-52 |
| (Suggestion from the professor, 2026-11-20) How should the DTV middleware deliver video to the Web application? | **Use HLS**; no separate server is expected. → The middleware writes HLS segments + playlist to a temporary directory, served by its existing HTTP server; the browser uses hls.js. | D-13, IF-INT-02, REQ-22 – REQ-25 |

## 5a. Professor's lecture notes (2026-11-20) and what is still open

Ivan's notes from the lecture, in short:

- Use a **custom DTV middleware** (earlier generations used Comedia / Cuttlefish / iWedia, which was too complicated); borrowing the structure of Comedia from the lectures is allowed.
- Application: HTML + CSS as View, JavaScript as Control (MVC/MVP); "special JavaScript code communicating through a plugin"; the drawing shows a box **"PMQ"** between "Web chrome" and "DTV MW" (meaning not known yet).
- AL: Web chrome + **DTV MW (C/C++) with an "HLS SRV" inside**. PAL: **OS + DTV SDK**. HAL: **TUNER** (fed by `.TS`) **+ SW DMX**. Mandatory blocks: **DTV MW, DTV SDK, SW DMX**. → built into spec 0.11 (HLR-25, Section 2.2.3, IF-INT-04).
- Content must be real `.TS` files; **TSDuck** (open-source C++ MPEG-TS toolkit) was recommended.
- Managed: RTP server on localhost, simplest fixed server architecture; existing server libraries (Apache, …) are allowed; a few JSON responses from the OTT MW REST server.
- Parsing is mandatory.
- "Server stops, client stops?" → built into spec 0.11 (keepalive, REQ-67 – REQ-70).
- "LLM — atomic" (small steps).
- **Seemed to conflict with the current spec** (the timeshift part is resolved, see question 1 below):
  - "Timeshift — happens in the DTV MW", "recording and viewing flows are different", "rewind back in seconds — a few seconds", "via a dialog", "one service". This reads like a **local timeshift in the middleware** (record the received stream, rewind a few seconds via a dialog), while the spec has a server-side archive (D-01), 30 s steps without a dialog (D-02, D-07), a looped archive (D-08) and 4 services (D-09).
  - "TS demux not necessary for broadband managed?" (with a question mark) vs. "SW DMX mandatory". **Treated as resolved:** the same drawing lists SW DMX as a mandatory block, so the spec keeps it (HLR-25).

Open questions (the specification was frozen on 2026-11-22 without these answers; an answer that requires a change is handled as a contract change by rule 2 of Section 2.2.7 — question 3 can only change the transport of IF-INT-01):

1. ~~Timeshift local in the DTV MW or server-side archive?~~ **Resolved (2026-11-21):** the professor confirmed the server-side archive without a local timeshift buffer as a fully valid solution ("you have the freedom, implement and argue"). D-01, D-02, D-07, D-08 stay unchanged; the lecture remarks about a local timeshift do not apply.
2. What does "one service" mean (one service in total, one protected service, one service per TS)?
3. What is "PMQ", and is a browser plugin required, or is the local WebSocket (allowed by the general guidelines for Linux + Web) acceptable?
4. Questions still to ask the professor (3 per day): looped 10-minute content; HTTP stream control instead of RTSP; incorrect key → message vs corrupted picture. (Server-side archive vs timeshift: answered, see question 1.)

## 6. Team decisions and working preferences

These were decided explicitly by the team during the session. Keep them unless the team changes them.

1. **The specification is written in English.** Conversation with team members in Serbian (Latin script).
2. **No "either is acceptable", "TBD" or open assumptions in the specification.** Where the task is not explicit, choose the variant that is **simplest to implement and integrate**, record it as a decision with a rationale, and write definite requirements.
3. **Division of work by course layers is mandatory:** one member per layer — **WP-APP** (application, Web UI), **WP-AL** (application layer, DTV middleware core, C++17), **WP-PAL** (PAL = DTV SDK + platform services in C++, HAL = software tuner / demux / descrambler in C++, and the operator side: head-end, OTT MW REST server, packaging tool in Python, content generator as a shell + FFmpeg script). The three layers are implemented **in parallel on separate Git branches** (`app`, `al`, `pal`). Contracts between layers (Section 3) must be frozen on `main` before coding.
4. **Who owns which layer is not decided yet.** The specification stays neutral: no names per work package.
5. **For now only the specification is written.** No source code, no repository skeleton, no other files until the team says so.
6. Internet is available at the defense, so hls.js is loaded from a CDN with a pinned version (HLR-21).
7. Work step by step: one section (or subsection) per step, then a short summary to the team member for review; wait for confirmation before the next step.
8. **Milestone dates** of Section 2.2.7 (I-1 … I-5) are not maintained for now; do not report them as inconsistencies (Ivan, 2026-11-22).
9. **Inconsistencies:** if the agent finds one and knows how to resolve it, it resolves it directly and reports what it changed (Ivan, 2026-11-22).
10. **This file (HANDOVER.md) is updated only when Ivan asks for it** (Ivan, 2026-11-22).
11. **The specification is frozen** as version 1.0 (Ivan, 2026-11-22), including Section 3; see §7.

## 7. Current state of `SPECIFICATION.md` (version 1.0, frozen)

| Section | Status | Content |
|---|---|---|
| Header, conventions | frozen | Tags `[TASK]`, `[COMMON]`, `[GUIDELINES]`, `[D-xx]`. |
| 1. User, goal, boundaries | frozen | Users, goals G-01 … G-09, context diagram, inputs (key bindings per context in 1.5.1), outputs (1.6.1 now lists every viewer message and dialog title), boundaries, **scope decisions D-01 … D-14**, user stories US-01 … US-22, traceability. |
| 2. Requirements | frozen | HLR-01 … HLR-25; technical postulates 2.2.1 – 2.2.8; REQ-01 … REQ-70; NFR-01 … NFR-17; EXT-01 … EXT-11; HLR → REQ/NFR traceability. 2.2.8 now also lists the verification tools GoogleTest 1.14.0, tcpdump 4.99.4, xdotool 3.20160805.1 and curl 8.5.0. |
| 3. Interface specification | frozen (on Ivan's decision, without the separate team review) | Contracts IF-INT-01, IF-INT-02, IF-INT-04, IF-EXT-03, IF-EXT-04 and — new in 2.2.5 / rule 1 of 2.2.7 — the TS content rules of IF-EXT-05 (WP-PAL ↔ WP-AL). |
| 4. Design | frozen | 4.1 – 4.8; see the summary below. |
| 5. Verification plan | frozen (5.9 is filled in during testing) | 168 tests; see the summary below. |

### Changes of the second session on 2026-11-22 outside Section 5 (all in version 1.0)

Consistency fixes: REQ-44 (protection decided on the first video packet with ≥ 16 B payload; S9, `EV-VIDEO1`); **new transition T-33** (`set_key` of the same service after PMT while the previous key is not yet accepted restarts the attempt, so that a rejection of the old key cannot discard the new one; T-25 guard, REQ-49, 3.6.2, rule 1 of 4.3.1, 4.2.2, diagram and coverage updated); 3.5 key from standard input; NFR-08 (no component writes a key; test keys only in Section 5.2.2 and in test code); HLR-16 (key never sent to / from the operator side); 4.1.3 AL-SVC without EPG cache; REQ-39 (`→` in LIVE only with a pending target; final target ≥ now → `"live"`); B7 at most 8 segment files + 1 temporary; 1.6.1 messages and dialog titles; 4.4.4 reactions to error responses of `play` and `set_key`; `ITuner::Stats::rtp_invalid` comment (1 – 7 × 188 B); 4.6.6 wording; REQ-48 (`K` in full-screen playback); IF-INT-05 name and IF-EXT-05 ownership in 2.2.5; US-05 AC2 (5 s after the head-end accepted).

Additions required by Section 5: NFR-13 time stamps with milliseconds; 4.4.6 diagnostics mode `?diag=1` (marks `key`, `osd`, `frame`, `frag`, `wait`, `resume`); 4.7 operator-side modules read the time through injected clock functions; 4.7.1 test files (`content/test/`, `content/SHA256SUMS`, `server/config.test.json`).

### Section 5 in short (confirmed by Ivan on 2026-11-22)

| Subsection | Key content and decisions |
|---|---|
| 5.1 Approach | Levels unit / integration / acceptance; environments E1 (unit), E2 (integration), E3 (demo laptop, all NFR measurements). One table row per test (columns ID, Verifies, Anchors, Preconditions and input, Steps, Expected result, Evidence); results only in 5.9. ID blocks: `TST-1xx` AL unit, `2xx` PAL/HAL unit, `3xx` operator-side unit, `4xx` APP unit, `5xx` integration, `6xx` acceptance, `7xx` NFR / robustness / concurrency. Status values passed / failed / not executed / not verifiable. Evidence `TESTOUT`, `LOG`, `FFPROBE`, `CAPTURE` (pcap deleted after analysis), `MEASURE`, `PHOTO`, `NOTE` (audio only) in `tests/evidence/<TST-id>/`. Rules for valid evidence (no golden files from own code, R5 cross-check, LLM tests reviewed by another WP, acceptance executed by a non-owner). Time measurement from one clock: log ms time stamps + `DIAG` lines in `logs/chromium.log`. |
| 5.2 Test data | Content files are **not committed** (≈ 262 MB each); `content/SHA256SUMS` is committed and checked before every acceptance run. Test set in `content/test/`: `novideo.ts`, `svc-2.altpid.ts`, `svc-4.altpid.enc.ts` (program 2, PIDs 768 / 1025 / 1026) used through `server/config.test.json` (no fifth service). Keys K3 `3A7F1C9E5B2D8046A1E3C5079B4D6F28`, K4 `C04E92B7165FD83A2E7B0C9145D6A3F1`, KX `00112233445566778899AABBCCDDEEFF`, K3L, KF1, KF2. Program titles of all services (5.2.3). EPG formulas and worked example at `t = 1795000000`. Invalid inputs IN-01 … IN-12. Section 5.2 is the test data documentation of NFR-17. |
| 5.3 Unit tests | TST-101 … 159 (AL incl. all T-01 … T-33, TST-148 queue overflow, TST-159 portability), TST-201 … 226 (HAL/PAL), TST-301 … 320 (operator side), TST-401 … 419 (APP). A test that needs missing content fails, never skips. |
| 5.4 Integration tests | Configurations CFG-OP, CFG-RXP (`pal_dump`), CFG-MW (WebSocket client + HLS checker), CFG-MOCK, CFG-ALL. Tools in `tests/tools/`: `rtp_capture.py`, `ws_client.py`, `hls_check.py`, `contract_check.py`, `mw_stub.py`, `headend_stub.py`. TST-501 … 543, incl. contract tests that run the same script against the mock and the real middleware (TST-532, 533). |
| 5.5 Acceptance tests | PRE-1 … PRE-3; TST-601 … 626, every user story; stubs only where a negative AC cannot be produced otherwise; TST-626 = rehearsal of the demo scenario. |
| 5.6 NFR / robustness | Intervals `t_play`, `t_picture`, `t_reject`, `t_osd`, `t_gap`; TST-701 … 720 (NFR-01 19/20 zaps; NFR-06 2 × 30 min; NFR-08 pcap + file search with a positive control inside `/ws/control`; TST-719 300 keys by `xdotool` from a fixed list, seed 15). |
| 5.7 Traceability | Generated from the test tables (script in §9); every REQ, NFR, US, T, ER and B has at least one test. First column in bold so that §9 does not count the rows as definitions. |
| 5.8 Demo scenario | Preparation checklist; 11 steps in 4 min 30 s + 30 s reserve; core: EPG → previous event of "Premium One" → key dialog by itself → KX rejected → K3 plays from `00:00` / `05:00`; contingencies; task coverage. No "No signal" in the demo. |
| 5.9 Results | Recording rules, summary (100 unit, 22 integration, 46 acceptance; all "not executed"), result row for every test (ID in bold). |

### Section 4 in short (decisions confirmed by Ivan on 2026-11-22)

| Subsection | Key content and decisions |
|---|---|
| 4.1 Architecture | Processes P1 Chromium, P2 `dtv-middleware`, P3 `server/headend.py`, P4 `server/mw_server.py`, offline `content/generate.sh`, `server/package.py`; start order P4 → P3 → P2 → P1. Mermaid block diagram over Application / AL / PAL / HAL / drivers / operator side, built / reused / simulated (HAL blocks and SRV-ARC simulated). Module table with requirements. Dependency rules R1 – R5 (R5: receiver and operator side share no D-04 code). Internal HAL API `hal::IpTuner`, `hal::Demux`, `hal::Descrambler` (not frozen). |
| 4.2 AL internal design | One AL worker thread with a task queue; PAL callbacks only copy and post. Control tasks never dropped, data tasks bounded. **Generation** counter (per stream attempt, incremented on `play` and on the `set_key` restart) separate from the session number; tuner status uses `untune → tuned_generation = g → tune`. Data structures, class diagram, session steps S1 – S12 (PAT filter and HLS directory opened **before** `POST /start`). No EPG cache in the AL. MW timeout 800 ms, head-end 2000 ms. Segment cut on `random_access_indicator`. Config `middleware/main/middleware.json`. Orderly stop ≤ 3 s. |
| 4.3 AL-SES state machine | Rules (data path in `starting` only sets flags, applied at `/start` response; RELEASE procedure incl. `untune`), events `EV-…`, Mermaid state diagram, transition table **T-01 … T-33** (T-33 added in the second session). Stream deadline 3 s after the first `Locked` → `INVALID_STREAM` (stored key kept). `FUTURE_TIME` → one retry as live. |
| 4.4 Application design | Files in `webui/`, model fields and derived values, UI contexts with state diagram and key table, reactions to IF-INT-01, timers. `key_required` opens the key dialog from any context; APP fetches EPG at start-up and every 300 s; AL answers `play` before the first event of the new session; `→` may undo a pending `←`; Chromium `--autoplay-policy=no-user-gesture-required`; service list without wrap-around; tests `node --test webui/tests/`. |
| 4.5 Producer–consumer | Basis ≤ 4 Mbit/s (added to REQ-62). Threads of P2 – P4; PAL command queue + `mg_wakeup`; filter-table mutex. Buffers **B1 … B10** with capacity, overflow policy, owner; AL data queue 8 192 tasks; `SO_RCVBUF` 1 MiB without changing `rmem_max`; segment removed 3 positions after leaving the playlist (REQ-23 aligned). |
| 4.6 Sequence diagrams | 4.6.1 start-up, 4.6.2 PSI via DTV SDK, 4.6.3 live zap, 4.6.4 archive from EPG, 4.6.5 no key → incorrect key → correct key from the same T, 4.6.6 rapid zapping, 4.6.7 steps and live, 4.6.8 head-end lost, 4.6.9 head-end `/start`. |
| 4.7 Operator side | FFmpeg parameters (testsrc2, drawtext, x264 closed GOP 25, AAC 128k, CBR mux 3.5 Mbit/s, default PIDs 4096 / 256 / 257); packaging algorithm (key from stdin; payload < 16 B stays clear → REQ-51 checks only scrambled PES starts); PCR index and `T mod D`; pacing by interpolated content time; session table and watchdog; EPG formula. |
| 4.8 Error handling | Principles and error matrix **ER-01 … ER-40**, coverage of all error codes, user-story negative cases and guideline cases. hls.js fatal error: one `recoverMediaError()`, one player re-creation, then "No signal"; "No signal" while the control WebSocket is down; `INVALID_KEY_FORMAT` reopens the key dialog; `start.sh` checks `GET /` before starting Chromium. |

### Key decisions to keep in mind (summary of Section 1.8)

| ID | Decision |
|---|---|
| D-01 | Server-side archive (catch-up); no timeshift buffer in the receiver. |
| D-02 | Time selection through the EPG (past event + ENTER) and `←`/`→` steps of 30 s; `B` = back to live. |
| D-03 | The viewer types the key: 32 hex characters, kept in memory per service for the session. |
| D-04 | One fixed AES-128 key per protected service; AES-128-CBC per TS packet payload of the A/V PIDs, IV = 0, residual < 16 B in clear; headers, adaptation fields, PAT, PMT in the clear; `transport_scrambling_control = '10'`; content encrypted offline. |
| D-05 | Unicast RTP (RFC 2250, PT 33, 7 TS packets per RTP packet), no multicast. |
| D-06 | Stream control with HTTP POST + JSON (`/start` with receiver-chosen SSRC, `/keepalive` every 5 s, `/stop`), not RTSP; the head-end stops sessions without keepalive for 15 s. |
| D-07 | Rewind step 30 s. |
| D-08 | Archive window 2 h; content file looped on wall-clock time (position = UNIX time mod file duration); EPG on the same cycle. |
| D-09 | 4 services (SPTS), FFmpeg-generated H.264 720p25 + AAC, 10-minute files = 2 programs × 5 min, burned-in service name / title / time code; services 1, 2 clear; 3, 4 protected with different keys. |
| D-10 | Protected services need the key in live and in the archive. |
| D-11 | Incorrect key: detected (decrypted PES must start with `00 00 01`), message shown, nothing passed to the player. |
| D-12 | One Ubuntu 24.04 laptop, loopback only, Chromium kiosk, HDMI. |
| D-13 | AL packages the decrypted SPTS as HLS (≈1 s TS segments, sliding-window `index.m3u8`, `#EXT-X-DISCONTINUITY` at the loop boundary) in a temporary directory; the middleware's existing HTTP server serves it under `/hls/<session>/`; the browser plays it with hls.js 1.7.4 (MSE). No extra server. |
| D-14 | After key entry, playback restarts from the originally requested start point. |

Technology (Section 2.2.8): C++17, GCC 13.3, CMake 3.28, OpenSSL 3.0.13 (PAL only), Mongoose 7.23 (PAL only), nlohmann/json 3.12.0, Python 3.12 (stdlib + `python3-cryptography`), FFmpeg 6.1.1, Chromium, hls.js 1.7.4 from jsDelivr, Node.js 18.19 (tests only); verification: GoogleTest 1.14.0, tcpdump 4.99.4, xdotool 3.20160805.1, curl 8.5.0. Default ports: MW REST 8080, head-end control 8081, RTP UDP 5004, middleware HTTP/WebSocket 8090 (Web application files, control WebSocket `/ws/control`, HLS under `/hls/`). HLS files in `/tmp/m15-hls`.

### Known risks and points to watch

- **Mermaid diagrams of Section 4 were not checked with a renderer** (no mermaid-cli installed). Open `SPECIFICATION.md` in the GitHub or VS Code preview; a diagram that does not render is fixed as an editorial change.
- **Section 3 was frozen without a separate team review** (Ivan's decision). Lazar and Smilja should read Section 3 and the changes listed in §7 before they start coding; disagreements are handled by rule 2 of Section 2.2.7.
- **Loop boundary of the content (REQ-24):** when the 10-minute file wraps, PCR/PTS/DTS jump back by 600 s. The AL closes the segment and marks the next one with `#EXT-X-DISCONTINUITY`; hls.js is expected to handle this. Test early (NFR-06).
- **HLS start position and latency:** playback must begin at the first segment of the playlist (hls.js `startPosition: 0`), otherwise the rewind would not start exactly at T (NFR-04, D-14). Verify this hls.js behavior at I-2. Zap time with HLS is longer than with a direct stream (NFR-01 allows 6 s to first picture).
- **Browser playback path** (HLS files → hls.js → MSE) is the main integration risk; milestone I-2 exists to prove it early.
- **WP-PAL is the largest work package** (DTV SDK, HAL tuner / demux / descrambler and the whole Python operator side). The team has not yet decided whether to move the operator side (or SRV-PKG / SRV-GEN) to another work package; this would be a change of Section 2.2.4 / 2.2.7.
- The PMQ / plugin question (§5a, question 3) is still open; its answer can change only the transport of IF-INT-01.
- **Content generation is a prerequisite of many tests** (every test marked "content" in 5.3, all of 5.4 – 5.6): run `content/generate.sh` and `server/package.py` early on every developer machine.
- **Test volume:** 168 tests; the unit tests are part of every pull request, so WP owners should write them together with the modules, not at the end.

## 8. Next steps

The specification work is finished (Sections 1 – 5, version 1.0, frozen). Nothing below is started without the team's explicit go-ahead (§6, item 5).

1. **Answers of the professor** to the open questions of §5a, if any arrive → contract change by rule 2 of Section 2.2.7 and a new document version.
2. **Assignment of the work packages** WP-APP, WP-AL, WP-PAL to the three members (§6, item 4).
3. **Repository skeleton on `main`:** the directories of Sections 2.2.7, 4.7.1, 5.3.1 and 5.4.2 (`webui/`, `middleware/{include/pal,core,main,pal,hal}/`, `server/`, `content/`, `scripts/`, `tests/{tools,integration,acceptance,evidence}/`), `middleware/include/pal/platform.hpp` exactly as in Section 3.9.2, empty CMake targets (`dtvcore`, `pal`, `hal`, `dtv-middleware`, `dtvcore_tests` with GoogleTest), `.gitignore` with `content/*.ts` and `content/test/*.ts`, `README.md` referring to Section 5.2. Then branches `app`, `al`, `pal`.
4. **First tools:** `tests/tools/trace_check.py` (from the script in §9) and `tests/tools/contract_check.py`, because they check the specification and the stand-ins independently of the implementation.
5. During testing: fill in Section 5.9 (only section that changes without a new version).

## 9. Conventions and consistency checks for editing `SPECIFICATION.md`

- IDs are stable once used: G-xx, D-xx, US-xx, HLR-xx, REQ-xx, NFR-xx, EXT-xx, IF-EXT-xx, IF-INT-xx, module IDs APP-/AL-/PAL-/SRV-xx, milestones I-x, tests TST-xx. Do not renumber; add new IDs at the end of a range.
- If a decision changes, change it in Section 1.8 **and** everywhere it is used (user stories, requirements, interfaces).
- Section 3 is a contract between two team members: changing it requires both owners' agreement (rule 2 of 2.2.7).
- Keep diagrams in Mermaid inside the Markdown file.
- The specification is frozen: a change needs the agreement of the owners concerned and a new `Document version` (1.1, 1.2, …) with a one-line description in the header. Section 5.9 is the only exception.
- Tables in Sections 5.7 and 5.9 print the ID in the first column in bold (`| **REQ-01** |`), so that the check below does not count them as definitions.
- After a change of a test table, regenerate Section 5.7 with the script at the end of this section.
- Use "they" or role names, never assumed pronouns.

Consistency check used during the session (run from the repository root; prints counts and any dangling references):

```bash
python3 - <<'EOF'
import re
s = open('SPECIFICATION.md').read()
for p in ['REQ', 'NFR', 'HLR', 'US', 'D', 'EXT', 'T', 'ER', 'TST']:
    # user stories are defined by their bold heading; all other IDs by the first cell of a table row
    defined = (re.findall(rf'^\*\*({p}-\d+) ', s, re.M) if p == 'US'
               else re.findall(rf'^\| ({p}-\d+) \|', s, re.M))
    used = set(re.findall(rf'\b{p}-\d+\b', s))
    dup = sorted({x for x in defined if defined.count(x) > 1})
    print(p, 'defined:', len(set(defined)), 'duplicates:', dup, 'dangling:', sorted(used - set(defined)))
print('code fences balanced:', s.count('```') % 2 == 0)
EOF
```

Expected output for version 1.0: REQ 70, NFR 17, HLR 25, US 22, D 14, EXT 11, T 33, ER 40, TST 168 defined; no duplicates; no dangling references; code fences balanced.

Generator of the tables of Section 5.7 (to become `tests/tools/trace_check.py`). It reads the test tables of Sections 5.3 – 5.6, prints the new Section 5.7 to standard output and lists on standard error every REQ, NFR, US, T, ER or B without a test (`missing: []` for version 1.0). Replace the text between `### 5.7 Traceability` and `### 5.8 Defense` with its output and then print the first column in bold (`re.sub(r'^\| ((?:REQ|NFR|HLR|US|T|ER)-\d+|B\d+) \|', r'| **\1** |', …)`), adding the sentence about the bold column before "Columns:".

```python
import re,sys
s=open('SPECIFICATION.md').read()
if '### 5.7 Traceability' in s: s=s[:s.index('### 5.7 Traceability')]
rows=re.findall(r'^\| (TST-\d+) \| ([^|]*) \| ([^|]*) \|',s,re.M)
def ids(text,p):
    pat = r'\bB(\d+)\b' if p=='B' else rf'\b{p}-(\d+)\b'
    out=set(int(x) for x in re.findall(pat,text))
    rp = r'\bB(\d+) – B(\d+)' if p=='B' else rf'{p}-(\d+) – {p}-(\d+)'
    for a,b in re.findall(rp,text): out|=set(range(int(a),int(b)+1))
    return out
def collect(p,cols):
    d={}
    for tid,ver,anc in rows:
        src={'v':ver,'a':anc,'va':ver+' '+anc}[cols]
        for i in ids(src,p): d.setdefault(i,set()).add(tid)
    return d
key=lambda t:int(t[4:])
lvl=lambda t:'U' if key(t)<500 else ('I' if key(t)<600 else 'A')
def cell(l,L):
    x=sorted((t for t in l if lvl(t)==L),key=key); return ', '.join(x) if x else '—'
verif={k:v.strip() for p in ['REQ','NFR'] for k,v in re.findall(rf'^\| ({p}-\d+) \|.*\| ([^|]+) \|\s*$',s,re.M)}
missing=[]
o=['''### 5.7 Traceability

The tables of this section are generated from the columns "Verifies" and "Anchors" of the test tables of Sections 5.3 – 5.6 by `tests/tools/trace_check.py`; a range such as `REQ-33 – REQ-36` counts for every requirement in it. The script fails if a detailed requirement, user story, transition, error or buffer of Sections 2 – 4 has no test. It is run after every change of a test table, and the tables below are replaced with its output, so that this section never differs from the test definitions. Columns: **U** unit tests (Section 5.3), **I** integration tests (5.4), **A** acceptance, non-functional and robustness tests (5.5, 5.6); "Verif." repeats the verification method of Section 2.

A high-level requirement is verified when all detailed requirements refined from it are verified (Section 2; traceability in Section 2.3.14); Section 5.7.3 lists in addition the tests that refer to a high-level requirement directly.

#### 5.7.1 Detailed functional requirements → tests

| Requirement | Verif. | U | I | A |
|---|---|---|---|---|''']
for P,n,title in [('REQ',70,None),('NFR',17,'#### 5.7.2 Non-functional requirements → tests')]:
    d=collect(P,'v')
    if title: o+=['',title,'','| Requirement | Verif. | U | I | A |','|---|---|---|---|---|']
    for i in range(1,n+1):
        k=f'{P}-{i:02d}'; l=d.get(i,set())
        if not l: missing.append(k)
        o.append(f'| {k} | {verif[k]} | {cell(l,"U")} | {cell(l,"I")} | {cell(l,"A")} |')
o+=['','#### 5.7.3 High-level requirements and user stories → tests','','| High-level requirement | Tests referring to it directly |','|---|---|']
d=collect('HLR','v')
for i in range(1,26):
    l=sorted(d.get(i,[]),key=key)
    o.append(f'| HLR-{i:02d} | {", ".join(l) if l else "— (through its detailed requirements, Section 2.3.14)"} |')
o+=['','| User story | Tests |','|---|---|']
d=collect('US','v')
for i in range(1,23):
    l=sorted(d.get(i,[]),key=key)
    if not l: missing.append(f'US-{i}')
    o.append(f'| US-{i:02d} | {", ".join(l)} |')
o+=['','#### 5.7.4 Design elements → tests','']
for P,n,name,intro in [('T',33,'Transition','Transitions of Section 4.3.4:'),('ER',40,'Error','Errors of Section 4.8.2:'),('B',10,'Buffer','Buffers of Section 4.5.4:')]:
    d=collect(P,'va')
    o+=[intro,'',f'| {name} | Tests |','|---|---|']
    for i in range(1,n+1):
        k=f'B{i}' if P=='B' else f'{P}-{i:02d}'
        l=sorted(d.get(i,[]),key=key)
        if not l: missing.append(k)
        o.append(f'| {k} | {", ".join(l)} |')
    o.append('')
sys.stderr.write('missing: %s\n'%missing)
print('\n'.join(o))
```
