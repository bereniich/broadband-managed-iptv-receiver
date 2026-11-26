// APP-PRS number-entry presenter: context NUMBER_ENTRY (Section 4.4.3; REQ-08, REQ-09).
import { Context, Text } from '../model.js';

export const COMMIT_MS = 2000;
export const MAX_DIGITS = 3;

export function createNumberEntryPresenter(app) {
  const { model, clock } = app;
  let commitTimer = null;

  function restartTimer() {
    if (commitTimer !== null) clock.clearTimeout(commitTimer);
    commitTimer = clock.setTimeout(() => {
      commitTimer = null;
      commit();
    }, COMMIT_MS);
  }

  function end() {
    if (commitTimer !== null) clock.clearTimeout(commitTimer);
    commitTimer = null;
    model.update({ numberBuffer: '', context: Context.PLAYBACK });
  }

  // Started by the first digit in PLAYBACK.
  function begin(digit) {
    model.update({ numberBuffer: digit, context: Context.NUMBER_ENTRY });
    restartTimer();
  }

  function commit() {
    const number = Number(model.state.numberBuffer);
    end();
    app.playback.cancelTimers();
    const service = model.state.services.find((s) => s.number === number);
    if (service) app.session.play(service.service_id, 'live');
    else app.overlays.showToast(Text.INVALID_SERVICE_NUMBER);
  }

  // Ends the entry without a change (key_required, Section 4.4.3 last row).
  function cancel() {
    if (model.state.context === Context.NUMBER_ENTRY) end();
  }

  function onKey(key) {
    if (key.length === 1 && key >= '0' && key <= '9') {
      if (model.state.numberBuffer.length < MAX_DIGITS) {
        model.update({ numberBuffer: model.state.numberBuffer + key });
      }
      restartTimer();
      return;
    }
    switch (key) {
      case 'BACKSPACE': {
        const buffer = model.state.numberBuffer.slice(0, -1);
        if (buffer === '') end();
        else model.update({ numberBuffer: buffer });
        break;
      }
      case 'ENTER': commit(); break;
      case 'ESC': end(); break;
      default: break;
    }
  }

  return { context: Context.NUMBER_ENTRY, onKey, begin, cancel };
}
