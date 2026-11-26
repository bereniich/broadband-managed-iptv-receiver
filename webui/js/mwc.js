// APP-MWC: control WebSocket of IF-INT-01 (Section 3.6): request ids, call(name, params) → Promise,
// event dispatch, reconnect every 2 s (REQ-57, Section 4.4.4). No DOM access; the WebSocket
// constructor and the clock are injected, so that the unit tests can replace them.

export const RECONNECT_MS = 2000;   // Section 4.4.5

// Error with which pending and new calls are rejected while there is no connection
// (internal to the application, not an error code of Section 3.10).
export const CONNECTION_CLOSED = 'CONNECTION_CLOSED';

// `url`: ws://<page host>/ws/control (built from the page origin by main.js).
export function createMiddlewareClient({ model, clock, url, WebSocketImpl = globalThis.WebSocket }) {
  let socket = null;
  let reconnectTimer = null;
  let nextId = 1;
  const pending = new Map();          // id -> {resolve, reject}
  const eventHandlers = new Map();    // event name -> [handler]
  const connectionHandlers = [];

  function connect() {
    reconnectTimer = null;
    socket = new WebSocketImpl(url);
    socket.onopen = onOpen;
    socket.onmessage = (message) => onMessage(message.data);
    socket.onclose = onClose;
    socket.onerror = () => {};        // a close event always follows
  }

  function onOpen() {
    model.update({ connected: true });
    for (const handler of connectionHandlers) handler(true);
  }

  // Also called when a connection attempt fails; then the state does not change.
  function onClose() {
    socket = null;
    if (model.state.connected) {
      model.update({ connected: false });
      for (const handler of connectionHandlers) handler(false);
    }
    for (const [id, completion] of [...pending]) {
      pending.delete(id);
      completion.reject({ code: CONNECTION_CLOSED, message: 'connection closed' });
    }
    if (reconnectTimer === null) reconnectTimer = clock.setTimeout(connect, RECONNECT_MS);
  }

  function onMessage(text) {
    let message;
    try {
      message = JSON.parse(text);
    } catch {
      return;                         // not JSON: ignored
    }
    if (message === null || typeof message !== 'object') return;
    if (message.type === 'response') {
      const completion = pending.get(message.id);
      if (!completion) return;
      pending.delete(message.id);
      if (message.ok) completion.resolve(message.result ?? {});
      else completion.reject(message.error ?? { code: 'INVALID_COMMAND', message: '' });
    } else if (message.type === 'event') {
      for (const handler of eventHandlers.get(message.name) || []) handler(message.data);
    }
  }

  // Sends a command; the returned Promise carries the request id in its property `id`.
  function call(name, params = {}) {
    const id = nextId++;
    let completion;
    const promise = new Promise((resolve, reject) => {
      completion = { resolve, reject };
    });
    promise.id = id;
    if (socket === null || !model.state.connected) {
      completion.reject({ code: CONNECTION_CLOSED, message: 'not connected' });
      return promise;
    }
    pending.set(id, completion);
    socket.send(JSON.stringify({ type: 'command', id, name, params }));
    return promise;
  }

  function on(eventName, handler) {
    if (!eventHandlers.has(eventName)) eventHandlers.set(eventName, []);
    eventHandlers.get(eventName).push(handler);
  }

  // handler(connected): called after model.connected has been set.
  function onConnection(handler) {
    connectionHandlers.push(handler);
  }

  return { connect, call, on, onConnection };
}
