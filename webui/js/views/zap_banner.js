// APP-VIEW zap banner (REQ-07): the service selected by ↑ / ↓ while the 300 ms debounce runs.
import { serviceById } from '../model.js';
import { line, render } from './dom.js';

export function createZapBannerView(element, model) {
  function update(state) {
    render(element, state.overlays.banner, (el) => {
      const service = serviceById(state, state.zapTarget);
      line(el, service ? `${service.number} ${service.name}` : '–');
    });
  }
  model.subscribe(update);
  update(model.state);
}
