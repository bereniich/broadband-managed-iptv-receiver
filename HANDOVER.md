# Handover — M15 specification work

**Written:** 2026-11-19, end of the first specification session (Ivan Berenić with Claude Code). **Updated:** 2026-11-20 — browser playback switched to HLS (D-13).
**For:** the next team member (Lazar or Smilja) and their coding agent, to continue writing `SPECIFICATION.md` without access to the previous conversation.
**Read first:** this file completely, then `SPECIFICATION.md` completely. The specification is the source of truth. This file explains how it was made, which rules apply and what comes next.

---

## 1. How to start (for the agent)

Suggested first prompt for the agent:

> Read `HANDOVER.md` and then `SPECIFICATION.md` in full. Do not write code or create other files. Confirm the current state (sections done, next section), list any inconsistencies you notice, and then continue with Section 4 as described in HANDOVER.md §8. Write the specification in English; talk to me in Serbian (Latin script).

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

## 6. Team decisions and working preferences

These were decided explicitly by the team during the session. Keep them unless the team changes them.

1. **The specification is written in English.** Conversation with team members in Serbian (Latin script).
2. **No "either is acceptable", "TBD" or open assumptions in the specification.** Where the task is not explicit, choose the variant that is **simplest to implement and integrate**, record it as a decision with a rationale, and write definite requirements.
3. **Division of work by course layers is mandatory:** one member per layer — **WP-APP** (application, Web UI), **WP-AL** (application layer, DTV middleware core, C++17), **WP-PAL** (platform layer adapters in C++ + the operator side in Python: head-end, OTT MW REST server, packaging tool, content generator). The three layers are implemented **in parallel on separate Git branches** (`app`, `al`, `pal`). Contracts between layers (Section 3) must be frozen on `main` before coding.
4. **Who owns which layer is not decided yet.** The specification stays neutral: no names per work package.
5. **For now only the specification is written.** No source code, no repository skeleton, no other files until the team says so.
6. Internet is available at the defense, so hls.js is loaded from a CDN with a pinned version (HLR-21).
7. Work step by step: one section (or subsection) per step, then a short summary to the team member for review.

## 7. Current state of `SPECIFICATION.md` (version 0.10)

| Section | Status | Content |
|---|---|---|
| Header, conventions | done | Tags `[TASK]`, `[COMMON]`, `[GUIDELINES]`, `[D-xx]`. |
| 1. User, goal, boundaries | done | Users, goals G-01 … G-09, context diagram, inputs (key bindings in 1.5.1), outputs, boundaries (built / reused / simulated, out of scope, external interfaces IF-EXT-01 … 06), **scope decisions D-01 … D-14**, user stories US-01 … US-22 with acceptance criteria, traceability. |
| 2.1 High-level requirements | done | HLR-01 … HLR-24 from user goals, boundaries and technical constraints. |
| 2.2 Technical postulates | done | Standards and formats, signal path (Mermaid, numbered media / metadata / control arrows), layering (App / AL / PAL / HAL, portability rule), modules (APP-xx, AL-xx, PAL-xx, SRV-xx), key interfaces, design patterns, parallel development (branches, rules, stand-ins, milestones I-1 … I-5), versions of reused components. |
| 2.3 Functional requirements | done | REQ-01 … REQ-66, each with layer (APP / AL / PAL / SRV), source and verification method; traceability HLR → REQ. |
| 2.4 Non-functional requirements | done | NFR-01 … NFR-17 (measurable). |
| 2.5 Extensions | done | EXT-01 … EXT-11 (not mandatory). |
| 3. Interface specification | done, **to be reviewed and frozen at I-1 (2026-11-21)** | Conventions; IF-EXT-03 MW REST; IF-EXT-04 stream control; IF-EXT-05 RTP; IF-INT-05 content config; IF-INT-01 UI control WebSocket (commands, events, session states); IF-INT-02 HLS media stream (playlist + segments over HTTP); IF-INT-03 web files; IF-INT-04 C++ platform header (`platform.hpp`, compiles with `g++ -std=c++17 -Wall -Wextra`); error codes. |
| 4. Design | **next** | See §8. |
| 5. Verification plan | after Section 4 | See §8. |

### Key decisions to keep in mind (summary of Section 1.8)

| ID | Decision |
|---|---|
| D-01 | Server-side archive (catch-up); no timeshift buffer in the receiver. |
| D-02 | Time selection through the EPG (past event + ENTER) and `←`/`→` steps of 30 s; `B` = back to live. |
| D-03 | The viewer types the key: 32 hex characters, kept in memory per service for the session. |
| D-04 | One fixed AES-128 key per protected service; AES-128-CBC per TS packet payload of the A/V PIDs, IV = 0, residual < 16 B in clear; headers, adaptation fields, PAT, PMT in the clear; `transport_scrambling_control = '10'`; content encrypted offline. |
| D-05 | Unicast RTP (RFC 2250, PT 33, 7 TS packets per RTP packet), no multicast. |
| D-06 | Stream control with HTTP POST + JSON (`/start`, `/stop`), not RTSP. |
| D-07 | Rewind step 30 s. |
| D-08 | Archive window 2 h; content file looped on wall-clock time (position = UNIX time mod file duration); EPG on the same cycle. |
| D-09 | 4 services (SPTS), FFmpeg-generated H.264 720p25 + AAC, 10-minute files = 2 programs × 5 min, burned-in service name / title / time code; services 1, 2 clear; 3, 4 protected with different keys. |
| D-10 | Protected services need the key in live and in the archive. |
| D-11 | Incorrect key: detected (decrypted PES must start with `00 00 01`), message shown, nothing passed to the player. |
| D-12 | One Ubuntu 24.04 laptop, loopback only, Chromium kiosk, HDMI. |
| D-13 | AL packages the decrypted SPTS as HLS (≈1 s TS segments, sliding-window `index.m3u8`, `#EXT-X-DISCONTINUITY` at the loop boundary) in a temporary directory; the middleware's existing HTTP server serves it under `/hls/<session>/`; the browser plays it with hls.js 1.7.4 (MSE). No extra server. |
| D-14 | After key entry, playback restarts from the originally requested start point. |

Technology (Section 2.2.8): C++17, GCC 13.3, CMake 3.28, OpenSSL 3.0.13 (PAL only), Mongoose 7.23 (PAL only), nlohmann/json 3.12.0, Python 3.12 (stdlib + `python3-cryptography`), FFmpeg 6.1.1, Chromium, hls.js 1.7.4 from jsDelivr. Default ports: MW REST 8080, head-end control 8081, RTP UDP 5004, middleware HTTP/WebSocket 8090 (Web application files, control WebSocket `/ws/control`, HLS under `/hls/`). HLS files in `/tmp/m15-hls`.

### Known risks and points to watch

- **Loop boundary of the content (REQ-24):** when the 10-minute file wraps, PCR/PTS/DTS jump back by 600 s. The AL closes the segment and marks the next one with `#EXT-X-DISCONTINUITY`; hls.js is expected to handle this. Test early (NFR-06).
- **HLS start position and latency:** playback must begin at the first segment of the playlist (hls.js `startPosition: 0`), otherwise the rewind would not start exactly at T (NFR-04, D-14). Verify this hls.js behavior at I-2. Zap time with HLS is longer than with a direct stream (NFR-01 allows 6 s to first picture).
- **Browser playback path** (HLS files → hls.js → MSE) is the main integration risk; milestone I-2 exists to prove it early.
- **WP-PAL is the largest work package** (C++ adapters + whole Python operator side). If it proves too large, SRV-PKG and SRV-GEN can move to WP-AL (the AL owner knows D-04 in detail anyway). This would be a change of Section 2.2.4 / 2.2.7.
- Section 3 has not yet been reviewed by the whole team; it must be agreed before 2026-11-21 (I-1).

## 8. Next steps

### 8.1 Section 4 — Design specification

Planned subsections (adjust if needed, but cover everything the guidelines require):

1. **4.1 Architecture block diagram** across **HAL, PAL, AL and application** (the course's Project 2 deliverable), with every module of 2.2.4 placed in its layer, marked built / reused / simulated. Mermaid.
2. **4.2 AL internal design:** threading model (proposal: PAL callbacks only post tasks to one AL worker thread with a task queue, so AL state needs no locks), main data structures (service list, session object with generation number, kept start point, key store), module collaboration.
3. **4.3 AL-SES session state machine:** states exactly as in IF-INT-01 §3.6.3 (`idle`, `starting`, `waiting_stream`, `key_required`, `validating_key`, `playing`, `no_signal`, `stopped`, `error`); events (play, HTTP ok / error, first data, PAT/PMT parsed, protection decided, set_key, key accepted / rejected, 5 s and 3 s timers, data resumed, stop); **state diagram (Mermaid `stateDiagram-v2`) + full transition table** (state × event → action, next state, emitted event).
4. **4.4 Application design:** MVP structure (APP-MOD, APP-PRS, APP-VIEW, APP-KEY, APP-MWC), UI contexts (full-screen playback, number entry, service list, EPG, key dialog, info overlay) as a state diagram/transition table matching the key bindings of 1.5.1, how the model reacts to `session_state`, `stream_info`, `mw_status`; the timers (300 ms zap debounce, 500 ms seek accumulation, 2 s number commit, 5 s info, 2 s volume indicator).
5. **4.5 Producer–consumer and buffers:** threads (PAL receive thread, PAL event loop, AL worker, head-end session threads), queues with capacity and overflow policy (RX → AL queue, validation hold buffer, HLS segment window of at most 10 files), who allocates and who frees each buffer.
6. **4.6 UML sequence diagrams** (Mermaid `sequenceDiagram`). Implementation level (real command / event / function names from Section 3) for APP and AL; demonstration level for PAL and operator side. At least:
   - start-up: service list (+ MW not reachable variant),
   - live zap of a clear service (play → /start → RTP → PAT/PMT → stream_info → first HLS segment → playing → hls.js loads `/hls/<n>/index.m3u8`),
   - archive from the EPG (get_epg → ENTER on an available event → play with T),
   - protected service: no key → key dialog → **incorrect key → key_rejected → correct key → playback from the same T** (the core M15 scenario),
   - rapid zapping / "latest request wins" (old session stopped, late events ignored),
   - `←`/`→` steps and return to live,
   - one operator-side sequence (head-end `/start`: archive time → file position → PAT/PMT → paced RTP).
7. **4.7 Operator-side design:** PCR index and time → position mapping, pacing algorithm, loop handling, packaging-tool algorithm (exact byte ranges per D-04), EPG generation formula on the 600 s cycle.
8. **4.8 Error handling:** a matrix error → where detected → layer → reaction → message to the viewer → recovery, covering every error code of 3.10 and every negative case of the user stories.

### 8.2 Section 5 — Verification plan

- Test case format required by the guidelines: `TST-xx` → REQ/NFR → preconditions and input → steps → measurable expected result → actual result and evidence → status (passed / failed / not executed / not verifiable).
- Levels: unit (per module, AL with fake PAL), integration (layer pairs over the real interfaces), acceptance (user stories in the target environment).
- Every REQ-xx and NFR-xx must be covered by at least one test; add a traceability table REQ → TST.
- Include negative and concurrency cases (malformed JSON, bad CRC, random datagrams, server restart, rapid zapping, key entered during an ongoing start).
- Independent verification: `ffprobe -show_programs -show_streams` for content and output streams, burned-in time code for rewind accuracy, `tcpdump` on loopback plus a search for the key bytes (NFR-08), log evidence.
- Define the test data set: the four content files with SHA-256 checksums, the concrete correct keys of `svc-3` and `svc-4`, one incorrect key, expected EPG.
- Define the 5-minute defense demo scenario (ordered steps that show every mandatory feature, the incorrect key and the correct key with resume from T).

### 8.3 After the specification

1. Team review of Section 3 and freeze at I-1 (2026-11-21), merged to `main`.
2. Only when the team says so: repository skeleton on `main` (directories of 2.2.7, `middleware/include/pal/platform.hpp` from Section 3.9.2, empty CMake targets), then branches `app`, `al`, `pal`.
3. Update the document header (version, status) after every step.

## 9. Conventions and consistency checks for editing `SPECIFICATION.md`

- IDs are stable once used: G-xx, D-xx, US-xx, HLR-xx, REQ-xx, NFR-xx, EXT-xx, IF-EXT-xx, IF-INT-xx, module IDs APP-/AL-/PAL-/SRV-xx, milestones I-x, tests TST-xx. Do not renumber; add new IDs at the end of a range.
- If a decision changes, change it in Section 1.8 **and** everywhere it is used (user stories, requirements, interfaces).
- Section 3 is a contract between two team members: changing it requires both owners' agreement (rule 2 of 2.2.7).
- Keep diagrams in Mermaid inside the Markdown file.
- Bump the `Document version` and `Status` rows in the header after each step.
- Use "they" or role names, never assumed pronouns.

Consistency check used during the session (run from the repository root; prints counts and any dangling references):

```bash
python3 - <<'EOF'
import re
s = open('SPECIFICATION.md').read()
for p in ['REQ', 'NFR', 'HLR', 'US', 'D', 'EXT']:
    # user stories are defined by their bold heading; all other IDs by the first cell of a table row
    defined = (re.findall(rf'^\*\*({p}-\d+) ', s, re.M) if p == 'US'
               else re.findall(rf'^\| ({p}-\d+) \|', s, re.M))
    used = set(re.findall(rf'\b{p}-\d+\b', s))
    dup = sorted({x for x in defined if defined.count(x) > 1})
    print(p, 'defined:', len(set(defined)), 'duplicates:', dup, 'dangling:', sorted(used - set(defined)))
print('code fences balanced:', s.count('```') % 2 == 0)
EOF
```
