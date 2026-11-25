// Test fixture of the presenter tests: the presenters connected with the fakes, and the test
// data of Section 5.2 (services of 5.2.3, EPG generated with the rules of Section 5.2.4).
// The EPG generator is test code, independent of the code under test (Section 5.1.7, rule 5).
import { createModel } from '../js/model.js';
import { createPresenters } from '../js/presenters/index.js';
import { FakeClock, FakeMiddlewareClient, FakePlayer, flush } from './fakes.js';

export const T0 = 1795000000;   // worked example of Section 5.2.4 (2026-11-18 11:06:40 UTC)

// GET /services of Section 3.2.1.
export const SERVICES = [
  { service_id: 'svc-1', number: 1, name: 'Demo One', protected: false },
  { service_id: 'svc-2', number: 2, name: 'Demo Two', protected: false },
  { service_id: 'svc-3', number: 3, name: 'Premium One', protected: true },
  { service_id: 'svc-4', number: 4, name: 'Premium Two', protected: true },
];

// Programs and descriptions of Section 5.2.3.
const PROGRAMS = {
  'svc-1': ['News at Nine', 'Weather Today', 'clear demo service'],
  'svc-2': ['Science Hour', 'Nature Walk', 'clear demo service'],
  'svc-3': ['Premium Morning', 'Premium Evening', 'protected demo service'],
  'svc-4': ['Cinema Classic', 'Late Movie', 'protected demo service'],
};

// get_epg result for [from, to) at server time `serverTime` (Sections 3.2.3, 5.2.4):
// events every 300 s, program 1 if start mod 600 < 300, in_archive for serverTime − 7200 ≤ start ≤ serverTime.
export function epgResult(from, to, serverTime, serviceIds = Object.keys(PROGRAMS)) {
  const services = serviceIds.map((id) => {
    const [first, second, description] = PROGRAMS[id];
    const events = [];
    for (let start = from - (from % 300); start < to; start += 300) {
      const one = start % 600 < 300;
      events.push({
        event_id: `${id}-${start}`,
        start,
        duration: 300,
        title: one ? first : second,
        description: `${one ? 'First' : 'Second'} program of the ${description}.`,
        in_archive: serverTime - 7200 <= start && start <= serverTime,
      });
    }
    return { service_id: id, events };
  });
  return { server_time: serverTime, services };
}

// session_state data of Section 3.6.3 with overrides.
export function sessionState(session, serviceId, state, extra = {}) {
  return {
    session,
    service_id: serviceId,
    state,
    reason: null,
    start: 'live',
    start_time: null,
    playing_since: null,
    protected: null,
    error: null,
    ...extra,
  };
}

export function createFixture({ nowS = T0 } = {}) {
  const clock = new FakeClock(nowS * 1000);
  const model = createModel();
  const mwc = new FakeMiddlewareClient(model);
  const player = new FakePlayer();
  const app = createPresenters({ model, mwc, clock, player });

  const fx = {
    clock, mwc, player, model, app,
    get state() { return model.state; },

    // Sends a remote-control key to the presenter of the active context.
    press(...keys) {
      for (const key of keys) app.byContext[model.state.context].onKey(key);
    },

    async advance(ms) {
      clock.advance(ms);
      await flush();
    },

    async respond(name, result) {
      mwc.respond(mwc.pendingCall(name).id, result);
      await flush();
    },

    async fail(name, code) {
      mwc.fail(mwc.pendingCall(name).id, code);
      await flush();
    },

    async emit(eventName, data) {
      mwc.emit(eventName, data);
      await flush();
    },

    // Connects, answers get_status (idle by default) and get_services; answers the get_epg of the
    // service-list load with the EPG of the current time. The first `play` stays pending.
    async start({ status = { state: 'idle' }, services = SERVICES } = {}) {
      mwc.setConnected(true);
      await flush();
      await fx.respond('get_status', status);
      await fx.respond('get_services', { services });
      await fx.respondEpg();
    },

    async respondEpg(serviceIds) {
      const { params } = mwc.pendingCall('get_epg');
      await fx.respond('get_epg', epgResult(params.from, params.to, Math.floor(clock.now() / 1000), serviceIds));
    },

    // Answers the pending play with session `n`; optionally sends `playing` of that session.
    async answerPlay(n, { playing = false, extra = {} } = {}) {
      const { params } = mwc.pendingCall('play');
      await fx.respond('play', { session: n });
      if (playing) {
        await fx.emit('session_state', sessionState(n, params.service_id, 'playing', { start: params.start, ...extra }));
      }
    },
  };
  return fx;
}
