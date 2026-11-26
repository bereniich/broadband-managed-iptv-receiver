// APP-VIEW number entry (REQ-08): the typed service number.
import { Context } from '../model.js';
import { line, render } from './dom.js';

export function createNumberEntryView(element, model) {
  function update(state) {
    render(element, state.context === Context.NUMBER_ENTRY, (el) => line(el, state.numberBuffer, 'title'));
  }
  model.subscribe(update);
  update(model.state);
}
