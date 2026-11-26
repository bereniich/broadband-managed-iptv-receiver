// APP-PRS playback presenter: keys of the context PLAYBACK (Section 4.4.3): zap with a 300 ms
// debounce (REQ-06, REQ-07), seek with a 500 ms accumulation (REQ-39), return to live (REQ-40),
// info dialog (REQ-28), volume and mute (REQ-29 – REQ-31).
import { Context, Text, mode, nowSeconds, playbackPosition } from '../model.js';

export const ZAP_MS = 300;
export const SEEK_MS = 500;
export const SEEK_STEP_S = 30;   // D-07
export const VOLUME_STEP = 10;

export function createPlaybackPresenter(app) {
  const { model, clock } = app;
  let zapTimer = null;
  let seekTimer = null;

  function cancelZap() {
    if (zapTimer !== null) clock.clearTimeout(zapTimer);
    zapTimer = null;
    model.update({ zapTarget: null, overlays: { ...model.state.overlays, banner: false } });
  }

  function cancelSeek() {
    if (seekTimer !== null) clock.clearTimeout(seekTimer);
    seekTimer = null;
    model.update({ seekTarget: null });
  }

  // Zap, number commit, a new play and key_required cancel the pending zap and seek.
  function cancelTimers() {
    cancelZap();
    cancelSeek();
  }

  // ------------------------------------------------------------ zap
  function zap(direction) {
    cancelSeek();
    const { services, zapTarget, currentServiceId } = model.state;
    let target = null;
    if (services.length > 0) {
      const from = services.findIndex((s) => s.service_id === (zapTarget ?? currentServiceId));
      const index = from < 0
        ? (direction > 0 ? 0 : services.length - 1)
        : (from + direction + services.length) % services.length;
      target = services[index].service_id;
    }
    model.update({ zapTarget: target, overlays: { ...model.state.overlays, banner: true } });
    if (zapTimer !== null) clock.clearTimeout(zapTimer);
    zapTimer = clock.setTimeout(onZapTimer, ZAP_MS);
  }

  function onZapTimer() {
    zapTimer = null;
    const target = model.state.zapTarget;
    model.update({ zapTarget: null, overlays: { ...model.state.overlays, banner: false } });
    if (target !== null) app.session.play(target, 'live');
  }

  // ------------------------------------------------------------ seek
  function seek(delta) {
    if (model.state.currentServiceId === null) return;
    const { seekTarget } = model.state;
    const base = seekTarget ?? playbackPosition(model.state, clock.now());
    model.update({ seekTarget: base + delta });
    if (seekTimer !== null) clock.clearTimeout(seekTimer);
    seekTimer = clock.setTimeout(onSeekTimer, SEEK_MS);
  }

  function onSeekTimer() {
    seekTimer = null;
    const target = model.state.seekTarget;
    model.update({ seekTarget: null });
    const startPoint = target >= nowSeconds(clock.now()) ? 'live' : target;
    app.session.play(model.state.currentServiceId, startPoint);
  }

  // ------------------------------------------------------------ volume and mute
  function changeVolume(delta) {
    const volume = Math.min(100, Math.max(0, model.state.volume + delta));
    model.update({ volume, muted: false });
    applyVolume();
  }

  function toggleMute() {
    model.update({ muted: !model.state.muted });
    applyVolume();
  }

  function applyVolume() {
    app.player.setVolume(model.state.volume, model.state.muted);
    app.overlays.showVolume();
  }

  // ------------------------------------------------------------ keys
  function onKey(key) {
    switch (key) {
      case 'UP': zap(+1); break;
      case 'DOWN': zap(-1); break;
      case 'LEFT': seek(-SEEK_STEP_S); break;
      case 'RIGHT':
        if (mode(model.state) === 'ARCHIVE' || model.state.seekTarget !== null) seek(+SEEK_STEP_S);
        break;
      case 'B':
        if (mode(model.state) === 'ARCHIVE') app.session.play(model.state.currentServiceId, 'live');
        break;
      case 'ENTER':
      case 'L':
        cancelZap();
        app.serviceList.open();
        break;
      case 'E':
        cancelZap();
        app.epg.open();
        break;
      case 'I': app.overlays.showInfo(); break;
      case 'ESC': app.overlays.hideInfo(); break;
      case 'K':
        if (model.state.currentServiceId === null) break;
        cancelZap();
        app.keyDialog.open(Text.ENTER_KEY);
        break;
      case 'M': toggleMute(); break;
      case '+': changeVolume(+VOLUME_STEP); break;
      case '-': changeVolume(-VOLUME_STEP); break;
      default:
        if (key >= '0' && key <= '9' && key.length === 1) {
          cancelZap();
          app.numberEntry.begin(key);
        }
        break;
    }
  }

  return { context: Context.PLAYBACK, onKey, cancelTimers };
}
