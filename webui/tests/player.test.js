// TST-418 (REQ-25, ER-35), TST-419 (Section 5.1.8), the player parts of TST-414 (REQ-43,
// first picture) and TST-410 (REQ-31, volume on the <video> element): the real player view with
// fakes of hls.js and the <video> element. Expected values from Sections 3.7, 4.4.2, 4.4.4,
// 4.8 (ER-35) and 5.1.8.
import { test, beforeEach } from 'node:test';
import assert from 'node:assert/strict';
import { Text, playbackPosition, statusMessage } from '../js/model.js';
import { createKeyRouter } from '../js/keys.js';
import { createPlayerView } from '../js/views/player.js';
import { createDiag, createKeyMarker, diagEnabled } from '../js/views/diag.js';
import { FakeHls, FakeVideo } from './fakes.js';
import { createFixture, sessionState, T0 } from './fixture.js';

beforeEach(() => FakeHls.reset());

// Fixture with the real player view; `lines` receives the DIAG lines.
function withPlayer({ search = '', lines = [] } = {}) {
  const video = new FakeVideo();
  let diag = null;
  const fx = createFixture({
    createPlayer: (model, clock) => {
      diag = createDiag({ enabled: diagEnabled(search), clock, log: (text) => lines.push(text) });
      return createPlayerView({ video, Hls: FakeHls, model, clock, diag });
    },
  });
  return { fx, video, lines, diag: () => diag };
}

// svc-1 started and `playing` as session `n`.
async function playing(fx, n = 1, extra = {}) {
  await fx.start();
  await fx.answerPlay(n, { playing: true, extra });
}

test('TST-418 the player is created on /hls/<n>/index.m3u8 with startPosition 0 and plays on MANIFEST_PARSED', async () => {
  const { fx, video } = withPlayer();
  await playing(fx, 3);
  const hls = FakeHls.last();
  assert.deepEqual(hls.config, { startPosition: 0 });
  assert.equal(hls.media, video);
  assert.equal(hls.source, '/hls/3/index.m3u8');
  assert.equal(video.plays, 0);
  hls.emit(FakeHls.Events.MANIFEST_PARSED);
  assert.equal(video.plays, 1);
});

test('TST-418 fatal errors: recoverMediaError once, then re-created once, the third destroys it (No signal until the next play)', async () => {
  const { fx } = withPlayer();
  await playing(fx, 3);
  const first = FakeHls.last();

  first.emit(FakeHls.Events.ERROR, { fatal: false, type: FakeHls.ErrorTypes.NETWORK_ERROR });
  assert.equal(first.recoveries, 0, 'non-fatal errors are left to hls.js');

  first.emit(FakeHls.Events.ERROR, { fatal: true, type: FakeHls.ErrorTypes.MEDIA_ERROR });   // step 1
  assert.equal(first.recoveries, 1);
  assert.equal(FakeHls.instances.length, 1);

  first.emit(FakeHls.Events.ERROR, { fatal: true, type: FakeHls.ErrorTypes.NETWORK_ERROR }); // step 2
  assert.equal(first.destroyed, true);
  assert.equal(FakeHls.instances.length, 2);
  const second = FakeHls.last();
  assert.equal(second.source, '/hls/3/index.m3u8');
  assert.deepEqual(second.config, { startPosition: 0 });
  assert.equal(statusMessage(fx.state), null);

  second.emit(FakeHls.Events.ERROR, { fatal: true, type: FakeHls.ErrorTypes.MEDIA_ERROR });  // step 3
  assert.equal(second.destroyed, true);
  assert.equal(second.recoveries, 0);
  assert.equal(FakeHls.instances.length, 2, 'not created again');
  assert.equal(statusMessage(fx.state), Text.NO_SIGNAL);

  await fx.emit('session_state', sessionState(3, 'svc-1', 'no_signal', { reason: 'data_lost' }));
  await fx.emit('session_state', sessionState(3, 'svc-1', 'playing'));
  assert.equal(FakeHls.instances.length, 2, 'the same session does not create a player again');
  assert.equal(statusMessage(fx.state), Text.NO_SIGNAL);

  fx.press('2', 'ENTER');                                        // the next play
  assert.equal(statusMessage(fx.state), null);
  await fx.answerPlay(4, { playing: true });
  assert.equal(FakeHls.instances.length, 3);
  assert.equal(FakeHls.last().source, '/hls/4/index.m3u8');
});

test('TST-418 errors of the player of a superseded session are ignored', async () => {
  const { fx } = withPlayer();
  await playing(fx, 3);
  const old = FakeHls.last();
  fx.press('2', 'ENTER');                                        // new play: session 3 superseded
  for (let i = 0; i < 3; i++) old.emit(FakeHls.Events.ERROR, { fatal: true, type: FakeHls.ErrorTypes.MEDIA_ERROR });
  assert.equal(old.recoveries, 0);
  assert.equal(old.destroyed, false);
  assert.equal(FakeHls.instances.length, 1);
  assert.equal(fx.state.playerFailed, false);
});

test('TST-418 without hls.js support the player shows No signal', async () => {
  FakeHls.supported = false;
  const { fx } = withPlayer();
  await playing(fx, 3);
  assert.equal(FakeHls.instances.length, 0);
  assert.equal(statusMessage(fx.state), Text.NO_SIGNAL);
});

test('TST-414 firstPictureAt is set at the first playing event of the <video> element of a new player', async () => {
  const { fx, video } = withPlayer();
  const T = T0 - 90;
  await fx.start();
  fx.app.session.play('svc-1', T);
  await fx.answerPlay(2);
  await fx.emit('session_state', sessionState(2, 'svc-1', 'playing', { start: T, start_time: T, playing_since: T0 }));
  assert.equal(fx.state.firstPictureAt, null);

  await fx.advance(1500);                                       // the picture appears 1.5 s later
  const a = fx.clock.now();
  video.dispatch('playing');
  assert.equal(fx.state.firstPictureAt, a);
  assert.equal(playbackPosition(fx.state, a), T);

  await fx.advance(10000);
  video.dispatch('waiting');                                     // an interruption does not reset it
  video.dispatch('playing');
  assert.equal(fx.state.firstPictureAt, a);
  assert.equal(playbackPosition(fx.state, fx.clock.now()), T + 10);

  fx.app.session.play('svc-1', T - 300);                          // a new player resets it
  await fx.answerPlay(3);
  await fx.emit('session_state', sessionState(3, 'svc-1', 'playing', { start: T - 300, start_time: T - 300 }));
  assert.equal(fx.state.firstPictureAt, null);
  video.dispatch('playing');
  assert.equal(fx.state.firstPictureAt, fx.clock.now());
});

test('TST-410 volume and mute are applied to the <video> element and kept for a new player', async () => {
  const { fx, video } = withPlayer();
  await playing(fx, 1);
  fx.press('-', '-', 'M');
  assert.equal(video.volume, 0.3);
  assert.equal(video.muted, true);
  fx.press('2', 'ENTER');
  await fx.answerPlay(2, { playing: true });
  assert.equal(FakeHls.instances.length, 2);
  assert.equal(video.volume, 0.3);
  assert.equal(video.muted, true);
});

// Key router and DIAG marks as connected by main.js; `frames` collects requestAnimationFrame callbacks.
function routerWithDiag(fx, diag) {
  const frames = [];
  const router = createKeyRouter(fx.model, fx.app.byContext, createKeyMarker(diag, (cb) => frames.push(cb), fx.model));
  const key = (k) => router({ key: k, repeat: false, preventDefault() {} });
  const nextFrame = () => frames.splice(0).forEach((cb) => cb());
  return { key, nextFrame };
}

test('TST-419 without ?diag=1 no DIAG output', async () => {
  const { fx, video, lines, diag } = withPlayer({ search: '' });
  await fx.start();
  const { key, nextFrame } = routerWithDiag(fx, diag());
  key('ArrowUp');
  nextFrame();
  await fx.answerPlay(1, { playing: true });
  video.presentFrame();
  FakeHls.last().emit(FakeHls.Events.FRAG_CHANGED, { frag: { sn: 0 } });
  video.dispatch('waiting');
  video.dispatch('playing');
  assert.deepEqual(lines, []);
});

test('TST-419 with ?diag=1: DIAG <epoch_ms> key, osd, frame in this order; frag, wait, resume', async () => {
  const { fx, video, lines, diag } = withPlayer({ search: '?diag=1' });
  await fx.start();
  await fx.answerPlay(1);
  const { key, nextFrame } = routerWithDiag(fx, diag());

  key('ArrowUp');                                               // key
  nextFrame();                                                   // render → osd
  await fx.advance(300);
  await fx.answerPlay(2, { playing: true });                     // new player
  video.presentFrame();                                          // first frame
  FakeHls.last().emit(FakeHls.Events.FRAG_CHANGED, { frag: { sn: 0 } });
  video.dispatch('waiting');
  video.dispatch('playing');

  const marks = lines.map((l) => l.split(' ').slice(2).join(' '));
  assert.deepEqual(marks, ['key UP', 'osd UP', 'frame /hls/2/index.m3u8', 'frag 0', 'wait', 'resume']);
  for (const l of lines) assert.match(l, /^DIAG \d{13} \S+/);
  const times = lines.map((l) => Number(l.split(' ')[1]));
  assert.equal(times[0], T0 * 1000);
});

test('TST-419 with ?diag=1 no character of a typed key is written (NFR-08, REQ-55)', async () => {
  const K3 = '3A7F1C9E5B2D8046A1E3C5079B4D6F28';
  const { fx, lines, diag } = withPlayer({ search: '?diag=1' });
  await playing(fx, 1);
  const { key, nextFrame } = routerWithDiag(fx, diag());
  lines.length = 0;

  key('k');                                                     // K in PLAYBACK opens the dialog
  for (const c of K3.toLowerCase()) key(c);                     // typed in lower case, mapped to upper case
  key('Backspace');
  key('8');
  key('Enter');
  nextFrame();
  assert.equal(fx.mwc.lastCall().params.key, K3, 'the key itself was entered');

  const details = lines.map((l) => l.split(' ').slice(3).join(' '));
  assert.equal(details.filter((d) => d === '•').length, 2 * 33, 'key and osd mark of every key character');
  assert.deepEqual([...new Set(details)].sort(), ['BACKSPACE', 'ENTER', 'K', '•'].sort());
  const log = lines.join('\n');
  for (const pattern of [K3, K3.toLowerCase(), K3.slice(0, 5)]) assert.equal(log.includes(pattern), false, pattern);

  lines.length = 0;                                             // outside the dialog digits keep their name
  key('2');
  nextFrame();
  assert.deepEqual(lines.map((l) => l.split(' ').slice(2).join(' ')), ['key 2', 'osd 2']);
});
