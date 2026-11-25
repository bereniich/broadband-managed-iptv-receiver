// TST-409 (REQ-27, REQ-28) and TST-410 (REQ-29 – REQ-31). Expected values from Sections 2.3.5,
// 2.3.6, 4.4.2, 4.4.5 and the worked example of Section 5.2.4. Local time zone of the test: UTC.
process.env.TZ = 'UTC';

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  Text, currentEvent, mode, playbackPosition, protectionStatus, serviceById,
} from '../js/model.js';
import { formatHHMM, formatHHMMSS } from '../js/views/format.js';
import { createFixture, sessionState, T0 } from './fixture.js';

// Content of the info dialog as the view shows it (REQ-27).
function infoContent(fx) {
  const state = fx.state;
  const now = fx.clock.now();
  const service = serviceById(state, state.currentServiceId);
  const event = currentEvent(state, now);
  return {
    service: `${service.number} ${service.name}`,
    event: event ? `${event.title} ${formatHHMM(event.start)} – ${formatHHMM(event.start + event.duration)}` : null,
    mode: mode(state) === 'LIVE' ? Text.LIVE : `${Text.ARCHIVE} ${formatHHMMSS(playbackPosition(state, now))}`,
    protection: protectionStatus(state),
  };
}

test('TST-409 info dialog content, 5 s timer not restarted by updates, I and ESC', async () => {
  const fx = createFixture({ nowS: T0 });
  await fx.start();
  await fx.answerPlay(1, { playing: true });

  fx.press('3', 'ENTER');                                        // step 1: play svc-3 "live"
  assert.equal(fx.state.overlays.info, true);
  assert.deepEqual(infoContent(fx), {
    service: '3 Premium One',
    event: 'Premium Evening 11:05 – 11:10',
    mode: 'LIVE',
    protection: null,
  });
  await fx.answerPlay(2);

  await fx.advance(3000);                                        // step 2: events at 3 s
  await fx.emit('stream_info', {
    session: 2, service_id: 'svc-3', program_number: 1, pmt_pid: 4096, pcr_pid: 256,
    video: { pid: 256, stream_type: 27, codec: 'H.264' }, audio: { pid: 257, stream_type: 15, codec: 'AAC' },
  });
  await fx.emit('session_state', sessionState(2, 'svc-3', 'playing', { start_time: T0, playing_since: T0 + 3, protected: true }));
  assert.equal(fx.state.streamInfo.pmt_pid, 4096);
  assert.equal(infoContent(fx).protection, Text.KEY_OK);
  await fx.advance(1999);
  assert.equal(fx.state.overlays.info, true);
  await fx.advance(1);                                           // step 3: 5 s after step 1
  assert.equal(fx.state.overlays.info, false);

  fx.press('I');                                                 // step 4
  assert.equal(fx.state.overlays.info, true);
  fx.press('ESC');
  assert.equal(fx.state.overlays.info, false);

  const T = 1794999600;                                          // step 5: archive session
  fx.app.session.play('svc-3', T);
  await fx.answerPlay(3);
  await fx.emit('session_state', sessionState(3, 'svc-3', 'playing', {
    start: T, start_time: T, playing_since: T0, protected: true,
  }));
  assert.deepEqual(infoContent(fx), {
    service: '3 Premium One',
    event: 'Premium Morning 11:00 – 11:05',
    mode: 'ARCHIVE 11:00:00',
    protection: Text.KEY_OK,
  });
});

test('TST-410 volume steps, clamping, indicator for 2 s, mute and unmute, kept for a new player', async () => {
  const fx = createFixture();
  await fx.start();
  await fx.answerPlay(1, { playing: true });
  assert.equal(fx.state.volume, 50);

  for (let i = 0; i < 6; i++) fx.press('+');                     // step 1
  assert.equal(fx.state.volume, 100);
  assert.equal(fx.state.overlays.volume, true);
  await fx.advance(1999);
  assert.equal(fx.state.overlays.volume, true);
  await fx.advance(1);
  assert.equal(fx.state.overlays.volume, false);

  for (let i = 0; i < 20; i++) fx.press('-');                    // step 2
  assert.equal(fx.state.volume, 0);
  assert.deepEqual(fx.player.volume, { volume: 0, muted: false });

  fx.press('+', '+', '+', 'M');                                  // step 3
  assert.equal(fx.state.muted, true);
  assert.deepEqual(fx.player.volume, { volume: 30, muted: true });
  fx.press('+');
  assert.equal(fx.state.muted, false);
  assert.equal(fx.state.volume, 40);

  fx.press('M');                                                 // step 4
  await fx.advance(2000);
  assert.equal(fx.state.overlays.volume, false);
  assert.equal(fx.state.muted, true, 'the mute indicator stays while muted');

  fx.press('2', 'ENTER');                                        // step 5: play and a new player
  await fx.answerPlay(2, { playing: true });
  const created = fx.player.log.findLastIndex((e) => e[0] === 'create');
  assert.deepEqual(fx.player.log.slice(created + 1), [['volume', 40, true]]);
  assert.equal(fx.state.volume, 40);
  assert.equal(fx.state.muted, true);
});
