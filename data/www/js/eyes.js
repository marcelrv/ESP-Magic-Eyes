// eyes.js — a live pair of cartoon eyes showing what the firmware is
// *commanding* the real eyes to do (pan/tilt and the four lids).
//
// Hobby servos give no position feedback, so this is the pose MotionTask
// publishes (the interpolated target it sends to the servos every 20 ms),
// not a measurement. Needs js/api.js loaded first. Plain global, no build.
//
//   const h = MagicEyes.mount(hostElement, { onPose: function (pose, nx, ny) {} });
//   h.stop();   // permanent: removes listeners, later responses are dropped
//
// onPose gets the raw pose plus nx, ny: the unit-circle-clamped gaze the eyes
// themselves draw (ny positive = down on screen). A throwing onPose is logged
// and never counts as a poll failure.
//
// A blink is only ~300 ms, so the next request goes out POLL_GAP_MS after the
// last answer landed (the device answers in 30-100 ms, ~12 samples/s); a
// slower poll turns a blink into one smoothed-away sample.
//
// Staleness is a watchdog, not per-request: the eyes dim (.eyes-stale) only
// when no good sample arrived for a second, so one dropped request does not
// flicker. They start dimmed with open lids ("no data", not "eyes shut").
// Failures back off (250 ms doubling to 5 s; 30 s after a 429 lockout), a
// request that hangs past 3 s counts as failed, and a 401 stops polling until
// the page is reloaded (else the browser's login prompt returns after every
// Cancel). Polling pauses while the tab is hidden or (unless onPose is set:
// the page then needs the samples itself) the widget is scrolled off-screen,
// and resumes on show / bfcache restore.

const MagicEyes = (() => {
  const LOOK_PX = 20;          // iris travel at full gaze, in the 100x100 eye viewBox
  const POLL_GAP_MS = 30;      // pause after each answer before the next request
  const REQUEST_TIMEOUT_MS = 3000;
  const STALE_AFTER_MS = 1000; // no good sample for this long -> dim the eyes
  const BACKOFF_BASE_MS = 250;
  const BACKOFF_MAX_MS = 5000;
  const LOCKOUT_MS = 30000;    // firmware's wrong-password lockout (HTTP 429)
  let mounts = 0;              // clipPath ids must be unique per page

  // The upper lid covers the top half and the lower lid the bottom half, each
  // scaled by (1 - openness): both at 0 meet in the middle, 1 tucks them away.
  // The clip sits on a wrapper <g>, not the scaled rect, or it would squash too.
  function eyeSvg(id) {
    return '<svg class="eye" viewBox="0 0 100 100">' +
      '<defs><clipPath id="' + id + '"><circle cx="50" cy="50" r="46"/></clipPath></defs>' +
      '<circle class="sclera" cx="50" cy="50" r="46"/>' +
      '<g class="look"><circle class="iris" cx="50" cy="50" r="22"/>' +
      '<circle class="pupil" cx="50" cy="50" r="11"/>' +
      '<circle class="glint" cx="57" cy="42" r="4.5"/></g>' +
      '<g clip-path="url(#' + id + ')">' +
      '<rect class="lid lid-upper" x="0" y="0" width="100" height="50"/>' +
      '<rect class="lid lid-lower" x="0" y="50" width="100" height="50"/></g>' +
      '</svg>';
  }

  function clamp01(v) { return Math.max(0, Math.min(1, v)); }

  // Shared yoke: both irises move together. Up on screen = positive tilt, so
  // ny (down positive) is -tilt. Clamped to the unit circle like the gaze pad.
  function gazeNorm(p) {
    let nx = p.panDeg / Api.GAZE_RANGE_DEG;
    let ny = -p.tiltDeg / Api.GAZE_RANGE_DEG;
    const mag = Math.hypot(nx, ny);
    if (mag > 1) { nx /= mag; ny /= mag; }
    return [nx, ny];
  }

  function mount(host, opts) {
    opts = opts || {};
    const n = ++mounts;
    host.classList.add('eyes-view');
    host.setAttribute('aria-hidden', 'true');
    host.innerHTML = eyeSvg('eye-clip-' + n + 'l') + eyeSvg('eye-clip-' + n + 'r');
    // Captured once: render() runs ~12x/s.
    const eyeEls = host.querySelectorAll('.eye');
    const looks = [eyeEls[0].querySelector('.look'), eyeEls[1].querySelector('.look')];
    const lids = [
      [eyeEls[0].querySelector('.lid-upper'), eyeEls[0].querySelector('.lid-lower')],
      [eyeEls[1].querySelector('.lid-upper'), eyeEls[1].querySelector('.lid-lower')],
    ];

    function setLid(pair, upper, lower) {
      pair[0].style.transform = 'scaleY(' + (1 - clamp01(upper)) + ')';
      pair[1].style.transform = 'scaleY(' + (1 - clamp01(lower)) + ')';
    }

    function render(p, nx, ny) {
      const shift = 'translate(' + (nx * LOOK_PX) + 'px,' + (ny * LOOK_PX) + 'px)';
      looks[0].style.transform = shift;
      looks[1].style.transform = shift;
      setLid(lids[0], p.lidUpperL, p.lidLowerL);
      setLid(lids[1], p.lidUpperR, p.lidLowerR);
    }

    // Before the first sample: lids open and dimmed, i.e. "no data".
    setLid(lids[0], 1, 1);
    setLid(lids[1], 1, 1);
    host.classList.add('eyes-stale');

    // --- staleness watchdog ----------------------------------------------
    let watchdog = null;
    let lastGoodAt = 0;
    function markStale() { host.classList.add('eyes-stale'); }
    function clearWatchdog() { if (watchdog) { clearTimeout(watchdog); watchdog = null; } }
    function armWatchdog() {
      clearWatchdog();
      watchdog = setTimeout(function () {
        watchdog = null;
        // Timers can fire a hair early/late; only dim if truly overdue.
        if (Date.now() - lastGoodAt >= STALE_AFTER_MS - 50) markStale();
        else armWatchdog();
      }, STALE_AFTER_MS);
    }

    // --- polling loop --------------------------------------------------------
    let stopped = false;     // permanent, set by stop()
    let authFailed = false;  // 401: no more polling until a page reload
    let stopPoll = null;     // non-null while a loop runs
    let gen = 0;             // bumped on every halt: late answers of an old loop are dropped
    let failures = 0;        // consecutive failed samples
    // With an observer, wait for its first report. No observer when onPose is set.
    let onScreen = !!opts.onPose || !window.IntersectionObserver;
    let observer = null;

    // The timeout also aborts the request: a fetch left pending keeps its
    // connection, and Chrome queues later GETs of the same URL behind it (HTTP
    // cache lock, up to 20 s), so the loop would not recover when the device
    // is back.
    function fetchPose() {
      let timer;
      const ctl = window.AbortController ? new AbortController() : null;
      const timeout = new Promise(function (_, reject) {
        timer = setTimeout(function () {
          reject(new Error('pose request timed out'));
          if (ctl) ctl.abort();
        }, REQUEST_TIMEOUT_MS);
      });
      const req = Api.getPose(ctl ? ctl.signal : undefined).then(function (p) {
        if (!p || typeof p !== 'object' || typeof p.panDeg !== 'number' || typeof p.tiltDeg !== 'number') {
          throw new Error('bad pose reply');
        }
        return p;
      });
      return Promise.race([req, timeout]).finally(function () { clearTimeout(timer); });
    }

    function delay(ms) { return new Promise(function (resolve) { setTimeout(resolve, ms); }); }

    function makeSample(myGen) {
      return function () {
        return fetchPose().then(function (p) {
          if (myGen !== gen) return;               // halted/replaced/stopped while in flight
          failures = 0;
          lastGoodAt = Date.now();
          host.classList.remove('eyes-stale');
          armWatchdog();
          const g = gazeNorm(p);
          render(p, g[0], g[1]);
          if (opts.onPose) {
            try { opts.onPose(p, g[0], g[1]); } catch (e) { console.error('onPose handler failed', e); }
          }
        }, function (err) {
          if (myGen !== gen) return;
          if (failures++ === 0) console.warn('pose poll failed', err);
          if (err && err.status === 401) {
            authFailed = true;                     // the browser's login prompt would just reappear
            halt();
            return;
          }
          if (err && err.status === 429) return delay(LOCKOUT_MS);
          return delay(Math.min(BACKOFF_MAX_MS, BACKOFF_BASE_MS * Math.pow(2, failures - 1)));
        });
      };
    }

    function shouldRun() {
      return !stopped && !authFailed && !document.hidden && onScreen;
    }
    function start() {
      if (stopPoll || !shouldRun()) return;
      stopPoll = Api.pollEvery(makeSample(++gen), POLL_GAP_MS);
    }
    function halt() {
      if (stopPoll) { stopPoll(); stopPoll = null; }
      gen++;
      clearWatchdog();
      markStale();   // a restored tab must not show an old pose as live
    }
    function update() { if (shouldRun()) start(); else halt(); }

    function onVisibility() { update(); }
    function onPageShow() { update(); }
    document.addEventListener('visibilitychange', onVisibility);
    window.addEventListener('pageshow', onPageShow);
    // Only when nobody consumes the samples: a page that feeds its own widgets
    // from onPose (gaze pad, status numbers) needs them while the eyes
    // themselves are scrolled away.
    if (!onScreen) {
      observer = new IntersectionObserver(function (entries) {
        onScreen = entries[entries.length - 1].isIntersecting;
        update();
      });
      observer.observe(host);
    }

    function stop() {
      stopped = true;
      document.removeEventListener('visibilitychange', onVisibility);
      window.removeEventListener('pageshow', onPageShow);
      if (observer) { observer.disconnect(); observer = null; }
      halt();
    }

    if (shouldRun()) start();
    return { stop: stop };
  }

  return { mount: mount };
})();
