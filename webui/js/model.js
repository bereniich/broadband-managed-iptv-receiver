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
