// TST-401 (REQ-60, Section 4.4.3 key mapping). Expected values from Sections 1.5.1 and 4.4.3.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mapKey, createKeyRouter } from '../js/keys.js';
import { createModel, Context } from '../js/model.js';

// Minimal KeyboardEvent: the fields APP-KEY reads, and a record of preventDefault().
function keyEvent(key, { code = '', repeat = false } = {}) {
  return {
    key,
    code,
    repeat,
    defaultPrevented: false,
    preventDefault() { this.defaultPrevented = true; },
  };
}

// Every key of the table of Section 1.5.1 with the KeyboardEvent.key value Chromium reports.
const MAPPED = [
  ['ArrowUp', 'UP'], ['ArrowDown', 'DOWN'], ['ArrowLeft', 'LEFT'], ['ArrowRight', 'RIGHT'],
  ['Enter', 'ENTER'], ['Escape', 'ESC'], ['Backspace', 'BACKSPACE'],
  ['0', '0'], ['1', '1'], ['2', '2'], ['3', '3'], ['4', '4'],
  ['5', '5'], ['6', '6'], ['7', '7'], ['8', '8'], ['9', '9'],
  ['A', 'A'], ['B', 'B'], ['C', 'C'], ['D', 'D'], ['E', 'E'], ['F', 'F'],
  ['L', 'L'], ['I', 'I'], ['K', 'K'], ['M', 'M'],
  ['+', '+'], ['-', '-'],
];

test('TST-401 every key of Section 1.5.1 is mapped and preventDefault() is called', () => {
  const model = createModel();
  const router = createKeyRouter(model, {});
  for (const [key, expected] of MAPPED) {
    assert.equal(mapKey(keyEvent(key)), expected, `key ${key}`);
    const event = keyEvent(key);
    router(event);
    assert.equal(event.defaultPrevented, true, `preventDefault for ${key}`);
  }
});

test('TST-401 numeric keypad + and - and digits map like the main keyboard', () => {
  assert.equal(mapKey(keyEvent('+', { code: 'NumpadAdd' })), '+');
  assert.equal(mapKey(keyEvent('-', { code: 'NumpadSubtract' })), '-');
  assert.equal(mapKey(keyEvent('7', { code: 'Numpad7' })), '7');
  assert.equal(mapKey(keyEvent('Enter', { code: 'NumpadEnter' })), 'ENTER');
});

test('TST-401 lower-case letters are converted to upper case', () => {
  for (const letter of ['a', 'b', 'c', 'd', 'e', 'f', 'l', 'i', 'k', 'm']) {
    assert.equal(mapKey(keyEvent(letter)), letter.toUpperCase(), `key ${letter}`);
  }
});

test('TST-401 an auto-repeated event is mapped as a normal press', () => {
  const pressed = [];
  const model = createModel();
  const router = createKeyRouter(model, { [Context.PLAYBACK]: { onKey: (k) => pressed.push(k) } });
  const first = keyEvent('ArrowUp');
  const repeated = keyEvent('ArrowUp', { repeat: true });
  router(first);
  router(repeated);
  assert.deepEqual(pressed, ['UP', 'UP']);
  assert.equal(repeated.defaultPrevented, true);
});

test('TST-401 Tab, F5 and Q are not mapped and not prevented', () => {
  const pressed = [];
  const model = createModel();
  const router = createKeyRouter(model, { [Context.PLAYBACK]: { onKey: (k) => pressed.push(k) } });
  for (const key of ['Tab', 'F5', 'Q', 'q']) {
    assert.equal(mapKey(keyEvent(key)), null, `key ${key}`);
    const event = keyEvent(key);
    router(event);
    assert.equal(event.defaultPrevented, false, `no preventDefault for ${key}`);
  }
  assert.deepEqual(pressed, []);
});

test('TST-401 the mapped key is passed to the presenter of the active context only', () => {
  const received = {};
  const presenters = {};
  for (const context of Object.values(Context)) {
    received[context] = [];
    presenters[context] = { onKey: (k) => received[context].push(k) };
  }
  const model = createModel();
  const router = createKeyRouter(model, presenters);

  router(keyEvent('ArrowDown'));
  model.update({ context: Context.KEY_DIALOG });
  router(keyEvent('e'));

  assert.deepEqual(received[Context.PLAYBACK], ['DOWN']);
  assert.deepEqual(received[Context.KEY_DIALOG], ['E']);
  for (const context of [Context.NUMBER_ENTRY, Context.SERVICE_LIST, Context.EPG]) {
    assert.deepEqual(received[context], []);
  }
});
