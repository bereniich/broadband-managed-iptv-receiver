// Composition root of the Web application (Section 4.4.1): creates the model and the
// key router and registers the keydown handler. The middleware client, the presenters and
// the views are connected here as they are added.
import { createModel } from './model.js';
import { createKeyRouter } from './keys.js';

const model = createModel();
const presenters = {};

document.addEventListener('keydown', createKeyRouter(model, presenters));
