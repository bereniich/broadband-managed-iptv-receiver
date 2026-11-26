// Diagnostics mode of APP-VIEW (Sections 4.4.6, 5.1.8): with the page parameter ?diag=1 the marks
// key, osd, frame, frag <sn>, wait and resume are written as "DIAG <epoch_ms> <mark> <details>".
// Without ?diag=1 nothing is written; the mode changes no behavior.
import { Context } from '../model.js';

const KEY_CHARACTER = /^[0-9A-F]$/;

export function diagEnabled(search) {
  return new URLSearchParams(search).get('diag') === '1';
}

// Returns the onMapped callback of the key router: mark `key` at once and `osd` in the next
// animation frame, after the views have rendered the change caused by the key. In the key dialog
// a key character is written as • (as on the screen), so that no key reaches a log (NFR-08, REQ-55).
export function createKeyMarker(diag, requestAnimationFrame, model) {
  return function onMapped(key) {
    if (!diag.enabled) return;
    const details = model.state.context === Context.KEY_DIALOG && KEY_CHARACTER.test(key) ? '•' : key;
    diag.mark('key', details);
    requestAnimationFrame(() => diag.mark('osd', details));
  };
}

// `log` receives one line (console.info in the browser); `clock.now()` gives epoch milliseconds.
export function createDiag({ enabled, clock, log }) {
  return {
    enabled,
    mark(name, details = '') {
      if (!enabled) return;
      log(details === '' ? `DIAG ${clock.now()} ${name}` : `DIAG ${clock.now()} ${name} ${details}`);
    },
  };
}
