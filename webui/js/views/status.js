// APP-VIEW status messages: the persistent status message of Section 4.4.2 and the toast
// ("Invalid service number", "Content not available in archive"; REQ-09, REQ-38).
import { statusMessage } from '../model.js';
import { line, render } from './dom.js';

export function createStatusView(statusElement, toastElement, model) {
  function update(state) {
    const message = statusMessage(state);
    render(statusElement, message !== null, (el) => line(el, message));
    render(toastElement, state.toast !== null, (el) => line(el, state.toast));
  }
  model.subscribe(update);
  update(model.state);
}
