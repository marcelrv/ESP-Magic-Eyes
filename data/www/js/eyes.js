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
// onPose gets the pose at the moment a reply is handled (all six values, same
// field names as the API) plus nx, ny: the unit-circle-clamped gaze the eyes
// themselves draw (ny positive = down on screen). It is called once per good
// reply, not per animation frame. A throwing onPose is logged and never counts
// as a poll failure.
//
// How the eyes move: GET /api/eyes/pose also returns, per axis, the
// interpolation segment the firmware is on ([from, to, durationMs, elapsedMs,
// easing]). The browser keeps those segments and evaluates the same curve with
// requestAnimationFrame, so a blink is drawn exactly, including the ~40 ms
// fully-closed hold that sampling would miss. That needs only ~4-5 requests/s
// (SEG_POLL_GAP_MS): every blink phase the eyes see is animated locally to its
// end, and a blink lasts ~300 ms, so a reply always lands inside it. When a
// reply's segment is already finished, or the firmware is too old to send
// `seg` (then the six plain values are targets and the poll runs back-to-back,
// POLL_GAP_MS between requests, ~12 samples/s), a displayed value that is off
// the new target is glided there over SNAP_MS instead of snapping. A segment
// that is still running but not where the eyes are drawn is finished from the
// drawn value in its remaining time, except a lid caught only while reopening:
// that reopening is replayed from fully shut so the blink is not lost. With
// prefers-reduced-motion nothing is interpolated: each reply shows where the
// axes are heading.
//
// `held: true` means the calibration page has the servos on raw pulses: the
// pose describes nothing real, so the eyes are dimmed and kept as they are
// until a reply without it arrives.
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
  // Extra wait after a good reply that carried segments, so ~150 ms between
  // requests (~4-5/s with the device's own 30-100 ms answer time). Safe because
  // the eyes animate every blink phase locally to its end and a blink lasts
  // ~300 ms: at least one reply always lands inside it and shows the rest.
  const SEG_POLL_EXTRA_MS = 120;
  const SNAP_MS = 90;          // glide length when a displayed value is off the new target
  const GAZE_EPS_DEG = 0.5;    // "off target" thresholds for that glide
  const LID_EPS = 0.01;
  // Axis order of `seg`, same as the API's pose fields.
  const FIELDS = ['panDeg', 'tiltDeg', 'lidUpperL', 'lidLowerL', 'lidUpperR', 'lidLowerR'];
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

  // Mirrors applyEasing() in src/motion/eye_pose.cpp: API easing 1 is the
  // smoothstep, anything else linear.
  function ease(kind, t) { return kind === 1 ? t * t * (3 - 2 * t) : t; }

  // Value of a local segment at time t (performance.now() clock).
  function segValue(s, t) {
    const el = t - s.startAt;
    if (s.durationMs === 0 || el >= s.durationMs) return s.to;
    return s.from + (s.to - s.from) * ease(s.easing, Math.max(0, el) / s.durationMs);
  }
  function segDone(s, t) { return s.durationMs === 0 || t - s.startAt >= s.durationMs; }

  // A usable `seg`: six rows of five finite numbers. Anything else is treated
  // as an older firmware that sends none.
  function validSeg(seg) {
    if (!Array.isArray(seg) || seg.length !== FIELDS.length) return false;
    return seg.every(function (r) {
      return Array.isArray(r) && r.length >= 5 &&
        r.slice(0, 5).every(function (v) { return typeof v === 'number' && isFinite(v); });
    });
  }

  function mount(host, opts) {
    opts = opts || {};
    const n = ++mounts;
    host.classList.add('eyes-view');
    host.setAttribute('aria-hidden', 'true');
    host.innerHTML = eyeSvg('eye-clip-' + n + 'l') + eyeSvg('eye-clip-' + n + 'r');
    // Captured once: render() runs on every animation frame.
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

    // --- local animation -----------------------------------------------------
    // One segment per axis (see the header), plus what is currently drawn: a
    // new reply whose segment is already over starts a glide from there.
    const t0 = performance.now();
    const segs = FIELDS.map(function (_, i) {
      const v = i < 2 ? 0 : 1;
      return { from: v, to: v, durationMs: 0, easing: 0, startAt: t0 };
    });
    const shown = FIELDS.map(function (_, i) { return i < 2 ? 0 : 1; });
    let rafId = null;
    let dirty = false;       // a reply landed since the last drawn frame

    // prefers-reduced-motion: the CSS rule only covers the opacity fade, the
    // pose is animated here. Asked per reply so a changed setting applies
    // without a reload.
    const motionQuery = window.matchMedia ? window.matchMedia('(prefers-reduced-motion: reduce)') : null;
    function reducedMotion() { return !!motionQuery && motionQuery.matches; }

    // Stops every axis where it is drawn right now: the last drawn value, not
    // the curve's value at `now`, which is up to a frame further along and
    // would move the eyes once more after the hold began.
    function freeze(now) {
      for (let i = 0; i < FIELDS.length; i++) {
        const v = shown[i];
        segs[i] = { from: v, to: v, durationMs: 0, easing: 0, startAt: now };
      }
      dirty = true;
    }

    function currentPose(t) {
      const p = {};
      for (let i = 0; i < FIELDS.length; i++) p[FIELDS[i]] = segValue(segs[i], t);
      return p;
    }

    // Takes a good, non-held reply's segments (or, without `seg`, its six
    // values as targets) and starts them at time `now`.
    function applySamples(p, now) {
      const useSeg = validSeg(p.seg);
      for (let i = 0; i < FIELDS.length; i++) {
        let s;
        if (useSeg) {
          const r = p.seg[i];
          const dur = Math.max(0, r[2]);
          s = { from: r[0], to: r[1], durationMs: dur, easing: r[4], startAt: now - Math.min(Math.max(0, r[3]), dur) };
        } else {
          s = { from: p[FIELDS[i]], to: p[FIELDS[i]], durationMs: 0, easing: 0, startAt: now };
        }
        const eps = i < 2 ? GAZE_EPS_DEG : LID_EPS;
        if (reducedMotion()) {
          // No interpolation at all: show where each axis is heading.
          s = { from: s.to, to: s.to, durationMs: 0, easing: 0, startAt: now };
        } else if (!segDone(s, now)) {
          // Mid-segment but not where we are drawn (first reply, a missed
          // segment, the end of a hold): finish it from the drawn value in the
          // time it has left, instead of jumping onto the curve.
          if (i >= 2 && s.to > s.from && shown[i] - s.from > eps) {
            // A lid reopening from further shut than it is drawn: the closing
            // half of a blink fell between two replies. Play the reopening
            // from its start (a moment late) so the blink is seen fully shut;
            // easing in from the drawn value would hide it.
            s.startAt = now;
          } else if (Math.abs(shown[i] - segValue(s, now)) > eps) {
            s = { from: shown[i], to: s.to, durationMs: s.durationMs - (now - s.startAt), easing: s.easing, startAt: now };
          }
        } else if (Math.abs(shown[i] - s.to) > eps) {
          // Finished (or no segment info) and not where we are drawn: glide, not snap.
          s = { from: shown[i], to: s.to, durationMs: SNAP_MS, easing: 0, startAt: now };
        }
        segs[i] = s;
      }
      dirty = true;
    }

    // Pan/tilt clamped to the gaze range first, then gazeNorm's circle clamp.
    function gazeOf(p) {
      const lim = Api.GAZE_RANGE_DEG;
      return gazeNorm({ panDeg: Math.max(-lim, Math.min(lim, p.panDeg)),
                        tiltDeg: Math.max(-lim, Math.min(lim, p.tiltDeg)) });
    }

    function frame() {
      rafId = requestAnimationFrame(frame);
      const t = performance.now();
      let moving = false;
      for (let i = 0; i < FIELDS.length; i++) {
        if (!segDone(segs[i], t)) moving = true;
      }
      // Idle: nothing changes between replies, so skip the style writes.
      if (!moving && !dirty) return;
      dirty = false;
      const p = currentPose(t);
      for (let i = 0; i < FIELDS.length; i++) shown[i] = p[FIELDS[i]];
      const g = gazeOf(p);
      render(p, g[0], g[1]);
    }
    function startFrames() { if (rafId === null) rafId = requestAnimationFrame(frame); }
    function stopFrames() { if (rafId !== null) { cancelAnimationFrame(rafId); rafId = null; } }

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
        // All six values are required: without `seg` they are the targets.
        if (!p || typeof p !== 'object' || !FIELDS.every(function (f) { return typeof p[f] === 'number' && isFinite(p[f]); })) {
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
          const segMode = validSeg(p.seg);
          if (p.held === true) {
            // Calibration owns the servos: the pose is fiction. Keep the eyes
            // as they are but dimmed, and do not re-arm the watchdog as fresh.
            // Frozen, or a segment still running (a gaze move can last up to
            // 60 s) would keep animating under the dimming.
            freeze(performance.now());
            clearWatchdog();
            markStale();
          } else {
            lastGoodAt = Date.now();
            host.classList.remove('eyes-stale');
            armWatchdog();
            const now = performance.now();
            applySamples(p, now);
            if (opts.onPose) {
              const cur = currentPose(now);
              const g = gazeOf(cur);
              try { opts.onPose(cur, g[0], g[1]); } catch (e) { console.error('onPose handler failed', e); }
            }
          }
          if (segMode) return delay(SEG_POLL_EXTRA_MS);
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
      startFrames();
    }
    function halt() {
      if (stopPoll) { stopPoll(); stopPoll = null; }
      stopFrames();
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
