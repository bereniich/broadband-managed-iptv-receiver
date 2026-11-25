// Test fakes of the Web application (Sections 4.4.6, 5.3.1): a fake clock that advances the
// timers of Section 4.4.5 explicitly, and a fake middleware client with scripted responses
// and events (JSON of Section 3.6). Not part of the code under test.

// Clock object with the interface passed to the presenters: now(), setTimeout, clearTimeout.
export class FakeClock {
  constructor(startMs = 0) {
    this.time = startMs;
    this.nextId = 1;
    this.timers = new Map();   // id -> {due, callback}
  }

  now() {
    return this.time;
  }

  setTimeout(callback, delayMs) {
    const id = this.nextId++;
    this.timers.set(id, { due: this.time + Math.max(0, delayMs), callback });
    return id;
  }

  clearTimeout(id) {
    this.timers.delete(id);
  }

  // Advances the time by `ms` and runs every timer that becomes due, in the order of its due
  // time (timers with the same due time in the order of creation). Timers started by a callback
  // run in the same call if they are due before the end of the interval.
  advance(ms) {
    const end = this.time + ms;
    for (;;) {
      let nextId = null;
      let next = null;
      for (const [id, timer] of this.timers) {
        if (timer.due <= end && (next === null || timer.due < next.due)) {
          nextId = id;
          next = timer;
        }
      }
      if (next === null) break;
      this.timers.delete(nextId);
      this.time = next.due;
      next.callback();
    }
    this.time = end;
  }

  pendingCount() {
    return this.timers.size;
  }
}

// Fake of APP-MWC. call() returns a Promise with the request id in its property `id`;
// the test completes it with respond() or fail(). Events are delivered with emit(), connection
// changes with setConnected(): as APP-MWC, it sets model.connected (Section 4.4.2), then calls
// the connection handlers; a closed connection rejects all pending calls (REQ-57).
export class FakeMiddlewareClient {
  constructor(model) {
    this.model = model;
    this.nextId = 1;
    this.calls = [];             // [{id, name, params}] in the order of the calls
    this.pending = new Map();    // id -> {resolve, reject}
    this.handlers = new Map();   // event name -> [handler]
    this.connectionHandlers = [];
  }

  onConnection(handler) {
    this.connectionHandlers.push(handler);
  }

  setConnected(connected) {
    this.model.update({ connected });
    for (const handler of this.connectionHandlers) handler(connected);
    if (!connected) {
      for (const id of [...this.pending.keys()]) this.fail(id, 'CONNECTION_CLOSED');
    }
  }

  // The last call with the given command name that has not been completed yet.
  pendingCall(name) {
    const open = this.calls.filter((c) => c.name === name && this.pending.has(c.id));
    if (open.length === 0) throw new Error(`no pending ${name} call`);
    return open[open.length - 1];
  }

  call(name, params = {}) {
    const id = this.nextId++;
    this.calls.push({ id, name, params });
    let completion;
    const promise = new Promise((resolve, reject) => {
      completion = { resolve, reject };
    });
    this.pending.set(id, completion);
    promise.id = id;
    return promise;
  }

  // Completes call `id` with `{"ok": true, "result": result}`.
  respond(id, result = {}) {
    this.take(id).resolve(result);
  }

  // Completes call `id` with `{"ok": false, "error": {code, message}}`.
  fail(id, code, message = '') {
    this.take(id).reject({ code, message });
  }

  on(eventName, handler) {
    if (!this.handlers.has(eventName)) this.handlers.set(eventName, []);
    this.handlers.get(eventName).push(handler);
  }

  emit(eventName, data) {
    for (const handler of this.handlers.get(eventName) || []) handler(data);
  }

  // Calls with the given command name, e.g. callsOf('play').
  callsOf(name) {
    return this.calls.filter((c) => c.name === name);
  }

  lastCall() {
    return this.calls[this.calls.length - 1];
  }

  take(id) {
    const completion = this.pending.get(id);
    if (!completion) throw new Error(`no pending call with id ${id}`);
    this.pending.delete(id);
    return completion;
  }
}

// Fake player view (Section 5.3.1): records create / destroy / URL / options / volume.
export class FakePlayer {
  constructor() {
    this.log = [];               // ['create', url, options] | ['destroy'] | ['volume', v, muted]
    this.current = null;         // {url, options} of the existing player
    this.volume = null;          // last {volume, muted} applied to the <video> element
  }

  create(url, options) {
    this.log.push(['create', url, options]);
    this.current = { url, options };
  }

  destroy() {
    this.log.push(['destroy']);
    this.current = null;
  }

  setVolume(volume, muted) {
    this.log.push(['volume', volume, muted]);
    this.volume = { volume, muted };
  }

  count(kind) {
    return this.log.filter((entry) => entry[0] === kind).length;
  }
}

// Lets the promise callbacks of completed calls run before the test continues.
export function flush() {
  return new Promise((resolve) => setImmediate(resolve));
}
