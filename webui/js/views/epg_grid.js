// APP-VIEW EPG grid (REQ-33 – REQ-36): one row per service with the events around the selected
// time, marks "available" / "not available", "No information" for a service without events,
// and the details of the selected event (title, start–end, description, availability).
import { Context, Text, epgEvents, epgMark, nowSeconds } from '../model.js';
import { formatHHMM } from './format.js';
import { line, render } from './dom.js';

const EVENTS_BEFORE = 1;   // events shown before and after the selected time in every row
const EVENTS_AFTER = 1;

export function createEpgGridView(element, model, clock) {
  function selectedEvent(state) {
    const events = epgEvents(state, state.epgService);
    return events ? events.find((e) => e.event_id === state.epgEvent) ?? null : null;
  }

  function markText(state, event) {
    return epgMark(event, state.epg.serverTime) ?? '';
  }

  function update(state) {
    render(element, state.context === Context.EPG, (el) => {
      const selected = selectedEvent(state);
      const t = selected ? selected.start : nowSeconds(clock.now());
      const grid = document.createElement('div');
      grid.className = 'epg-rows';
      el.appendChild(grid);
      for (const service of state.services) {
        const row = document.createElement('div');
        row.className = service.service_id === state.epgService ? 'epg-row selected-row' : 'epg-row';
        line(row, `${service.number} ${service.name}`, 'epg-service');
        const events = epgEvents(state, service.service_id);
        if (events === null) {
          line(row, Text.NO_INFORMATION, 'epg-event');
        } else {
          let index = events.findIndex((e) => e.start <= t && t < e.start + e.duration);
          if (index < 0) index = events.findIndex((e) => e.start > t);
          if (index < 0) index = events.length - 1;
          const from = Math.max(0, index - EVENTS_BEFORE);
          for (const event of events.slice(from, index + EVENTS_AFTER + 1)) {
            const mark = markText(state, event);
            const cell = line(row, `${formatHHMM(event.start)} ${event.title}${mark ? ` (${mark})` : ''}`, 'epg-event');
            if (event.event_id === state.epgEvent) cell.classList.add('selected');
          }
        }
        grid.appendChild(row);
      }
      if (selected) {
        const details = document.createElement('div');
        details.className = 'epg-details';
        line(details, `${selected.title}  ${formatHHMM(selected.start)} – ${formatHHMM(selected.start + selected.duration)}`, 'title');
        line(details, selected.description);
        const mark = markText(state, selected);
        if (mark) line(details, mark);
        el.appendChild(details);
      }
    });
  }
  model.subscribe(update);
  update(model.state);
}
