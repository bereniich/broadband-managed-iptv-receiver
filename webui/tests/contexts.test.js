// TST-402 (REQ-60, HLR-08): table-driven against Sections 1.5.1 and 4.4.3. For every context and
// every remote-control key of APP-KEY, the key either does the action of the table and leads to
// its next context, or changes nothing (model, middleware calls, timers, player).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Context, Text, nowSeconds } from '../js/model.js';
import { createFixture, sessionState, T0 } from './fixture.js';

// Every key that APP-KEY can produce (keys.js).
const ALL_KEYS = [
  'UP', 'DOWN', 'LEFT', 'RIGHT', 'ENTER', 'ESC', 'BACKSPACE',
  '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
  'A', 'B', 'C', 'D', 'E', 'F', 'I', 'K', 'L', 'M', '+', '-',
];
const DIGITS = ['0', '1', '2', '3', '4', '5', '6', '7', '8', '9'];
const HEX = [...DIGITS, 'A', 'B', 'C', 'D', 'E', 'F'];
const K3 = '3A7F1C9E5B2D8046A1E3C5079B4D6F28';
const ARCHIVE_START = T0 - 600;

function lastPlay(fx) {
  return fx.mwc.callsOf('play').at(-1).params;
}

// PLAYBACK: svc-2 in ARCHIVE at T0 − 600, first picture shown, info dialog hidden.
async function inPlayback() {
  const fx = createFixture({ nowS: T0 });
  await fx.start();
  await fx.answerPlay(1, { playing: true });
  fx.app.session.play('svc-2', ARCHIVE_START);
  await fx.answerPlay(2);
  await fx.emit('session_state', sessionState(2, 'svc-2', 'playing', { start: ARCHIVE_START, start_time: ARCHIVE_START }));
  fx.model.update({ firstPictureAt: fx.clock.now() });
  await fx.advance(5000);
  return fx;
}

const SETUP = {
  [Context.PLAYBACK]: inPlayback,
  async [Context.NUMBER_ENTRY]() { const fx = await inPlayback(); fx.press('1'); return fx; },
  async [Context.SERVICE_LIST]() { const fx = await inPlayback(); fx.press('L'); return fx; },
  async [Context.EPG]() {
    const fx = await inPlayback();
    fx.press('E');
    await fx.respondEpg();
    return fx;
  },
  async [Context.KEY_DIALOG]() { const fx = await inPlayback(); fx.press('K'); return fx; },
};

// Listed keys per context: [keys, next context, check(fx), optional keys pressed before].
const position = () => ARCHIVE_START + 5;   // 5 s after the first picture
// Start of the event running at the position: t − (t mod 300) (Section 5.2.4).
const EVENT_START = position() - (position() % 300);
const TABLE = {
  [Context.PLAYBACK]: [
    [['UP'], Context.PLAYBACK, (fx) => assert.equal(fx.state.zapTarget, 'svc-3')],
    [['DOWN'], Context.PLAYBACK, (fx) => assert.equal(fx.state.zapTarget, 'svc-1')],
    [['LEFT'], Context.PLAYBACK, (fx) => assert.equal(fx.state.seekTarget, position() - 30)],
    [['RIGHT'], Context.PLAYBACK, (fx) => assert.equal(fx.state.seekTarget, position() + 30)],
    [['B'], Context.PLAYBACK, (fx) => assert.deepEqual(lastPlay(fx), { service_id: 'svc-2', start: 'live' })],
    [DIGITS, Context.NUMBER_ENTRY, (fx, key) => assert.equal(fx.state.numberBuffer, key)],
    [['ENTER', 'L'], Context.SERVICE_LIST, (fx) => assert.equal(fx.state.listIndex, 1)],
    [['E'], Context.EPG, (fx) => assert.deepEqual(fx.mwc.lastCall().params, {
      from: nowSeconds(fx.clock.now()) - 7200, to: nowSeconds(fx.clock.now()) + 7200,
    })],
    [['I'], Context.PLAYBACK, (fx) => assert.equal(fx.state.overlays.info, true)],
    [['ESC'], Context.PLAYBACK, (fx) => assert.equal(fx.state.overlays.info, false), ['I']],
    [['K'], Context.KEY_DIALOG, (fx) => assert.equal(fx.state.keyDialogTitle, Text.ENTER_KEY)],
    [['M'], Context.PLAYBACK, (fx) => assert.equal(fx.state.muted, true)],
    [['+'], Context.PLAYBACK, (fx) => assert.equal(fx.state.volume, 60)],
    [['-'], Context.PLAYBACK, (fx) => assert.equal(fx.state.volume, 40)],
  ],
  [Context.NUMBER_ENTRY]: [
    [DIGITS, Context.NUMBER_ENTRY, (fx, key) => assert.equal(fx.state.numberBuffer, `1${key}`)],
    [['BACKSPACE'], Context.PLAYBACK, (fx) => assert.equal(fx.state.numberBuffer, '')],
    [['ENTER'], Context.PLAYBACK, (fx) => assert.deepEqual(lastPlay(fx), { service_id: 'svc-1', start: 'live' })],
    [['ESC'], Context.PLAYBACK, (fx) => assert.deepEqual(lastPlay(fx), { service_id: 'svc-2', start: ARCHIVE_START })],
  ],
  [Context.SERVICE_LIST]: [
    [['UP'], Context.SERVICE_LIST, (fx) => assert.equal(fx.state.listIndex, 0)],
    [['DOWN'], Context.SERVICE_LIST, (fx) => assert.equal(fx.state.listIndex, 2)],
    [['ENTER'], Context.PLAYBACK, (fx) => assert.deepEqual(lastPlay(fx), { service_id: 'svc-2', start: 'live' })],
    [['ESC'], Context.PLAYBACK, (fx) => assert.deepEqual(lastPlay(fx), { service_id: 'svc-2', start: ARCHIVE_START })],
  ],
  [Context.EPG]: [
    [['UP'], Context.EPG, (fx) => assert.equal(fx.state.epgService, 'svc-1')],
    [['DOWN'], Context.EPG, (fx) => assert.equal(fx.state.epgService, 'svc-3')],
    [['LEFT'], Context.EPG, (fx) => assert.equal(fx.state.epgEvent, `svc-2-${EVENT_START - 300}`)],
    [['RIGHT'], Context.EPG, (fx) => assert.equal(fx.state.epgEvent, `svc-2-${EVENT_START + 300}`)],
    [['ENTER'], Context.PLAYBACK, (fx) => assert.deepEqual(lastPlay(fx), { service_id: 'svc-2', start: EVENT_START })],
    [['E', 'ESC'], Context.PLAYBACK, (fx) => assert.equal(fx.mwc.callsOf('play').length, 2)],
  ],
  [Context.KEY_DIALOG]: [
    [HEX, Context.KEY_DIALOG, (fx, key) => assert.equal(fx.state.keyBuffer, key)],
    [['BACKSPACE'], Context.KEY_DIALOG, (fx) => assert.equal(fx.state.keyBuffer, ''), ['A']],
    [['ENTER'], Context.PLAYBACK, (fx) => assert.deepEqual(fx.mwc.lastCall().params, { service_id: 'svc-2', key: K3 }), [...K3]],
    [['ESC'], Context.PLAYBACK, (fx) => assert.equal(fx.state.keyDialogCancelled, false)],
  ],
};

function snapshot(fx) {
  return {
    state: JSON.stringify(fx.state),
    calls: fx.mwc.calls.length,
    timers: fx.clock.pendingCount(),
    player: fx.player.log.length,
  };
}

for (const context of Object.values(Context)) {
  test(`TST-402 context ${context}: listed keys act as in Section 4.4.3, other keys change nothing`, async () => {
    const listed = new Set();
    for (const [keys, next, check, before = []] of TABLE[context]) {
      for (const key of keys) {
        listed.add(key);
        const fx = await SETUP[context]();
        assert.equal(fx.state.context, context);
        fx.press(...before);
        fx.press(key);
        await fx.advance(0);
        assert.equal(fx.state.context, next, `${context} ${key}`);
        check(fx, key);
      }
    }
    for (const key of ALL_KEYS.filter((k) => !listed.has(k))) {
      const fx = await SETUP[context]();
      const before = snapshot(fx);
      fx.press(key);
      await fx.advance(0);
      assert.deepEqual(snapshot(fx), before, `${context} ${key} must change nothing`);
    }
  });
}
