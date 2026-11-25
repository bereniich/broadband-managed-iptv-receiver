// APP-PRS EPG presenter: context EPG (Section 4.4.3; REQ-33 – REQ-38) and the EPG refresh of
// Section 4.4.4 (now ± 2 h after the service list is loaded, every 300 s, when the EPG opens).
import {
  Context, Text, epgEvents, epgMark, eventAt, nowSeconds, playbackPosition,
} from '../model.js';

export const EPG_RANGE_S = 7200;
export const EPG_REFRESH_MS = 300000;

export function createEpgPresenter(app) {
  const { model, mwc, clock } = app;
  let refreshTimer = null;
  let selectedTime = null;   // time used to select an event when moving between services

  function refresh() {
    const nowS = nowSeconds(clock.now());
    const from = nowS - EPG_RANGE_S;
    const to = nowS + EPG_RANGE_S;
    mwc.call('get_epg', { from, to }).then(
      (result) => {
        model.update({ epg: { from, to, serverTime: result.server_time, services: result.services } });
        reselectIfOpen();
      },
      () => model.update({ epg: null }),   // "No information" for all services (REQ-36)
    );
  }

  // Started once, when the service list is loaded.
  function startRefresh() {
    if (refreshTimer !== null) return;
    const tick = () => {
      refreshTimer = clock.setTimeout(tick, EPG_REFRESH_MS);
      refresh();
    };
    tick();
  }

  function selectedEvent() {
    const events = epgEvents(model.state, model.state.epgService);
    if (events === null) return null;
    return events.find((e) => e.event_id === model.state.epgEvent) ?? null;
  }

  function selectAt(serviceId, t) {
    selectedTime = t;
    const event = eventAt(epgEvents(model.state, serviceId), t);
    model.update({ epgService: serviceId, epgEvent: event ? event.event_id : null });
  }

  function reselectIfOpen() {
    if (model.state.context === Context.EPG && selectedEvent() === null) {
      selectAt(model.state.epgService, selectedTime);
    }
  }

  function open() {
    const serviceId = model.state.currentServiceId ?? model.state.services[0]?.service_id ?? null;
    model.update({ context: Context.EPG });
    selectAt(serviceId, playbackPosition(model.state, clock.now()));
    refresh();
  }

  function moveService(delta) {
    const { services, epgService } = model.state;
    const index = services.findIndex((s) => s.service_id === epgService) + delta;
    if (index < 0 || index >= services.length) return;
    const event = selectedEvent();
    selectAt(services[index].service_id, event ? event.start : selectedTime);
  }

  function moveEvent(delta) {
    const events = epgEvents(model.state, model.state.epgService);
    if (events === null) return;
    const index = events.findIndex((e) => e.event_id === model.state.epgEvent) + delta;
    if (index < 0 || index >= events.length) return;
    selectedTime = events[index].start;
    model.update({ epgEvent: events[index].event_id });
  }

  function choose() {
    const event = selectedEvent();
    if (event === null) return;
    const mark = epgMark(event, model.state.epg.serverTime);
    if (mark === Text.AVAILABLE) {
      model.update({ context: Context.PLAYBACK });
      app.session.play(model.state.epgService, event.start);
    } else if (mark === Text.NOT_AVAILABLE) {
      app.overlays.showToast(Text.NOT_IN_ARCHIVE);
    }
  }

  function onKey(key) {
    switch (key) {
      case 'UP': moveService(-1); break;
      case 'DOWN': moveService(+1); break;
      case 'LEFT': moveEvent(-1); break;
      case 'RIGHT': moveEvent(+1); break;
      case 'ENTER': choose(); break;
      case 'E':
      case 'ESC': model.update({ context: Context.PLAYBACK }); break;
      default: break;
    }
  }

  return { context: Context.EPG, onKey, open, refresh, startRefresh };
}
