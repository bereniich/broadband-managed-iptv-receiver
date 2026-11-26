// APP-VIEW key dialog (REQ-46, REQ-47): title, typed characters masked with •, counter n/32.
// The typed key itself is never shown.
import { Context } from '../model.js';
import { line, render } from './dom.js';

export function createKeyDialogView(element, model) {
  function update(state) {
    render(element, state.context === Context.KEY_DIALOG, (el) => {
      const n = state.keyBuffer.length;
      line(el, state.keyDialogTitle ?? '', 'title');
      line(el, n > 0 ? '•'.repeat(n) : ' ', 'mask');
      line(el, `${n}/32`);
    });
  }
  model.subscribe(update);
  update(model.state);
}
