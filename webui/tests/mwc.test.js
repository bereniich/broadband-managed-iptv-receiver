// TST-408 (REQ-57, ER-34): APP-MWC with a fake WebSocket, and the reconnect with the real
// presenters. Expected values from Sections 3.6.1, 4.4.4, 4.4.5 and 4.8 (ER-34).
import { test, beforeEach } from 'node:test';
import assert from 'node:assert/strict';
import { createModel, statusMessage, Text } from '../js/model.js';
import { createMiddlewareClient, RECONNECT_MS } from '../js/mwc.js';
import { createPresenters } from '../js/presenters/index.js';
import { FakeClock, FakePlayer, FakeWebSocket, flush } from './fakes.js';
import { SERVICES, T0, epgResult, sessionState } from './fixture.js';

const URL = 'ws://127.0.0.1:8090/ws/control';

beforeEach(() => FakeWebSocket.reset());

function client() {
  const model = createModel();
  const clock = new FakeClock(T0 * 1000);
  const mwc = createMiddlewareClient({ model, clock, url: URL, WebSocketImpl: FakeWebSocket });
  return { model, clock, mwc };
}

test('TST-408 commands carry unique ids; responses complete the call with the same id; events are dispatched', async () => {
  const { model, mwc } = client();
  const connections = [];
  mwc.onConnection((c) => connections.push([c, model.state.connected]));
  mwc.connect();
  const ws = FakeWebSocket.last();
  assert.equal(ws.url, URL);
  ws.open();
  assert.deepEqual(connections, [[true, true]], 'model.connected is set before the handlers run');

  const a = mwc.call('get_services');
  const b = mwc.call('play', { service_id: 'svc-1', start: 'live' });
  assert.deepEqual(ws.sent, [
    { type: 'command', id: a.id, name: 'get_services', params: {} },
    { type: 'command', id: b.id, name: 'play', params: { service_id: 'svc-1', start: 'live' } },
  ]);
  assert.notEqual(a.id, b.id);

  ws.receive({ type: 'response', id: b.id, ok: true, result: { session: 12 } });   // out of order
  ws.receive({ type: 'response', id: a.id, ok: false, error: { code: 'MW_UNREACHABLE', message: 'x' } });
  assert.deepEqual(await b, { session: 12 });
  await assert.rejects(a, { code: 'MW_UNREACHABLE' });

  const events = [];
  mwc.on('mw_status', (data) => events.push(data));
  ws.receive({ type: 'event', name: 'mw_status', data: { reachable: false } });
  ws.receive('not json');
  ws.receive({ type: 'event', name: 'stream_info', data: {} });
  assert.deepEqual(events, [{ reachable: false }]);
});

test('TST-408 closing rejects pending calls, sets connected false and reconnects every 2 s', async () => {
  const { model, clock, mwc } = client();
  const connections = [];
  mwc.onConnection((c) => connections.push(c));
  mwc.connect();
  FakeWebSocket.last().open();
  const pending = mwc.call('get_epg', { from: 1, to: 2 });

  FakeWebSocket.last().close();
  assert.equal(model.state.connected, false);
  assert.deepEqual(connections, [true, false]);
  await assert.rejects(pending, { code: 'CONNECTION_CLOSED' });
  await assert.rejects(mwc.call('get_status'), { code: 'CONNECTION_CLOSED' }, 'no call while disconnected');

  const attempts = () => FakeWebSocket.instances.length;
  clock.advance(RECONNECT_MS - 1);
  assert.equal(attempts(), 1);
  clock.advance(1);
  assert.equal(attempts(), 2);
  FakeWebSocket.last().close();                       // failed attempt
  clock.advance(RECONNECT_MS);
  assert.equal(attempts(), 3);
  FakeWebSocket.last().close();
  assert.deepEqual(connections, [true, false], 'a failed attempt changes nothing');
  clock.advance(RECONNECT_MS);
  assert.equal(attempts(), 4);
  FakeWebSocket.last().open();
  assert.equal(model.state.connected, true);
  assert.deepEqual(connections, [true, false, true]);
  clock.advance(10 * RECONNECT_MS);
  assert.equal(attempts(), 4, 'no attempt while connected');
});

test('TST-408 reconnect: No signal while disconnected, then get_status and get_services, session 5 adopted with its player', async () => {
  const model = createModel();
  const clock = new FakeClock(T0 * 1000);
  const player = new FakePlayer();
  const mwc = createMiddlewareClient({ model, clock, url: URL, WebSocketImpl: FakeWebSocket });
  createPresenters({ model, mwc, clock, player });

  // Session 5 of svc-2 playing, player of session 5 created.
  mwc.connect();
  let ws = FakeWebSocket.last();
  ws.open();
  ws.respond('get_status', sessionState(5, 'svc-2', 'playing', { start_time: T0, playing_since: T0 }));
  ws.respond('get_services', { services: SERVICES });
  await flush();
  const { params } = ws.command('get_epg');
  ws.respond('get_epg', epgResult(params.from, params.to, T0));
  ws.respond('play', { session: 5 });
  await flush();
  ws.receive({ type: 'event', name: 'session_state', data: sessionState(5, 'svc-2', 'playing', { start_time: T0, playing_since: T0 }) });
  assert.equal(model.state.playerSession, 5);
  const plays = ws.sent.filter((m) => m.name === 'play').length;
  player.log.length = 0;

  ws.close();                                          // step 1
  await flush();
  assert.equal(model.state.connected, false);
  assert.equal(statusMessage(model.state), Text.NO_SIGNAL);

  clock.advance(RECONNECT_MS);                          // step 2: attempts every 2 s, 3rd connects
  FakeWebSocket.last().close();
  clock.advance(RECONNECT_MS);
  FakeWebSocket.last().close();
  clock.advance(RECONNECT_MS);
  ws = FakeWebSocket.last();
  assert.equal(FakeWebSocket.instances.length, 4);
  ws.open();
  assert.deepEqual(ws.sent.map((m) => m.name), ['get_status', 'get_services']);
  ws.respond('get_status', sessionState(5, 'svc-2', 'playing', { start_time: T0, playing_since: T0 }));
  ws.respond('get_services', { services: SERVICES });
  await flush();
  assert.equal(model.state.session.number, 5);
  assert.equal(model.state.playerSession, 5);
  assert.deepEqual(player.log, [], 'player kept');
  assert.equal(ws.sent.filter((m) => m.name === 'play').length, 0, 'no play');
  assert.equal(plays, 1);
  assert.equal(statusMessage(model.state), null);
});
