// APP-PRS service-list presenter: context SERVICE_LIST (Section 4.4.3; REQ-10, REQ-11).
import { Context } from '../model.js';

export function createServiceListPresenter(app) {
  const { model } = app;

  function open() {
    const index = model.state.services.findIndex((s) => s.service_id === model.state.currentServiceId);
    model.update({ listIndex: Math.max(0, index), context: Context.SERVICE_LIST });
  }

  function move(delta) {
    const last = model.state.services.length - 1;
    const listIndex = Math.min(Math.max(0, model.state.listIndex + delta), Math.max(0, last));
    model.update({ listIndex });   // no wrap-around
  }

  function onKey(key) {
    switch (key) {
      case 'UP': move(-1); break;
      case 'DOWN': move(+1); break;
      case 'ENTER': {
        const service = model.state.services[model.state.listIndex];
        model.update({ context: Context.PLAYBACK });
        if (service) app.session.play(service.service_id, 'live');
        break;
      }
      case 'ESC': model.update({ context: Context.PLAYBACK }); break;
      default: break;
    }
  }

  return { context: Context.SERVICE_LIST, onKey, open };
}
