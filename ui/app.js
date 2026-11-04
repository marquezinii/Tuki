(function () {
  "use strict";

  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

  // ---------------------------------------------------------------- vocabulary

  const ACTION_GROUPS = [
    ["Mouse", [
      ["default", "Padrão do botão"],
      ["disabled", "Desativado"],
      ["leftClick", "Clique esquerdo"],
      ["rightClick", "Clique direito"],
      ["middleClick", "Clique do meio"],
      ["doubleClick", "Clique duplo"],
      ["back", "Voltar"],
      ["forward", "Avançar"],
    ]],
    ["Edição", [
      ["copy", "Copiar (Ctrl+C)"],
      ["paste", "Colar (Ctrl+V)"],
      ["undo", "Desfazer (Ctrl+Z)"],
      ["closeTab", "Fechar aba (Ctrl+W)"],
    ]],
    ["Mídia", [
      ["playPause", "Tocar ou pausar"],
      ["nextTrack", "Próxima faixa"],
      ["prevTrack", "Faixa anterior"],
      ["volumeUp", "Aumentar volume"],
      ["volumeDown", "Diminuir volume"],
      ["mute", "Silenciar"],
    ]],
    ["Windows", [
      ["showDesktop", "Mostrar área de trabalho"],
      ["taskView", "Visão de tarefas"],
      ["screenshot", "Capturar parte da tela"],
    ]],
  ];
  const ACTION_LABEL = Object.fromEntries(ACTION_GROUPS.flatMap(([, items]) => items));

  const BUTTON_LABEL = {
    left: "Botão esquerdo",
    right: "Botão direito",
    middle: "Botão da roda",
    x1: "Lateral de trás",
    x2: "Lateral da frente",
  };

  const DIR_WORD = { up: "cima", down: "baixo", left: "a esquerda", right: "a direita" };

  // ---------------------------------------------------------------- state

  let state = null;
  let saveTimer = 0;
  let pointerTimer = 0;
  let statsTimer = 0;
  let currentPage = "scroll";

  const getPath = (path) => path.split(".").reduce((o, k) => (o == null ? o : o[k]), state && state.config);
  function setPath(path, value) {
    const keys = path.split(".");
    let o = state.config;
    for (const k of keys.slice(0, -1)) o = o[k];
    o[keys[keys.length - 1]] = value;
  }

  function saveConfig(delay = 0) {
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => tuki.send({ type: "setConfig", config: state.config }), delay);
  }

  const nf = new Intl.NumberFormat("pt-BR", { maximumFractionDigits: 2 });
  function format(kind, value) {
    if (kind === "times") return `${nf.format(value)}×`;
    if (kind === "ms") return `${value} ms`;
    return String(value);
  }

  function paintRange(range) {
    const min = Number(range.min);
    const max = Number(range.max);
    const pct = ((Number(range.value) - min) / (max - min)) * 100;
    range.style.setProperty("--fill", `${pct}%`);
  }

  // ---------------------------------------------------------------- render

  function render() {
    if (!state) return;

    $$("[data-bind]").forEach((el) => {
      const value = getPath(el.dataset.bind);
      if (el.classList.contains("switch")) {
        el.setAttribute("aria-checked", String(!!value));
      } else if (el.type === "range") {
        if (document.activeElement !== el) el.value = value;
        paintRange(el);
        const out = $(`output[data-out="${el.dataset.bind}"]`);
        if (out) out.textContent = format(el.dataset.format, Number(el.value));
      } else if (el.tagName === "SELECT") {
        el.value = value;
        el.toggleAttribute("data-changed", value !== "default");
      }
    });

    $$("[data-show-if]").forEach((el) => {
      el.hidden = !getPath(el.dataset.showIf);
    });

    $$('input[name="mode"]').forEach((r) => {
      r.checked = r.value === state.config.scroll.mode;
    });

    renderPower();
    renderPointer();
    renderApps();
    renderStats();
    renderGeneral();
  }

  function renderPower() {
    const paused = state.paused || !state.config.enabled;
    const power = $("#power");
    power.setAttribute("aria-checked", String(!paused));
    $("#power-title").textContent = paused ? "Tuki pausado" : "Tuki ativo";
    $("#power-hint").textContent = state.config.general.hotkey && state.hotkeyAvailable
      ? `${state.hotkey} ${paused ? "retoma" : "pausa"}`
      : `Clique para ${paused ? "retomar" : "pausar"}`;
    document.documentElement.toggleAttribute("data-paused", paused);
    $("#lens").dataset.state = paused ? "inactive" : "active";
    if (paused) setCaption("Tuki em pausa. A roda passa para o Windows sem alteração.");
    else if ($("#lens-caption").dataset.idle !== "false") setCaption(idleCaption());
  }

  function renderPointer() {
    const range = $("#pointer-speed");
    if (document.activeElement !== range) range.value = state.pointer.speed;
    paintRange(range);
    $("#pointer-speed-out").textContent = range.value;
    $("#pointer-precision").setAttribute("aria-checked", String(!!state.pointer.enhancePrecision));
  }

  function renderStats() {
    const s = state.stats;
    const n = (v) => Number(v).toLocaleString("pt-BR");
    $("#stat-notches").textContent = n(s.notches);
    $("#stat-corrected").textContent = n(s.corrected);
    $("#stat-clicks").textContent = n(s.clicks);
    $("#stat-ghost").textContent = n(s.ghostClicks);
    $("#ghost-count").textContent = s.ghostClicks > 0
      ? `Até agora o Tuki descartou ${n(s.ghostClicks)} cliques fantasmas.`
      : "";
  }

  function renderGeneral() {
    const desc = $("#hotkey-desc");
    const unavailable = state.config.general.hotkey && !state.hotkeyAvailable;
    desc.textContent = unavailable
      ? `Outro programa já usa ${state.hotkey}, então o atalho não está ativo.`
      : `${state.hotkey} liga e desliga o Tuki de qualquer lugar.`;
    desc.style.color = unavailable ? "var(--alert)" : "";
    $("#elevation-notice").hidden = !!state.elevated;
    $("#version").textContent = `Tuki ${state.version}. Configurações salvas em ${state.configPath}`;
  }

  // ---------------------------------------------------------------- navigation

  function showPage(name) {
    currentPage = name;
    $$(".page").forEach((p) => (p.hidden = p.id !== `page-${name}`));
    $$(".nav-item").forEach((b) => {
      if (b.dataset.page === name) b.setAttribute("aria-current", "page");
      else b.removeAttribute("aria-current");
    });
    $("#content").scrollTop = 0;
    try {
      localStorage.setItem("tuki.page", name);
    } catch (_) {
      /* storage can be unavailable */
    }
    clearInterval(statsTimer);
    if (name === "general" || name === "clicks") {
      tuki.send({ type: "getStats" });
      statsTimer = setInterval(() => tuki.send({ type: "getStats" }), 3000);
    }
    if (name === "pointer") tuki.send({ type: "ready" });
  }

  // ---------------------------------------------------------------- lens

  const MAX_MARKS = 18;
  const chevronTpl = $("#chevron");

  function idleCaption() {
    const mode = state ? state.config.scroll.mode : "normal";
    if (mode === "down") return "Gire a roda para qualquer lado: a tela vai sempre para baixo.";
    if (mode === "up") return "Gire a roda para qualquer lado: a tela vai sempre para cima.";
    if (mode === "inverted") return "Gire a roda: a tela vai para o lado oposto.";
    return "Gire a roda do mouse para ver o Tuki em ação.";
  }

  function setCaption(text) {
    const el = $("#lens-caption");
    if (el.textContent !== text) el.textContent = text;
  }

  function pushMark(track, dir, changed) {
    const mark = chevronTpl.content.firstElementChild.cloneNode(true);
    mark.dataset.dir = dir;
    if (changed) mark.setAttribute("data-changed", "");
    track.appendChild(mark);
    while (track.children.length > MAX_MARKS) track.firstElementChild.remove();
  }

  function onWheel(msg) {
    const inDir = msg.hIn ? (msg.raw > 0 ? "right" : "left") : msg.raw > 0 ? "up" : "down";
    let outDir = "none";
    if (!msg.dropped && msg.out !== 0) outDir = msg.hOut ? (msg.out > 0 ? "right" : "left") : msg.out > 0 ? "up" : "down";
    const changed = outDir !== "none" && outDir !== inDir;

    pushMark($("#lane-in"), inDir, false);
    pushMark($("#lane-out"), outDir, changed);

    const caption = $("#lens-caption");
    caption.dataset.idle = "false";
    if (msg.inactive) {
      setCaption("Tuki em pausa. A roda passa para o Windows sem alteração.");
    } else if (msg.dropped) {
      setCaption("Tremor descartado: esse giro voltou rápido demais para ser seu.");
    } else if (outDir === "none") {
      setCaption(`Você girou para ${DIR_WORD[inDir]}. O Tuki está juntando giros lentos.`);
    } else {
      setCaption(`Você girou para ${DIR_WORD[inDir]}. A tela rolou para ${DIR_WORD[outDir]}.`);
    }
  }

  function buildModeGlyphs() {
    const chevron = '<svg viewBox="0 0 12 12"><path d="M2.5 7.5 6 4l3.5 3.5"/></svg>';
    $$(".mode-glyph").forEach((g) => {
      const cells = [
        ...g.dataset.in.split(" ").map((d) => `<i data-dir="${d}">${chevron}</i>`),
        ...g.dataset.out.split(" ").map((d) => `<i class="out" data-dir="${d}">${chevron}</i>`),
      ];
      g.innerHTML = cells.join("");
    });
  }

  // ---------------------------------------------------------------- buttons

  function buildActionSelects() {
    const html = ACTION_GROUPS.map(
      ([group, items]) =>
        `<optgroup label="${group}">${items.map(([v, label]) => `<option value="${v}">${label}</option>`).join("")}</optgroup>`
    ).join("");
    $$(".action-select").forEach((s) => (s.innerHTML = html));
  }

  const releaseTimers = {};
  function onButton(msg) {
    const part = $(`.mouse-btn[data-btn="${msg.button}"]`);
    if (part) {
      clearTimeout(releaseTimers[msg.button]);
      if (msg.down) {
        const eaten = msg.debounced || (msg.blocked && !msg.action && !msg.mappedTo);
        part.toggleAttribute("data-blocked", eaten);
        part.toggleAttribute("data-pressed", !eaten);
      } else {
        releaseTimers[msg.button] = setTimeout(() => {
          part.removeAttribute("data-pressed");
          part.removeAttribute("data-blocked");
        }, 140);
      }
    }
    if (!msg.down) return;

    const label = BUTTON_LABEL[msg.button] || msg.button;
    let text;
    if (msg.inactive) text = `${label}: Tuki em pausa, função padrão.`;
    else if (msg.debounced) text = `${label}: clique fantasma descartado.`;
    else if (msg.action) text = `${label}: ${ACTION_LABEL[msg.action] || msg.action}.`;
    else if (msg.mappedTo) text = `${label} agiu como ${BUTTON_LABEL[msg.mappedTo].toLowerCase()}.`;
    else if (msg.blocked) text = `${label}: desativado.`;
    else text = `${label}: função padrão.`;
    $("#button-log").textContent = text;
  }

  // ---------------------------------------------------------------- click test pad

  const lastDown = {};
  const intervals = [];
  function onTestpadDown(e) {
    const t = e.timeStamp;
    const prev = lastDown[e.button];
    lastDown[e.button] = t;
    if (prev === undefined || t - prev > 1000) return;
    intervals.unshift(Math.round(t - prev));
    intervals.length = Math.min(intervals.length, 12);
    const limit = state ? state.config.debounce.ms : 60;
    $("#intervals").innerHTML = intervals
      .map((ms) => `<li${ms < limit ? " data-ghost" : ""}>${ms} ms</li>`)
      .join("");
  }

  // ---------------------------------------------------------------- apps

  function renderApps() {
    const list = $("#app-list");
    const apps = state.config.apps.excluded;
    list.innerHTML = "";
    for (const exe of apps) {
      const li = document.createElement("li");
      const name = document.createElement("span");
      name.className = "exe";
      name.textContent = exe;
      const remove = document.createElement("button");
      remove.className = "btn btn-quiet";
      remove.type = "button";
      remove.textContent = "Remover";
      remove.setAttribute("aria-label", `Remover ${exe}`);
      remove.addEventListener("click", () => {
        state.config.apps.excluded = state.config.apps.excluded.filter((a) => a !== exe);
        saveConfig();
        render();
      });
      li.append(name, remove);
      list.appendChild(li);
    }
    $("#app-empty").hidden = apps.length > 0;
    $$("#picker-list button").forEach((b) => (b.disabled = apps.includes(b.dataset.exe)));
  }

  function addApp(raw) {
    const input = $("#add-app-input");
    const error = $("#add-app-error");
    let exe = raw.trim().toLowerCase().split(/[\\/]/).pop();
    if (exe && !exe.endsWith(".exe")) exe += ".exe";
    let message = "";
    if (!/^[^\\/:*?"<>|]+\.exe$/.test(exe)) message = "Digite o nome do executável, como jogo.exe.";
    else if (state.config.apps.excluded.includes(exe)) message = `${exe} já está na lista.`;
    error.textContent = message;
    input.setAttribute("aria-invalid", String(!!message));
    if (message) return false;
    state.config.apps.excluded = [...state.config.apps.excluded, exe];
    saveConfig();
    render();
    return true;
  }

  function onApps(apps) {
    const list = $("#picker-list");
    list.innerHTML = "";
    if (!apps.length) {
      list.innerHTML = '<li class="empty">Nenhuma janela aberta encontrada.</li>';
      return;
    }
    for (const app of apps) {
      const li = document.createElement("li");
      const b = document.createElement("button");
      b.type = "button";
      b.dataset.exe = app.exe;
      b.disabled = state.config.apps.excluded.includes(app.exe);
      const exe = document.createElement("span");
      exe.className = "exe";
      exe.textContent = app.exe;
      const title = document.createElement("span");
      title.className = "title";
      title.textContent = app.title;
      b.append(exe, title);
      b.addEventListener("click", () => {
        if (addApp(app.exe)) $("#picker").hidden = true;
      });
      li.appendChild(b);
      list.appendChild(li);
    }
  }

  // ---------------------------------------------------------------- wiring

  function bindControls() {
    $$(".nav-item").forEach((b) => b.addEventListener("click", () => showPage(b.dataset.page)));

    $$(".switch[data-bind]").forEach((el) =>
      el.addEventListener("click", () => {
        setPath(el.dataset.bind, !getPath(el.dataset.bind));
        render();
        saveConfig();
      })
    );

    $$('input[type="range"][data-bind]').forEach((el) =>
      el.addEventListener("input", () => {
        setPath(el.dataset.bind, Number(el.value));
        paintRange(el);
        $(`output[data-out="${el.dataset.bind}"]`).textContent = format(el.dataset.format, Number(el.value));
        saveConfig(150);
      })
    );

    $$("select[data-bind]").forEach((el) =>
      el.addEventListener("change", () => {
        setPath(el.dataset.bind, el.value);
        render();
        saveConfig();
      })
    );

    $$('input[name="mode"]').forEach((r) =>
      r.addEventListener("change", () => {
        state.config.scroll.mode = r.value;
        $("#lens-caption").dataset.idle = "true";
        render();
        saveConfig();
      })
    );

    $("#power").addEventListener("click", () => {
      if (!state.config.enabled) {
        state.config.enabled = true;
        saveConfig();
      }
      tuki.send({ type: "setPaused", value: !state.paused });
    });

    const pointerRange = $("#pointer-speed");
    pointerRange.addEventListener("input", () => {
      paintRange(pointerRange);
      $("#pointer-speed-out").textContent = pointerRange.value;
      clearTimeout(pointerTimer);
      pointerTimer = setTimeout(() => tuki.send({ type: "setPointer", speed: Number(pointerRange.value) }), 120);
    });
    $("#pointer-precision").addEventListener("click", () => {
      tuki.send({ type: "setPointer", enhancePrecision: !state.pointer.enhancePrecision });
    });

    // Light up the part of the mouse a settings row talks about.
    $$("[data-hover-btn]").forEach((row) => {
      const parts = row.dataset.hoverBtn.split(" ").map((b) => $(`.mouse-btn[data-btn="${b}"]`));
      const on = () => parts.forEach((p) => p && p.setAttribute("data-hover", ""));
      const off = () => parts.forEach((p) => p && p.removeAttribute("data-hover"));
      row.addEventListener("mouseenter", on);
      row.addEventListener("mouseleave", off);
      row.addEventListener("focusin", on);
      row.addEventListener("focusout", off);
    });

    $("#testpad").addEventListener("mousedown", onTestpadDown);

    $("#add-app-form").addEventListener("submit", (e) => {
      e.preventDefault();
      const input = $("#add-app-input");
      if (addApp(input.value)) input.value = "";
    });
    $("#add-app-input").addEventListener("input", () => {
      $("#add-app-error").textContent = "";
      $("#add-app-input").removeAttribute("aria-invalid");
    });
    $("#pick-app").addEventListener("click", () => {
      const picker = $("#picker");
      picker.hidden = !picker.hidden;
      if (!picker.hidden) {
        $("#picker-list").innerHTML = '<li class="empty">Procurando janelas abertas...</li>';
        tuki.send({ type: "listApps" });
      }
    });

    $("#reset-stats").addEventListener("click", () => tuki.send({ type: "resetStats" }));
    $("#open-folder").addEventListener("click", () => tuki.send({ type: "openConfigFolder" }));
  }

  function onMessage(msg) {
    if (!msg || typeof msg !== "object") return;
    switch (msg.type) {
      case "state":
        state = msg;
        render();
        break;
      case "saved":
        if (state) {
          state.hotkeyAvailable = msg.hotkeyAvailable;
          renderPower();
          renderGeneral();
        }
        break;
      case "paused":
        if (state) {
          state.paused = msg.value;
          $("#lens-caption").dataset.idle = "true";
          renderPower();
        }
        break;
      case "wheel":
        if (state) onWheel(msg);
        break;
      case "button":
        onButton(msg);
        break;
      case "stats":
        if (state) {
          state.stats = msg.stats;
          renderStats();
        }
        break;
      case "pointer":
        if (state) {
          state.pointer = { speed: msg.speed, enhancePrecision: msg.enhancePrecision };
          renderPointer();
        }
        break;
      case "apps":
        onApps(msg.apps || []);
        break;
    }
  }

  // ---------------------------------------------------------------- start

  buildModeGlyphs();
  buildActionSelects();
  bindControls();
  let startPage = "scroll";
  try {
    const saved = localStorage.getItem("tuki.page");
    if (saved && $(`#page-${saved}`)) startPage = saved;
  } catch (_) {
    /* storage can be unavailable */
  }
  showPage(startPage);
  tuki.on(onMessage);
  tuki.send({ type: "ready" });
})();
