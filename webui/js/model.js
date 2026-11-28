// APP-MOD: application state and change notification (Section 4.4.2).
// No DOM access: the model is used by the presenters in Node.js unit tests (Section 4.4.6).

// UI contexts of Section 4.4.3; the context decides which presenter receives a key.
export const Context = Object.freeze({
  PLAYBACK: 'PLAYBACK',
  NUMBER_ENTRY: 'NUMBER_ENTRY',
  SERVICE_LIST: 'SERVICE_LIST',
  EPG: 'EPG',
  KEY_DIALOG: 'KEY_DIALOG',
});

export const INITIAL_VOLUME = 50;   // REQ-29

// Fields of the table of Section 4.4.2 with their initial values.
export function createInitialState() {
  return {
    connected: false,
    // Until the first mw_status arrives (sent to every new client, Section 3.6.3)
    // the server is not reported as unreachable.
    mwReachable: true,
    services: [],               // [{service_id, number, name, protected}] sorted by number
    currentServiceId: null,
    lastPlayId: null,
    // {number, serviceId, state, reason, start, startTime, playingSince, protected, error}
    session: null,
    playerSession: null,
    firstPictureAt: null,       // now() of the APP at the first picture of the current player
    playerFailed: false,        // third fatal hls.js error: "No signal" until the next play (ER-35)
    streamInfo: null,
    epg: null,                  // {from, to, serverTime, services}
    volume: INITIAL_VOLUME,
    muted: false,
    context: Context.PLAYBACK,
    zapTarget: null,
    seekTarget: null,
    numberBuffer: '',
    listIndex: 0,
    epgService: null,
    epgEvent: null,
    keyBuffer: '',
    keyDialogTitle: null,
    keyDialogCancelled: false,
    overlays: { info: false, volume: false, banner: false },
    toast: null,
  };
}

// Texts shown to the viewer (Section 1.6.1, Section 3.10).
export const Text = Object.freeze({
  NO_SIGNAL: 'No signal',
  SERVER_NOT_REACHABLE: 'Server not reachable',
  INVALID_SERVICE_NUMBER: 'Invalid service number',
  NOT_IN_ARCHIVE: 'Content not available in archive',
  PRESS_K: 'Protected service – press K to enter the key',
  ENTER_KEY: 'Enter the decryption key',
  INCORRECT_KEY: 'Incorrect decryption key',
  CLEAR: 'Clear',
  KEY_OK: 'Protected – key OK',
  KEY_REQUIRED: 'Protected – key required',
  NO_INFORMATION: 'No information',
  AVAILABLE: 'available',
  NOT_AVAILABLE: 'not available',
  LIVE: 'LIVE',
  ARCHIVE: 'ARCHIVE',
});

// Viewer message per error code of Section 3.10; codes without a message show "No signal".
const ERROR_TEXT = Object.freeze({
  SERVICE_NOT_FOUND: Text.INVALID_SERVICE_NUMBER,
  OUT_OF_ARCHIVE: Text.NOT_IN_ARCHIVE,
  MW_UNREACHABLE: Text.SERVER_NOT_REACHABLE,
  HEADEND_UNREACHABLE: Text.NO_SIGNAL,
  INVALID_STREAM: Text.NO_SIGNAL,
  RECEIVER_ERROR: Text.NO_SIGNAL,
});

export function errorText(code) {
  return ERROR_TEXT[code] ?? Text.NO_SIGNAL;
}

// ---------------------------------------------------------------- derived values (Section 4.4.2)
// `nowMs` is now() of the APP clock in milliseconds; all other times are UNIX seconds.

export function nowSeconds(nowMs) {
  return Math.floor(nowMs / 1000);
}

export function mode(state) {
  return state.session !== null && Number.isInteger(state.session.start) ? 'ARCHIVE' : 'LIVE';
}

// Playback position (REQ-43): startTime + (now − firstPictureAt) in ARCHIVE mode (startTime while
// firstPictureAt is null), now in LIVE mode. Before start_time is known, the requested start is used.
// firstPictureAt belongs to the player of the current session (Section 4.4.2); while the player of a
// previous session still plays (a new session has not reached its first playing yet), it does not count.
export function playbackPosition(state, nowMs) {
  if (mode(state) === 'LIVE') return nowSeconds(nowMs);
  const base = state.session.startTime ?? state.session.start;
  if (state.firstPictureAt === null || state.playerSession !== state.session.number) return base;
  return base + Math.floor((nowMs - state.firstPictureAt) / 1000);
}

// Distance from live in seconds (0 in LIVE mode).
export function distanceFromLive(state, nowMs) {
  return nowSeconds(nowMs) - playbackPosition(state, nowMs);
}

// Protection line of the info dialog (REQ-27), or null when there is none.
export function protectionStatus(state) {
  const s = state.session;
  if (s === null) return null;
  if (s.protected === false) return Text.CLEAR;
  if (s.protected === true && (s.state === 'playing' || s.state === 'no_signal')) return Text.KEY_OK;
  if (s.state === 'key_required') return Text.KEY_REQUIRED;
  return null;
}

// Persistent status message: the highest-priority condition that is true, or null.
export function statusMessage(state) {
  if (!state.connected) return Text.NO_SIGNAL;
  if (state.services.length === 0 && !state.mwReachable) return Text.SERVER_NOT_REACHABLE;
  if (state.playerFailed) return Text.NO_SIGNAL;
  const s = state.session;
  if (s === null) return null;
  if (s.state === 'no_signal') return Text.NO_SIGNAL;
  if (s.state === 'error') return errorText(s.error?.code);
  if (s.state === 'key_required' && state.keyDialogCancelled) return Text.PRESS_K;
  return null;
}

export function serviceById(state, serviceId) {
  return state.services.find((s) => s.service_id === serviceId) ?? null;
}

// Events of a service in the last EPG, or null for "No information" (REQ-36).
export function epgEvents(state, serviceId) {
  if (state.epg === null) return null;
  const entry = state.epg.services.find((s) => s.service_id === serviceId);
  if (!entry || entry.events.length === 0) return null;
  return entry.events;
}

export function eventAt(events, t) {
  if (events === null) return null;
  return events.find((e) => e.start <= t && t < e.start + e.duration) ?? null;
}

// EPG mark (REQ-35): "available", "not available" for a past event outside the archive,
// null for a future event. Past and future are relative to the server_time of the EPG.
export function epgMark(event, serverTime) {
  if (event.in_archive) return Text.AVAILABLE;
  if (event.start <= serverTime) return Text.NOT_AVAILABLE;
  return null;
}

// EPG event of the current service that contains the playback position (REQ-27), or null.
export function currentEvent(state, nowMs) {
  return eventAt(epgEvents(state, state.currentServiceId), playbackPosition(state, nowMs));
}

export function createModel() {
  const state = createInitialState();
  const listeners = new Set();

  return {
    state,

    // Applies the changed fields and notifies every listener once.
    update(changes) {
      Object.assign(state, changes);
      for (const listener of listeners) listener(state);
    },

    // Returns a function that removes the listener.
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
  };
}
