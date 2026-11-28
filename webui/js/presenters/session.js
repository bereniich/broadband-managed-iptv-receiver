// APP-PRS session presenter: sends `play`, reacts to the events and responses of IF-INT-01 and
// adopts a session after a reconnect or a rejected `play` (Section 4.4.4).
import { Context, Text, nowSeconds } from '../model.js';

export const POSITION_UPDATE_MS = 1000;   // Section 4.4.5

export function hlsUrl(session) {
  return `/hls/${session}/index.m3u8`;
}

// session_state data (Section 3.6.3) → model `session`.
function sessionFromData(d) {
  return {
    number: d.session,
    serviceId: d.service_id,
    state: d.state,
    reason: d.reason ?? null,
    start: d.start,
    startTime: d.start_time ?? null,
    playingSince: d.playing_since ?? null,
    protected: d.protected ?? null,
    error: d.error ?? null,
  };
}

export function createSessionPresenter(app) {
  const { model, mwc, clock } = app;
  let positionTimer = null;
  let servicesPending = false;
  let epgStarted = false;
  // Set when get_status reported `idle`: service number 1 is started once the list is known.
  let startFirstService = false;

  function start() {
    mwc.onConnection(onConnection);
    mwc.on('session_state', onSessionState);
    mwc.on('stream_info', onStreamInfo);
    mwc.on('mw_status', onMwStatus);
  }

  // ------------------------------------------------------------ connection (REQ-03, REQ-57)
  // APP-MWC sets model.connected (Section 4.4.2) before it calls this handler.
  function onConnection(connected) {
    if (!connected) return;            // pending calls are rejected by APP-MWC
    startFirstService = false;
    mwc.call('get_status').then(adopt, () => {});
    requestServices();
  }

  function requestServices() {
    if (servicesPending) return;
    servicesPending = true;
    mwc.call('get_services').then(
      (result) => {
        servicesPending = false;
        const services = [...result.services].sort((a, b) => a.number - b.number);
        model.update({ services });
        if (!epgStarted) {
          epgStarted = true;
          app.epg.startRefresh();
        }
        maybeStartFirstService();
      },
      () => { servicesPending = false; },   // MW_UNREACHABLE: the next mw_status true retries
    );
  }

  function maybeStartFirstService() {
    const services = model.state.services;
    if (!startFirstService || services.length === 0) return;
    startFirstService = false;
    const first = services.find((s) => s.number === 1) ?? services[0];
    play(first.service_id, 'live');
  }

  // ------------------------------------------------------------ adopting a session (Section 4.4.4)
  function adopt(data) {
    if (data.state === 'idle') {
      startFirstService = true;
      maybeStartFirstService();
      return;
    }
    const session = sessionFromData(data);
    model.update({ session, currentServiceId: session.serviceId, lastPlayId: null });
    if (model.state.playerSession === session.number) return;   // keep the existing player
    if (session.playingSince !== null && (session.state === 'playing' || session.state === 'no_signal')) {
      const nowS = nowSeconds(clock.now());
      const startPoint = session.start === 'live'
        ? 'live'
        : session.startTime + (nowS - session.playingSince);
      play(session.serviceId, startPoint);
      return;
    }
    react(session);
  }

  // ------------------------------------------------------------ play
  function play(serviceId, startPoint) {
    app.playback.cancelTimers();
    stopPositionTimer();               // the new play ends the session of the position indicator
    const call = mwc.call('play', { service_id: serviceId, start: startPoint });
    model.update({
      lastPlayId: call.id,
      currentServiceId: serviceId,
      seekTarget: null,
      zapTarget: null,
      keyDialogCancelled: false,
      session: null,
      streamInfo: null,
      playerFailed: false,
    });
    app.overlays.showInfo();
    call.then(
      (result) => {
        if (model.state.lastPlayId !== call.id) return;   // response to an older play
        model.update({
          session: {
            number: result.session,
            serviceId,
            state: 'starting',
            reason: null,
            start: startPoint,
            startTime: null,
            playingSince: null,
            protected: null,
            error: null,
          },
        });
      },
      (error) => {
        if (model.state.lastPlayId !== call.id || !model.state.connected) return;
        // The AL kept its current session (ER-07).
        if (error.code === 'SERVICE_NOT_FOUND') app.overlays.showToast(Text.INVALID_SERVICE_NUMBER);
        mwc.call('get_status').then(adopt, () => {});
      },
    );
  }

  // ------------------------------------------------------------ events
  function onSessionState(data) {
    const current = model.state.session;
    if (current === null || data.session !== current.number) return;   // REQ-26
    const session = sessionFromData(data);
    model.update({ session });
    react(session);
  }

  function react(session) {
    switch (session.state) {
      case 'playing':
        if (model.state.playerSession !== session.number) createPlayer(session.number);
        break;
      case 'key_required':
        destroyPlayer();
        if (model.state.context !== Context.KEY_DIALOG) {
          app.playback.cancelTimers();
          app.numberEntry.cancel();
          app.keyDialog.open(session.reason === 'key_rejected' ? Text.INCORRECT_KEY : Text.ENTER_KEY);
        }
        break;
      case 'error':
      case 'stopped':
        destroyPlayer();
        break;
      default:
        break;
    }
  }

  function onStreamInfo(data) {
    const current = model.state.session;
    if (current === null || data.session !== current.number) return;
    model.update({ streamInfo: data });
  }

  function onMwStatus(data) {
    model.update({ mwReachable: data.reachable });
    if (data.reachable && model.state.services.length === 0) requestServices();
  }

  // ------------------------------------------------------------ player (REQ-25, REQ-31)
  function createPlayer(session) {
    if (model.state.playerSession !== null) app.player.destroy();
    app.player.create(hlsUrl(session), { startPosition: 0 });
    app.player.setVolume(model.state.volume, model.state.muted);
    model.update({ playerSession: session, firstPictureAt: null });
    startPositionTimer();
  }

  function destroyPlayer() {
    stopPositionTimer();
    if (model.state.playerSession === null) return;
    app.player.destroy();
    model.update({ playerSession: null, firstPictureAt: null });
  }

  // Re-renders the position indicator every second (REQ-43). Once the first picture is known, every
  // tick falls on firstPictureAt + k · 1 s, the moments at which the position of Section 4.4.2
  // changes, so the shown second changes together with the picture and not up to 1 s later.
  function startPositionTimer() {
    stopPositionTimer();
    const tick = () => {
      positionTimer = clock.setTimeout(tick, nextPositionDelay());
      model.update({});
    };
    positionTimer = clock.setTimeout(tick, nextPositionDelay());
  }

  // Delay to the next tick: 1 s before the first picture, then to the next whole second after it.
  function nextPositionDelay() {
    const first = model.state.firstPictureAt;
    if (first === null) return POSITION_UPDATE_MS;
    const intoSecond = (((clock.now() - first) % POSITION_UPDATE_MS) + POSITION_UPDATE_MS) % POSITION_UPDATE_MS;
    return POSITION_UPDATE_MS - intoSecond;
  }

  function stopPositionTimer() {
    if (positionTimer !== null) clock.clearTimeout(positionTimer);
    positionTimer = null;
  }

  return { start, play };
}
