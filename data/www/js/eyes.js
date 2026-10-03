// eyes.js — a live pair of cartoon eyes showing what the firmware is
// *commanding* the real eyes to do (pan/tilt and the four lids).
//
// Hobby servos give no position feedback, so this is the pose MotionTask
// publishes (the interpolated target it sends to the servos every 20 ms),
// not a measurement. Needs js/api.js loaded first. Plain global, no build.
//
//   MagicEyes.mount(hostElement, { intervalMs: 30, onPose: fn });
//
// A blink is only ~300 ms, so the next request goes out almost as soon as the
// last one lands (the device answers in 30-100 ms, ~12 samples/s); a slower
// poll turns a blink into one smoothed-away sample. Pages that already poll
// the pose can take it from onPose instead of polling again. Polling pauses
// while the tab is hidden.

const MagicEyes = (() => {
  const GAZE_RANGE_DEG = 45; // same working range as the Manual page's gaze pad
  const LOOK_PX = 20;        // iris travel at full gaze, in the 100x100 eye viewBox
  let mounts = 0;            // clipPath ids must be unique per page

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

  function mount(host, opts) {
    opts = opts || {};
    const intervalMs = opts.intervalMs || 30;
    const n = ++mounts;
    host.classList.add('eyes-view');
    host.setAttribute('aria-hidden', 'true');
    host.innerHTML = eyeSvg('eye-clip-' + n + 'l') + eyeSvg('eye-clip-' + n + 'r');
    const eyes = host.querySelectorAll('.eye');

    function setLid(eye, upper, lower) {
      eye.querySelector('.lid-upper').style.transform = 'scaleY(' + (1 - clamp01(upper)) + ')';
      eye.querySelector('.lid-lower').style.transform = 'scaleY(' + (1 - clamp01(lower)) + ')';
    }

    function render(p) {
      // Shared yoke: both irises move together. Up on screen = positive tilt.
      let nx = p.panDeg / GAZE_RANGE_DEG;
      let ny = -p.tiltDeg / GAZE_RANGE_DEG;
      const mag = Math.hypot(nx, ny);
      if (mag > 1) { nx /= mag; ny /= mag; }
      const shift = 'translate(' + (nx * LOOK_PX) + 'px,' + (ny * LOOK_PX) + 'px)';
      eyes.forEach(function (eye) { eye.querySelector('.look').style.transform = shift; });
      setLid(eyes[0], p.lidUpperL, p.lidLowerL);
      setLid(eyes[1], p.lidUpperR, p.lidLowerR);
    }

    let stop = null;
    function start() {
      if (stop) return;
      stop = Api.pollEvery(function () {
        return Api.getPose().then(function (p) {
          host.classList.remove('eyes-stale');
          render(p);
          if (opts.onPose) opts.onPose(p);
        });
      }, intervalMs, function () { host.classList.add('eyes-stale'); });
    }
    function halt() { if (stop) { stop(); stop = null; } }

    document.addEventListener('visibilitychange', function () {
      if (document.hidden) halt(); else start();
    });
    if (!document.hidden) start();
    return { stop: halt };
  }

  return { mount: mount };
})();
