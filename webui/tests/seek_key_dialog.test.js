// TST-413 (REQ-39, REQ-40) and TST-415 (REQ-46 – REQ-48). Expected values from Sections 2.3.8,
// 2.3.9, 4.4.3, 4.4.5 and the keys of Section 5.2.2.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Context, Text, nowSeconds, statusMessage } from '../js/model.js';
import { createFixture, sessionState, T0 } from './fixture.js';

const K3 = '3A7F1C9E5B2D8046A1E3C5079B4D6F28';

function plays(fx) {
  return fx.mwc.callsOf('play').map((c) => c.params);
}

// Answers the pending play as session `n` and sends playing; in ARCHIVE the first picture is now.
async function playingAt(fx, n) {
  const { params } = fx.mwc.pendingCall('play');
  await fx.answerPlay(n);
  await fx.emit('session_state', sessionState(n, params.service_id, 'playing', {
    start: params.start, start_time: params.start === 'live' ? nowSeconds(fx.clock.now()) : params.start,
  }));
  fx.model.update({ firstPictureAt: fx.clock.now() });           // `playing` event of the <video> element
}

test('TST-413 seek steps accumulate into one play; forward to live; B only in ARCHIVE', async () => {
  const t = T0;
  const fx = createFixture({ nowS: t });
  await fx.start();
  await playingAt(fx, 1);                                        // LIVE svc-1
  let n = plays(fx).length;

  fx.press('LEFT');                                              // step 1
  await fx.advance(100);
  fx.press('LEFT');
  assert.equal(fx.state.seekTarget, t - 60);
  await fx.advance(499);
  assert.equal(plays(fx).length, n);
  await fx.advance(1);
  assert.deepEqual(plays(fx).slice(n), [{ service_id: 'svc-1', start: t - 60 }]);
  n = plays(fx).length;

  await playingAt(fx, 2);                                        // step 2: ARCHIVE at t − 60
  fx.press('RIGHT', 'RIGHT', 'RIGHT');
  await fx.advance(500);
  assert.deepEqual(plays(fx).slice(n), [{ service_id: 'svc-1', start: 'live' }]);
  n = plays(fx).length;

  await playingAt(fx, 3);                                        // step 3: LIVE, →
  fx.press('RIGHT');
  assert.equal(fx.state.seekTarget, null);
  await fx.advance(1000);
  assert.equal(plays(fx).length, n);

  fx.press('LEFT', 'RIGHT');                                     // step 4
  await fx.advance(500);
  assert.deepEqual(plays(fx).slice(n), [{ service_id: 'svc-1', start: 'live' }]);
  await playingAt(fx, 4);
  n = plays(fx).length;

  fx.app.session.play('svc-1', nowSeconds(fx.clock.now()) - 600);   // step 5: ARCHIVE, B
  await playingAt(fx, 5);
  n = plays(fx).length;
  fx.press('B');
  assert.deepEqual(plays(fx).slice(n), [{ service_id: 'svc-1', start: 'live' }]);
  n = plays(fx).length;

  await playingAt(fx, 6);                                        // step 6: LIVE, B
  fx.press('B');
  await fx.advance(1000);
  assert.equal(plays(fx).length, n);
});

test('TST-415 key dialog: automatic opening, hexadecimal input, ENTER only with 32, rejected key, ESC, K', async () => {
  const fx = createFixture();
  await fx.start();
  await fx.answerPlay(6, { playing: true });
  fx.app.session.play('svc-3', 'live');
  await fx.answerPlay(7);
  fx.press('UP', 'LEFT', 'E');                                   // zap and seek running, context EPG
  assert.equal(fx.state.context, Context.EPG);
  const n = plays(fx).length;

  await fx.emit('session_state', sessionState(7, 'svc-3', 'key_required', { reason: 'no_key', protected: true }));
  assert.equal(fx.state.context, Context.KEY_DIALOG);            // step 1
  assert.equal(fx.state.keyDialogTitle, Text.ENTER_KEY);
  assert.equal(fx.state.seekTarget, null);
  assert.equal(fx.state.zapTarget, null);
  await fx.advance(1000);
  assert.equal(plays(fx).length, n, 'zap and seek timers cancelled');

  fx.press(...K3.slice(0, 7), 'G', 'L', ...K3.slice(7), '5');    // step 2: K3 contains E and B
  assert.equal(fx.state.keyBuffer, K3);
  assert.equal(fx.state.keyBuffer.length, 32);
  assert.equal(fx.state.context, Context.KEY_DIALOG, 'E and B are key characters, no shortcut');
  assert.equal(plays(fx).length, n);

  fx.press('BACKSPACE', 'ENTER');                                // step 3
  assert.equal(fx.state.keyBuffer.length, 31);
  assert.equal(fx.state.context, Context.KEY_DIALOG);
  assert.equal(fx.mwc.callsOf('set_key').length, 0);
  fx.press('8', 'ENTER');
  assert.deepEqual(fx.mwc.callsOf('set_key').map((c) => c.params), [{ service_id: 'svc-3', key: K3 }]);
  assert.equal(fx.state.context, Context.PLAYBACK);
  assert.equal(fx.state.keyBuffer, '');
  await fx.respond('set_key', {});

  await fx.emit('session_state', sessionState(7, 'svc-3', 'starting'));   // step 4
  await fx.emit('session_state', sessionState(7, 'svc-3', 'key_required', { reason: 'key_rejected', protected: true }));
  assert.equal(fx.state.context, Context.KEY_DIALOG);
  assert.equal(fx.state.keyDialogTitle, Text.INCORRECT_KEY);
  fx.press('ESC');
  assert.equal(fx.state.context, Context.PLAYBACK);
  assert.equal(statusMessage(fx.state), Text.PRESS_K);

  fx.press('K');                                                 // step 5
  assert.equal(fx.state.context, Context.KEY_DIALOG);
  assert.equal(fx.state.keyDialogTitle, Text.ENTER_KEY);
});
