// APP-PRS key-dialog presenter: context KEY_DIALOG (Section 4.4.3; REQ-46 – REQ-48).
// All letter and digit keys are text input; only 0–9 and A–F are key characters.
import { Context, Text } from '../model.js';

export const KEY_LENGTH = 32;
const HEX = /^[0-9A-F]$/;

export function createKeyDialogPresenter(app) {
  const { model, mwc } = app;

  function open(title) {
    model.update({ keyBuffer: '', keyDialogTitle: title, context: Context.KEY_DIALOG });
  }

  function confirm() {
    if (model.state.keyBuffer.length !== KEY_LENGTH) return;
    const serviceId = model.state.currentServiceId;
    const key = model.state.keyBuffer;
    model.update({ keyBuffer: '', keyDialogCancelled: false, context: Context.PLAYBACK });
    mwc.call('set_key', { service_id: serviceId, key }).then(
      () => {},
      (error) => {
        if (error.code === 'INVALID_KEY_FORMAT') open(Text.ENTER_KEY);            // ER-32
        else if (error.code === 'SERVICE_NOT_FOUND') app.overlays.showToast(Text.INVALID_SERVICE_NUMBER);
      },
    );
  }

  function cancel() {
    const keyRequired = model.state.session !== null && model.state.session.state === 'key_required';
    model.update({ keyBuffer: '', keyDialogCancelled: keyRequired, context: Context.PLAYBACK });
  }

  function onKey(key) {
    if (HEX.test(key)) {
      if (model.state.keyBuffer.length < KEY_LENGTH) {
        model.update({ keyBuffer: model.state.keyBuffer + key });
      }
      return;
    }
    switch (key) {
      case 'BACKSPACE': model.update({ keyBuffer: model.state.keyBuffer.slice(0, -1) }); break;
      case 'ENTER': confirm(); break;
      case 'ESC': cancel(); break;
      default: break;
    }
  }

  return { context: Context.KEY_DIALOG, onKey, open };
}
