// TST-414 (REQ-43) and TST-417 (REQ-20, REQ-21): derived values of Section 4.4.2.
// Expected values from Sections 4.4.2, 3.10 and 1.6.1. Local time zone of the test: UTC.
process.env.TZ = 'UTC';

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  createModel, Text, mode, playbackPosition, distanceFromLive, statusMessage,
} from '../js/model.js';
import { formatHHMMSS, formatDistance } from '../js/views/format.js';
import { T0, createFixture } from './fixture.js';

function session(extra) {
  return {
    number: 1, serviceId: 'svc-1', state: 'playing', reason: null, start: 'live',
    startTime: null, playingSince: null, protected: false, error: null, ...extra,
  };
}

test('TST-414 ARCHIVE position advances from the first picture, distance stays -01:30', () => {
  const t = T0;
  const a = t * 1000;   // APP time of the first picture
  const model = createModel();
  model.update({
    // first session_state playing at a − 1.5 s: playingSince is not used for the position
    session: session({ start: t - 90, startTime: t - 90, playingSince: t - 2 }),
    playerSession: 1,   // the player of session 1 showed its first picture at a
    firstPictureAt: a,
  });
  const expected = [[0, '11:05:10'], [1000, '11:05:11'], [10000, '11:05:20']];
  for (const [advanceMs, hhmmss] of expected) {
    const now = a + advanceMs;
    assert.equal(mode(model.state), 'ARCHIVE');
    assert.equal(playbackPosition(model.state, now), t - 90 + advanceMs / 1000);
    assert.equal(formatHHMMSS(playbackPosition(model.state, now)), hhmmss);
    assert.equal(formatDistance(distanceFromLive(model.state, now)), '−01:30');
  }
});

test('TST-414 before the first picture the position is startTime', () => {
  const model = createModel();
  model.update({ session: session({ start: T0 - 90, startTime: T0 - 90 }), firstPictureAt: null });
  assert.equal(playbackPosition(model.state, (T0 + 5) * 1000), T0 - 90);
});

test('TST-414 a new archive session shows startTime until the first picture of its own player', () => {
  // Seek from a playing session 4 (its player showed the first picture 25 s ago) to T: session 5 is
  // starting while the player of session 4 still plays (REQ-25), so firstPictureAt is not yet the one
  // of the current session (Section 4.4.2) and the position is T, not T + 25.
  const T = T0 - 30;
  const now = (T0 + 25) * 1000;
  const model = createModel();
  model.update({
    session: session({ number: 5, state: 'starting', start: T, startTime: null }),
    playerSession: 4,
    firstPictureAt: T0 * 1000,
  });
  assert.equal(playbackPosition(model.state, now), T);
  assert.equal(formatDistance(distanceFromLive(model.state, now)), '−00:55');
  // The first picture of the player of session 5 at now: from there the position advances.
  model.update({ session: session({ number: 5, start: T, startTime: T }), playerSession: 5, firstPictureAt: now });
  assert.equal(playbackPosition(model.state, now + 3000), T + 3);
});

test('TST-414 LIVE mode shows LIVE and the position is now', () => {
  const model = createModel();
  model.update({ session: session({ start: 'live', startTime: T0 - 10 }), firstPictureAt: T0 * 1000 });
  assert.equal(mode(model.state), 'LIVE');
  assert.equal(playbackPosition(model.state, (T0 + 7) * 1000), T0 + 7);
  assert.equal(distanceFromLive(model.state, (T0 + 7) * 1000), 0);
});

test('TST-414 distance of one hour and more is shown as -H:MM:SS', () => {
  assert.equal(formatDistance(7195), '−1:59:55');
  assert.equal(formatDistance(7200), '−2:00:00');
});

test('TST-414 the position is re-rendered every second until the session ends with a new play', async () => {
  const fx = createFixture();
  await fx.start();
  await fx.answerPlay(1, { playing: true });
  // A position update is a notification that changes no field (Section 4.4.5, re-render).
  let last = JSON.stringify(fx.state);
  let ticks = 0;
  fx.model.subscribe((state) => {
    const now = JSON.stringify(state);
    if (now === last) ticks++;
    last = now;
  });

  await fx.advance(3000);
  assert.equal(ticks, 3, 'one update per second while playing');

  fx.app.session.play('svc-2', 'live');
  last = JSON.stringify(fx.state);
  ticks = 0;
  await fx.advance(10000);
  assert.equal(ticks, 0, 'no position update after the new play');
});

test('TST-414 after the first picture the position is re-rendered when its second changes', async () => {
  const fx = createFixture();
  await fx.start();
  await fx.answerPlay(1, { playing: true });
  const T = T0 - 600;
  fx.app.session.play('svc-1', T);
  await fx.answerPlay(2, { playing: true, extra: { start_time: T } });   // player created at P
  await fx.advance(1300);
  const first = fx.clock.now();                // first picture 1.3 s after P (set by the player view)
  fx.model.update({ firstPictureAt: first });

  let last = JSON.stringify(fx.state);
  const ticks = [];                            // [ms after the first picture, shown position]
  fx.model.subscribe((state) => {
    const now = JSON.stringify(state);
    if (now === last) ticks.push([fx.clock.now() - first, playbackPosition(state, fx.clock.now())]);
    last = now;
  });
  await fx.advance(3500);

  // The tick of the old phase at 0.7 s still shows T; from then on every tick falls on a whole second
  // after the first picture, where the position (Section 4.4.2) has just advanced by 1 s.
  assert.deepEqual(ticks, [[700, T], [1000, T + 1], [2000, T + 2], [3000, T + 3]]);
});

test('TST-417 persistent status message by priority (Section 4.4.2)', () => {
  const svc = [{ service_id: 'svc-1', number: 1, name: 'Demo One', protected: false }];
  const cases = [
    // [state changes, expected message]
    [{ connected: false, services: [], mwReachable: false, session: session({ state: 'no_signal' }) }, Text.NO_SIGNAL],
    [{ connected: true, services: [], mwReachable: false, session: session({ state: 'error', error: { code: 'OUT_OF_ARCHIVE' } }) }, Text.SERVER_NOT_REACHABLE],
    [{ connected: true, services: [], mwReachable: true, session: null }, null],
    [{ connected: true, services: svc, mwReachable: false, session: null }, null],
    [{ connected: true, services: svc, session: session({ state: 'no_signal', reason: 'data_lost' }) }, Text.NO_SIGNAL],
    [{ connected: true, services: svc, session: session({ state: 'error', error: { code: 'OUT_OF_ARCHIVE' } }) }, 'Content not available in archive'],
    [{ connected: true, services: svc, session: session({ state: 'error', error: { code: 'SERVICE_NOT_FOUND' } }) }, 'Invalid service number'],
    [{ connected: true, services: svc, session: session({ state: 'error', error: { code: 'MW_UNREACHABLE' } }) }, 'Server not reachable'],
    [{ connected: true, services: svc, session: session({ state: 'error', error: { code: 'HEADEND_UNREACHABLE' } }) }, 'No signal'],
    [{ connected: true, services: svc, session: session({ state: 'error', error: { code: 'INVALID_STREAM' } }) }, 'No signal'],
    [{ connected: true, services: svc, session: session({ state: 'error', error: { code: 'RECEIVER_ERROR' } }) }, 'No signal'],
    [{ connected: true, services: svc, session: session({ state: 'error', error: { code: 'INVALID_REQUEST' } }) }, 'No signal'],
    [{ connected: true, services: svc, keyDialogCancelled: true, session: session({ state: 'key_required', reason: 'no_key' }) }, 'Protected service – press K to enter the key'],
    [{ connected: true, services: svc, keyDialogCancelled: false, session: session({ state: 'key_required', reason: 'no_key' }) }, null],
    [{ connected: true, services: svc, keyDialogCancelled: true, session: session({ state: 'playing' }) }, null],
    [{ connected: true, services: svc, session: session({ state: 'validating_key' }) }, null],
  ];
  for (const [changes, expected] of cases) {
    const model = createModel();
    model.update(changes);
    assert.equal(statusMessage(model.state), expected, JSON.stringify(changes));
  }
});
