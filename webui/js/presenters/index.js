// Creates and connects the presenters of APP-PRS. `deps` = {model, mwc, clock, player}:
// the middleware client (APP-MWC), the clock object of Section 4.4.1 and the player view.
// Used by main.js and by the unit tests, so that both connect the presenters the same way.
import { createOverlays } from './overlays.js';
import { createSessionPresenter } from './session.js';
import { createPlaybackPresenter } from './playback.js';
import { createNumberEntryPresenter } from './number_entry.js';
import { createServiceListPresenter } from './service_list.js';
import { createEpgPresenter } from './epg.js';
import { createKeyDialogPresenter } from './key_dialog.js';

export function createPresenters(deps) {
  const app = { ...deps };
  app.overlays = createOverlays(app);
  app.session = createSessionPresenter(app);
  app.playback = createPlaybackPresenter(app);
  app.numberEntry = createNumberEntryPresenter(app);
  app.serviceList = createServiceListPresenter(app);
  app.epg = createEpgPresenter(app);
  app.keyDialog = createKeyDialogPresenter(app);

  // Presenter per context, for the key router of APP-KEY.
  app.byContext = {};
  for (const p of [app.playback, app.numberEntry, app.serviceList, app.epg, app.keyDialog]) {
    app.byContext[p.context] = p;
  }
  app.session.start();
  return app;
}
