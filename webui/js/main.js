// Composition root of the Web application (Section 4.4.1): creates the model, the middleware
// client, the presenters, the views and the player, connects them and registers the keydown
// handler. The WebSocket URL is built from the page origin and the HLS URLs are relative, so a
// port change needs no code change (Section 3.8, TST-718).
import { createModel } from './model.js';
import { createKeyRouter } from './keys.js';
import { createMiddlewareClient } from './mwc.js';
import { createPresenters } from './presenters/index.js';
import { createDiag, createKeyMarker, diagEnabled } from './views/diag.js';
import { createPlayerView } from './views/player.js';
import { createInfoBarView } from './views/info_bar.js';
import { createZapBannerView } from './views/zap_banner.js';
import { createNumberEntryView } from './views/number_entry.js';
import { createServiceListView } from './views/service_list.js';
import { createEpgGridView } from './views/epg_grid.js';
import { createKeyDialogView } from './views/key_dialog.js';
import { createVolumeView } from './views/volume.js';
import { createStatusView } from './views/status.js';

const byId = (id) => document.getElementById(id);

// Clock object of Section 4.4.1, passed to the presenters (replaced by a fake in the tests).
const clock = {
  now: () => Date.now(),
  setTimeout: (callback, delayMs) => window.setTimeout(callback, delayMs),
  clearTimeout: (id) => window.clearTimeout(id),
};

const model = createModel();
const diag = createDiag({ enabled: diagEnabled(location.search), clock, log: (text) => console.info(text) });
const mwc = createMiddlewareClient({ model, clock, url: `ws://${location.host}/ws/control` });
const player = createPlayerView({ video: byId('video'), Hls: window.Hls, model, clock, diag });
const app = createPresenters({ model, mwc, clock, player });

createInfoBarView(byId('info-bar'), byId('position'), model, clock);
createZapBannerView(byId('zap-banner'), model);
createNumberEntryView(byId('number-entry'), model);
createServiceListView(byId('service-list'), model);
createEpgGridView(byId('epg-grid'), model, clock);
createKeyDialogView(byId('key-dialog'), model);
createVolumeView(byId('volume'), model);
createStatusView(byId('status'), byId('toast'), model);

// DIAG marks key (mapped by APP-KEY) and osd (next animation frame, after the views rendered).
const onMapped = createKeyMarker(diag, (callback) => requestAnimationFrame(callback), model);

document.addEventListener('keydown', createKeyRouter(model, app.byContext, onMapped));
mwc.connect();
