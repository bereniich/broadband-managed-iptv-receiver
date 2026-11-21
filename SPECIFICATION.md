# M15 — Broadband Managed IPTV Receiver: Protected Rewind

## Technical Specification

| Item | Value |
|---|---|
| Course | Software in Digital Television (Softver u digitalnoj televiziji) |
| Group / task code | Group VII — M15 (group project, 3 students) |
| Team | Lazar Zubović (RA 13/2023), Smilja Krulj (RA 145/2023), Ivan Berenić (RA 59/2023) |
| Implementation variant | Linux + Web |
| Defense | 2026-12-03 |
| Document version | 0.11 (draft): layering aligned with the lecture (DTV SDK in PAL; tuner, SW demux, descrambler in HAL); head-end keepalive for a stopped receiver |
| Status | Sections 1, 2, 3 — done (Section 3 to be frozen at I-1); Section 4 (design) and Section 5 (verification plan) — next |

### Document conventions

- **[TASK]** — stated explicitly in the assigned project task.
- **[COMMON]** — part of the common requirements that apply to all groups.
- **[GUIDELINES]** — derived from the general project guidelines of the course.
- **[D-xx]** — a scope decision made by the team where the task is not explicit. All decisions are listed in Section 1.8 and are binding for the rest of this specification.
- Requirement IDs (`REQ-xx`) and test IDs (`TST-xx`) are introduced in later sections; user stories (`US-xx`) defined here are the source they trace back to.

---

## 1. User, Goal and System Boundaries

### 1.1 Source of requirements

Original task text (translated from Serbian):

> **Broadband Managed IPTV receiver: protected rewind.**
> Enable time-based selection of a previously broadcast protected program and its playback using a decryption key. Demonstrate operation with a correct and with an incorrect key.
>
> **Common requirements (mandatory for all):** Live TV zapper (selection of next/previous service, direct number entry and a service list), info dialog on service change, volume control and mute, EPG/TV guide, and keyboard control acting as a remote control (arrows, ENTER, digits; letters for dialogs).
>
> **Transport type — Broadband managed:** MPEG-TS over RTP on client request (with the server-side production part). PAT/PMT must be parsed from the received TS, while service information and EPG are obtained from an OTT MW REST server.
>
> **Expected results:** the specification must contain functional and non-functional requirements, components and interfaces, data flows, application behavior, error handling and a verification plan. Source code, run instructions and a demonstration are to be provided.

Additional sources: the course lecture notes (Modules 1–7) and the general project guidelines ("Instrukcije za izradu projekta").

### 1.2 Users

#### 1.2.1 Primary user — TV viewer (subscriber)

| Attribute | Description |
|---|---|
| Who | A subscriber of a managed IPTV service watching TV on a receiver connected to the operator's network (living-room scenario, Lecture 4). |
| Technical skill | Non-technical. Knows how to use a TV remote control; does not know anything about PIDs, RTP or encryption. |
| Interaction device | A PC keyboard acting as a remote control [COMMON]: arrow keys, ENTER, digits, letters for dialogs. No mouse is required for any function. |
| Viewing device | Screen and speakers (in the demo: projector over HDMI). |
| Main motivation | Watch live TV, switch between services quickly, find out what is on, and **watch a protected program that has already been broadcast** without waiting for a repeat. |
| Owns | The decryption key of each protected service the viewer is entitled to, as a 32-character hexadecimal string [D-03]. |

#### 1.2.2 Secondary user — Operator / content administrator

| Attribute | Description |
|---|---|
| Who | The IPTV operator who runs the head-end (production side) and the OTT middleware server. In the project, this role is played by the team. |
| Goals | Publish a list of services, provide EPG data, keep an archive of previously broadcast content, protect selected services by encryption and control which key unlocks them. |
| Interaction | Configuration files and command-line tools on the server side; no end-user UI is required. |

#### 1.2.3 Secondary user — Demonstrator / evaluator

| Attribute | Description |
|---|---|
| Who | A team member presenting the system at the defense, and the course staff evaluating it. |
| Goals | Start the complete system with a known, repeatable procedure; show both the correct-key and incorrect-key scenarios; see evidence (logs, status) of what the system is doing internally. |

### 1.3 Goal of the system

**Main goal [TASK]:**
Provide a managed IPTV receiver that lets the viewer choose a point in time in the past of a protected service, request the corresponding archived content from the operator's server, receive it as MPEG-TS over RTP, decrypt it with the decryption key and play it — and, when the key is wrong, detect that and inform the viewer instead of presenting corrupted content.

**Supporting goals:**

| ID | Goal | Source |
|---|---|---|
| G-01 | Watch live services delivered as MPEG-TS over RTP, requested on demand by the client. | [TASK] |
| G-02 | Select services by next/previous, by direct number entry and from a service list. | [COMMON] |
| G-03 | See basic information about the service and the current program on every service change. | [COMMON] |
| G-04 | Control volume and mute. | [COMMON] |
| G-05 | Browse the EPG (TV guide), including past programs that are available in the archive. | [COMMON] + [TASK] |
| G-06 | Select a previously broadcast program or moment of a service and play it from that moment. | [TASK] |
| G-07 | Enter the decryption key; play protected content when the key is correct; get a clear error when the key is incorrect. | [TASK] |
| G-08 | Operate everything with the keyboard as a remote control. | [COMMON] |
| G-09 | Obtain service information and EPG from an OTT MW REST server; obtain the stream structure (PAT/PMT) by parsing the received TS. | [TASK] |

### 1.4 System context

```mermaid
flowchart LR
    viewer([TV viewer]) -- "key presses (keyboard as remote)" --> RX
    RX -- "video + audio + OSD" --> screen([Screen / speakers])

    subgraph SERVER["Operator side (production / head-end) — built by the team"]
        MW["OTT MW REST server<br/>services, service info, EPG"]
        HE["Streaming head-end<br/>archive + RTP sender"]
    end

    subgraph RX["Receiver (client) — built by the team, Linux + Web"]
        UI["Web application (UI)"]
        DTV["Native DTV middleware"]
    end

    RX -- "HTTP GET (JSON)" --> MW
    MW -- "service list, service info, EPG (JSON)" --> RX
    RX -- "HTTP stream control: service + start time" --> HE
    HE -- "MPEG-TS over RTP/UDP, unicast (encrypted payload for protected services)" --> RX
    operator([Operator]) -- "content, keys, EPG data, config" --> SERVER
```

The receiver is split internally into a Web application (the "application" of the course model) and a native DTV middleware process, which contains the application layer (AL, DTV middleware core) on top of the platform layer (PAL). The layering is defined in Section 2.2.3 and is only shown here to explain the boundary.

### 1.5 Inputs

#### 1.5.1 User inputs (keyboard as remote control)

The task prescribes the key categories (arrows, ENTER, digits, letters for dialogs); the concrete bindings below are fixed by the team.

| Key | Context: full-screen playback | Context: service list / EPG | Context: key dialog |
|---|---|---|---|
| `↑` / `↓` | Next / previous service (zap) | Move selection up / down | — |
| `←` / `→` | Move playback position 30 s back / forward in the archive [D-02] | Move selection left / right (EPG time axis) | — |
| `ENTER` | Open the service list | Confirm the selection | Confirm the key |
| `0`–`9` | Direct service number entry | — | Key characters |
| `A`–`F` | — | — | Key characters |
| `L` | Open the service list | — | Key character `L` is not valid — ignored |
| `E` | Open the EPG / TV guide | Close the EPG | Key character |
| `I` | Show the info dialog | — | — |
| `K` | Open the key dialog for the current service | — | — |
| `M` | Mute / unmute | — | — |
| `+` / `-` | Volume up / down | — | — |
| `B` | Return to live [D-02] | — | Key character |
| `ESC` | Close the info dialog | Close the list / EPG without changes | Cancel the dialog |
| `BACKSPACE` | Delete the last typed digit of a service number | — | Delete the last character |

In the key dialog all letter and digit keys are treated as text input; the playback shortcuts (`E`, `B`, `M`, …) are not active while the dialog is open.

#### 1.5.2 Data inputs to the receiver

| Input | Source | Format / protocol | Notes |
|---|---|---|---|
| Service list | OTT MW REST server | HTTP GET, JSON | Service number, name, identifier, protection flag (used for display only). |
| Service info | OTT MW REST server | HTTP GET, JSON | Head-end control address, archive window. |
| EPG data | OTT MW REST server | HTTP GET, JSON | Events per service: start, duration, title, description. Includes past events. |
| Media stream | Streaming head-end | MPEG-TS over RTP over UDP, unicast [D-05] | One program per stream (SPTS); PAT and PMT in the clear; audio/video payload encrypted for protected services [D-04]. |
| Decryption key | Viewer, through the key dialog [D-03] | 128-bit AES key as 32 hexadecimal characters | Kept in memory for the running session only. |
| Receiver configuration | Local configuration file | JSON | MW server URL, local ports, default volume. |

#### 1.5.3 Inputs to the server side (operator)

| Input | Format | Notes |
|---|---|---|
| Source content per service | MPEG-TS files, H.264 video + AAC audio [D-09] | Generated with FFmpeg by the team. |
| Content keys | One fixed 128-bit AES key per protected service [D-04] | Used by the team's packaging tool to encrypt the content file once, before streaming. |
| Service and EPG definitions | JSON | Served by the OTT MW REST server; EPG follows the same cyclic timeline as the archive [D-08]. |
| Server configuration | JSON | Ports, archive depth, mapping of content files to services. |

### 1.6 Outputs

#### 1.6.1 Outputs to the viewer

| Output | Description |
|---|---|
| Video and audio | Decoded program of the selected service, live or from the archive, in full screen. |
| Info dialog (OSD) | Shown on every service change: service number and name, current program (title, start–end time), LIVE or ARCHIVE indicator with the playback time, protection status (clear / protected – key OK). |
| Service list (OSD) | List of services with numbers and names, protected services marked, the current one highlighted. |
| EPG / TV guide (OSD) | Programs per service on a time axis; past programs inside the archive window marked as available. |
| Playback position indicator | In ARCHIVE mode: the playback time and the distance from live (e.g. "−01:30"). |
| Key dialog | Masked entry of the decryption key with a character counter (n/32). |
| Volume / mute indicator | Current volume level and mute state. |
| Status and error messages | "Incorrect decryption key", "No signal", "Server not reachable", "Invalid service number", "Content not available in archive". |

#### 1.6.2 Outputs to other systems

| Output | Destination | Format / protocol |
|---|---|---|
| Metadata requests | OTT MW REST server | HTTP GET |
| Stream control requests (start live / start at time T / stop) | Streaming head-end | HTTP POST, JSON [D-06] |
| Logs / diagnostics | Local log files / console | Text; used as verification evidence |

### 1.7 System boundaries

#### 1.7.1 What the team builds, reuses and simulates

| Part | Built by the team | Reused (library / platform) | Simulated |
|---|---|---|---|
| Packaging tool: encryption of content files | ✔ | Crypto library | — |
| Streaming head-end: archive timeline, time-based start, RTP packetization, HTTP stream control | ✔ | Socket API, HTTP server library | The "broadcast" itself: a looping content file mapped to wall-clock time stands in for a live channel and its archive [D-08]. |
| OTT MW REST server: services, service info, EPG | ✔ | HTTP server library | Operator back-end (JSON files instead of a database). |
| Native receiver process: DTV middleware (AL: PAT/PMT parsing, protection detection, key management and validation, session state, HLS packaging, API to the UI), DTV SDK (PAL) and software tuner, demultiplexer and descrambler (HAL: RTP reception, TS synchronization, PID filtering, decryption) | ✔ | Socket API, crypto library (OpenSSL), HTTP/WebSocket library | The tuner, demultiplexer and descrambler hardware blocks of a DTV receiver are implemented in software (HAL). |
| Web application (UI): zapper, service list, info, EPG, archive navigation, key dialog, volume, keyboard handling | ✔ | Browser, HTML/CSS/JS | Remote control is replaced by the PC keyboard. |
| Video/audio decoding and rendering | — | Browser media stack (Media Source Extensions) and the hls.js player library [D-13] | — |
| Managed network | — | Loopback interface | One machine instead of an operator network [D-12]. |

Note [GUIDELINES]: the use of a ready-made player for decoding does **not** replace the team's own implementation of RTP reception, PAT/PMT parsing, PID selection, protection detection, decryption and key validation. These are done in the team's code; the player receives an already decrypted single-program transport stream.

#### 1.7.2 Explicitly out of scope

- Real RF reception (tuner, demodulator) and the physical layer of broadcast standards.
- IP multicast and IGMP group management [D-05].
- Real commercial CAS/DRM systems (NAGRA, Widevine …), smart cards and secure hardware; the documented scheme of D-04 is used instead.
- Key rotation and the CAS key hierarchy (control words, ECM, EMM, service and user keys); one fixed key per protected service is used [D-04].
- Obtaining keys from a server, subscriber management, billing, user accounts and login [D-03].
- Local timeshift buffer and recording (PVR) in the receiver [D-01]; the short-lived HLS segments of D-13 are an output buffer for the browser, not a timeshift buffer.
- A free time-entry dialog for rewind; time is selected through the EPG and the fixed-step keys [D-02].
- Adaptive bitrate streaming (the HLS output of D-13 has one quality level only), subtitles, teletext, multiple audio languages, multi-program transport streams (MPTS).
- Video transcoding on the client side.

#### 1.7.3 External interfaces (crossing the system boundary)

| ID | Interface | Between | Direction |
|---|---|---|---|
| IF-EXT-01 | Keyboard events | Viewer → Web application | In |
| IF-EXT-02 | Video/audio/OSD output | Receiver → screen/speakers | Out |
| IF-EXT-03 | OTT MW REST API (HTTP/JSON) | Receiver ↔ OTT MW server | Request / response |
| IF-EXT-04 | Stream control API (HTTP/JSON) | Receiver ↔ streaming head-end | Request / response |
| IF-EXT-05 | MPEG-TS over RTP/UDP | Streaming head-end → receiver | In |
| IF-EXT-06 | Configuration and content files | Operator → server side; local config → receiver | In |

Internal interfaces (Web application ↔ native middleware, middleware ↔ player) are defined in the architecture section.

### 1.8 Scope decisions

Where the task does not prescribe a solution, the team fixed the variant that is the simplest to implement and integrate while still fulfilling the task. These decisions are binding for the rest of the specification.

| ID | Topic | Decision | Rationale |
|---|---|---|---|
| D-01 | Source of previously broadcast content | **Server-side archive (network catch-up).** The head-end can stream every service from any moment inside the archive window. The receiver has no local timeshift buffer and no storage. | Rewind becomes just a new stream request with a start time; the receiver needs no disk buffer or index. |
| D-02 | How the viewer selects a time | (a) **EPG:** selecting an available past program and pressing `ENTER` starts it from the program's start time. (b) **Step keys:** `←` / `→` move the playback position by **30 s** back / forward, relative to the current position. `B` returns to live. Moving forward past the live point switches to live. Moving back past the start of the archive window stops at the window start. | Both methods reuse the same "start at time T" request; no separate time-entry dialog is needed. |
| D-03 | Source of the decryption key | **The viewer enters the key** in the key dialog: exactly 32 hexadecimal characters (`0–9`, `A–F`), representing a 128-bit key. The key is kept by the middleware in memory, per service, for the running session; it is not written to disk. | Correct and incorrect key are demonstrated simply by typing two different keys; no authorization logic on the server; uses "letters for dialogs" from the task. |
| D-04 | Protection scheme | **One fixed AES-128 key per protected service**, the same for live and archive, never changing. **Encrypted:** the payload of TS packets carrying the audio and video PIDs. **Mode:** AES-128-CBC, IV = 16 zero bytes, restarted for every TS packet; the largest multiple of 16 bytes from the payload start is encrypted, the remaining bytes (fewer than 16) stay in the clear. **In the clear:** TS headers, adaptation fields (including PCR), PAT and PMT. **Signalling:** encrypted packets have `transport_scrambling_control = '10'`, all others `'00'`. The receiver detects protection from these bits; no CA descriptor, ECM or EMM is used. Content files are encrypted once, offline, by the team's packaging tool. | Fixed key as confirmed by course staff; per-packet encryption with the header in the clear follows the scrambling model of Lecture 3 (headers and PSI readable, payload protected); offline encryption keeps the head-end a plain file streamer. |
| D-05 | Unicast vs multicast | **Unicast** RTP over UDP to the requesting receiver; no IP multicast, no IGMP. RTP payload: MPEG-2 TS as in RFC 2250 (payload type 33), **7 TS packets (1316 bytes) per RTP packet**. | "On client request" delivery from the task is naturally one-to-one; unicast works on loopback without any network configuration. |
| D-06 | Stream control protocol | **HTTP POST with JSON** to the head-end: *start* (service, start time or "live", receiver UDP port, RTP SSRC chosen by the receiver) returns a session identifier; *keepalive* (session identifier) every 5 s keeps the session alive; *stop* (session identifier) ends the stream. Starting a new session for the same receiver stops the previous one; a session without keepalive for 15 s is stopped by the head-end, so a receiver that stops or crashes does not leave a stream running. | Same technology as the MW REST API, so one HTTP client is used in the receiver; simpler than implementing an RTSP server and client. |
| D-07 | Rewind step | **30 s** per key press (see D-02). | Large enough to see a visible jump in the demo, small enough to re-watch a moment. |
| D-08 | Archive depth and timeline | **Archive window: the last 2 hours.** Each service's content file is played in an endless loop on a timeline bound to wall-clock time: playback position = (UNIX time in seconds) mod (file duration). The EPG is generated on the same cycle, so the program shown at time T in the EPG is exactly the content streamed for time T. | Any moment of the last 2 hours exists without storing 2 hours of content; the server can be restarted without losing the archive; EPG and content always match. |
| D-09 | Test content | **4 services**, each a separate single-program transport stream (SPTS) generated with FFmpeg: H.264 video 1280×720, 25 fps; AAC-LC stereo audio, 48 kHz; **10-minute file = 2 programs of 5 minutes**. Each picture shows burned-in text with the service name, program title and the content time code, so the correct service and position can be verified visually. Services 1 and 2 are clear; services 3 and 4 are protected with two different keys. | Codecs playable in the browser via MSE; known content makes the expected result of every test exact; two protected services with different keys show that a key is bound to its service. |
| D-10 | Protected services in live | Protected services are **protected in live and in the archive** with the same key (D-04); the key dialog appears for both. | The same encrypted file serves live and archive; no extra logic. |
| D-11 | Incorrect key behavior | The receiver **detects** the incorrect key and shows the message "Incorrect decryption key"; **no corrupted data is passed to the player**. Detection: after decryption, a TS packet of the video PID with `payload_unit_start_indicator = 1` must begin with the PES start code `00 00 01`; if it does not, the key is rejected. | Deterministic, measurable result for the test; a browser decoder fed with garbage would not show a "corrupted picture" reliably, but would stall or report an error. |
| D-12 | Demo environment | **One Linux laptop** (Ubuntu 24.04 LTS) runs the OTT MW server, the head-end, the native middleware and the browser; all communication over the loopback interface. The Web application runs in Chromium in full-screen kiosk mode; the screen is mirrored to the projector over HDMI. | No network setup at the defense; one start procedure. |
| D-13 | Playback in the browser | The native middleware packages the **decrypted** single-program TS of the current session as an **HLS live stream** (RFC 8216): TS segments of about 1 s, each starting with PAT, PMT and a video key frame, and a sliding-window playlist `index.m3u8`, written to a temporary directory. The middleware's existing HTTP server (the one that serves the Web application) serves these files; **no additional server** is used. The Web application plays the playlist with **hls.js** (MSE). The content loop boundary of D-08 is signalled with `#EXT-X-DISCONTINUITY`. Volume and mute are controlled on the HTML video element. | Suggested by course staff; HLS is part of the course material (Lecture 4). Browsers cannot receive RTP or play TS natively; hls.js plays HLS with TS segments through MSE. The discontinuity tag lets the player handle the timestamp jump at the loop boundary without rewriting timestamps. Live, zap and rewind all use the same path. |
| D-14 | Playback position while waiting for a key | The receiver remembers the **requested start point** of the current request: either "live" or the archive time T selected by the viewer (EPG event start or step position). When the stream of a protected service is stopped because no key or an incorrect key is known, this start point is kept. After a correct key is entered, playback is requested again **from the same start point**: archive content continues from time T (not from the moment the key was accepted); a live request continues at the current live point. The start point is discarded only when the viewer selects another service, another time, or returns to live. | Confirmed by course staff: playback continues from where it was started. Because the team's own head-end can start any service at any time T (D-01, D-06), resuming is just a repeated start request with the stored T. |

### 1.9 User stories

Format: *As a \<role\>, I want \<capability\>, so that \<benefit\>.* Each story has acceptance criteria (AC) in Given / When / Then form. The numeric values in the criteria are fixed by the team and become measurable requirements in Section 2.

#### Epic E1 — Start-up and service list

**US-01 — Service list on start-up** [TASK][COMMON]
As a viewer, I want the receiver to load the list of available services when it starts, so that I can start watching without any manual setup.
- AC1: Given the OTT MW server is reachable, when the receiver starts, then the service list (number, name, protection flag) is obtained from the REST API and service number 1 starts playing live.
- AC2: Given the OTT MW server is not reachable, when the receiver starts, then the message "Server not reachable" is shown and the receiver retries every 5 s without blocking the UI.
- AC3: Given the server returns an entry with missing or invalid fields, when the list is parsed, then that entry is ignored, the error is logged and the valid services are still shown.

#### Epic E2 — Live TV zapper

**US-02 — Next / previous service** [COMMON]
As a viewer, I want to switch to the next or previous service with the arrow keys, so that I can browse channels quickly.
- AC1: Given a service is playing, when I press `↑` (`↓`), then the next (previous) service in the list starts live; the list wraps around at the ends.
- AC2: When I press the key several times quickly, then only the last selected service is started and the earlier requests are cancelled.

**US-03 — Direct number entry** [COMMON]
As a viewer, I want to type a service number with the digit keys, so that I can jump straight to a known channel.
- AC1: When I type digits, then the typed number is shown on screen; the switch happens after `ENTER` or 2 s after the last digit.
- AC2: Given the number does not exist, then "Invalid service number" is shown and the current service keeps playing.

**US-04 — Service list** [COMMON]
As a viewer, I want to open a list of all services and choose one, so that I can see what is available and pick a service by name.
- AC1: When I press `ENTER` or `L` during playback, then the list opens with the current service highlighted.
- AC2: When I move with `↑`/`↓` and press `ENTER`, then the selected service starts live and the list closes; `ESC` closes the list without changes.
- AC3: Protected services are marked in the list.

**US-05 — Live playback of a service** [TASK]
As a viewer, I want the selected service to start playing live, so that I can watch what is currently broadcast.
- AC1: When a service is selected, then the receiver requests its stream from the head-end, receives MPEG-TS over RTP, finds the audio and video PIDs by parsing PAT and PMT, and presents video and audio.
- AC2: Given no stream data arrives within 5 s of the request, then "No signal" is shown.

#### Epic E3 — Information and audio

**US-06 — Info dialog on service change** [COMMON]
As a viewer, I want to see basic information every time the service changes, so that I know what I am watching.
- AC1: On every service change, the info dialog shows service number, service name, current program title and start–end time, and the protection status.
- AC2: When the content is from the archive, the dialog shows "ARCHIVE" and the playback time instead of "LIVE".
- AC3: The dialog disappears automatically after 5 s and can be shown again with `I`.

**US-07 — Volume control** [COMMON]
As a viewer, I want to raise and lower the volume, so that I can listen comfortably.
- AC1: `+`/`-` change the volume in steps of 10 %, within 0–100 %; a volume indicator is shown for 2 s.
- AC2: The volume level is kept when the service changes.

**US-08 — Mute** [COMMON]
As a viewer, I want to mute and unmute the sound with one key, so that I can silence the TV instantly.
- AC1: `M` toggles mute; a mute indicator stays visible while muted.
- AC2: Unmuting restores the previous volume level; pressing `+` or `-` while muted unmutes.

#### Epic E4 — EPG / TV guide

**US-09 — Browse the EPG** [COMMON][TASK]
As a viewer, I want to open a TV guide with programs of all services, so that I can see what is on now, what comes next and what was on earlier.
- AC1: When I press `E`, then the EPG opens with the events obtained from the OTT MW REST server for all services, centered on the current time.
- AC2: I can move between services (`↑`/`↓`) and between events (`←`/`→`); the selected event's title, start–end time and description are shown.
- AC3: Given EPG data is not available for a service, then the guide shows "No information" for that service instead of failing.

**US-10 — See which past programs are available** [TASK]
As a viewer, I want past programs that are still in the archive to be marked in the EPG, so that I know what I can watch again.
- AC1: Past events that start inside the 2-hour archive window are marked as available; older events are shown as not available.

#### Epic E5 — Protected rewind (core of M15)

**US-11 — Select a past program from the EPG** [TASK]
As a viewer, I want to select a program that has already been broadcast and start watching it from its beginning, so that I can catch up on something I missed.
- AC1: Given an available past event is selected in the EPG, when I press `ENTER`, then the receiver requests the stream of that service starting at the event start time, the EPG closes and the program plays from its beginning in ARCHIVE mode.
- AC2: Given the selected event is not available, then "Content not available in archive" is shown and the current playback continues.
- AC3: Given the selected event is the one currently running, then playback starts from its start time (in the archive), not from live.

**US-12 — Step back and forward in time** [TASK]
As a viewer, I want to move playback back in time on the current service, so that I can re-watch a moment I just missed.
- AC1: When I press `←` during playback, then the playback position moves 30 s back from the current position and playback restarts from there in ARCHIVE mode; the playback time and the distance from live are shown.
- AC2: When I press `→` in ARCHIVE mode, then the position moves 30 s forward; if that reaches the live point, playback switches to LIVE.
- AC3: Given the new position would be before the start of the archive window, then the position is set to the window start.
- AC4: When I press `←`/`→` several times quickly, then only the final position is requested from the head-end.

**US-13 — Return to live** [TASK]
As a viewer, I want to get back to the live broadcast with one key, so that I do not have to search for the current moment.
- AC1: When I press `B` in ARCHIVE mode, then live playback of the same service resumes and "LIVE" is shown.

**US-14 — Enter the decryption key** [TASK]
As a viewer, I want to enter my decryption key for a protected service, so that I can watch protected content.
- AC1: Given a service is started (live or archive) whose audio/video packets are scrambled, and no key is stored for it in this session, then playback does not start, the requested start point is kept (D-14) and the key dialog opens automatically.
- AC2: The key dialog can also be opened with `K` for the current service.
- AC3: The dialog accepts only `0–9` and `A–F`, at most 32 characters, masks the input and shows the count (n/32); `BACKSPACE` deletes, `ESC` cancels; `ENTER` is accepted only when exactly 32 characters have been typed.
- AC4: After confirmation, the key is applied and playback of the protected content starts from the kept start point: from the selected archive time T for an archive request, or at the live point for a live request.
- AC5: Given the dialog is cancelled with `ESC`, then no protected content is played, the message "Protected service – press K to enter the key" is shown and the start point is still kept; entering the correct key later with `K` starts playback from that start point.

**US-15 — Playback with the correct key** [TASK]
As a viewer, I want protected content to play normally when my key is correct, so that I can watch what I am entitled to.
- AC1: Given the correct key, when a past program of a protected service is selected, then the content is decrypted and video and audio are presented without visible corruption; the burned-in time code matches the selected time.
- AC2: The info dialog shows the status "Protected – key OK".
- AC3: Given the key was accepted once, then further zaps to that service and rewinds on it in the same session do not ask for the key again.

**US-16 — Incorrect key is detected and reported** [TASK]
As a viewer, I want to be told clearly when my key is wrong, so that I understand why I cannot watch and can correct it.
- AC1: Given an incorrect key, when protected content is requested, then the receiver detects within 3 s that decryption does not produce valid data and shows "Incorrect decryption key".
- AC2: No video or audio of the protected service is presented; the stream session is stopped, the incorrect key is discarded and the key dialog opens again.
- AC3: After entering the correct key, playback starts without restarting the application, from the same start point that was requested before the incorrect key (D-14): for an archive request, the burned-in time code shows the originally selected time T, regardless of how long the key entry took.
- AC4: The key of one protected service does not unlock the other protected service (services 3 and 4 have different keys).

**US-17 — Clear services need no key**
As a viewer, I want unprotected services to play without asking for a key, so that normal viewing is not interrupted.
- AC1: Given a service whose audio/video packets are not scrambled (`transport_scrambling_control = '00'`), then it plays without opening the key dialog.

#### Epic E6 — Robustness

**US-18 — Changing my mind during an operation** [GUIDELINES]
As a viewer, I want my latest action to win when I press keys before the previous operation has finished, so that the receiver always follows what I want now.
- AC1: Given a service start, a rewind or a key check is in progress, when a new service or position is requested, then the previous stream session is stopped, its resources are released, and only the latest request takes effect.

**US-19 — Loss of stream or server** [GUIDELINES]
As a viewer, I want to be informed if the stream stops or a server becomes unavailable, so that I know the problem is not on my side.
- AC1: Given RTP data stops arriving during playback, then "No signal" is shown after 3 s and playback resumes automatically when data returns.
- AC2: Given the OTT MW server becomes unavailable after start-up, then the already loaded service list remains usable and the EPG shows "No information".
- AC3: Given the head-end stops during playback, then "No signal" is shown; after the head-end is started again, selecting a service plays it without restarting the receiver.

#### Epic E7 — Operator and demonstrator

**US-20 — Publish services and EPG** [TASK]
As an operator, I want to define services and EPG data that the OTT MW server publishes, so that receivers can present them.
- AC1: Services and program titles are defined in JSON files; after server start they are available through the REST API.
- AC2: EPG events follow the archive timeline of the head-end (D-08).

**US-21 — Provide archived and protected streams** [TASK]
As an operator, I want the head-end to stream any service from a requested time in the past, with selected services encrypted, so that subscribers with the key can catch up on protected programs.
- AC1: Given a start request with a time inside the archive window, then the head-end sends MPEG-TS over RTP starting at the content position that corresponds to that time.
- AC2: For protected services, audio/video payloads are encrypted with the service key (D-04), while PAT and PMT remain readable.
- AC3: Given a start time outside the archive window or an unknown service, then the head-end rejects the request with an error response and sends no stream.
- AC4: Given the receiver stops or crashes during a session, then the head-end stops sending that stream within 15 s.

**US-22 — Repeatable demonstration** [GUIDELINES]
As a demonstrator, I want to start the whole system with a documented procedure and a known test data set, so that the defense demo is repeatable.
- AC1: A documented sequence of commands starts the OTT MW server, the head-end, the native middleware and the Web application.
- AC2: The test data set (content files with checksums, the correct keys of services 3 and 4, an incorrect key, the expected services and EPG) is documented.
- AC3: Logs show the key internal events: stream request, PAT/PMT parsed with PIDs, protection detected, key accepted / rejected.

### 1.10 Traceability: goals → user stories

| Goal | User stories |
|---|---|
| G-01 Live services over RTP | US-01, US-05, US-21 |
| G-02 Zapper | US-02, US-03, US-04, US-18 |
| G-03 Info dialog | US-06 |
| G-04 Volume / mute | US-07, US-08 |
| G-05 EPG | US-09, US-10, US-20 |
| G-06 Time selection of past content | US-10, US-11, US-12, US-13, US-21 |
| G-07 Decryption key | US-14, US-15, US-16, US-17 |
| G-08 Keyboard as remote | all viewer stories (Section 1.5.1) |
| G-09 MW REST + PAT/PMT parsing | US-01, US-05, US-09, US-17, US-20 |
| Robustness / demo | US-18, US-19, US-22 |

---

## 2. Requirements

Structure of this section:

- **2.1 High-level requirements** (`HLR-xx`) — derived from (1) user goals, (2) system boundaries, (3) technical constraints of the development and runtime environment.
- **2.2 Technical postulates** — standards and formats, signal path, software layering, key interfaces, modules and design patterns.
- **2.3 Detailed functional requirements** (`REQ-xx`) — mandatory functionality, refined from the HLRs.
- **2.4 Non-functional requirements** (`NFR-xx`).
- **2.5 Possible extensions** (`EXT-xx`) — not part of the mandatory scope.

**Verification methods** used in this section:

| Code | Method | Meaning |
|---|---|---|
| T | Test | Executed test with a measurable expected result (unit, integration or acceptance test, Section 5). |
| D | Demonstration | Observed behavior in the target environment, without measurement beyond pass/fail. |
| I | Inspection | Review of source code, configuration, documentation or logs. |
| A | Analysis | Reasoning from design documents, captured traffic or measurements. |

A high-level requirement is considered fulfilled when all detailed requirements refined from it (Section 2.3, 2.4) are verified.

### 2.1 High-level requirements

#### 2.1.1 Requirements derived from user goals

| ID | Requirement | Source | Verification |
|---|---|---|---|
| HLR-01 | The receiver shall play live services of a managed IPTV network, delivered as MPEG-TS over RTP on the receiver's request. | G-01; US-05 | T, D |
| HLR-02 | The receiver shall let the viewer change the service by next/previous selection, by direct number entry and by choosing from a service list. | G-02; US-02 – US-04 | T, D |
| HLR-03 | The receiver shall show an info dialog with service and program information on every service change and on request. | G-03; US-06 | T, D |
| HLR-04 | The receiver shall let the viewer change the volume and mute/unmute the sound. | G-04; US-07, US-08 | T, D |
| HLR-05 | The receiver shall present an EPG (TV guide) with the programs of all services, marking past programs that are available in the archive. | G-05; US-09, US-10 | T, D |
| HLR-06 | The receiver shall let the viewer play a service from a moment in the past, selected through the EPG or by fixed time steps, and return to live. | G-06; US-11 – US-13; D-02 | T, D |
| HLR-07 | The receiver shall play protected content only after the viewer has entered a key, shall play it when the key is correct, and shall detect and report an incorrect key without presenting the protected content; after a correct key, playback shall start from the originally requested point. | G-07; US-14 – US-17; D-03, D-11, D-14 | T, D |
| HLR-08 | Every viewer function shall be operable with the keyboard alone, acting as a remote control. | G-08; Section 1.5.1 | T, D |
| HLR-09 | The receiver shall inform the viewer about every error that prevents the requested function (server not reachable, no signal, invalid service number, content not in archive, incorrect key) and shall stay operable afterwards. | US-01, US-03, US-11, US-16, US-19 | T |
| HLR-10 | When a new request arrives before the previous one has completed, the receiver shall cancel the previous one and execute only the latest. | US-02, US-12, US-18 | T |

#### 2.1.2 Requirements derived from system boundaries

| ID | Requirement | Source | Verification |
|---|---|---|---|
| HLR-11 | The receiver shall obtain the service list, service information and EPG only from the OTT MW REST server (HTTP, JSON). | [TASK]; IF-EXT-03 | I, T |
| HLR-12 | The receiver shall determine the stream structure (program number, PMT PID, PCR PID, audio and video PIDs and stream types) only by parsing PAT and PMT from the received transport stream, with the team's own parser. | [TASK]; G-09 | I, T |
| HLR-13 | RTP reception, TS packet synchronization, PID selection, protection detection, decryption and key validation shall be implemented in the team's own code in the receiver; the playback library shall receive only already decrypted HLS segments of a single-program transport stream. | [GUIDELINES]; Section 1.7.1; D-13 | I |
| HLR-14 | The system shall include the operator (production) side built by the team: a packaging tool that encrypts content, a streaming head-end and an OTT MW REST server. | [TASK]; Section 1.7.1 | I, D |
| HLR-15 | The head-end shall deliver any configured service, live or from any moment of the archive window, as a unicast MPEG-TS over RTP stream, started and stopped on the receiver's request. | [TASK]; D-01, D-05, D-06, D-08 | T |
| HLR-16 | Protected content shall leave the operator side only in encrypted form; the decryption key shall never be transmitted by any component of the system. | D-03, D-04 | I, A |
| HLR-17 | The receiver shall communicate with the outside world only through the external interfaces IF-EXT-01 … IF-EXT-06; it shall not store content, keys or archive data persistently. The only content written to disk are the temporary HLS files of the current session (D-13): at most 10 segments at a time, deleted when the session ends and at start-up. | Section 1.7.3; D-01, D-03, D-13 | I, T |

#### 2.1.3 Requirements derived from technical constraints

| ID | Requirement | Source | Verification |
|---|---|---|---|
| HLR-18 | The receiver shall follow the Linux + Web variant: the user interface is a Web application (HTML, CSS, JavaScript) running in a browser; the DTV logic runs in a separate native process on Linux, implemented in C++17 and built with CMake. | Team choice of variant; [GUIDELINES] | I |
| HLR-19 | The server side (packaging tool, head-end, OTT MW REST server) shall be implemented in Python 3. | [GUIDELINES] (helper services in Python are allowed) | I |
| HLR-20 | The complete system shall run on one laptop with Ubuntu 24.04 LTS, using only the loopback network interface, with the Web application in Chromium in full-screen kiosk mode. | D-12 | D |
| HLR-21 | The Web application shall load the browser player library (hls.js) from a public CDN with an exact, pinned version number; Internet access is available in the runtime environment. All other system components (servers, native middleware, test content) shall run locally without Internet access. | D-12, D-13 | I, D |
| HLR-22 | The system shall use the standards and formats defined in Section 2.2 (MPEG-TS, PSI, RTP, H.264, AAC, AES-128, HTTP/JSON, WebSocket, HLS). | [TASK]; D-04, D-05, D-09, D-13 | I, A |
| HLR-23 | The complete system shall be started by one documented script and stopped by one documented script; all components shall write logs that serve as verification evidence. | US-22; [GUIDELINES] | D, I |
| HLR-24 | The source code, specification and test data shall be kept in one Git repository, with run instructions in the repository. | [TASK]; [GUIDELINES] | I |
| HLR-25 | The receiver shall contain, as separate software blocks in the layers of Section 2.2.3, a DTV middleware (AL), a DTV SDK (PAL) and a software demultiplexer (HAL), with a tuner block as the input of the transport stream. | Course staff (lecture: mandatory blocks DTV MW, DTV SDK, SW DMX) | I |

#### 2.1.4 Traceability: high-level requirements → user stories

| User story | High-level requirements |
|---|---|
| US-01 | HLR-09, HLR-11 |
| US-02, US-03, US-04 | HLR-02, HLR-08, HLR-09, HLR-10 |
| US-05 | HLR-01, HLR-12, HLR-13, HLR-15 |
| US-06 | HLR-03 |
| US-07, US-08 | HLR-04 |
| US-09, US-10 | HLR-05, HLR-11 |
| US-11, US-12, US-13 | HLR-06, HLR-10, HLR-15 |
| US-14, US-15, US-16, US-17 | HLR-07, HLR-12, HLR-13, HLR-16 |
| US-18 | HLR-10 |
| US-19 | HLR-09 |
| US-20, US-21 | HLR-14, HLR-15, HLR-16 |
| US-22 | HLR-20, HLR-21, HLR-23, HLR-24, HLR-25 |

### 2.2 Technical postulates

This section fixes the technical foundations of the system. The work is divided **by software layers** of the course model (application, application layer / DTV middleware, platform layer — Lecture 5 and the general project guidelines), one layer per team member, and the three layers are developed **in parallel on separate Git branches** (Section 2.2.7). Everything that crosses a layer boundary — formats, time base, ports, interfaces — is fixed here and may only be changed by agreement of the owners concerned.

#### 2.2.1 Standards and formats

| Layer | Standard / format | Used for | Parameters fixed by the team |
|---|---|---|---|
| Container | MPEG-2 Transport Stream, ISO/IEC 13818-1 | Media transport from head-end to receiver and from middleware to the player | 188-byte packets, sync byte `0x47`, 13-bit PID; one program per stream (SPTS) [D-09]. |
| Container | PES, ISO/IEC 13818-1 | Audio and video elementary streams inside TS | PTS on every PES; PES start code `00 00 01` used for key validation [D-11]. |
| Metadata in stream | PSI: PAT (`table_id 0x00`, PID 0) and PMT (`table_id 0x02`), ISO/IEC 13818-1 | Stream structure parsed by the receiver | Sections validated with CRC-32; PAT/PMT repeated at least every 100 ms in the content. |
| Timing | PCR in the adaptation field, ISO/IEC 13818-1 | Head-end pacing and seek position; A/V synchronization in the player | PCR carried in the video PID; adaptation field never encrypted [D-04]. |
| Video | H.264 / AVC, ITU-T H.264 (PMT `stream_type 0x1B`) | Video of all services | 1280×720, 25 fps, High profile, **closed GOP of 25 frames (1 s)**, so that playback started at any second reaches a key frame within 1 s. |
| Audio | AAC-LC in ADTS, ISO/IEC 14496-3 (PMT `stream_type 0x0F`) | Audio of all services | 48 kHz, stereo. |
| Protection | AES-128, FIPS-197; CBC mode, NIST SP 800-38A | Encryption of protected services [D-04] | 128-bit key, IV = 16 zero bytes per TS packet, residual < 16 bytes in clear; `transport_scrambling_control = '10'` on encrypted packets. |
| Network | IPv4, UDP (RFC 768) | RTP media delivery | Unicast over loopback [D-05, D-12]. |
| Real-time transport | RTP, RFC 3550; MPEG-TS payload, RFC 2250 | Head-end → middleware | Version 2, payload type 33, 90 kHz timestamp, sequence number used for loss detection, **7 TS packets (1316 bytes) per RTP packet**. |
| Request/response | HTTP/1.1 (RFC 9112) with JSON (RFC 8259), UTF-8 | MW REST API, stream control API | Responses `200` on success, `4xx` with a JSON error object on invalid requests [D-06]. |
| Receiver ↔ UI, control | WebSocket, RFC 6455 | Control messages (JSON text frames) between middleware and Web application (IF-INT-01) | — |
| Receiver ↔ UI, media | HTTP Live Streaming (HLS), RFC 8216, over HTTP/1.1 | Decrypted content from middleware to the browser player (IF-INT-02) | Playlist version 3, live sliding window, MPEG-TS segments of about 1 s (at most 2 s), one quality level, `#EXT-X-DISCONTINUITY` at the content loop boundary. |
| Time base | UNIX time in whole seconds, UTC | All times on all interfaces (EPG, start points, archive window) | The Web application converts to local time only for display. |

#### 2.2.2 Signal path

The system implements the **managed IPTV, unicast, on-request** signal path of Lecture 4, with a software reception chain on the receiver side as in the Internet TV consumption path of Lecture 2 (Stream RX → decryption → software demux → decoders → composition). The receiver has no DTV hardware, so the hardware blocks of Lecture 2 — tuner, demultiplexer, descrambler — are implemented **in software in the HAL** and accessed by the DTV middleware only through the team's **DTV SDK** in the PAL, in the same way as Nexus or the Android Tuner SDK of Lecture 6. Decoding and composition run in the browser.

Three kinds of paths are distinguished:

- **Media path** (content data) — solid arrows, numbered 1–12 in the order in which content data moves for one playback,
- **Metadata path** (service list, EPG, PSI) — dotted arrows, M1–M4,
- **Control path** (commands and events) — thick arrows, C1–C9.

```mermaid
flowchart LR
    subgraph OP["Operator side — head-end and OTT MW (WP-PAL)"]
        SRC[("Clear TS files")] -->|"1 · TS (offline)"| PKG["Packaging tool<br/>AES-128 encryption"]
        PKG -->|"2 · encrypted TS files"| HE["Head-end<br/>archive timeline + RTP sender"]
        MWS["OTT MW REST server<br/>services · EPG"]
    end

    subgraph HAL["Receiver — HAL: software tuner, demux, descrambler (WP-PAL)"]
        TUN["IP tuner<br/>UDP + RTP · lock status"]
        DMX["SW demux<br/>sync · PID filters · sections"]
        DSC["Descrambler<br/>AES-128-CBC per D-04"]
    end

    subgraph PAL["Receiver — PAL: OS + DTV SDK (WP-PAL)"]
        SDK["DTV SDK<br/>ITuner · IDemux"]
        HTTPC["HTTP client"]
        WSS["HTTP / WebSocket server"]
        HLSF[("HLS files<br/>temporary directory")]
    end

    subgraph AL["Receiver — AL: DTV middleware (WP-AL)"]
        PSI["PSI parser<br/>PAT · PMT"]
        KEY["Key manager +<br/>key validator"]
        OUT["HLS packager<br/>(HLS server)"]
        SES["Session controller<br/>state machine"]
        SVC["Service & metadata<br/>manager"]
        FAC["UI API facade"]
    end

    subgraph WEB["Browser — application (WP-APP)"]
        PLY["hls.js<br/>TS demux + MSE"] -->|"11 · decoded frames / samples"| VID["video element"]
        OSD["OSD views<br/>info · list · EPG · dialogs"]
        VID -->|"12 · composition: OSD layers over video"| SCR(["Screen / speakers"])
        OSD --> SCR
        PRES["Presenters + key input router"]
    end

    HE -->|"3 · MPEG-TS over RTP / UDP"| TUN
    TUN -->|"4 · TS packets"| DMX
    DMX -->|"5 · A/V packets (scrambled)"| DSC
    DSC -->|"6 · A/V packets (decrypted)"| SDK
    SDK -->|"7 · TS filter data (IDemux)"| KEY
    KEY -->|"8 · validated A/V packets"| OUT
    OUT -->|"9 · HLS segments + playlist (IHlsStore)"| HLSF
    HLSF -->|"10 · served by"| WSS
    WSS -->|"10 · HLS over HTTP GET"| PLY

    MWS -.->|"M1 · services, EPG (HTTP/JSON)"| HTTPC
    HTTPC -.->|"M1 · IHttpClient"| SVC
    SVC -.->|"M2 · services, EPG, program info"| FAC
    FAC -.->|"M2 · metadata messages (IUiChannel)"| WSS
    WSS -.->|"M2 · WebSocket JSON"| PRES
    PRES -.-> OSD
    DMX -.->|"M3 · PAT/PMT sections"| SDK
    SDK -.->|"M3 · section filter data (IDemux)"| PSI
    PSI -.->|"M4 · PIDs, stream types"| SES

    PRES ==>|"C1 · commands (zap, seek, key, …)"| WSS
    WSS ==>|"C2 · IUiChannel"| FAC
    FAC ==>|"C3"| SES
    SES ==>|"C4 · start / keepalive / stop (IHttpClient)"| HTTPC
    HTTPC ==>|"C4 · HTTP/JSON"| HE
    SES ==>|"C5 · tune, open / start / close filters"| SDK
    KEY ==>|"C6 · set key"| SDK
    SDK ==>|"C7 · configure tuner, PID filters, key"| HAL
    TUN ==>|"C8 · Locked / Unlocked"| SDK
    SES ==>|"C9 · events (playing, key required, …) → UI"| FAC
```

The arrows between the AL and the PAL are calls and callbacks through the DTV SDK and platform interfaces of IF-INT-04 (named on the arrows). The HAL is reached by the AL only through the DTV SDK. The browser's composition of HTML OSD layers above the `<video>` element corresponds to the GPU composition and layering block of Lecture 2.

#### 2.2.3 Software layering

The receiver follows the host-software layering of Lecture 5. Each layer answers its own question. The course staff named three blocks that every solution must contain — **DTV middleware, DTV SDK and a software demultiplexer** — and placed them in the AL, the PAL and the HAL respectively (HLR-25).

- **Application** — *what does the viewer see and press?* Presentation and user interaction only; no protocol, parsing or decryption logic.
- **Application layer (AL) — DTV middleware** — *what does the viewer want?* (zap, rewind, unlock). Service model, session control, PAT/PMT parsing, protection detection, key management and validation, HLS packaging of the output (the "HLS server" of the middleware).
- **Platform layer (PAL) — OS and DTV SDK** — *what does the programmer want?* Tune, open a PID filter, set a descrambling key, write a file, send an HTTP request, talk to the UI, start a timer — as objects with a defined lifecycle, on top of the OS and common libraries.
- **HAL** — *what can the hardware do?* Receive a transport stream (tuner), filter PIDs and assemble sections (demultiplexer), decrypt packets with a key (descrambler). Because the receiver has no DTV hardware, these blocks are implemented in software.

| Layer (Lecture 5) | In this system | Built / reused / simulated | Work package |
|---|---|---|---|
| Application | Web application: views, presenters, model, keyboard handling (Model-View-Presenter, Section 2.2.6) | Built | **WP-APP** |
| Middleware / AL — DTV middleware | DTV middleware library `dtvcore` (AL-xx modules, Section 2.2.4) and the middleware process entry point | Built | **WP-AL** |
| Middleware / AL — media player | hls.js in the browser, integrated by the application | Reused | (WP-APP integrates) |
| Middleware / AL — application runtime | Chromium in kiosk mode | Reused | — |
| Platform / PAL — DTV SDK | `ITuner` and `IDemux` (IF-INT-04): tuning, filters, descrambler keys over the HAL | Built | **WP-PAL** |
| Platform / PAL — other platform services | `IHttpClient`, `IUiChannel` (HTTP/WebSocket server), `IHlsStore`, `IClock`, `ILogger` | Built (over reused libraries) | **WP-PAL** |
| Platform / PAL — OS and common libraries | Linux, sockets, C++17 threads, OpenSSL libcrypto, Mongoose, nlohmann/json; browser MSE | Reused | — |
| HAL — tuner, demux, descrambler | IP tuner (RTP/UDP reception), software demultiplexer, software descrambler | Built — **software simulation of hardware blocks** | **WP-PAL** |
| HAL — drivers | Linux network stack (loopback), GPU/audio drivers used by the browser | Reused, not modified; the operator network is simulated by loopback | — |

**Operator side.** The head-end, the OTT MW REST server and the packaging tool are not layers of the receiver; they are the production / distribution side of the signal path. They are implemented by **WP-PAL**, which is responsible for the whole path up to "filter data enters the DTV middleware" — the role of the front-end in the broadcast receiver of Lecture 2. The operator side is layered internally into a **domain layer** (archive timeline, EPG generation, session management, encryption) and an **I/O layer** (HTTP handlers, UDP sending, file access).

**Portability rule.** The AL library `dtvcore` depends only on the C++17 standard library, nlohmann/json and the DTV SDK and platform interface headers of IF-INT-04 — never on HAL code, Mongoose, OpenSSL or socket calls. This is verifiable by inspection of its CMake target (no link dependency on `hal`, `ssl`, `crypto` or `mongoose`). Platform-dependent code lives only in the libraries `pal` and `hal`. This is the same role as the DTV SDK wrappers of Lecture 6 and the glue layer (Comedia CHAL) of Lecture 7: the AL can be unit-tested with a fake DTV SDK and moved to another platform by replacing the PAL and HAL.

#### 2.2.4 Components and modules

| Work package (layer) | Module | ID | Responsibility |
|---|---|---|---|
| WP-APP (Application) | Middleware client | APP-MWC | Control WebSocket connection: sends commands with request ids, receives responses, events and metadata (IF-INT-01). |
| WP-APP | Application model | APP-MOD | Application state: services, current service, mode (LIVE / ARCHIVE), position, EPG, volume, mute, open dialog. |
| WP-APP | Key input router | APP-KEY | Maps keyboard events to remote-control commands depending on the active context (Section 1.5.1). |
| WP-APP | Presenters | APP-PRS | One presenter per view; react to commands and model changes; call the middleware client. |
| WP-APP | Views | APP-VIEW | Player view (video element + hls.js on IF-INT-02, volume and mute), info bar, service list, number entry, EPG grid, key dialog, volume/mute indicator, message toast. |
| WP-AL (AL) | UI API facade | AL-API | Decodes IF-INT-01 commands received through `IUiChannel`, dispatches them to the AL modules, encodes responses and events. Owns the semantics of IF-INT-01. |
| WP-AL | Service & metadata manager | AL-SVC | Client logic of the MW REST API (IF-EXT-03) over `IHttpClient`; service list and EPG cache; maps service numbers to service identifiers. |
| WP-AL | Session controller | AL-SES | Playback session state machine; tunes the tuner and opens / closes demux filters through the DTV SDK; reacts to tuner lock status; keeps the requested start point (D-14); cancels superseded requests; timeouts via `IClock`; emits events. |
| WP-AL | Head-end client | AL-HEC | Builds and sends start, keepalive and stop requests (IF-EXT-04) over `IHttpClient`; interprets responses. |
| WP-AL | PSI parser | AL-PSI | Opens section filters for PAT and PMT through `IDemux`; checks CRC-32 and parses the sections; produces the stream description (PMT PID, PCR PID, A/V PIDs, stream types). |
| WP-AL | Key manager & validator | AL-DSC | Decides protection from the `scrambled` flag of the filter data; keeps keys per service for the session; sets them on the descrambler through `IDemux`; validates the key on the decrypted video packets (D-11); holds the output during validation. |
| WP-AL | HLS packager | AL-OUT | The "HLS server" of the DTV middleware: builds the output stream (clear PAT/PMT + decrypted A/V packets), cuts it into HLS segments, maintains the live playlist and removes old segments, through `IHlsStore` (IF-INT-02). |
| WP-AL | Process entry point | AL-MAIN | `main()` of the middleware process: reads the configuration, creates the platform (PAL and HAL), injects the interfaces into the AL modules (composition root), starts and stops the process. |
| WP-PAL (PAL) | DTV SDK | PAL-SDK | `ITuner` and `IDemux` over the HAL: tuning, filter handles and their lifecycle (open → start → stop → close), section and TS filter callbacks, descrambler keys per PID, statistics. |
| WP-PAL | HTTP client | PAL-HTTP | `IHttpClient`: asynchronous HTTP GET/POST with JSON body, timeout, result callback (Mongoose). |
| WP-PAL | HTTP / WebSocket server | PAL-WS | Serves the Web application files (IF-INT-03) and the HLS files of the HLS file store under `/hls/` (IF-INT-02); `IUiChannel` on the control WebSocket (Mongoose). |
| WP-PAL | HLS file store | PAL-FS | `IHlsStore`: per-session directory in the temporary HLS directory; atomic file writes (write to a temporary name, then rename); file removal; directory removal at session end and clean-up at start-up. |
| WP-PAL | Clock and timers | PAL-CLK | `IClock`: wall-clock UNIX time, monotonic time, one-shot timers. |
| WP-PAL | Logger | PAL-LOG | `ILogger`: timestamped log lines with module tag and level, to console and file. |
| WP-PAL (HAL) | IP tuner | HAL-TUN | UDP socket on the tuned port, RTP header check (version, payload type 33, tuned SSRC), sequence-number loss counting, extraction of the TS packets, lock status, receive thread. |
| WP-PAL (HAL) | Software demultiplexer | HAL-DMX | TS sync check, continuity counter check, PID filtering, assembly of PSI sections (also over several TS packets), routing of packets to the descrambler and to the filters. |
| WP-PAL (HAL) | Software descrambler | HAL-DSC | Decryption of the packets of PIDs with a key set, according to D-04 (OpenSSL libcrypto); clears the scrambling bits. |
| WP-PAL (operator side) | Content generator | SRV-GEN | FFmpeg script that generates the four clear content files according to D-09 and Section 2.2.1. |
| WP-PAL (operator side) | Packaging tool | SRV-PKG | Command-line tool: reads a clear TS file and a key, writes the encrypted TS file according to D-04. |
| WP-PAL (operator side) | Head-end — control API | SRV-HEC | HTTP stream control API (IF-EXT-04): validates requests, creates, keeps alive and stops sessions; stops sessions without keepalive. |
| WP-PAL (operator side) | Head-end — archive timeline | SRV-ARC | Maps a requested UNIX time to a file position (D-08), using a PCR index built when the file is loaded; checks the archive window. |
| WP-PAL (operator side) | Head-end — RTP sender | SRV-RTP | Per session: sends PAT/PMT first, then TS packets from the computed position, paced by PCR, packed 7 per RTP packet with the session's SSRC (IF-EXT-05). |
| WP-PAL (operator side) | OTT MW REST server | SRV-MW | Serves services, service info and EPG (IF-EXT-03); generates EPG events from the program titles on the archive cycle. |

#### 2.2.5 Key interfaces

These interfaces are the **contracts between layers and work packages**. Their exact formats (JSON messages, C++ headers) are specified in Section 3 and frozen before parallel implementation starts.

| ID | Interface | Provider → consumer (modules) | Contract between | Transport | Purpose | Default endpoint |
|---|---|---|---|---|---|---|
| IF-INT-01 | UI control API | AL-API ↔ APP-MWC (transport by PAL-WS) | **WP-AL ↔ WP-APP** | WebSocket, JSON text frames | UI → AL: commands (get services, get EPG, play live, play at time, set key, …), each with a request id. AL → UI: responses and asynchronous events (session state changes, key required, key rejected, no signal, …). | `ws://127.0.0.1:8090/ws/control` |
| IF-INT-02 | UI media stream (HLS) | AL-OUT → APP-VIEW / hls.js (files through PAL-FS, served by PAL-WS) | **WP-AL ↔ WP-APP** | HLS (RFC 8216) over HTTP GET | Live playlist and TS segments of the decrypted SPTS of the current session only. | `http://127.0.0.1:8090/hls/<session>/index.m3u8` |
| IF-INT-03 | Web application files | PAL-WS → browser | WP-PAL ↔ WP-APP | HTTP GET | Static HTML/CSS/JS of the Web application, from the same origin as IF-INT-01/02, so no CORS rules are needed. | `http://127.0.0.1:8090/` |
| IF-INT-04 | DTV SDK and platform interfaces | PAL-xx → AL-xx | **WP-PAL ↔ WP-AL** | C++ abstract classes (headers in `middleware/include/pal/`) | DTV SDK: `ITuner`, `IDemux` (filters and descrambler key); platform: `IHttpClient`, `IUiChannel`, `IHlsStore`, `IClock`, `ILogger`: operations, callbacks, threading rules, buffer ownership. | — |
| IF-EXT-03 | OTT MW REST API | SRV-MW → AL-SVC (via PAL-HTTP) | **WP-PAL ↔ WP-AL** | HTTP GET, JSON | Service list, service info (head-end control URL, archive window), EPG events for a time range. | `http://127.0.0.1:8080` |
| IF-EXT-04 | Stream control API | SRV-HEC → AL-HEC (via PAL-HTTP) | **WP-PAL ↔ WP-AL** | HTTP POST, JSON | `start` (service, start point "live" or UNIX time, receiver UDP port, SSRC) → session id; `keepalive` (session id) every 5 s; `stop` (session id). | `http://127.0.0.1:8081` |
| IF-EXT-05 | Media stream | SRV-RTP → HAL-TUN | within WP-PAL | RTP/UDP, RFC 2250 | MPEG-TS of one service, encrypted per D-04 for protected services. | UDP `127.0.0.1:5004` |
| IF-INT-05 | Encrypted content files | SRV-PKG → SRV-ARC / SRV-RTP | within WP-PAL | Files + JSON content configuration | Encrypted TS files and the mapping service → file → protection. | `content/` directory |

Ports are defaults; every component reads them from its configuration file. The interfaces marked in bold are the ones that must be frozen at milestone I-1, because two different team members implement their two sides.

#### 2.2.6 Design patterns

| Pattern | Problem it solves | Where it appears |
|---|---|---|
| Layered architecture (Lecture 5) | Separates what the viewer sees (application), what the viewer wants (AL) and what the platform provides (PAL), so that each layer can be developed and tested independently — by a different team member. | Section 2.2.3; one work package per layer. |
| Producer–consumer with bounded queues | The network delivers packets at its own rate, the AL and the browser consume at theirs; a slow consumer must not stall reception. Each stage owns its buffers; queues have a fixed capacity and a defined overflow policy. | HAL-TUN receive thread (producer) → HAL-DMX / HAL-DSC → DTV SDK filter callbacks → AL queue → AL-PSI / AL-DSC / AL-OUT (consumer and producer) → HLS segments in the file store (bounded to 10 segments) → browser fetching them with hls.js (consumer); on the operator side, SRV-ARC reading the file and SRV-RTP sending paced RTP. |
| State machine | Playback depends on the current state (starting, waiting for PSI, waiting for a key, validating a key, playing, no signal); an event that is valid in one state must be ignored or handled differently in another. | AL-SES session state machine; context handling in APP-KEY. Specified as a state diagram and a transition table in Section 4. |
| Asynchronous command / event with "latest request wins" | Starting a stream takes time; the viewer may press a new key before the previous request completes. Acceptance of a command is separated from its completion; every session carries a generation number, and events or data of an older generation are discarded. | IF-INT-01 (request id → response, later events); AL-SES stops the previous session before starting a new one; APP-PRS ignores events of superseded requests. |
| Adapter / wrapper (DTV SDK and glue layer, Lectures 6–7) | Keeps the AL independent of the HAL blocks, Mongoose, OpenSSL and the socket API, so that it is portable and can be unit-tested with fakes while the PAL and HAL are still being written. The DTV SDK plays the role of Nexus / the Android Tuner SDK: it turns the tuner, demux and descrambler into objects with a tune / open filter / start / event lifecycle. | IF-INT-04: interfaces used by the AL, implementations in the PAL (over the HAL), fake implementations in the AL tests. |
| Dependency injection (composition root) | The AL must not create platform objects itself, otherwise it would depend on them. | AL-MAIN creates the PAL objects and passes them to the AL modules as interface references. |
| Observer | Lower layers must notify upper layers without depending on them. | PAL → AL callbacks (`ITuner` status, `IDemux` filter data, `IHttpClient`, `IUiChannel`); AL-SES → AL-API events; APP-MOD → views. |
| Model-View-Presenter | Keeps rendering (views) free of logic and logic free of DOM code, so the presenters can be tested without a browser and the views changed without touching logic. | Application: APP-MOD (model), APP-VIEW (views), APP-PRS (presenters). |

#### 2.2.7 Parallel development

The system is developed by three team members in parallel, **one layer per member, each on its own Git branch**.

| Work package | Layer | Branch | Directories | Modules | Stand-ins for the neighboring layers during development |
|---|---|---|---|---|---|
| WP-APP | Application | `app` | `webui/` | APP-MWC, APP-MOD, APP-KEY, APP-PRS, APP-VIEW | Mock middleware script (`webui/mock/`) that serves the Web application files, answers IF-INT-01 commands with the JSON examples of Section 3, emits scripted events (including "key required" and "key rejected") and serves an HLS live stream made from a clear content file with `ffmpeg -re -i <file>.ts -c copy -f hls -hls_time 1 -hls_list_size 6 -hls_flags delete_segments` as IF-INT-02. |
| WP-AL | Application layer — DTV middleware | `al` | `middleware/core/`, `middleware/main/` | AL-API, AL-SVC, AL-SES, AL-HEC, AL-PSI, AL-DSC, AL-OUT, AL-MAIN | Fake DTV SDK and platform implementations in the unit tests (`middleware/core/tests/fakes/`): tuner and demux fed from a clear or encrypted TS file (lock event, section and TS filter callbacks, descrambling by a reference implementation of D-04), HTTP client returning the JSON examples of Section 3, scripted UI channel, HLS store writing to a temporary directory (playlist and segments checked with `ffprobe` / `ffplay`), controllable clock. No network is needed to test the AL. |
| WP-PAL | Platform layer (DTV SDK) + HAL + operator side | `pal` | `middleware/pal/`, `middleware/hal/`, `server/`, `content/` | PAL-SDK, PAL-HTTP, PAL-WS, PAL-FS, PAL-CLK, PAL-LOG, HAL-TUN, HAL-DMX, HAL-DSC, SRV-GEN, SRV-PKG, SRV-HEC, SRV-ARC, SRV-RTP, SRV-MW | Small test programs (`middleware/pal/tools/`), e.g. tune → open a section filter on PID 0 → print PAT sections; tune → TS filter → write a TS file → `ffprobe`; until the own head-end works, RTP is produced with `ffmpeg -re -i <file>.ts -c copy -f rtp_mpegts rtp://127.0.0.1:5004`. Server side checked with `curl`, `ffprobe` and `ffplay`. |

Shared files, changed only on `main` and merged into all branches: `SPECIFICATION.md`, the platform interface headers `middleware/include/pal/` (IF-INT-04), the top-level `middleware/CMakeLists.txt`, `scripts/` (start/stop scripts), `README.md`.

**Rules:**

1. **Contract first.** The interface specification of Section 3 (IF-INT-01, IF-INT-02, IF-INT-04, IF-EXT-03, IF-EXT-04) and the platform interface headers are written and merged into `main` before parallel implementation starts. Each layer is implemented against the contract, not against the other member's code.
2. **Contract changes** are made only on `main`, with the agreement of both sides of the interface, and are merged into all branches immediately.
3. Each member merges `main` into their branch at least once a day; finished, tested increments are merged from the branch into `main` through a pull request reviewed by one other member.
4. Every layer ships its own stand-ins for its neighbors (column above), so that it can be developed and tested without waiting for the others.
5. Each layer keeps unit tests for its modules; integration tests run on `main`.

**Integration milestones:**

| Milestone | Date | Done when |
|---|---|---|
| I-1 Contracts frozen | 2026-11-21 | Section 3 interface specification and the IF-INT-04 headers merged into `main`. |
| I-2 Clear live end-to-end | 2026-11-25 | A clear service travels head-end → PAL → AL → browser and plays; service list shown. |
| I-3 Protected rewind end-to-end | 2026-11-28 | Archive start at time T, correct key plays, incorrect key reported, resume from T. |
| I-4 Feature complete | 2026-12-01 | All mandatory requirements implemented; acceptance tests executed and results recorded. |
| I-5 Demo rehearsal | 2026-12-02 | Full demo scenario run from the start script, on the defense laptop, over HDMI. |

**Defense presentation.** The three content slides follow the same division: application (WP-APP), application layer / DTV middleware (WP-AL), platform layer (DTV SDK), HAL and operator side (WP-PAL); each member presents the layer they implemented.

#### 2.2.8 Reused components and versions

| Component | Version | Used by | Purpose |
|---|---|---|---|
| Ubuntu | 24.04 LTS | All | Development and runtime OS |
| GCC (g++) | 13.3 | WP-AL, WP-PAL | C++17 compiler |
| CMake | 3.28 | WP-AL, WP-PAL | Build system |
| OpenSSL (libcrypto) | 3.0.13 | WP-PAL (HAL-DSC only) | AES-128-CBC decryption |
| Mongoose | 7.23 | WP-PAL (PAL-HTTP, PAL-WS only) | HTTP client, HTTP server, WebSocket server (single source file) |
| nlohmann/json | 3.12.0 | WP-AL | JSON parsing and generation |
| Python | 3.12 | WP-PAL (operator side) | Head-end, OTT MW server, packaging tool (standard library `http.server`, `socket`, `threading`, `json`) |
| python3-cryptography | Ubuntu 24.04 package | WP-PAL (SRV-PKG) | AES-128-CBC encryption |
| FFmpeg / ffprobe | 6.1.1 | WP-PAL (SRV-GEN), all (verification) | Content generation; independent inspection of TS files |
| Chromium | Ubuntu 24.04 package | WP-APP | Runtime of the Web application (kiosk mode) |
| hls.js | 1.7.4, loaded from jsDelivr | WP-APP | HLS playback (TS demux in JavaScript, playback through MSE) [HLR-21] |

### 2.3 Detailed functional requirements

All requirements in this section are **mandatory**. Each requirement names the layer that implements it (**APP**, **AL**, **PAL**, **HAL**, or **SRV** for the operator side; PAL, HAL and SRV are implemented by WP-PAL), its source and its verification method (Section 2, legend). Test cases (`TST-xx`) that verify each requirement are defined in Section 5. Where a requirement involves two layers, the layer named first owns the behavior and the other one only provides what the interfaces of Section 3 define.

#### 2.3.1 Start-up and service list

| ID | Requirement | Layer | Source | Verif. |
|---|---|---|---|---|
| REQ-01 | At start-up the AL shall request the service list (`GET /services`, IF-EXT-03). If the request fails, the AL shall repeat it every 5 s until it succeeds, and shall send `mw_status` with `reachable: false` after the first failure and `reachable: true` after the first success. | AL | US-01; HLR-11 | T |
| REQ-02 | The AL shall validate every service entry (`service_id` non-empty string, `number` integer 1–999 and unique, `name` non-empty string, `protected` boolean). Invalid entries shall be discarded and logged; valid entries shall still be used. | AL | US-01 AC3 | T |
| REQ-03 | After connecting to IF-INT-01, the APP shall request the service list (`get_services`) and, when it is received, start service number 1 live. | APP | US-01 AC1 | T |
| REQ-04 | While the service list is not available and `mw_status.reachable` is `false`, the APP shall show the message "Server not reachable" and shall keep processing keys; when the list becomes available it shall continue as in REQ-03. | APP | US-01 AC2; HLR-09 | T |
| REQ-05 | When the OTT MW server becomes unreachable after the service list was loaded, the AL shall keep answering `get_services` with the last valid list. | AL | US-19 AC2 | T |

#### 2.3.2 Live TV zapper

| ID | Requirement | Layer | Source | Verif. |
|---|---|---|---|---|
| REQ-06 | In full-screen playback, `↑` shall select the service with the next higher number and `↓` the next lower number, wrapping from the highest to the lowest number and vice versa. | APP | US-02 AC1 | T |
| REQ-07 | On every zap key the APP shall immediately show the newly selected service number and name, and shall send `play` with `start: "live"` only when no further zap key has been pressed for 300 ms, so that a series of quick presses results in one `play` for the last selected service. | APP | US-02 AC2; HLR-10 | T |
| REQ-08 | Digit keys in full-screen playback shall build a service number of at most 3 digits, displayed on screen; `BACKSPACE` deletes the last digit. The number is committed with `ENTER` or 2 s after the last digit. | APP | US-03 AC1 | T |
| REQ-09 | If a committed number does not belong to any service, the APP shall show "Invalid service number" for 3 s and the current playback shall continue unchanged. | APP | US-03 AC2 | T |
| REQ-10 | `ENTER` or `L` in full-screen playback shall open the service list with all services (number, name, lock symbol for `protected: true`) and the current service highlighted; `↑`/`↓` move the highlight, `ENTER` starts the highlighted service live and closes the list, `ESC` closes the list without changes. | APP | US-04 | T |
| REQ-11 | Every service change (REQ-06 – REQ-10) shall start the selected service live. | APP | US-02 AC1; US-04 AC2 | T |

#### 2.3.3 Stream session and reception

| ID | Requirement | Layer | Source | Verif. |
|---|---|---|---|---|
| REQ-12 | On `play`, the AL shall obtain the service info of the service (`GET /services/{id}`, cached after the first success), choose a new random SSRC for the session, tune the tuner (`ITuner::tune`) to the configured receiver UDP port and that SSRC, and then send `POST /start` with the port and the SSRC to the service's `stream_control_url`; the session shall be in state `starting` until the response arrives and in `waiting_stream` after a successful response. | AL | US-05 AC1; HLR-15 | T |
| REQ-13 | The tuner shall receive RTP datagrams on the tuned UDP port and accept only datagrams with `V = 2`, `PT = 33`, the tuned SSRC and a payload of 1–7 whole TS packets; it shall count dropped datagrams and sequence-number gaps, pass the TS packets to the demux, report `Locked` at the first accepted datagram after tuning, `Unlocked` after 3 s without accepted datagrams, and `Locked` again when they return. | HAL | IF-EXT-05; HLR-13; HLR-25 | T |
| REQ-14 | Before starting a new session, the AL shall close all demux filters of the previous session and re-tune the tuner with the new SSRC, so that no data of an older session can reach the AL (IF-INT-04, rule 4). | AL | HLR-10; US-18 | T |
| REQ-15 | The demux shall check the sync byte `0x47` of every TS packet and discard packets without it; it shall check continuity counters per PID and count discontinuities without stopping. | HAL | HLR-12; HLR-25 | T |
| REQ-16 | The AL shall open a section filter on PID 0 (`table_id 0x00`) through `IDemux` and parse the PAT: verify `table_id`, section length and CRC-32, take the first program with `program_number ≠ 0` and its PMT PID. Sections with an invalid CRC shall be discarded. | AL | HLR-12 | T |
| REQ-17 | The AL shall open a section filter on the PMT PID (`table_id 0x02`) and parse the PMT: verify `table_id`, section length and CRC-32, read the PCR PID and select the first elementary stream with `stream_type 0x1B` as video and the first with `stream_type 0x0F` as audio. If no video stream is found, the session shall go to state `error`. | AL | HLR-12 | T |
| REQ-18 | When PAT and PMT have been parsed, the AL shall send `stream_info` once per session and log the program number, PMT PID, PCR PID, video PID and audio PID. | AL | HLR-12; US-22 AC3 | T |
| REQ-19 | After PMT parsing, the AL shall open TS filters only for PID 0, the PMT PID, the video PID and the audio PID (for the output stream), in addition to the section filters of REQ-16 and REQ-17; the demux shall deliver to each filter only the packets of its PID, one TS packet per callback. | AL, HAL | HLR-12; HLR-25 | T |
| REQ-20 | If the tuner does not report `Locked` within 5 s after `waiting_stream` was entered, the AL shall set state `no_signal` (`reason: "no_data_at_start"`); if the tuner reports `Unlocked` while `playing`, it shall set `no_signal` (`reason: "data_lost"`); when the tuner reports `Locked` again it shall return to the previous state. The APP shall show "No signal" while the state is `no_signal`. | AL, APP | US-05 AC2; US-19 AC1 | T |
| REQ-21 | If the head-end is not reachable or answers `POST /start` with an error, the AL shall set state `error` with the corresponding error code (Section 3.10); the APP shall show the message of Section 3.10 for that code. | AL, APP | HLR-09 | T |

#### 2.3.4 Output to the player and presentation

| ID | Requirement | Layer | Source | Verif. |
|---|---|---|---|---|
| REQ-22 | The AL shall build the output stream of the current session from the filtered packets (REQ-19) in the received order, with `transport_scrambling_control = '00'` and decrypted payloads, and package it as an HLS live stream as defined in IF-INT-02, through `IHlsStore`. | AL | HLR-13; D-13 | T |
| REQ-23 | The AL shall cut segments as defined in IF-INT-02: every segment starts with one PAT, one PMT and a video PES that starts a key frame; a segment is closed at the first such key-frame start after at least 1 s of content (by PCR). A segment file shall be complete before it is listed in the playlist; the playlist shall list the last 6 segments; segment files no longer listed shall be removed so that at most 10 exist. | AL | IF-INT-02; HLR-17 | T |
| REQ-24 | At the loop boundary of the content (D-08) — the PCR of the PCR PID decreases, or increases by more than 1 s between two consecutive PCRs — the AL shall close the current segment and mark the next segment in the playlist with `#EXT-X-DISCONTINUITY`, keeping `#EXT-X-DISCONTINUITY-SEQUENCE` correct when marked segments leave the playlist. Timestamps are not rewritten. | AL | D-08; D-13; NFR-06 | T |
| REQ-25 | When the APP receives `session_state` with `state: "playing"` for its current session, it shall destroy the previous hls.js player and create a new one on `/hls/<n>/index.m3u8`, attached to the `<video>` element, starting with the first segment of the playlist (not at the live edge), so that playback begins at the requested start point. | APP | D-13; D-14; IF-INT-02 | T |
| REQ-26 | The APP shall ignore every event whose `session` is not the session number returned by its last `play`. | APP | HLR-10; US-18 | T |

#### 2.3.5 Info dialog

| ID | Requirement | Layer | Source | Verif. |
|---|---|---|---|---|
| REQ-27 | After every service change and after every new start point on the same service (rewind, return to live), the APP shall show the info dialog with: service number and name; title and start–end time (local `HH:MM`) of the EPG event that contains the playback position; `LIVE`, or `ARCHIVE` with the playback time (`HH:MM:SS`); protection status `Clear`, `Protected – key OK` or `Protected – key required`. | APP | US-06 AC1, AC2 | T |
| REQ-28 | The info dialog shall update its content when `stream_info` or `session_state` of the current session arrives, shall disappear 5 s after it was shown, and shall be shown again with `I`; `ESC` hides it. | APP | US-06 AC3 | T |

#### 2.3.6 Volume and mute

| ID | Requirement | Layer | Source | Verif. |
|---|---|---|---|---|
| REQ-29 | `+` and `-` shall change the volume of the `<video>` element in steps of 10 % within 0–100 % (initial value 50 %) and show a volume indicator for 2 s. | APP | US-07 AC1 | T |
| REQ-30 | `M` shall toggle mute; while muted, a mute indicator shall stay visible. Unmuting shall restore the volume set before muting; `+` or `-` while muted shall unmute and then change the volume. | APP | US-08 | T |
| REQ-31 | Volume and mute state shall be kept across service changes, start-point changes and player re-creation (REQ-25). | APP | US-07 AC2 | T |

#### 2.3.7 EPG

| ID | Requirement | Layer | Source | Verif. |
|---|---|---|---|---|
| REQ-32 | On `get_epg`, the AL shall request `GET /epg` for the given range (IF-EXT-03), validate the response (sorted, non-overlapping events with all fields of Section 3.2.3; invalid events discarded and logged) and return it; if the server is not reachable, it shall return `MW_UNREACHABLE`. | AL | US-09 AC1, AC3; HLR-11 | T |
| REQ-33 | `E` in full-screen playback shall open the EPG for the range from 2 hours before to 2 hours after the current time, for all services, with the current service and the event that contains the playback position selected. `E` or `ESC` closes it. | APP | US-09 AC1 | T |
| REQ-34 | In the EPG, `↑`/`↓` shall move between services and `←`/`→` between the events of the selected service; the title, start–end time, description and availability of the selected event shall be shown. | APP | US-09 AC2 | T |
| REQ-35 | Events with `in_archive: true` shall be marked "available"; past events with `in_archive: false` shall be marked "not available"; future events have no mark. | APP | US-10 | T |
| REQ-36 | A service without events shall be shown with "No information"; if `get_epg` fails, all services shall be shown with "No information". | APP | US-09 AC3; US-19 AC2 | T |

#### 2.3.8 Archive navigation (protected rewind)

| ID | Requirement | Layer | Source | Verif. |
|---|---|---|---|---|
| REQ-37 | `ENTER` on an event marked "available" (including the event currently running) shall send `play` with `start` = the event's `start` and close the EPG. | APP | US-11 AC1, AC3 | T |
| REQ-38 | `ENTER` on an event marked "not available" shall show "Content not available in archive" for 3 s and keep the EPG open and the current playback unchanged; `ENTER` on a future event shall have no effect. | APP | US-11 AC2 | T |
| REQ-39 | `←` in full-screen playback shall set the target to the playback position minus 30 s (from LIVE: current time minus 30 s); `→` in ARCHIVE mode shall set it to the playback position plus 30 s; `→` in LIVE mode has no effect. Repeated presses accumulate on the target; the APP shall send one `play` with the final target when no `←`/`→` has been pressed for 500 ms. | APP | US-12 AC1, AC2, AC4; D-02, D-07 | T |
| REQ-40 | `B` in ARCHIVE mode shall send `play` with `start: "live"` for the current service; in LIVE mode it has no effect. | APP | US-13 | T |
| REQ-41 | On `play` with a time `T`, the AL shall use `max(T, now − 7200 + 5)` as start point and shall play `"live"` if `T ≥ now`; the effective start point shall be reported in `session_state.start`. | AL | US-12 AC2, AC3 | T |
| REQ-42 | The head-end shall start the content at the position that corresponds to the requested time `T` (position = `T mod D`, `D` = file duration), beginning at the TS packet with the largest PCR not later than that position, so that the presented content corresponds to `T` within ±1 s. | SRV | US-21 AC1; D-08 | T |
| REQ-43 | In ARCHIVE mode the APP shall show the playback time (`HH:MM:SS`) and the distance from live (`−MM:SS`), updated every second; in LIVE mode it shall show `LIVE`. | APP | US-12 AC1; US-13 | T |

#### 2.3.9 Protection, decryption and keys

| ID | Requirement | Layer | Source | Verif. |
|---|---|---|---|---|
| REQ-44 | The AL shall decide that a stream is protected when the first packet delivered by the video TS filter has the `scrambled` flag set (it was received with `transport_scrambling_control ≠ '00'`), and clear otherwise. A clear stream goes to `playing` without key handling. | AL | US-17; D-04 | T |
| REQ-45 | For a protected stream without a stored key for the service, the AL shall stop the head-end session, keep the start point (D-14) and set state `key_required` with `reason: "no_key"`. No HLS segment of the session shall be written. | AL | US-14 AC1; D-14 | T |
| REQ-46 | On `key_required` of its current session, the APP shall open the key dialog automatically, titled "Enter the decryption key" for `no_key` and "Incorrect decryption key" for `key_rejected`. | APP | US-14 AC1; US-16 AC1, AC2 | T |
| REQ-47 | The key dialog shall accept only `0–9` and `A–F` (letters case-insensitive), at most 32 characters, display each typed character masked (`•`) with a counter `n/32`, delete with `BACKSPACE`, cancel with `ESC`, and accept `ENTER` only when 32 characters have been typed; on `ENTER` the APP sends `set_key`. While the dialog is open, no playback shortcut is active. | APP | US-14 AC3 | T |
| REQ-48 | When the key dialog is cancelled while the state is `key_required`, the APP shall show "Protected service – press K to enter the key" until the state changes; `K` shall open the key dialog for the current service at any time. | APP | US-14 AC2, AC5 | T |
| REQ-49 | On `set_key`, the AL shall validate the format and store the key for the service in memory only. Whenever a session of a service with a stored key reaches PMT parsing, the AL shall set that key on the descrambler for the video and audio PIDs (`IDemux::set_key`) before starting the TS filters. If the current session of that service is in `key_required`, the AL shall restart it from the kept start point with the same `start` (D-14), entering `validating_key` once the stream is protected. | AL | US-14 AC4; US-16 AC3; D-14 | T |
| REQ-50 | The descrambler shall decrypt every packet of a PID with a key set and `transport_scrambling_control = '10'` as defined in D-04 (payload after the header and adaptation field; the largest multiple of 16 bytes decrypted with AES-128-CBC, IV = 16 zero bytes; the remaining bytes unchanged), set its scrambling bits to `'00'` and deliver it with `scrambled = true`. Packets of PIDs without a key are delivered unchanged with `scrambled = true`. | HAL | D-04; HLR-13; HLR-25 | T |
| REQ-51 | In `validating_key`, the AL shall check the first 3 packets of the video TS filter with `payload_unit_start_indicator = 1`: the decrypted payload must begin with `00 00 01`. If all 3 pass, the key is accepted and the state becomes `playing`; if any fails, the key is rejected. The decision shall be made within 3 s after the tuner reported `Locked`. | AL | US-16 AC1; D-11 | T |
| REQ-52 | During `validating_key`, the AL shall hold the decrypted output in memory and write the first HLS segment only after the key is accepted, so that the first segment begins at the requested start point. | AL | D-14; US-16 AC3 | T |
| REQ-53 | On key rejection, the AL shall discard the key of the service, clear it from the descrambler (`IDemux::clear_keys`), discard the held output, stop the head-end session, keep the start point and set `key_required` with `reason: "key_rejected"`. | AL | US-16 AC2 | T |
| REQ-54 | Keys shall be stored per `service_id`: a key stored for one service shall never be used for another. An accepted key shall be reused for later sessions of the same service in the same run of the middleware, without opening the key dialog. | AL | US-15 AC3; US-16 AC4 | T |
| REQ-55 | No layer of the receiver shall write a key to disk or to a log; log lines that refer to a key shall show at most its first 4 characters followed by `…`. | AL, PAL, HAL | HLR-16 | I, T |

#### 2.3.10 Session control and concurrency

| ID | Requirement | Layer | Source | Verif. |
|---|---|---|---|---|
| REQ-56 | On every `play`, the AL shall end the current session before starting the new one: cancel its pending HTTP requests and timers, send `POST /stop` for its head-end session, close its demux filters and clear the descrambler keys (`IDemux::close_filter`, `IDemux::clear_keys`), discard its held data, remove its HLS directory (`IHlsStore::close_session`), and send `session_state` with `state: "stopped"` for it. After that, no other event of the old session shall be sent. | AL | HLR-10; US-18 | T |
| REQ-57 | If the control WebSocket is closed, the APP shall reconnect every 2 s; after reconnecting it shall request `get_status` and `get_services` and restore its state from the answers. The AL session shall continue independently of the UI connection. | APP, AL | HLR-09 | T |
| REQ-58 | Invalid input on any interface (malformed JSON, unknown commands, invalid TS packets, invalid PSI sections, invalid RTP datagrams) shall be rejected as defined in Section 3 and logged, and shall never terminate a component. | AL, PAL, SRV | HLR-09 | T |
| REQ-59 | The AL shall log with a timestamp: every `play` (service, start), every head-end session (session id, SSRC, start time), PAT/PMT results, protection decision, key accepted / rejected, every state change and every error. | AL | US-22 AC3; HLR-23 | I |

#### 2.3.11 Keyboard handling

| ID | Requirement | Layer | Source | Verif. |
|---|---|---|---|---|
| REQ-60 | The APP shall implement the key bindings of Section 1.5.1 for each context (full-screen playback, service list, EPG, key dialog). Keys without a binding in the active context shall be ignored. A held key (auto-repeat) shall act as repeated presses. | APP | HLR-08 | T |
| REQ-61 | Every viewer function shall be reachable without a mouse; the Web application shall hide the mouse cursor in kiosk mode. | APP | HLR-08 | D |

#### 2.3.12 Operator side

| ID | Requirement | Layer | Source | Verif. |
|---|---|---|---|---|
| REQ-62 | The content generator shall produce the four clear content files of D-09: H.264 1280×720 at 25 fps with a closed GOP of 25 frames, AAC-LC 48 kHz stereo, duration 600 s (±0.1 s), two programs of 300 s, PAT/PMT at least every 100 ms, with service name, program title and content time code (`MM:SS` within the file) burned into the picture. | SRV | D-09; Section 2.2.1 | T |
| REQ-63 | The packaging tool shall encrypt a clear file according to D-04 (only video/audio PIDs; headers and adaptation fields unchanged; scrambling bits `'10'`) and produce a file of exactly the same size. Decrypting the output with the same key shall reproduce the input byte for byte, except for the scrambling bits. | SRV | D-04; US-21 AC2 | T |
| REQ-64 | The OTT MW server shall implement IF-EXT-03 as specified in Section 3.2, generating the EPG on the same cycle as the head-end (event starts at multiples of 300 s, titles from `services.json`). | SRV | US-20 | T |
| REQ-65 | The head-end shall implement IF-EXT-04 as specified in Section 3.3, including validation, error codes and the rule "one session per receiver endpoint". | SRV | US-21 AC3; D-06 | T |
| REQ-66 | The head-end shall send each session as specified in Section 3.4, in real time paced by PCR (average bitrate over 10 s within ±5 % of the content bitrate), and at the end of the file shall continue seamlessly with the beginning of the file without interrupting the RTP stream. | SRV | US-21; D-08 | T |

#### 2.3.13 Failure of the head-end or the receiver

| ID | Requirement | Layer | Source | Verif. |
|---|---|---|---|---|
| REQ-67 | While a session has a head-end session, the AL shall send `POST /keepalive` for it every 5 s (IF-EXT-04). | AL | US-21 AC4; D-06 | T |
| REQ-68 | The head-end shall stop a session that has received neither its start request nor a keepalive for 15 s, and shall log the reason. | SRV | US-21 AC4; D-06 | T |
| REQ-69 | If a keepalive is answered with `SESSION_NOT_FOUND`, or the head-end does not answer keepalives for 15 s, the AL shall close the filters of the session, untune the tuner and set state `error` with `HEADEND_UNREACHABLE`; the APP shows "No signal". A new `play` shall start a new head-end session without restarting the receiver. | AL, APP | US-19 AC3; NFR-07 | T |
| REQ-70 | When the middleware process is stopped in an orderly way, the AL shall send `POST /stop` for its current head-end session before terminating. | AL | US-21 AC4; HLR-23 | T |

#### 2.3.14 Traceability: high-level requirements → detailed requirements

| HLR | Detailed requirements |
|---|---|
| HLR-01 Live playback | REQ-12 – REQ-25 |
| HLR-02 Zapper | REQ-06 – REQ-11 |
| HLR-03 Info dialog | REQ-27, REQ-28 |
| HLR-04 Volume / mute | REQ-29 – REQ-31 |
| HLR-05 EPG | REQ-32 – REQ-36 |
| HLR-06 Archive navigation | REQ-37 – REQ-43 |
| HLR-07 Protection and keys | REQ-44 – REQ-55 |
| HLR-08 Keyboard | REQ-60, REQ-61 |
| HLR-09 Error reporting | REQ-04, REQ-09, REQ-20, REQ-21, REQ-36, REQ-38, REQ-57, REQ-58, REQ-67 – REQ-70 |
| HLR-10 Latest request wins | REQ-07, REQ-14, REQ-26, REQ-39, REQ-56 |
| HLR-11 Metadata from MW | REQ-01, REQ-02, REQ-05, REQ-32 |
| HLR-12 PAT/PMT parsing | REQ-15 – REQ-19 |
| HLR-13 Own DTV processing | REQ-13, REQ-15, REQ-22, REQ-50, NFR-09 |
| HLR-14 Operator side | REQ-62 – REQ-66 |
| HLR-15 Head-end streaming | REQ-12, REQ-42, REQ-65, REQ-66, REQ-67, REQ-68 |
| HLR-16 Key never transmitted | REQ-55, NFR-08 |
| HLR-17 Interfaces only, no persistence | REQ-55, NFR-08, NFR-16 |
| HLR-18 – HLR-24 Technical constraints | NFR-09 – NFR-17 |
| HLR-25 DTV MW, DTV SDK, SW demux | REQ-13, REQ-15, REQ-19, REQ-50, NFR-09 |

### 2.4 Non-functional requirements

Measured values refer to the demo laptop (D-12) with all components running.

| ID | Category | Requirement | Verif. |
|---|---|---|---|
| NFR-01 | Performance — zap time | For a clear service, the time from sending `play` to the `session_state` `playing` event shall be at most 3 s, and the first picture shall be visible at most 6 s after the key press, in at least 19 of 20 consecutive zaps. | T |
| NFR-02 | Performance — protected start | For a protected service with a stored correct key, the time from `play` to `playing` shall be at most 4 s, and the first picture shall be visible at most 7 s after the key press. | T |
| NFR-03 | Performance — incorrect key | An incorrect key shall be reported (`key_required`, `key_rejected`) at most 3 s after the first RTP data of the session. | T |
| NFR-04 | Accuracy — rewind position | The burned-in time code of the first presented picture shall differ from the requested position (`T mod 600`) by at most 1 s. | T |
| NFR-05 | Responsiveness | Every key press shall produce a visible reaction of the OSD (indicator, dialog, list, number) within 200 ms. | T |
| NFR-06 | Stability | The system shall play live for 30 minutes (at least 3 loop boundaries of the content) without a component terminating, without an interruption of the picture longer than 1 s at a loop boundary, with a growth of the middleware's resident memory of at most 20 MB, and with at most 10 segment files in the HLS directory at any time. | T |
| NFR-07 | Robustness | No component shall terminate when: malformed JSON or random bytes are sent to its interfaces; random datagrams are sent to the RTP port; the OTT MW server or the head-end is stopped and restarted during playback. After the restart, playback of a newly selected service shall work without restarting the receiver. When the middleware process is stopped, the head-end shall stop sending its stream within 20 s. | T |
| NFR-08 | Security of keys | Keys shall not appear in any log file, any file on disk, or any network traffic other than the local control WebSocket (IF-INT-01). Verified by inspection of the code, by searching the log files for the test keys, and by searching a loopback packet capture of a full demo run for the key bytes outside IF-INT-01. | I, A |
| NFR-09 | Portability | The AL library `dtvcore` shall build and pass its unit tests without OpenSSL, Mongoose, socket or HAL headers, using only the C++17 standard library, nlohmann/json and the IF-INT-04 headers (DTV SDK and platform interfaces). | I, T |
| NFR-10 | Testability | Unit tests shall exist at least for AL-PSI, AL-DSC, AL-OUT and AL-SES (with a fake DTV SDK), for HAL-TUN, HAL-DMX and HAL-DSC, for each PAL module, and for SRV-PKG (encryption round trip) and SRV-ARC (time → position). Native tests shall run with one `ctest` command, server tests with one `python3 -m unittest` command, Web application tests with one documented command. | I, T |
| NFR-11 | Code quality | C++ code shall compile with `-Wall -Wextra` without warnings; Python code shall use only the standard library and `cryptography`; the Web application shall be plain ES2020 modules without a build step. | I |
| NFR-12 | Operability | One script (`scripts/start.sh`) shall start the OTT MW server, the head-end, the middleware and Chromium in kiosk mode within 15 s, checking that each component has opened its port and stopping with an error message naming the component that failed; one script (`scripts/stop.sh`) shall stop all components. | D |
| NFR-13 | Logging | Every component shall write timestamped log lines (`<ISO time> <LEVEL> [<module>] <message>`) to `logs/<component>.log`; the logs of a demo run shall contain the evidence listed in REQ-59. | I |
| NFR-14 | Usability | The OSD shall be readable on the projector: text at least 28 px high at 1920×1080, light text on a dark semi-transparent background; all texts in English. | D |
| NFR-15 | Resource usage | During playback, the middleware process shall use on average at most 25 % of one CPU core. | T |
| NFR-16 | Configurability | Ports, URLs, file paths and timeouts shall be read from configuration files; values in the code are only defaults. | I |
| NFR-17 | Reproducibility | The repository shall contain run instructions (`README.md`) and the test data documentation: SHA-256 checksums of the content files, the correct keys of services 3 and 4, an incorrect key and the expected services and EPG. | I |

### 2.5 Possible extensions

The following extensions are **not** part of the mandatory scope. They may be implemented only after all mandatory requirements are verified, and only if they do not change the interfaces of Section 3 in an incompatible way.

| ID | Extension | Relation to the course material |
|---|---|---|
| EXT-01 | Rotating control words delivered in ECM, service keys in EMM, protected by the viewer's key. | Lecture 3: CAS key hierarchy. |
| EXT-02 | Key delivery from the OTT MW server after viewer authentication (login), instead of manual entry. | Lecture 4: access management server. |
| EXT-03 | Live delivery over IP multicast with IGMP group join. | Lecture 4: managed IPTV streaming stack. |
| EXT-04 | RTSP (SETUP / PLAY with range / TEARDOWN) instead of the HTTP stream control API. | Lecture 4: RTP/RTSP. |
| EXT-05 | Pause and local timeshift buffer in the receiver. | Lecture 2: timeshift use case. |
| EXT-06 | Dialog for entering an arbitrary time inside the archive window. | — |
| EXT-07 | Several audio languages and subtitles (DVB subtitles or WebVTT). | Lectures 3–4. |
| EXT-08 | Several services in one transport stream (MPTS), selected by program number. | Lecture 3: PAT program loop. |
| EXT-09 | SDT and EIT parsing from the transport stream as a second source of service names and EPG. | Lecture 3: DVB-SI. |
| EXT-10 | Remembering the last service and the stored keys across restarts (persistent setup module). | Lecture 7: setup module. |
| EXT-11 | RTP packet reordering and loss concealment. | Lecture 4: RTP. |
---

## 3. Interface Specification

This section is the **contract between the three layers** (Section 2.2.5). It is frozen at milestone I-1; after that, changes follow rule 2 of Section 2.2.7. Each interface is described from the point of view of both sides: what the provider guarantees and what the consumer may rely on. The JSON examples in this section are normative: the stand-ins of Section 2.2.7 (mock middleware, fake HTTP client) return exactly these examples.

Planned structure of the rest of the document: **Section 4** — design (module behavior, state machines, sequence diagrams, error handling), **Section 5** — verification plan.

### 3.1 Common conventions

| Topic | Convention |
|---|---|
| Character encoding | UTF-8 everywhere. |
| JSON | Field names in `snake_case`. Unknown fields must be ignored by the receiver of a message (forward compatibility). Missing mandatory fields make the message invalid. |
| Time | Integer UNIX time in seconds, UTC (`int64`). Durations in seconds (`int`) unless the name ends in `_ms`. |
| Service identifier | `service_id`: string `svc-<n>`, e.g. `svc-3`; stable, used on all interfaces. `number`: integer channel number shown to the viewer (1–4). |
| Start point | `start`: either the string `"live"` or an integer UNIX time `T` in the past (archive). |
| Key | `key`: string of exactly 32 hexadecimal characters, case-insensitive (`[0-9A-Fa-f]{32}`). Keys travel only on IF-INT-01 (UI → AL, same machine) and IF-INT-04 (AL → PAL cipher call); never over HTTP to any server [HLR-16]. |
| Archive window | Time `T` is inside the archive window if `now − 7200 ≤ T < now` (D-08). |
| Error object | Every error on IF-EXT-03, IF-EXT-04 and IF-INT-01 has the form `{"code": "<ERROR_CODE>", "message": "<text for logs>"}`. Error codes are listed in Section 3.10. |

### 3.2 IF-EXT-03 — OTT MW REST API

Provider: SRV-MW (WP-PAL). Consumer: AL-SVC through PAL-HTTP (WP-AL). Base URL: `http://127.0.0.1:8080/api/v1`. All responses are `Content-Type: application/json`. Successful responses have status `200`; errors have `400` or `404` and the body `{"error": {...}}`.

#### 3.2.1 `GET /services` — service list

Response `200`:

```json
{
  "services": [
    { "service_id": "svc-1", "number": 1, "name": "Demo One",   "protected": false },
    { "service_id": "svc-2", "number": 2, "name": "Demo Two",   "protected": false },
    { "service_id": "svc-3", "number": 3, "name": "Premium One", "protected": true },
    { "service_id": "svc-4", "number": 4, "name": "Premium Two", "protected": true }
  ]
}
```

- Sorted by `number`, numbers unique.
- `protected` is informative (used to mark services in the list); the receiver decides about protection from the stream (D-04, US-17).

#### 3.2.2 `GET /services/{service_id}` — service info

Response `200`:

```json
{
  "service_id": "svc-3",
  "number": 3,
  "name": "Premium One",
  "protected": true,
  "description": "Protected demo service",
  "stream_control_url": "http://127.0.0.1:8081/api/v1/stream",
  "archive_window_s": 7200
}
```

Errors: `404 SERVICE_NOT_FOUND`.

#### 3.2.3 `GET /epg?from=<T1>&to=<T2>[&service_id=<id>]` — EPG

Returns every event that overlaps the interval `[from, to)`, for all services or for one service. Constraints: `from < to`, `to − from ≤ 86400`.

Response `200`:

```json
{
  "server_time": 1795000000,
  "from": 1794993000,
  "to": 1795003000,
  "services": [
    {
      "service_id": "svc-3",
      "events": [
        {
          "event_id": "svc-3-1794999600",
          "start": 1794999600,
          "duration": 300,
          "title": "Premium Morning",
          "description": "First program of the protected demo service.",
          "in_archive": true
        }
      ]
    }
  ]
}
```

- Events of a service are sorted by `start` and do not overlap; `start` is a multiple of 300 (D-08, D-09).
- `event_id` = `<service_id>-<start>`, unique.
- `in_archive` is `true` when `server_time − 7200 ≤ start ≤ server_time` (the event has started and its beginning is still in the archive).
- `server_time` is the server's current UNIX time at the moment of the response.

Errors: `400 INVALID_RANGE` (missing, non-integer or too large range), `404 SERVICE_NOT_FOUND`.

### 3.3 IF-EXT-04 — Stream control API

Provider: SRV-HEC (WP-PAL). Consumer: AL-HEC through PAL-HTTP (WP-AL). Base URL: the `stream_control_url` from the service info (default `http://127.0.0.1:8081/api/v1/stream`). Requests and responses are JSON.

#### 3.3.1 `POST /start` — start a stream session

Request:

```json
{ "service_id": "svc-3", "start": 1794999600, "client_host": "127.0.0.1", "client_port": 5004, "ssrc": 3054235519 }
```

or, for live:

```json
{ "service_id": "svc-1", "start": "live", "client_host": "127.0.0.1", "client_port": 5004, "ssrc": 1180331842 }
```

Response `200`:

```json
{
  "session_id": "hs-000017",
  "service_id": "svc-3",
  "start_time": 1794999600,
  "ssrc": 3054235519,
  "payload_type": 33
}
```

Provider guarantees:

- `start_time` is the UNIX time of the first content sent: the requested `T`, or the server's current time for `"live"`.
- If a session already exists for the same `client_host:client_port`, it is stopped **before** the new one is created; after the response, no packet of the old session is sent (D-06).
- The RTP packets of the session carry the `ssrc` given in the request (chosen by the receiver, so that the receiver can tune to it before the first packet arrives); the response repeats it.
- The session is stopped by the head-end when no `POST /start`, `POST /keepalive` for it has been received for 15 s (Section 3.3.3).
- The first RTP packets of a session carry PAT and PMT, then the TS packets from the content position of `start_time` (Section 3.4).
- After the start time, the session continues in real time without end (an archive session becomes "delayed live") until it is stopped.

Errors:

| Status | Code | When |
|---|---|---|
| 400 | `INVALID_REQUEST` | Missing or malformed field (including `ssrc` not an unsigned 32-bit integer). |
| 400 | `OUT_OF_ARCHIVE` | `T < now − 7200`. |
| 400 | `FUTURE_TIME` | `T ≥ now` (the consumer must use `"live"`). |
| 404 | `SERVICE_NOT_FOUND` | Unknown `service_id`. |

#### 3.3.2 `POST /stop` — stop a stream session

Request: `{ "session_id": "hs-000017" }` → response `200`: `{ "stopped": true }`. After the response, no packet of that session is sent.
Errors: `404 SESSION_NOT_FOUND` (also for an already stopped session; the consumer treats it as success).

#### 3.3.3 `POST /keepalive` — keep a stream session alive

Request: `{ "session_id": "hs-000017" }` → response `200`: `{ "expires_in_s": 15 }`.

- The receiver sends a keepalive for its current session every 5 s while the session is running (REQ-67).
- A session that has received neither its start nor a keepalive for 15 s is stopped by the head-end (REQ-68).
- Errors: `404 SESSION_NOT_FOUND` — the session was stopped or expired, or the head-end was restarted; the receiver treats the stream as lost (REQ-69).

### 3.4 IF-EXT-05 — RTP media stream

Provider: SRV-RTP (WP-PAL). Consumer: HAL-TUN (WP-PAL). The content of the packets is consumed by the AL, so the TS-level rules below are also a contract with WP-AL.

| Item | Specification |
|---|---|
| Transport | UDP unicast to `client_host:client_port` from the start request. |
| RTP header (RFC 3550) | 12 bytes, no CSRC, no extension, no padding: `V = 2`, `P = 0`, `X = 0`, `CC = 0`, `M = 0`, `PT = 33`. |
| Sequence number | Random initial value per session, incremented by 1 per packet (wraps at 65535). |
| Timestamp | 90 kHz clock derived from the PCR of the content (RFC 2250). |
| SSRC | Value given by the receiver in `POST /start`; constant during the session. |
| Payload | 1 to 7 complete TS packets of 188 bytes (normally 7 = 1316 bytes). |
| Pacing | Real time, paced by the PCR of the content. |
| Start of session | The first TS packets of a session are one PAT and one PMT; then packets from the TS packet with the largest PCR not later than the requested position. |
| Loop boundary | At the end of the content file the session continues with the beginning of the file without interruption (D-08). At that point PCR, PTS and DTS jump back by the file duration and continuity counters may be discontinuous; the receiver must tolerate this (REQ-24). |
| TS content | One program (SPTS): PID 0 (PAT), PMT PID, video PID (H.264, also PCR PID), audio PID (AAC). PID values are **not** fixed by this contract; the receiver must take them from PAT/PMT [HLR-12]. |
| Protection | For protected services, packets of the video and audio PIDs have `transport_scrambling_control = '10'` and an encrypted payload according to D-04; all other packets and all adaptation fields are in the clear. |

### 3.5 IF-INT-05 — Content configuration (operator side)

Internal to WP-PAL; documented because the start scripts and the test data set (US-22) depend on it. File `content/services.json`:

```json
{
  "archive_window_s": 7200,
  "services": [
    {
      "service_id": "svc-3",
      "number": 3,
      "name": "Premium One",
      "description": "Protected demo service",
      "protected": true,
      "file": "svc-3.enc.ts",
      "programs": [
        { "title": "Premium Morning", "description": "First program of the protected demo service.", "duration": 300 },
        { "title": "Premium Evening", "description": "Second program of the protected demo service.", "duration": 300 }
      ]
    }
  ]
}
```

- The sum of `programs[].duration` equals the duration of `file` (600 s, D-09); SRV-MW generates the EPG cyclically from this list, SRV-ARC uses the same cycle.
- Keys are **not** stored in `services.json`. The packaging tool takes the key on its command line; the test keys are listed in the test data documentation (US-22).

### 3.6 IF-INT-01 — UI control API (WebSocket, JSON)

Provider of the semantics: AL-API (WP-AL). Consumer: APP-MWC (WP-APP). Transport: PAL-WS. URL: `ws://127.0.0.1:8090/ws/control`. Every WebSocket text frame contains exactly one JSON message.

#### 3.6.1 Message envelopes

| Direction | Envelope |
|---|---|
| UI → AL, command | `{"type": "command", "id": <int>, "name": "<command>", "params": {...}}` — `id` chosen by the UI, unique per connection. |
| AL → UI, response | `{"type": "response", "id": <same id>, "ok": true, "result": {...}}` or `{"type": "response", "id": <same id>, "ok": false, "error": {"code": "...", "message": "..."}}` |
| AL → UI, event | `{"type": "event", "name": "<event>", "data": {...}}` — sent to all connected UI clients. |

Rules:

- Every command gets exactly one response. A response means the command was **accepted** (or rejected); for `play` and `set_key` the **completion** is reported later by events (asynchronous command, Section 2.2.6).
- The AL responds within 1 s to every command (all commands are non-blocking).
- A message that is not valid JSON or has an unknown `type`/`name` gets an error response `INVALID_COMMAND` (if `id` is readable) and is otherwise ignored.

#### 3.6.2 Commands

| Command | `params` | `result` (on `ok: true`) | Errors |
|---|---|---|---|
| `get_services` | `{}` | `{"services": [ … ]}` — same objects as in 3.2.1 | `MW_UNREACHABLE` |
| `get_epg` | `{"from": T1, "to": T2}` | `{"server_time": …, "services": [ … ]}` — same objects as in 3.2.3 | `MW_UNREACHABLE`, `INVALID_RANGE` |
| `play` | `{"service_id": "svc-3", "start": "live" \| T}` | `{"session": <int>}` — the new session number | `SERVICE_NOT_FOUND`, `INVALID_COMMAND` |
| `set_key` | `{"service_id": "svc-3", "key": "<32 hex>"}` | `{}` | `INVALID_KEY_FORMAT`, `SERVICE_NOT_FOUND` |
| `stop` | `{}` | `{}` | — |
| `get_status` | `{}` | The last `session_state` event data (3.6.3), or `{"state": "idle"}` | — |

Behavior that the UI may rely on:

- **`play`** stops the current session (if any), creates a new session with a number higher than every previous one and returns it immediately. If `start` is a time `T`: `T` before the archive window is moved to the window start; `T ≥ now` is played as `"live"` (US-12 AC2, AC3). The effective start point is reported in the `session_state` events.
- **`set_key`** stores the key for `service_id` for the running session of the middleware process (D-03). If the current session is of that service and waits for a key (`key_required`), it is restarted from the kept start point (D-14). A correct format does not mean a correct key: correctness is reported by `session_state` (`playing` or `key_required` with `reason: "key_rejected"`).
- **Session numbers** let the UI ignore late events: the UI only reacts to events whose `session` equals the number returned by its last `play` (latest request wins).

#### 3.6.3 Events

**`session_state`** — sent on every state change of the current session.

```json
{
  "type": "event",
  "name": "session_state",
  "data": {
    "session": 12,
    "service_id": "svc-3",
    "state": "playing",
    "reason": null,
    "start": 1794999600,
    "start_time": 1794999600,
    "protected": true,
    "error": null
  }
}
```

| Field | Meaning |
|---|---|
| `session` | Session number from `play`. |
| `state` | One of the states below. |
| `reason` | Additional reason for `key_required` and `no_signal`, otherwise `null`. |
| `start` | Requested start point after clamping: `"live"` or `T`. |
| `start_time` | UNIX time of the first content of the stream (from IF-EXT-04), `null` until known. The UI computes the playback position as `start_time + (now − time at which state became playing)`. |
| `protected` | `true` when scrambled A/V packets were detected, `null` until known. |
| `error` | Error object for state `error`, otherwise `null`. |

| `state` | Meaning | HLS stream on IF-INT-02 |
|---|---|---|
| `starting` | Start request sent to the head-end. | no |
| `waiting_stream` | Head-end accepted; waiting for RTP data and for PAT/PMT. | no |
| `key_required` | Stream is protected and no key is stored (`reason: "no_key"`) or the stored key was rejected (`reason: "key_rejected"`). The head-end session is stopped; the start point is kept (D-14). | no |
| `validating_key` | A key is stored; the first PES starts are being checked (D-11). | no |
| `playing` | Content is decrypted (if protected); the first HLS segment of the session is listed in its playlist. | **yes** |
| `no_signal` | No RTP data within 5 s after the head-end accepted the request (`reason: "no_data_at_start"`), or no RTP data for 3 s while `playing` (`reason: "data_lost"`). The session continues and returns to `playing` when data arrives again (US-05, US-19). | no |
| `stopped` | Session ended by `stop` or replaced by a newer `play`. | no |
| `error` | Session failed (e.g. `HEADEND_UNREACHABLE`, `OUT_OF_ARCHIVE`); see `error`. | no |

**`stream_info`** — sent once per session when PAT and PMT have been parsed (evidence of HLR-12; data for the info dialog).

```json
{
  "type": "event",
  "name": "stream_info",
  "data": {
    "session": 12,
    "service_id": "svc-3",
    "program_number": 1,
    "pmt_pid": 4096,
    "pcr_pid": 256,
    "video": { "pid": 256, "stream_type": 27, "codec": "H.264" },
    "audio": { "pid": 257, "stream_type": 15, "codec": "AAC" },
    "scrambled": true
  }
}
```

**`mw_status`** — sent when the reachability of the OTT MW server changes, and once to every newly connected UI client.

```json
{ "type": "event", "name": "mw_status", "data": { "reachable": false } }
```

### 3.7 IF-INT-02 — UI media stream (HLS)

Provider: AL-OUT (WP-AL), which writes the files through `IHlsStore` (PAL-FS); PAL-WS serves them. Consumer: APP-VIEW with hls.js (WP-APP). Standard: HTTP Live Streaming, RFC 8216.

| Item | Specification |
|---|---|
| URLs | Playlist: `http://127.0.0.1:8090/hls/<session>/index.m3u8`; segments: `http://127.0.0.1:8090/hls/<session>/seg-<sequence>.ts`, where `<sequence>` starts at 0 for every session. Same origin as the Web application, so no CORS rules are needed. |
| HTTP | `Content-Type: application/vnd.apple.mpegurl` for the playlist and `video/mp2t` for segments; `Cache-Control: no-cache` for the playlist. Unknown session or file: `404`. |
| Playlist | Live media playlist, `#EXT-X-VERSION:3`, `#EXT-X-TARGETDURATION:2`, `#EXT-X-MEDIA-SEQUENCE:<sequence of the first listed segment>`, `#EXT-X-DISCONTINUITY-SEQUENCE` when needed, one `#EXTINF:<duration with 3 decimals>,` per segment, at most the last 6 segments, no `#EXT-X-ENDLIST`. The playlist file is replaced atomically. |
| Segments | MPEG-TS of session `n` only: PAT, PMT, video and audio PIDs; `transport_scrambling_control = '00'` on all packets; payloads decrypted. Every segment starts with one PAT, one PMT and the start of a video key frame, and contains about 1 s of content (at most 2 s). A segment is listed only after its file is complete. |
| Loop boundary | The first segment after the content loop boundary is preceded by `#EXT-X-DISCONTINUITY` in the playlist (REQ-24); timestamps inside the segments are not changed. |
| Example | `#EXTM3U` / `#EXT-X-VERSION:3` / `#EXT-X-TARGETDURATION:2` / `#EXT-X-MEDIA-SEQUENCE:41` / `#EXTINF:1.000,` / `seg-41.ts` / `#EXTINF:1.000,` / `seg-42.ts` / `#EXT-X-DISCONTINUITY` / `#EXTINF:1.000,` / `seg-43.ts` (one line each). |
| When to load | The playlist of session `n` exists from the moment `session_state` reports `state: "playing"` for session `n` (the first segment is already listed). The UI creates a new hls.js player at that moment and destroys the previous one. |
| Start position | The player starts with the **first** segment listed in the playlist, not at the live edge (hls.js configuration `startPosition: 0`), so that playback begins at the requested start point (D-14, NFR-04). |
| End | When session `n` leaves `playing` for `stopped`, `key_required` or `error`, or a newer session starts, the AL removes the directory of session `n`; requests for it then return `404`. |
| Volume | Not part of the stream: volume and mute are set on the `<video>` element by the UI (D-13). |

### 3.8 IF-INT-03 — Web application files

Provider: PAL-WS (WP-PAL). Consumer: browser (WP-APP). `GET http://127.0.0.1:8090/<path>` serves the files of the directory `webui/` (configured as `webui_dir`); `/` serves `index.html`. Paths under `/hls/` are reserved for IF-INT-02. The Web application loads hls.js from `https://cdn.jsdelivr.net/npm/hls.js@1.7.4/dist/hls.min.js` (HLR-21).

### 3.9 IF-INT-04 — DTV SDK and platform interfaces (C++ headers)

Provider: WP-PAL (implementations in `middleware/pal/` over the HAL in `middleware/hal/`). Consumer: WP-AL (`middleware/core/`). The headers below are placed in `middleware/include/pal/` on `main` and are the normative form of this contract. The HAL has no interface towards the AL; it is reached only through the DTV SDK (`ITuner`, `IDemux`).

The DTV SDK follows the object lifecycle of the DTV SDKs of Lecture 6 (Nexus, Android Tuner SDK): *tune* the frontend, *open* a filter on a PID, *start* it, receive *filter events* with data, *stop* and *close* it. Mapping to the Android table-parsing flow of Lecture 6: `Tuner.openFilter` → `IDemux::open_filter`, `Filter.start` → `start_filter`, `onFilterEvent` + `Filter.read` → `DataCallback` (data delivered with the event).

#### 3.9.1 Rules common to all interfaces

1. **Threads.** The PAL and HAL run their own threads: one event-loop thread (HTTP client, HTTP/WebSocket server, timers) and one tuner receive thread, on which the demux, the descrambler and the filter callbacks also run. Callbacks into the AL are called **on these threads**. The AL must not block in a callback for more than 10 ms; it hands the work to its own thread.
2. **Thread safety.** All interface methods may be called from any thread.
3. **Buffer ownership.** Pointers passed to a callback are valid only during the callback; the AL copies what it needs. Data passed to an interface method is copied before the method returns.
4. **Cancellation.** After `untune()`, `close_filter()`, `cancel()`, `cancel_timer()` or `Platform::stop()` returns, the corresponding callbacks are never called again.
5. **Errors.** Interface methods do not throw exceptions; failures are reported by return values or by the result object passed to the callback.

#### 3.9.2 Headers

```cpp
// middleware/include/pal/platform.hpp — IF-INT-04, frozen at I-1
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pal {

constexpr std::size_t kTsPacketSize = 188;

// ================================================================ DTV SDK

// ---------------------------------------------------------------- Tuner (HAL-TUN)
// IP tuner: receives MPEG-TS over RTP/UDP (IF-EXT-05) and feeds the demux.
struct TuneParams {
    std::uint16_t udp_port;   // receiver UDP port
    std::uint32_t ssrc;       // only RTP packets with this SSRC are accepted
};

enum class TunerStatus { Unlocked, Locked };

class ITuner {
public:
    struct Stats {
        std::uint64_t rtp_packets;   // accepted RTP packets
        std::uint64_t rtp_lost;      // gaps detected in sequence numbers
        std::uint64_t rtp_invalid;   // datagrams dropped (bad header, PT != 33, other SSRC, size not n*188)
    };
    using StatusCallback = std::function<void(TunerStatus status)>;

    virtual ~ITuner() = default;
    // Must be called before Platform::start().
    virtual void set_status_handler(StatusCallback on_status) = 0;
    // Starts reception; replaces any previous tuning. The status is Unlocked after tune();
    // it becomes Locked at the first accepted packet, Unlocked after 3 s without accepted packets,
    // and Locked again when they return. Returns false if the port cannot be opened.
    virtual bool tune(const TuneParams& params) = 0;
    // Stops reception; no status callback and no demux data after it returns.
    virtual void untune() = 0;
    virtual Stats stats() const = 0;
};

// ---------------------------------------------------------------- Demux + descrambler (HAL-DMX, HAL-DSC)
enum class FilterType {
    Section,   // delivers complete PSI sections (table_id … CRC_32; the CRC is not checked by the demux)
    Ts         // delivers whole TS packets of the PID, one packet per callback
};

struct FilterConfig {
    FilterType type;
    std::uint16_t pid;
    int table_id = -1;   // Section filters only: deliver only sections with this table_id; -1 = all
};
// Several filters (of any type) may be open on the same PID at the same time; each receives its own copy.

class IDemux {
public:
    using FilterId = std::uint32_t;   // 0 = invalid
    struct FilterData {
        FilterId filter;
        const std::uint8_t* data;   // section or one TS packet (188 bytes), valid only during the callback
        std::size_t len;
        bool scrambled;             // Ts filters: the packet was received with transport_scrambling_control != '00'
                                    // (if a key is set for the PID, `data` is already decrypted and its bits are '00')
    };
    using DataCallback = std::function<void(const FilterData& data)>;
    struct Stats {
        std::uint64_t ts_packets;    // packets received from the tuner
        std::uint64_t sync_errors;   // packets dropped because the sync byte is not 0x47
        std::uint64_t cc_errors;     // continuity counter discontinuities
    };

    virtual ~IDemux() = default;
    // Creates a stopped filter; returns 0 if it cannot be created.
    virtual FilterId open_filter(const FilterConfig& config, DataCallback on_data) = 0;
    virtual bool start_filter(FilterId id) = 0;
    virtual void stop_filter(FilterId id) = 0;
    // Destroys the filter; no callback for it after return.
    virtual void close_filter(FilterId id) = 0;
    // Descrambler: AES-128 key (16 bytes) for the given PIDs, applied according to D-04.
    virtual void set_key(const std::uint16_t* pids, std::size_t pid_count, const std::uint8_t key[16]) = 0;
    // Removes all descrambler keys.
    virtual void clear_keys() = 0;
    virtual Stats stats() const = 0;
};

// ================================================================ Other platform services

// ---------------------------------------------------------------- HTTP client
struct HttpResponse {
    bool transport_ok;     // false: connection failed or timeout (status is then 0)
    int status;            // HTTP status code
    std::string body;      // response body
    std::string error;     // transport error text when transport_ok == false
};

class IHttpClient {
public:
    using RequestId = std::uint64_t;
    using Callback = std::function<void(const HttpResponse&)>;

    virtual ~IHttpClient() = default;
    // Asynchronous requests; the callback is called exactly once, unless the request is cancelled.
    virtual RequestId get(const std::string& url, int timeout_ms, Callback on_done) = 0;
    virtual RequestId post_json(const std::string& url, const std::string& json_body,
                                int timeout_ms, Callback on_done) = 0;
    virtual void cancel(RequestId id) = 0;
};

// ---------------------------------------------------------------- UI control channel (IF-INT-01 transport)
class IUiChannel {
public:
    using ClientId = std::uint64_t;
    using MessageCallback = std::function<void(ClientId client, const std::string& text)>;
    using ConnectionCallback = std::function<void(ClientId client, bool connected)>;

    virtual ~IUiChannel() = default;
    // Must be called before Platform::start().
    virtual void set_handlers(MessageCallback on_message, ConnectionCallback on_connection) = 0;
    virtual void send(ClientId client, const std::string& text) = 0;  // one WebSocket text frame
    virtual void broadcast(const std::string& text) = 0;              // to all connected clients
};

// ---------------------------------------------------------------- HLS file store (IF-INT-02 storage)
// Files of one session live in <hls_dir>/<session>/ and are served under /hls/<session>/.
class IHlsStore {
public:
    using SessionId = std::uint64_t;

    virtual ~IHlsStore() = default;
    // Creates the empty directory of `session`. Returns false on a file-system error.
    virtual bool open_session(SessionId session) = 0;
    // Writes a complete file atomically (temporary name, then rename), replacing an existing one.
    // `name` is a plain file name such as "seg-17.ts" or "index.m3u8". Returns false on error.
    virtual bool write_file(SessionId session, const std::string& name,
                            const std::uint8_t* data, std::size_t len) = 0;
    // Removes one file of `session`; removing a missing file is not an error.
    virtual bool remove_file(SessionId session, const std::string& name) = 0;
    // Removes the directory of `session` with all its files.
    virtual void close_session(SessionId session) = 0;
};

// ---------------------------------------------------------------- Clock and timers
class IClock {
public:
    using TimerId = std::uint64_t;
    using TimerCallback = std::function<void()>;

    virtual ~IClock() = default;
    virtual std::int64_t unix_time_s() const = 0;    // wall clock, UTC
    virtual std::int64_t monotonic_ms() const = 0;   // for timeouts and durations
    virtual TimerId start_timer(int delay_ms, TimerCallback on_expired) = 0;  // one-shot
    virtual void cancel_timer(TimerId id) = 0;
};

// ---------------------------------------------------------------- Logger
enum class LogLevel { Debug, Info, Warn, Error };

class ILogger {
public:
    virtual ~ILogger() = default;
    // Thread-safe. Line format: "<ISO time> <LEVEL> [<module>] <message>"
    virtual void log(LogLevel level, const char* module, const std::string& message) = 0;
};

// ================================================================ Platform (factory)
struct PlatformConfig {
    std::uint16_t ui_http_port = 8090;      // IF-INT-01/02/03
    std::string webui_dir = "webui";        // IF-INT-03
    std::string hls_dir = "/tmp/m15-hls";   // IF-INT-02; served under /hls/, emptied at start()
    std::string log_file = "logs/middleware.log";
};

class Platform {
public:
    virtual ~Platform() = default;
    // DTV SDK
    virtual ITuner& tuner() = 0;
    virtual IDemux& demux() = 0;
    // Other platform services
    virtual IHttpClient& http() = 0;
    virtual IUiChannel& ui() = 0;
    virtual IHlsStore& hls() = 0;
    virtual IClock& clock() = 0;
    virtual ILogger& logger() = 0;
    // Empties hls_dir, starts the event-loop thread and the HTTP/WebSocket server.
    // Returns false if the port cannot be opened or hls_dir cannot be created.
    virtual bool start() = 0;
    // Stops all PAL and HAL threads; no callback is called after it returns.
    virtual void stop() = 0;
};

// Implemented by WP-PAL (Linux: sockets, OpenSSL, Mongoose; HAL in software).
std::unique_ptr<Platform> create_platform(const PlatformConfig& config);

}  // namespace pal
```

### 3.10 Error codes

| Code | Interface | Meaning | Shown to the viewer as |
|---|---|---|---|
| `INVALID_REQUEST` | IF-EXT-04 | Malformed request to the head-end. | — (logged; programming error) |
| `INVALID_RANGE` | IF-EXT-03, IF-INT-01 | Invalid EPG time range. | — (logged) |
| `SERVICE_NOT_FOUND` | IF-EXT-03, IF-EXT-04, IF-INT-01 | Unknown `service_id`. | "Invalid service number" |
| `OUT_OF_ARCHIVE` | IF-EXT-04, IF-INT-01 (`error` state) | Requested time before the archive window. | "Content not available in archive" |
| `FUTURE_TIME` | IF-EXT-04 | Requested time not in the past. | — (the AL sends `"live"` instead) |
| `SESSION_NOT_FOUND` | IF-EXT-04 | Unknown, stopped or expired head-end session. | — on stop (treated as success); "No signal" on keepalive (REQ-69) |
| `MW_UNREACHABLE` | IF-INT-01 | OTT MW REST server not reachable or returned invalid data. | "Server not reachable" |
| `HEADEND_UNREACHABLE` | IF-INT-01 (`error` state) | Head-end not reachable or returned an error. | "No signal" |
| `INVALID_COMMAND` | IF-INT-01 | Malformed or unknown command. | — (logged; programming error) |
| `INVALID_KEY_FORMAT` | IF-INT-01 | Key is not 32 hexadecimal characters. | Key dialog stays open (cannot happen with the dialog of US-14 AC3) |

