// api.js — shared fetch/polling wrapper used by every page (plan §7).
//
// No WebSocket infrastructure exists on the device yet (confirmed absent
// through Phase 6 — see PROGRESS.md) — everything here is REST polling.
// Every page is expected to go through Api.pollEvery()/Api.get()/Api.post()
// rather than calling fetch() directly, so that if/when a `/ws` endpoint is
// added later, only this one file needs to change, not every page.
//
// Plain global `Api` object, no module system, no build step.

const Api = (() => {
  // Password protection uses HTTP Digest: the browser shows its own login
  // prompt on a 401 and then signs every request itself, so nothing here
  // handles credentials. These only make the two auth failures readable.
  function statusMessage(status, data) {
    if (status === 401) return "Not logged in (or wrong password) — reload the page to log in";
    if (status === 429) return "Too many wrong passwords — wait 30 seconds and reload";
    return (data && (data.error || data.message)) || ("HTTP " + status);
  }

  async function request(method, path, body) {
    const opts = { method };
    if (body !== undefined) {
      opts.headers = { "Content-Type": "application/json" };
      opts.body = JSON.stringify(body);
    }
    const res = await fetch(path, opts);
    let data = null;
    try {
      data = await res.json();
    } catch (e) {
      // empty or non-JSON body — leave data null
    }
    if (!res.ok) {
      const err = new Error(statusMessage(res.status, data));
      err.status = res.status;
      err.data = data;
      throw err;
    }
    return data;
  }

  const get = (path) => request("GET", path);
  const post = (path, body) => request("POST", path, body === undefined ? {} : body);

  // Calls fn() immediately, then again every intervalMs, forever, until the
  // returned stop() function is called. fn may be async; a rejection is
  // passed to onError (if provided) and polling continues regardless — a
  // single failed request (e.g. device rebooting) must never kill the
  // polling loop for the rest of the page's life.
  function pollEvery(fn, intervalMs, onError) {
    let stopped = false;
    let timer = null;

    async function tick() {
      if (stopped) return;
      try {
        await fn();
      } catch (err) {
        if (onError) {
          try { onError(err); } catch (e) { /* ignore handler errors */ }
        }
      }
      if (!stopped) {
        timer = setTimeout(tick, intervalMs);
      }
    }

    tick();
    return function stop() {
      stopped = true;
      if (timer) clearTimeout(timer);
    };
  }

  // Rate-limits fn to at most once per intervalMs, always eventually firing
  // with the latest call's arguments (trailing edge). Used by the manual
  // gaze pad so a fast drag doesn't fire a request per pixel of movement.
  function throttle(fn, intervalMs) {
    let last = 0;
    let pendingArgs = null;
    let timer = null;

    function fire(args) {
      last = Date.now();
      fn(...args);
    }

    return (...args) => {
      const now = Date.now();
      const remaining = intervalMs - (now - last);
      if (remaining <= 0) {
        pendingArgs = null;
        fire(args);
      } else {
        pendingArgs = args;
        if (!timer) {
          timer = setTimeout(() => {
            timer = null;
            if (pendingArgs) {
              const a = pendingArgs;
              pendingArgs = null;
              fire(a);
            }
          }, remaining);
        }
      }
    };
  }

  // Multipart file upload via XMLHttpRequest (not fetch()) so upload
  // progress events are available for a progress bar (setup/ota.html).
  function uploadFile(path, file, onProgress) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open("POST", path, true);
      const form = new FormData();
      form.append("file", file, file.name);
      if (xhr.upload && onProgress) {
        xhr.upload.onprogress = (e) => {
          if (e.lengthComputable) onProgress(e.loaded / e.total);
        };
      }
      xhr.onload = () => {
        let data = null;
        try { data = JSON.parse(xhr.responseText); } catch (e) { /* ignore */ }
        if (xhr.status >= 200 && xhr.status < 300) {
          resolve(data);
        } else {
          const err = new Error(statusMessage(xhr.status, data));
          err.status = xhr.status;
          err.data = data;
          reject(err);
        }
      };
      xhr.onerror = () => reject(new Error("network_error"));
      xhr.send(form);
    });
  }

  return {
    get,
    post,
    pollEvery,
    throttle,
    uploadFile,

    // --- system -------------------------------------------------------
    getSystemInfo: () => get("/api/system/info"),
    getSystemStatus: () => get("/api/system/status"),
    setSystemConfig: (cfg) => post("/api/system/config", cfg),
    reboot: () => post("/api/system/reboot"),

    // --- passwords ------------------------------------------------------
    getAuthStatus: () => get("/api/auth/status"),
    setPassword: (level, password) => post("/api/auth/password", { level, password }),

    // --- wifi -----------------------------------------------------------
    wifiScan: () => get("/api/wifi/scan"),
    wifiConnect: (ssid, password) => post("/api/wifi/connect", { ssid, password }),
    wifiForget: () => post("/api/wifi/forget"),

    // --- servos / calibration -------------------------------------------
    getServoConfig: () => get("/api/servos/config"),
    setServoConfig: (cfg) => post("/api/servos/config", cfg),
    testServo: (servoId, pulseUs) => post("/api/servos/test", { servoId, pulseUs }),
    servoHold: (enabled) => post("/api/servos/hold", { enabled }),
    servoPose: (pulses) => post("/api/servos/pose", { pulses }),

    // --- manual eye control ----------------------------------------------
    setGaze: (fields) => post("/api/eyes/gaze", fields),
    setEyelids: (fields) => post("/api/eyes/eyelids", fields),
    getPose: () => get("/api/eyes/pose"),
    setNaturalMode: (enabled) => post("/api/eyes/natural", { enabled }),

    // --- gestures ---------------------------------------------------------
    getGestures: () => get("/api/gestures"),
    triggerGesture: (id) => post("/api/gestures/" + encodeURIComponent(id) + "/trigger"),

    // --- play modes ---------------------------------------------------------
    getPlayModes: () => get("/api/playmodes"),
    getActivePlayMode: () => get("/api/playmodes/active"),
    activatePlayMode: (id) => post("/api/playmodes/" + encodeURIComponent(id) + "/activate"),

    // --- radar ---------------------------------------------------------------
    getRadarStatus: () => get("/api/radar/status"),
    getRadarLatest: () => get("/api/radar/latest"),
    getRadarConfig: () => get("/api/radar/config"),
    setRadarConfig: (cfg) => post("/api/radar/config", cfg),

    // --- LED -------------------------------------------------------------------
    getLedConfig: () => get("/api/led/config"),
    setLedConfig: (cfg) => post("/api/led/config", cfg),

    // --- OTA -------------------------------------------------------------------
    getOtaStatus: () => get("/api/ota/status"),
  };
})();
