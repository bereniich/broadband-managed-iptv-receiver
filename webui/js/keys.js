// APP-KEY: maps a KeyboardEvent to a remote-control key and passes it to the presenter
// of the active context (Sections 1.5.1, 4.4.3; REQ-60). No DOM access beyond the event object.

const NAMED_KEYS = Object.freeze({
  ArrowUp: 'UP',
  ArrowDown: 'DOWN',
  ArrowLeft: 'LEFT',
  ArrowRight: 'RIGHT',
  Enter: 'ENTER',
  Escape: 'ESC',
  Backspace: 'BACKSPACE',
  '+': '+',   // main keyboard and numeric keypad (NumpadAdd) report the same key
  '-': '-',   // main keyboard and numeric keypad (NumpadSubtract)
});

// Letters that have a binding in some context of Section 1.5.1.
const LETTERS = new Set(['A', 'B', 'C', 'D', 'E', 'F', 'I', 'K', 'L', 'M']);

// Returns the remote-control key ('UP', 'ENTER', '7', 'A', '+', …) or null for an unmapped key.
// Auto-repeated events (event.repeat) are mapped like normal presses.
export function mapKey(event) {
  const key = event.key;
  if (Object.prototype.hasOwnProperty.call(NAMED_KEYS, key)) return NAMED_KEYS[key];
  if (key.length === 1 && key >= '0' && key <= '9') return key;
  if (key.length === 1) {
    const upper = key.toUpperCase();
    if (LETTERS.has(upper)) return upper;
  }
  return null;
}

// Creates the keydown handler. `presenters` maps a context (model.js Context) to an object
// with onKey(key). For every mapped key preventDefault() is called, so that the browser
// does not scroll or navigate; unmapped keys are ignored. `onMapped(key)` is called for every
// mapped key before the presenter (DIAG mark `key`, Section 5.1.8).
export function createKeyRouter(model, presenters, onMapped = () => {}) {
  return function onKeyDown(event) {
    const key = mapKey(event);
    if (key === null) return;
    event.preventDefault();
    onMapped(key);
    const presenter = presenters[model.state.context];
    if (presenter) presenter.onKey(key);
  };
}
