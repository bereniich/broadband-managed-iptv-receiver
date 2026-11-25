// APP-VIEW info dialog (REQ-27, REQ-28) and playback position indicator (REQ-39, REQ-43):
// service number and name, current EPG event, LIVE / ARCHIVE with the playback time,
// protection status; in ARCHIVE mode or while a seek target is set, the position and the
// distance from live.
import {
  Text, currentEvent, mode, nowSeconds, playbackPosition, distanceFromLive, protectionStatus, serviceById,
} from '../model.js';
import { formatDistance, formatHHMM, formatHHMMSS } from './format.js';
import { line, render } from './dom.js';

export function createInfoBarView(infoElement, positionElement, model, clock) {
  function renderInfo(state, now) {
    render(infoElement, state.overlays.info && state.currentServiceId !== null, (el) => {
      const service = serviceById(state, state.currentServiceId);
      if (service) line(el, `${service.number} ${service.name}`, 'title');
      const event = currentEvent(state, now);
      if (event) {
        line(el, `${event.title}  ${formatHHMM(event.start)} – ${formatHHMM(event.start + event.duration)}`);
      }
      line(el, mode(state) === 'LIVE'
        ? Text.LIVE
        : `${Text.ARCHIVE} ${formatHHMMSS(playbackPosition(state, now))}`);
      const protection = protectionStatus(state);
      if (protection !== null) line(el, protection);
    });
  }

  function renderPosition(state, now) {
    const seeking = state.seekTarget !== null;
    render(positionElement, seeking || mode(state) === 'ARCHIVE', (el) => {
      const position = seeking ? state.seekTarget : playbackPosition(state, now);
      const distance = seeking ? nowSeconds(now) - state.seekTarget : distanceFromLive(state, now);
      line(el, `${formatHHMMSS(position)}  ${formatDistance(distance)}`);
    });
  }

  function update(state) {
    const now = clock.now();
    renderInfo(state, now);
    renderPosition(state, now);
  }

  model.subscribe(update);
  update(model.state);
}
