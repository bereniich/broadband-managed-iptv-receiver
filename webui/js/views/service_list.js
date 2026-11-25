// APP-VIEW service list (REQ-10): number, name, lock symbol for protected services, highlight.
import { Context } from '../model.js';
import { line, render } from './dom.js';

const LOCK = '\u{1F512}';

export function createServiceListView(element, model) {
  function update(state) {
    render(element, state.context === Context.SERVICE_LIST, (el) => {
      state.services.forEach((service, index) => {
        const text = `${service.number}  ${service.name}${service.protected ? `  ${LOCK}` : ''}`;
        line(el, text, index === state.listIndex ? 'item selected' : 'item');
      });
    });
  }
  model.subscribe(update);
  update(model.state);
}
