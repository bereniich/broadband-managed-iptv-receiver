// APP-VIEW volume and mute indicator (REQ-29, REQ-30): the volume level for 2 s after a change,
// the mute indicator for as long as the sound is muted.
import { line, render } from './dom.js';

export function createVolumeView(element, model) {
  function update(state) {
    render(element, state.overlays.volume || state.muted, (el) => {
      if (state.muted) line(el, 'Muted');
      else line(el, `Volume ${state.volume} %`);
    });
  }
  model.subscribe(update);
  update(model.state);
}
