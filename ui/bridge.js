// Messaging with the native host (WebView2).
// Opened in a regular browser, it falls back to a small simulator so the UI can be previewed.
(function () {
  "use strict";
  const listeners = new Set();
  const emit = (msg) => listeners.forEach((fn) => fn(msg));
  const webview = window.chrome && window.chrome.webview;

  if (webview) {
    webview.addEventListener("message", (e) => emit(e.data));
    window.tuki = {
      native: true,
      send: (msg) => webview.postMessage(msg),
      on: (fn) => listeners.add(fn),
    };
    return;
  }

  // ---------------------------------------------------------------- simulator
  const sim = {
    config: {
      enabled: true,
      scroll: { mode: "down", speed: 1, accel: false, accelStrength: 1, shiftHorizontal: false, invertHorizontal: false, jitterFilter: false, jitterMs: 80 },
      buttons: { swapLeftRight: false, middle: "default", x1: "default", x2: "copy" },
      debounce: { left: false, right: false, middle: false, ms: 60 },
      apps: { excluded: ["valorant.exe"] },
      general: { startWithWindows: true, hotkey: true },
    },
    stats: { clicks: 18342, notches: 50211, corrected: 23870, ghostClicks: 41, jitterDropped: 12 },
    paused: false,
    pointer: { speed: 10, enhancePrecision: true },
  };
  const later = (msg) => setTimeout(() => emit(msg), 0);
  const state = () => ({
    type: "state",
    config: JSON.parse(JSON.stringify(sim.config)),
    stats: { ...sim.stats },
    paused: sim.paused,
    pointer: { ...sim.pointer },
    version: "0.1.0 (prévia no navegador)",
    elevated: false,
    hotkey: "Ctrl+Alt+F12",
    hotkeyAvailable: true,
    configPath: "C:\\Users\\voce\\AppData\\Roaming\\Tuki\\config.json",
  });

  function direct(mode, d) {
    if (mode === "inverted") return -d;
    if (mode === "down") return -Math.abs(d);
    if (mode === "up") return Math.abs(d);
    return d;
  }

  window.addEventListener(
    "wheel",
    (e) => {
      const horizontal = Math.abs(e.deltaX) > Math.abs(e.deltaY);
      const raw = horizontal ? (e.deltaX > 0 ? 120 : -120) : e.deltaY < 0 ? 120 : -120;
      const s = sim.config.scroll;
      const active = sim.config.enabled && !sim.paused;
      let out = raw;
      let hOut = horizontal;
      if (active) {
        if (horizontal) out = Math.round(raw * (s.invertHorizontal ? -1 : 1) * s.speed);
        else {
          out = Math.round(direct(s.mode, raw) * s.speed);
          if (s.shiftHorizontal && e.shiftKey) {
            out = -out;
            hOut = true;
          }
        }
      }
      sim.stats.notches++;
      emit({ type: "wheel", raw, out, hIn: horizontal, hOut, dropped: false, inactive: !active });
    },
    { passive: true }
  );

  const names = ["left", "middle", "right", "x1", "x2"];
  const press = (e, down) => {
    const button = names[e.button] || "left";
    const action = down && sim.config.buttons[button];
    const msg = { type: "button", button, down, blocked: false, debounced: false, inactive: sim.paused };
    if (action && action !== "default") {
      msg.blocked = true;
      msg.action = action;
    }
    if (down) sim.stats.clicks++;
    emit(msg);
  };
  window.addEventListener("mousedown", (e) => press(e, true));
  window.addEventListener("mouseup", (e) => press(e, false));
  window.addEventListener("contextmenu", (e) => e.preventDefault());

  window.tuki = {
    native: false,
    on: (fn) => listeners.add(fn),
    send(msg) {
      switch (msg.type) {
        case "ready":
          later(state());
          break;
        case "setConfig":
          sim.config = JSON.parse(JSON.stringify(msg.config));
          later({ type: "saved", hotkeyAvailable: true });
          break;
        case "setPaused":
          sim.paused = !!msg.value;
          later({ type: "paused", value: sim.paused });
          break;
        case "listApps":
          later({
            type: "apps",
            apps: [
              { exe: "chrome.exe", title: "Nova guia - Google Chrome" },
              { exe: "code.exe", title: "main.cpp - Tuki - Visual Studio Code" },
              { exe: "explorer.exe", title: "Downloads" },
              { exe: "valorant.exe", title: "VALORANT" },
            ],
          });
          break;
        case "getStats":
          later({ type: "stats", stats: { ...sim.stats } });
          break;
        case "resetStats":
          Object.keys(sim.stats).forEach((k) => (sim.stats[k] = 0));
          later({ type: "stats", stats: { ...sim.stats } });
          break;
        case "setPointer":
          if (msg.speed !== undefined) sim.pointer.speed = msg.speed;
          if (msg.enhancePrecision !== undefined) sim.pointer.enhancePrecision = msg.enhancePrecision;
          later({ type: "pointer", ...sim.pointer });
          break;
      }
    },
  };
})();
