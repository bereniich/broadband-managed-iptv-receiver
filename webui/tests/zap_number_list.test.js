// TST-403 (REQ-06, REQ-07, REQ-11), TST-404 (REQ-08, REQ-09), TST-405 (REQ-10, REQ-11).
// Expected values from Sections 1.5.1, 2.3.2, 4.4.3 and 4.4.5.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Context, Text } from '../js/model.js';
import { createFixture } from './fixture.js';

// Started fixture with service `serviceId` playing live as session `n`.
async function playing(serviceId = 'svc-1', n = 1) {
  const fx = createFixture();
  await fx.start();
  if (serviceId === 'svc-1') {
    await fx.answerPlay(n, { playing: true });
  } else {
    await fx.answerPlay(n - 1, { playing: true });
    fx.app.session.play(serviceId, 'live');
    await fx.answerPlay(n, { playing: true });
  }
  return fx;
}

function plays(fx) {
  return fx.mwc.callsOf('play').map((c) => c.params);
}

test('TST-403 three quick zap keys show each service at once and send one play after 300 ms', async () => {
  const fx = await playing('svc-1');
  const before = plays(fx).length;
  const banners = [];
  for (let i = 0; i < 3; i++) {
    fx.press('UP');
    banners.push([fx.state.zapTarget, fx.state.overlays.banner]);
    await fx.advance(100);
  }
  assert.deepEqual(banners, [['svc-2', true], ['svc-3', true], ['svc-4', true]]);
  assert.equal(plays(fx).length, before, 'no play while keys follow within 300 ms');
  await fx.advance(199);
  assert.equal(plays(fx).length, before);
  await fx.advance(1);
  assert.deepEqual(plays(fx).slice(before), [{ service_id: 'svc-4', start: 'live' }]);
  assert.equal(fx.state.zapTarget, null);
});

test('TST-403 zapping wraps around at the highest and the lowest number', async () => {
  const fx = await playing('svc-4', 2);
  fx.press('UP');
  await fx.advance(300);
  assert.deepEqual(plays(fx).at(-1), { service_id: 'svc-1', start: 'live' });

  const fx1 = await playing('svc-1');
  fx1.press('DOWN');
  await fx1.advance(300);
  assert.deepEqual(plays(fx1).at(-1), { service_id: 'svc-4', start: 'live' });
});

test('TST-404 number entry commits with ENTER or after 2 s, at most 3 digits', async () => {
  const fx = await playing('svc-1');
  let n = plays(fx).length;

  fx.press('2', 'ENTER');                                       // step 1
  assert.deepEqual(plays(fx).slice(n), [{ service_id: 'svc-2', start: 'live' }]);
  assert.equal(fx.state.context, Context.PLAYBACK);
  n = plays(fx).length;

  fx.press('1', '2', '3', '4');                                 // step 2
  assert.equal(fx.state.context, Context.NUMBER_ENTRY);
  assert.equal(fx.state.numberBuffer, '123');
  fx.press('ESC');

  fx.press('3');                                                // step 3
  await fx.advance(1999);
  assert.equal(plays(fx).length, n);
  await fx.advance(1);
  assert.deepEqual(plays(fx).slice(n), [{ service_id: 'svc-3', start: 'live' }]);
  n = plays(fx).length;
  await fx.advance(5000);                                      // let the info dialog of that play expire

  fx.press('9', 'ENTER');                                       // step 4
  assert.equal(fx.state.toast, Text.INVALID_SERVICE_NUMBER);
  assert.equal(plays(fx).length, n);
  await fx.advance(2999);
  assert.equal(fx.state.toast, Text.INVALID_SERVICE_NUMBER);
  await fx.advance(1);
  assert.equal(fx.state.toast, null);

  fx.press('4', 'BACKSPACE');                                   // step 5
  assert.equal(fx.state.context, Context.PLAYBACK);
  assert.equal(fx.state.numberBuffer, '');
  fx.press('4', 'ESC');                                         // step 6
  assert.equal(fx.state.context, Context.PLAYBACK);
  assert.equal(fx.state.numberBuffer, '');
  await fx.advance(3000);
  assert.equal(plays(fx).length, n, 'no play after BACKSPACE / ESC');
});

test('TST-405 service list opens on the current service, moves without wrap-around, plays live', async () => {
  const fx = await playing('svc-3', 2);
  let n = plays(fx).length;

  fx.press('L');                                                // step 1
  assert.equal(fx.state.context, Context.SERVICE_LIST);
  assert.equal(fx.state.services.length, 4);
  assert.deepEqual(fx.state.services.map((s) => s.protected), [false, false, true, true]);
  assert.equal(fx.state.listIndex, 2);

  fx.press('UP', 'UP', 'UP');                                   // step 2
  assert.equal(fx.state.listIndex, 0);

  fx.press('ENTER');                                            // step 3
  assert.deepEqual(plays(fx).slice(n), [{ service_id: 'svc-1', start: 'live' }]);
  assert.equal(fx.state.context, Context.PLAYBACK);
  n = plays(fx).length;

  fx.press('ENTER', 'DOWN', 'ESC');                             // step 4
  assert.equal(fx.state.context, Context.PLAYBACK);
  assert.equal(plays(fx).length, n);
});
