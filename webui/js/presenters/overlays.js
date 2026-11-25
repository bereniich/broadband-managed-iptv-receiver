// Overlay timers shared by the presenters (Section 4.4.5): info dialog 5 s, volume indicator 2 s,
// toast 3 s. Overlays are not contexts and never take keys (Section 4.4.3).

export const INFO_MS = 5000;
export const VOLUME_MS = 2000;
export const TOAST_MS = 3000;

export function createOverlays(app) {
  const { model, clock } = app;
  const timers = { info: null, volume: null, toast: null };

  function setOverlay(name, visible) {
    model.update({ overlays: { ...model.state.overlays, [name]: visible } });
  }

  function restart(name, delayMs, onExpiry) {
    if (timers[name] !== null) clock.clearTimeout(timers[name]);
    timers[name] = clock.setTimeout(() => {
      timers[name] = null;
      onExpiry();
    }, delayMs);
  }

  function cancel(name) {
    if (timers[name] !== null) clock.clearTimeout(timers[name]);
    timers[name] = null;
  }

  return {
    // Every display restarts the 5 s timer; a content update (REQ-28) does not call this.
    showInfo() {
      setOverlay('info', true);
      restart('info', INFO_MS, () => setOverlay('info', false));
    },

    hideInfo() {
      cancel('info');
      setOverlay('info', false);
    },

    // Volume level for 2 s; the mute indicator follows `muted` and is not timed (REQ-30).
    showVolume() {
      setOverlay('volume', true);
      restart('volume', VOLUME_MS, () => setOverlay('volume', false));
    },

    // A newer toast replaces the shown one and restarts the 3 s timer.
    showToast(text) {
      model.update({ toast: text });
      restart('toast', TOAST_MS, () => model.update({ toast: null }));
    },
  };
}
