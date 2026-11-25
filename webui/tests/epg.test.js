// TST-411 (REQ-33 – REQ-36) and TST-412 (REQ-37, REQ-38). Expected values from Sections 2.3.7,
// 2.3.8, 4.4.3 and the worked example of Section 5.2.4 (t = 1795000000).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Context, Text, epgEvents, epgMark } from '../js/model.js';
import { createFixture, sessionState, T0 } from './fixture.js';

const T = 1794999600;   // previous event of svc-3 (Premium Morning)

// svc-3 current in ARCHIVE at T, EPG opened and answered with the EPG of the clock time.
async function epgOpen({ serviceIds } = {}) {
  const fx = createFixture({ nowS: T0 });
  await fx.start();
  await fx.answerPlay(1, { playing: true });
  fx.app.session.play('svc-3', T);
  await fx.answerPlay(2);
  await fx.emit('session_state', sessionState(2, 'svc-3', 'playing', { start: T, start_time: T, playing_since: T0, protected: true }));
  fx.press('E');
  return { fx, request: fx.mwc.pendingCall('get_epg'), answer: () => fx.respondEpg(serviceIds) };
}

function selected(fx) {
  const event = epgEvents(fx.state, fx.state.epgService).find((e) => e.event_id === fx.state.epgEvent);
  return { service: fx.state.epgService, id: event.event_id, title: event.title };
}

test('TST-411 EPG opens for now ± 2 h on the current service and the event of the playback position', async () => {
  const { fx, request, answer } = await epgOpen();
  assert.equal(fx.state.context, Context.EPG);
  assert.deepEqual(request.params, { from: T0 - 7200, to: T0 + 7200 });   // step 1
  await answer();
  assert.deepEqual(selected(fx), { service: 'svc-3', id: 'svc-3-1794999600', title: 'Premium Morning' });

  fx.press('DOWN');                                              // step 2
  assert.deepEqual(selected(fx), { service: 'svc-4', id: 'svc-4-1794999600', title: 'Cinema Classic' });
  fx.press('RIGHT');
  assert.deepEqual(selected(fx), { service: 'svc-4', id: 'svc-4-1794999900', title: 'Late Movie' });
  fx.press('LEFT');
  assert.equal(selected(fx).id, 'svc-4-1794999600');
  fx.press('UP');
  assert.deepEqual(selected(fx), { service: 'svc-3', id: 'svc-3-1794999600', title: 'Premium Morning' });
  const event = epgEvents(fx.state, 'svc-3').find((e) => e.event_id === 'svc-3-1794999600');
  assert.equal(event.description, 'First program of the protected demo service.');
  assert.equal(event.duration, 300);

  const events = epgEvents(fx.state, 'svc-3');                    // step 3
  const mark = (start) => epgMark(events.find((e) => e.start === start), fx.state.epg.serverTime);
  assert.equal(mark(1794999900), Text.AVAILABLE);                // running event
  assert.equal(mark(1794993000), Text.AVAILABLE);                // oldest available event
  assert.equal(mark(1794992700), Text.NOT_AVAILABLE);
  assert.equal(mark(1795000200), null);                          // future event
  assert.equal(events.length, 49);
});

test('TST-411 No information for a service without events and for all services when get_epg fails', async () => {
  const { fx, answer } = await epgOpen({ serviceIds: ['svc-1', 'svc-3', 'svc-4'] });
  await answer();
  assert.equal(epgEvents(fx.state, 'svc-2'), null);              // step 4: "No information"
  assert.notEqual(epgEvents(fx.state, 'svc-1'), null);

  fx.press('ESC', 'E');
  await fx.fail('get_epg', 'MW_UNREACHABLE');
  for (const id of ['svc-1', 'svc-2', 'svc-3', 'svc-4']) assert.equal(epgEvents(fx.state, id), null, id);
});

test('TST-412 ENTER plays available events from their start, refuses unavailable ones, ignores future ones', async () => {
  const { fx, answer } = await epgOpen();
  await answer();
  const plays = () => fx.mwc.callsOf('play').map((c) => c.params);
  let n = plays().length;

  fx.press('RIGHT', 'ENTER');                                    // step 1: running event
  assert.deepEqual(plays().slice(n), [{ service_id: 'svc-3', start: 1794999900 }]);
  assert.equal(fx.state.context, Context.PLAYBACK);
  n = plays().length;

  fx.press('E');                                                 // step 2: available past event
  await fx.respondEpg();
  fx.press('LEFT', 'LEFT', 'ENTER');
  assert.deepEqual(plays().slice(n), [{ service_id: 'svc-3', start: 1794999300 }]);
  n = plays().length;

  fx.press('E');                                                 // step 3: event of start 1794992700
  await fx.respondEpg();
  for (let i = 0; i < 30; i++) fx.press('LEFT');
  assert.equal(fx.state.epgEvent, 'svc-3-1794992700');
  fx.press('ENTER');
  assert.equal(fx.state.toast, Text.NOT_IN_ARCHIVE);
  assert.equal(fx.state.context, Context.EPG);
  assert.equal(plays().length, n);
  await fx.advance(3000);
  assert.equal(fx.state.toast, null);

  fx.press('ESC', 'E');                                          // step 4: future event
  await fx.respondEpg();
  fx.press('RIGHT', 'ENTER');
  assert.equal(fx.state.epgEvent, 'svc-3-1795000200');
  assert.equal(fx.state.context, Context.EPG);
  assert.equal(plays().length, n);
  assert.equal(fx.state.toast, null);
});
