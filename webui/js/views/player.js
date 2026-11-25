// APP-VIEW player: <video> element with hls.js on IF-INT-02 (Sections 3.7, 4.4.4), error policy
// ER-35, volume and mute (REQ-29 – REQ-31), first picture for the playback position (Section 4.4.2)
// and the DIAG marks frame, frag, wait and resume (Section 5.1.8).
// Interface used by the session presenter: create(url, options), destroy(), setVolume(volume, muted).

export function createPlayerView({ video, Hls, model, clock, diag }) {
  let hls = null;             // current hls.js instance
  let url = null;
  let options = null;
  let fatalErrors = 0;        // fatal errors of the player of the current session
  let pictureShown = false;   // firstPictureAt already set for this player

  video.addEventListener('playing', () => {
    diag.mark('resume');
    if (hls !== null && !pictureShown) {
      pictureShown = true;
      model.update({ firstPictureAt: clock.now() });
    }
  });
  video.addEventListener('waiting', () => diag.mark('wait'));

  function attach() {
    const instance = new Hls(options);
    hls = instance;
    instance.on(Hls.Events.MANIFEST_PARSED, () => {
      if (hls !== instance) return;
      const started = video.play();
      if (started && typeof started.catch === 'function') started.catch(() => {});
    });
    instance.on(Hls.Events.FRAG_CHANGED, (_event, data) => {
      if (hls === instance) diag.mark('frag', String(data.frag.sn));
    });
    instance.on(Hls.Events.ERROR, (_event, data) => {
      if (hls === instance && data.fatal) onFatalError(data);
    });
    instance.attachMedia(video);
    instance.loadSource(url);
    if (typeof video.requestVideoFrameCallback === 'function') {
      video.requestVideoFrameCallback(() => {
        if (hls === instance) diag.mark('frame', url);
      });
    }
  }

  function detach() {
    if (hls === null) return;
    const instance = hls;
    hls = null;
    instance.destroy();
  }

  // ER-35: only errors of the player of the current session are handled; MEDIA_ERROR is recovered
  // once, a further fatal error re-creates the player once, the third destroys it ("No signal"
  // until the next play, model.playerFailed).
  function onFatalError(data) {
    const { session, playerSession } = model.state;
    if (session === null || playerSession !== session.number) return;
    fatalErrors += 1;
    if (fatalErrors === 1 && data.type === Hls.ErrorTypes.MEDIA_ERROR) {
      hls.recoverMediaError();
    } else if (fatalErrors <= 2) {
      detach();
      attach();
    } else {
      detach();
      model.update({ playerFailed: true });
    }
  }

  return {
    // Destroys the previous player and creates a new one on `playlistUrl` (REQ-25).
    create(playlistUrl, playerOptions) {
      detach();
      url = playlistUrl;
      options = { ...playerOptions };
      fatalErrors = 0;
      pictureShown = false;
      if (!Hls || !Hls.isSupported()) {       // hls.js not loaded from the CDN, or no MSE
        model.update({ playerFailed: true });
        return;
      }
      attach();
    },

    destroy() {
      detach();
      url = null;
    },

    // Applied to the <video> element, so it is kept across player re-creation (REQ-31).
    setVolume(volume, muted) {
      video.volume = volume / 100;
      video.muted = muted;
    },
  };
}
