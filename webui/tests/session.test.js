// TST-406 (REQ-25, REQ-26), TST-407 (REQ-03, REQ-04), TST-408 (REQ-57, adopting a session;
// the reconnect timing of APP-MWC is tested with mwc.js), TST-416 (ER-07, ER-32).
// Expected values from Sections 3.6, 3.7, 4.4.2 and 4.4.4.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Context, Text, statusMessage } from '../js/model.js';
import { createFixture, sessionState, SERVICES, T0 } from './fixture.js';

test('TST-406 only the response to the last play sets the session; events of other sessions are ignored', async () => {
  const fx = createFixture();
  await fx.start();
  await fx.answerPlay(10, { playing: true });                    // existing player of session 10
  fx.player.log.length = 0;

  fx.app.session.play('svc-1', 'live');                          // play A
  const idA = fx.state.lastPlayId;
  fx.app.session.play('svc-2', 'live');                          // play B
  const idB = fx.state.lastPlayId;
  assert.notEqual(idA, idB);

  fx.mwc.respond(idA, { session: 11 });                          // step 1: response of A, then B
  await fx.advance(0);
  assert.equal(fx.state.session, null);
  fx.mwc.respond(idB, { session: 12 });
  await fx.advance(0);
  assert.equal(fx.state.session.number, 12);
  assert.equal(fx.state.session.serviceId, 'svc-2');

  await fx.emit('session_state', sessionState(11, 'svc-1', 'playing'));   // step 2
  assert.equal(fx.state.session.state, 'starting');
  assert.deepEqual(fx.player.log, []);

  await fx.emit('session_state', sessionState(12, 'svc-2', 'playing'));   // step 3
  assert.deepEqual(fx.player.log.slice(0, 2), [
    ['destroy'],
    ['create', '/hls/12/index.m3u8', { startPosition: 0 }],
  ]);
  assert.equal(fx.state.playerSession, 12);

  fx.player.log.length = 0;                                      // step 4
  await fx.emit('session_state', sessionState(12, 'svc-2', 'no_signal', { reason: 'data_lost' }));
  await fx.emit('session_state', sessionState(12, 'svc-2', 'playing'));
  assert.equal(fx.player.count('create') + fx.player.count('destroy'), 0, 'player kept');

  for (const state of ['key_required', 'error', 'stopped']) {      // step 5
    await fx.emit('session_state', sessionState(12, 'svc-2', 'playing'));
    fx.player.log.length = 0;
    const extra = state === 'key_required' ? { reason: 'no_key', protected: true } : {};
    await fx.emit('session_state', sessionState(12, 'svc-2', state, extra));
    assert.deepEqual(fx.player.log, [['destroy']], state);
    assert.equal(fx.state.playerSession, null, state);
    if (state === 'key_required') fx.press('ESC');
  }
});

test('TST-407 Server not reachable while the list is missing; keys are processed; play of number 1 when the list arrives', async () => {
  const fx = createFixture();
  fx.mwc.setConnected(true);                                     // step 1
  await fx.advance(0);
  await fx.respond('get_status', { state: 'idle' });
  await fx.fail('get_services', 'MW_UNREACHABLE');
  await fx.emit('mw_status', { reachable: false });
  assert.equal(statusMessage(fx.state), Text.SERVER_NOT_REACHABLE);

  fx.press('UP');                                                // step 2
  assert.equal(fx.state.overlays.banner, true);
  fx.press('E');
  assert.equal(fx.state.context, Context.EPG);

  const before = fx.mwc.callsOf('get_services').length;          // step 3
  await fx.emit('mw_status', { reachable: true });
  assert.equal(fx.mwc.callsOf('get_services').length, before + 1);
  await fx.respond('get_services', { services: SERVICES });
  assert.deepEqual(fx.mwc.callsOf('play').map((c) => c.params), [{ service_id: 'svc-1', start: 'live' }]);
  assert.equal(statusMessage(fx.state), null);
});

// Session 5 of svc-2 playing with its player (TST-408 precondition).
async function session5() {
  const fx = createFixture();
  await fx.start();
  await fx.answerPlay(4, { playing: true });
  fx.app.session.play('svc-2', 'live');
  await fx.answerPlay(5, { playing: true, extra: { start_time: T0, playing_since: T0 } });
  return fx;
}

test('TST-408 connection loss rejects pending calls and shows No signal; the running session is adopted with its player', async () => {
  const fx = await session5();
  fx.press('E');                                                 // a pending get_epg
  const pending = fx.mwc.pendingCall('get_epg');
  fx.mwc.setConnected(false);                                    // step 1
  await fx.advance(0);
  assert.equal(fx.state.connected, false);
  assert.equal(fx.mwc.pending.has(pending.id), false);
  assert.equal(statusMessage(fx.state), Text.NO_SIGNAL);
  fx.press('ESC');

  const plays = fx.mwc.callsOf('play').length;                   // step 2
  fx.player.log.length = 0;
  fx.mwc.setConnected(true);
  await fx.advance(0);
  assert.ok(fx.mwc.pendingCall('get_status'));
  assert.ok(fx.mwc.pendingCall('get_services'));
  await fx.respond('get_status', sessionState(5, 'svc-2', 'playing', { start_time: T0, playing_since: T0 }));
  assert.equal(fx.state.session.number, 5);
  assert.equal(fx.state.playerSession, 5);
  assert.deepEqual(fx.player.log, [], 'player kept');
  assert.equal(fx.mwc.callsOf('play').length, plays, 'no play');
});

test('TST-408 after a page reload the running session is requested again from its position', async () => {
  const t = T0;
  // step 3: live session
  let fx = createFixture({ nowS: t });
  await fx.start({ status: sessionState(5, 'svc-2', 'playing', { start: 'live', start_time: t - 100, playing_since: t - 100 }) });
  assert.deepEqual(fx.mwc.callsOf('play').map((c) => c.params), [{ service_id: 'svc-2', start: 'live' }]);

  // step 4: ARCHIVE session, start = start_time = t − 600, playing since t − 100
  fx = createFixture({ nowS: t });
  await fx.start({ status: sessionState(5, 'svc-2', 'playing', { start: t - 600, start_time: t - 600, playing_since: t - 100 }) });
  assert.deepEqual(fx.mwc.callsOf('play').map((c) => c.params), [{ service_id: 'svc-2', start: t - 500 }]);

  // step 5: key_required of session 6
  fx = createFixture({ nowS: t });
  await fx.start({ status: sessionState(6, 'svc-3', 'key_required', { reason: 'no_key', protected: true }) });
  assert.equal(fx.state.context, Context.KEY_DIALOG);
  assert.equal(fx.state.keyDialogTitle, Text.ENTER_KEY);
  assert.equal(fx.mwc.callsOf('play').length, 0);

  // step 6: idle
  fx = createFixture({ nowS: t });
  await fx.start({ status: { state: 'idle' } });
  assert.deepEqual(fx.mwc.callsOf('play').map((c) => c.params), [{ service_id: 'svc-1', start: 'live' }]);
});

test('TST-416 rejected play: toast, get_status and the returned session adopted with its player', async () => {
  const fx = createFixture();
  await fx.start();
  await fx.answerPlay(4, { playing: true, extra: { start_time: T0, playing_since: T0 } });
  fx.player.log.length = 0;
  const plays = fx.mwc.callsOf('play').length;

  fx.app.session.play('svc-2', 'live');                          // step 1
  await fx.fail('play', 'SERVICE_NOT_FOUND');
  assert.equal(fx.state.toast, Text.INVALID_SERVICE_NUMBER);
  await fx.respond('get_status', sessionState(4, 'svc-1', 'playing', { start_time: T0, playing_since: T0 }));
  assert.equal(fx.state.session.number, 4);
  assert.equal(fx.state.currentServiceId, 'svc-1');
  assert.deepEqual(fx.player.log, [], 'player kept');
  assert.equal(fx.mwc.callsOf('play').length, plays + 1, 'no further play');
  await fx.advance(3000);
  assert.equal(fx.state.toast, null);
});

test('TST-416 set_key answered INVALID_KEY_FORMAT reopens the key dialog', async () => {
  const fx = createFixture();
  await fx.start();
  await fx.answerPlay(4, { playing: true });
  fx.press('K', ...'3A7F1C9E5B2D8046A1E3C5079B4D6F28', 'ENTER');
  assert.equal(fx.state.context, Context.PLAYBACK);
  await fx.fail('set_key', 'INVALID_KEY_FORMAT');
  assert.equal(fx.state.context, Context.KEY_DIALOG);
  assert.equal(fx.state.keyDialogTitle, Text.ENTER_KEY);
  assert.equal(fx.state.keyBuffer, '');
});
