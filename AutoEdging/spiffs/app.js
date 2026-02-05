const el = {
  conn: document.getElementById('conn'),
  pressure: document.getElementById('pressure'),
  dac: document.getElementById('dac'),
  pwm: document.getElementById('pwm'),
  ble: document.getElementById('ble'),
  chartMeta: document.getElementById('chart-meta'),
  toast: document.getElementById('toast'),
  game: {
    state: document.getElementById('game-state'),
    running: document.getElementById('game-running'),
    intensity: document.getElementById('game-intensity'),
    target: document.getElementById('game-target'),
    pressure: document.getElementById('game-pressure'),
    thresholds: document.getElementById('game-thresholds'),
    counts: document.getElementById('game-counts'),
    stimTime: document.getElementById('game-stim-time'),
    toast: document.getElementById('game-toast'),
    configToast: document.getElementById('game-config-toast'),
    inputs: {
      duration: document.getElementById('game-duration'),
      critical: document.getElementById('game-critical'),
      mid: document.getElementById('game-mid'),
      max: document.getElementById('game-max'),
      delay: document.getElementById('game-delay'),
      rate: document.getElementById('game-rate'),
      sensitivity: document.getElementById('game-sensitivity'),
      random: document.getElementById('game-random'),
      increase: document.getElementById('game-increase'),
      shockVoltage: document.getElementById('game-shock-voltage'),
      midMin: document.getElementById('game-mid-min'),
      pwm0: document.getElementById('game-pwm0'),
      pwm1: document.getElementById('game-pwm1'),
      pwm2: document.getElementById('game-pwm2'),
      pwm3: document.getElementById('game-pwm3'),
      pwm0Min: document.getElementById('game-pwm0-min'),
      pwm1Min: document.getElementById('game-pwm1-min'),
      pwm2Min: document.getElementById('game-pwm2-min'),
      pwm3Min: document.getElementById('game-pwm3-min'),
    },
    buttons: {
      start: document.getElementById('btn-game-start'),
      pause: document.getElementById('btn-game-pause'),
      stop: document.getElementById('btn-game-stop'),
      shock: document.getElementById('btn-game-shock'),
      save: document.getElementById('btn-game-save'),
      reset: document.getElementById('btn-game-reset'),
    },
  },
  inputs: {
    sample: document.getElementById('cfg-sample'),
    ws: document.getElementById('cfg-ws'),
    window: document.getElementById('cfg-window'),
    dacCode: document.getElementById('cfg-dac-code'),
    dacPd: document.getElementById('cfg-dac-pd'),
    pwm0: document.getElementById('cfg-pwm0'),
    pwm1: document.getElementById('cfg-pwm1'),
    pwm2: document.getElementById('cfg-pwm2'),
    pwm3: document.getElementById('cfg-pwm3'),
    bleSwing: document.getElementById('cfg-ble-swing'),
    bleVibrate: document.getElementById('cfg-ble-vibrate'),
  },
  buttons: {
    save: document.getElementById('btn-save'),
    reset: document.getElementById('btn-reset'),
  },
};

let points = [];
let windowSec = 60;
let ws = null;
let reconnectDelay = 500;
let lastPressure = null;
let gameStatus = {};
let gameConfig = {};
let gameRunning = false;

const GAME_STATE_TEXT = {
  INITIAL_CALM: '初始平静期',
  MIDDLE: '中期抑制期',
  EDGING: '边缘等待回落',
  DELAY: '冷却倒计时',
  SUB_CALM: '后续平静期',
};

const CHART_Y_MIN = 0;
const CHART_Y_MAX = 41;

function getGameThresholds() {
  const midRaw = Number(gameConfig.midPressure);
  const critRaw = Number(gameConfig.criticalPressure);
  const mid = Number.isFinite(midRaw) ? midRaw : null;
  const critical = Number.isFinite(critRaw) ? critRaw : null;
  return { mid, critical };
}

function updatePressureColor(pressure) {
  if (!el.pressure) return;
  const { mid, critical } = getGameThresholds();
  if (!Number.isFinite(pressure) || !Number.isFinite(mid) || !Number.isFinite(critical)) {
    el.pressure.classList.remove('pressure-high', 'pressure-low', 'pressure-mid');
    return;
  }
  if (pressure >= critical) {
    el.pressure.classList.add('pressure-high');
    el.pressure.classList.remove('pressure-low', 'pressure-mid');
  } else if (pressure >= mid) {
    el.pressure.classList.add('pressure-mid');
    el.pressure.classList.remove('pressure-low', 'pressure-high');
  } else {
    el.pressure.classList.add('pressure-low');
    el.pressure.classList.remove('pressure-high', 'pressure-mid');
  }
}

const chart = {
  canvas: document.getElementById('chart'),
  ctx: null,
  width: 0,
  height: 0,
  resize() {
    const dpr = window.devicePixelRatio || 1;
    this.width = this.canvas.clientWidth;
    this.height = this.canvas.clientHeight;
    this.canvas.width = this.width * dpr;
    this.canvas.height = this.height * dpr;
    this.ctx = this.canvas.getContext('2d');
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    renderChart();
  },
};

function setToast(msg, isError = false) {
  el.toast.textContent = msg || '';
  el.toast.style.color = isError ? '#ffb3b3' : 'rgba(230,237,243,0.65)';
}

function setGameToast(msg, isError = false) {
  if (!el.game.toast) return;
  el.game.toast.textContent = msg || '';
  el.game.toast.style.color = isError ? '#ffb3b3' : 'rgba(230,237,243,0.65)';
}

function setGameConfigToast(msg, isError = false) {
  if (!el.game.configToast) return;
  el.game.configToast.textContent = msg || '';
  el.game.configToast.style.color = isError ? '#ffb3b3' : 'rgba(230,237,243,0.65)';
}

function setConnection(online) {
  if (online) {
    el.conn.textContent = '在线';
    el.conn.classList.remove('offline');
    el.conn.classList.add('online');
  } else {
    el.conn.textContent = '离线';
    el.conn.classList.remove('online');
    el.conn.classList.add('offline');
  }
}

function updateStatus(data) {
  if (!data) return;
  if (typeof data.pressure_kpa === 'number') {
    el.pressure.textContent = data.pressure_kpa.toFixed(2) + ' kPa';
    lastPressure = data.pressure_kpa;
    updatePressureColor(lastPressure);
  }
  if (data.dac) {
    const code = data.dac.code ?? data.dac_code ?? 0;
    const voltage = data.dac.voltage ?? 0;
    el.dac.textContent = `code ${code}, ${voltage.toFixed(3)} V`;
  }
  if (Array.isArray(data.pwm)) {
    el.pwm.textContent = data.pwm
      .map(v => `${(Number(v) / 10).toFixed(1)}%`)
      .join(' / ');
  }
  if (data.ble) {
    el.ble.textContent = `swing ${data.ble.swing}, vibrate ${data.ble.vibrate}`;
  }
}

function updateConfigForm(cfg) {
  if (!cfg) return;
  el.inputs.sample.value = cfg.sample_hz ?? 25;
  el.inputs.ws.value = cfg.ws_hz ?? 5;
  el.inputs.window.value = cfg.window_sec ?? 60;
  if (cfg.dac) {
    el.inputs.dacCode.value = cfg.dac.code ?? 0;
    el.inputs.dacPd.value = cfg.dac.pd_mode ?? 0;
  }
  if (Array.isArray(cfg.pwm)) {
    el.inputs.pwm0.value = ((cfg.pwm[0] ?? 0) / 10).toFixed(1);
    el.inputs.pwm1.value = ((cfg.pwm[1] ?? 0) / 10).toFixed(1);
    el.inputs.pwm2.value = ((cfg.pwm[2] ?? 0) / 10).toFixed(1);
    el.inputs.pwm3.value = ((cfg.pwm[3] ?? 0) / 10).toFixed(1);
  }
  if (cfg.ble) {
    el.inputs.bleSwing.value = cfg.ble.swing ?? 0;
    el.inputs.bleVibrate.value = cfg.ble.vibrate ?? 0;
  }
  if (typeof cfg.window_sec === 'number') {
    windowSec = cfg.window_sec;
    el.chartMeta.textContent = `最近 ${windowSec} 秒`;
  }
  updatePressureColor(lastPressure);
  renderChart();
}

function setManualEnabled(enabled) {
  Object.values(el.inputs).forEach((input) => {
    if (input) input.disabled = !enabled;
  });
  Object.values(el.buttons).forEach((btn) => {
    if (btn) btn.disabled = !enabled;
  });
}

function collectConfig() {
  return {
    sample_hz: parseInt(el.inputs.sample.value, 10),
    ws_hz: parseInt(el.inputs.ws.value, 10),
    window_sec: parseInt(el.inputs.window.value, 10),
    dac: {
      code: parseInt(el.inputs.dacCode.value, 10),
      pd_mode: parseInt(el.inputs.dacPd.value, 10),
    },
    pwm: [
      Math.round(parseFloat(el.inputs.pwm0.value || '0') * 10),
      Math.round(parseFloat(el.inputs.pwm1.value || '0') * 10),
      Math.round(parseFloat(el.inputs.pwm2.value || '0') * 10),
      Math.round(parseFloat(el.inputs.pwm3.value || '0') * 10),
    ],
    ble: {
      swing: parseInt(el.inputs.bleSwing.value, 10),
      vibrate: parseInt(el.inputs.bleVibrate.value, 10),
    },
  };
}

function updateGameConfigForm(cfg) {
  if (!cfg) return;
  gameConfig = cfg;
  el.game.inputs.duration.value = cfg.duration ?? 20;
  el.game.inputs.critical.value = cfg.criticalPressure ?? 20;
  el.game.inputs.mid.value = cfg.midPressure ?? 18;
  el.game.inputs.max.value = cfg.maxMotorIntensity ?? 50;
  el.game.inputs.delay.value = cfg.lowPressureDelay ?? 5;
  el.game.inputs.rate.value = cfg.stimulationRampRateLimit ?? 2;
  el.game.inputs.sensitivity.value = cfg.pressureSensitivity ?? 15;
  el.game.inputs.random.value = cfg.stimulationRampRandomPercent ?? 0;
  el.game.inputs.increase.value = cfg.intensityGradualIncrease ?? 2;
  el.game.inputs.shockVoltage.value = cfg.shockIntensity ?? 1.2;
  el.game.inputs.midMin.value = cfg.midMinIntensity ?? 5;
  if (Array.isArray(cfg.pwmMaxPermille)) {
    el.game.inputs.pwm0.value = ((cfg.pwmMaxPermille[0] ?? 0) / 10).toFixed(1);
    el.game.inputs.pwm1.value = ((cfg.pwmMaxPermille[1] ?? 0) / 10).toFixed(1);
    el.game.inputs.pwm2.value = ((cfg.pwmMaxPermille[2] ?? 0) / 10).toFixed(1);
    el.game.inputs.pwm3.value = ((cfg.pwmMaxPermille[3] ?? 0) / 10).toFixed(1);
  }
  if (Array.isArray(cfg.pwmMinPermille)) {
    el.game.inputs.pwm0Min.value = ((cfg.pwmMinPermille[0] ?? 0) / 10).toFixed(1);
    el.game.inputs.pwm1Min.value = ((cfg.pwmMinPermille[1] ?? 0) / 10).toFixed(1);
    el.game.inputs.pwm2Min.value = ((cfg.pwmMinPermille[2] ?? 0) / 10).toFixed(1);
    el.game.inputs.pwm3Min.value = ((cfg.pwmMinPermille[3] ?? 0) / 10).toFixed(1);
  } else {
    el.game.inputs.pwm0Min.value = 0;
    el.game.inputs.pwm1Min.value = 0;
    el.game.inputs.pwm2Min.value = 0;
    el.game.inputs.pwm3Min.value = 0;
  }
  updatePressureColor(lastPressure);
  renderChart();
}

function collectGameConfig() {
  return {
    duration: parseFloat(el.game.inputs.duration.value),
    criticalPressure: parseFloat(el.game.inputs.critical.value),
    midPressure: parseFloat(el.game.inputs.mid.value),
    maxMotorIntensity: parseFloat(el.game.inputs.max.value),
    lowPressureDelay: parseFloat(el.game.inputs.delay.value),
    stimulationRampRateLimit: parseFloat(el.game.inputs.rate.value),
    pressureSensitivity: parseFloat(el.game.inputs.sensitivity.value),
    stimulationRampRandomPercent: parseFloat(el.game.inputs.random.value),
    intensityGradualIncrease: parseFloat(el.game.inputs.increase.value),
    shockIntensity: parseFloat(el.game.inputs.shockVoltage.value),
    midMinIntensity: parseFloat(el.game.inputs.midMin.value),
    pwmMaxPermille: [
      Math.round(parseFloat(el.game.inputs.pwm0.value || '0') * 10),
      Math.round(parseFloat(el.game.inputs.pwm1.value || '0') * 10),
      Math.round(parseFloat(el.game.inputs.pwm2.value || '0') * 10),
      Math.round(parseFloat(el.game.inputs.pwm3.value || '0') * 10),
    ],
    pwmMinPermille: [
      Math.round(parseFloat(el.game.inputs.pwm0Min.value || '0') * 10),
      Math.round(parseFloat(el.game.inputs.pwm1Min.value || '0') * 10),
      Math.round(parseFloat(el.game.inputs.pwm2Min.value || '0') * 10),
      Math.round(parseFloat(el.game.inputs.pwm3Min.value || '0') * 10),
    ],
  };
}

async function postGameConfig(save = true, reset = false) {
  setGameConfigToast('正在提交...');
  const body = reset ? { reset: true, save } : { ...collectGameConfig(), save };
  try {
    const res = await fetch('/api/game/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    const data = await res.json();
    if (!res.ok) {
      throw new Error(data.error || '提交失败');
    }
    updateGameConfigForm(data);
    setGameConfigToast(save ? '保存成功' : '已应用');
  } catch (err) {
    setGameConfigToast(err.message, true);
  }
}

async function postGameControl(action) {
  setGameToast('指令发送中...');
  try {
    const res = await fetch('/api/game/control', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action }),
    });
    const data = await res.json();
    if (!res.ok) {
      throw new Error(data.error || '指令失败');
    }
    setGameToast('指令已发送');
  } catch (err) {
    setGameToast(err.message, true);
  }
}

function updateGameStatus(data) {
  if (!data) return;
  gameStatus = data;
  gameRunning = !!data.running;
  if (Number.isFinite(data.midPressure)) {
    gameConfig = { ...gameConfig, midPressure: data.midPressure };
  }
  if (Number.isFinite(data.criticalPressure)) {
    gameConfig = { ...gameConfig, criticalPressure: data.criticalPressure };
  }
  if (el.game.state) {
    const k = data.state || '';
    el.game.state.textContent = GAME_STATE_TEXT[k] || k || '--';
  }
  if (el.game.running) {
    el.game.running.textContent = data.running ? (data.paused ? '暂停' : '运行中') : '停止';
  }
  if (el.game.intensity) {
    const v = typeof data.currentIntensity === 'number' ? data.currentIntensity.toFixed(1) : '--';
    el.game.intensity.textContent = v;
  }
  if (el.game.target) {
    const v = typeof data.targetIntensity === 'number' ? data.targetIntensity.toFixed(1) : '--';
    el.game.target.textContent = v;
  }
  if (el.game.pressure) {
    const p = typeof data.currentPressure === 'number' ? data.currentPressure.toFixed(2) : '--';
    const a = typeof data.averagePressure === 'number' ? data.averagePressure.toFixed(2) : '--';
    el.game.pressure.textContent = `${p} (Avg ${a})`;
  }
  if (el.game.thresholds) {
    const mid = data.midPressure ?? gameConfig.midPressure ?? '--';
    const crit = data.criticalPressure ?? gameConfig.criticalPressure ?? '--';
    el.game.thresholds.textContent = `${mid} / ${crit}`;
  }
  if (el.game.counts) {
    const e = data.edgingCount ?? 0;
    const s = data.shockCount ?? 0;
    el.game.counts.textContent = `${e} / ${s}`;
  }
  if (el.game.stimTime) {
    const t = typeof data.totalStimulationTime === 'number' ? data.totalStimulationTime.toFixed(1) : '--';
    el.game.stimTime.textContent = `${t}s`;
  }
  if (el.game.buttons.pause) {
    el.game.buttons.pause.textContent = data.paused ? '继续' : '暂停';
  }
  setManualEnabled(!gameRunning);
  updatePressureColor(lastPressure);
  renderChart();
}

async function postConfig(save = true, reset = false) {
  setToast('正在提交...');
  const body = reset ? { reset: true, save } : { ...collectConfig(), save };
  try {
    const res = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    const data = await res.json();
    if (!res.ok) {
      throw new Error(data.error || '提交失败');
    }
    updateConfigForm(data);
    setToast(save ? '保存成功' : '已应用');
  } catch (err) {
    setToast(err.message, true);
  }
}

function addPoint(ts, value) {
  if (!ts) return;
  points.push({ ts, value });
  const tMin = ts - windowSec * 1000;
  while (points.length > 0 && points[0].ts < tMin - 1000) {
    points.shift();
  }
  renderChart();
}

function renderChart() {
  if (!chart.ctx) return;
  const ctx = chart.ctx;
  const w = chart.width;
  const h = chart.height;
  ctx.clearRect(0, 0, w, h);

  if (points.length < 2) {
    ctx.fillStyle = 'rgba(230,237,243,0.4)';
    ctx.fillText('等待数据...', 12, 20);
    return;
  }

  const tMax = points[points.length - 1].ts;
  const tMin = tMax - windowSec * 1000;
  const visible = points.filter(p => p.ts >= tMin);
  if (visible.length < 2) {
    return;
  }

  const minVal = CHART_Y_MIN;
  const maxVal = CHART_Y_MAX;

  ctx.strokeStyle = 'rgba(255,255,255,0.08)';
  ctx.lineWidth = 1;
  for (let i = 1; i <= 4; i++) {
    const y = (h / 5) * i;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  const { critical } = getGameThresholds();
  if (Number.isFinite(critical)) {
    const ratio = (critical - minVal) / (maxVal - minVal);
    const clamped = Math.min(1, Math.max(0, ratio));
    const y = h - clamped * h;
    ctx.save();
    ctx.setLineDash([6, 6]);
    ctx.strokeStyle = 'rgba(255, 107, 107, 0.7)';
    ctx.lineWidth = 1.2;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
    ctx.restore();
  }

  ctx.strokeStyle = '#00d2ff';
  ctx.lineWidth = 2;
  ctx.beginPath();
  visible.forEach((p, idx) => {
    const x = ((p.ts - tMin) / (tMax - tMin)) * w;
    const ratio = (p.value - minVal) / (maxVal - minVal);
    const clamped = Math.min(1, Math.max(0, ratio));
    const y = h - clamped * h;
    if (idx === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  });
  ctx.stroke();

  ctx.fillStyle = 'rgba(230,237,243,0.6)';
  ctx.fillText(maxVal.toFixed(1), 6, 12);
  ctx.fillText(minVal.toFixed(1), 6, h - 6);
}

function connectWs() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);

  ws.onopen = () => {
    setConnection(true);
    reconnectDelay = 500;
  };

  ws.onclose = () => {
    setConnection(false);
    setTimeout(connectWs, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 1.8, 5000);
  };

  ws.onerror = () => {
    setConnection(false);
  };

  ws.onmessage = (event) => {
    try {
      const data = JSON.parse(event.data);
      updateStatus(data);
      if (data.game) {
        updateGameStatus(data.game);
      }
      if (typeof data.ts === 'number' && typeof data.pressure_kpa === 'number') {
        addPoint(data.ts, data.pressure_kpa);
      }
    } catch (err) {
      console.warn('ws parse error', err);
    }
  };
}

async function loadInitial() {
  try {
    const [cfgRes, stRes, gameCfgRes, gameStRes] = await Promise.all([
      fetch('/api/config'),
      fetch('/api/status'),
      fetch('/api/game/config'),
      fetch('/api/game/status'),
    ]);
    const cfg = await cfgRes.json();
    updateConfigForm(cfg);
    const st = await stRes.json();
    updateStatus(st);
    const gcfg = await gameCfgRes.json();
    updateGameConfigForm(gcfg);
    const gst = await gameStRes.json();
    updateGameStatus(gst);
  } catch (err) {
    setToast('加载配置失败', true);
  }
}

let helpPopover = null;
let helpAnchor = null;

function closeHelpPopover() {
  if (!helpPopover) return;
  helpPopover.remove();
  helpPopover = null;
  helpAnchor = null;
}

function positionHelpPopover(anchor) {
  if (!helpPopover || !anchor) return;
  const r = anchor.getBoundingClientRect();
  const pad = 10;
  const pop = helpPopover;
  const w = pop.offsetWidth;
  const h = pop.offsetHeight;

  let left = r.left + r.width / 2 - w / 2;
  left = Math.max(pad, Math.min(window.innerWidth - w - pad, left));

  let top = r.bottom + pad;
  if (top + h > window.innerHeight - pad) {
    top = r.top - h - pad;
  }
  top = Math.max(pad, Math.min(window.innerHeight - h - pad, top));

  pop.style.left = `${Math.round(left)}px`;
  pop.style.top = `${Math.round(top)}px`;
}

function openHelpPopover(btn) {
  const title = btn.dataset.helpTitle || '说明';
  const body = btn.dataset.help || '';
  if (!body) return;

  closeHelpPopover();
  helpAnchor = btn;

  const pop = document.createElement('div');
  pop.className = 'help-popover';
  const t = document.createElement('div');
  t.className = 'help-popover-title';
  t.textContent = title;
  const b = document.createElement('div');
  b.className = 'help-popover-body';
  b.textContent = body;
  pop.appendChild(t);
  pop.appendChild(b);
  document.body.appendChild(pop);
  helpPopover = pop;

  positionHelpPopover(btn);
}

function setupHelpPopovers() {
  document.querySelectorAll('.help-btn').forEach((btn) => {
    btn.addEventListener('click', (e) => {
      e.preventDefault();
      e.stopPropagation();
      if (helpPopover && helpAnchor === btn) {
        closeHelpPopover();
        return;
      }
      openHelpPopover(btn);
    });
  });

  document.addEventListener('click', (e) => {
    if (!helpPopover) return;
    if (helpPopover.contains(e.target)) return;
    if (e.target && e.target.classList && e.target.classList.contains('help-btn')) return;
    closeHelpPopover();
  });

  window.addEventListener('resize', () => {
    if (helpPopover && helpAnchor) positionHelpPopover(helpAnchor);
  });
  window.addEventListener('scroll', () => {
    if (helpPopover) closeHelpPopover();
  }, true);
}

el.buttons.save.addEventListener('click', () => postConfig(true, false));
el.buttons.reset.addEventListener('click', () => postConfig(true, true));

el.game.buttons.save.addEventListener('click', () => postGameConfig(true, false));
el.game.buttons.reset.addEventListener('click', () => postGameConfig(true, true));
el.game.buttons.start.addEventListener('click', () => postGameControl('start'));
el.game.buttons.pause.addEventListener('click', () => postGameControl('pause'));
el.game.buttons.stop.addEventListener('click', () => postGameControl('stop'));
el.game.buttons.shock.addEventListener('click', () => postGameControl('shockOnce'));

setupHelpPopovers();

window.addEventListener('resize', () => chart.resize());
chart.resize();
loadInitial();
connectWs();
