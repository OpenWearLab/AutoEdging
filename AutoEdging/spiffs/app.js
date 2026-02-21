const el = {
  conn: document.getElementById('conn'),
  pressure: document.getElementById('pressure'),
  dac: document.getElementById('dac'),
  pwm: document.getElementById('pwm'),
  ble: document.getElementById('ble'),
  chartMeta: document.getElementById('chart-meta'),
  nav: {
    tabs: Array.from(document.querySelectorAll('.tab-btn')),
    pages: Array.from(document.querySelectorAll('.page')),
  },
  game: {
    state: document.getElementById('game-state'),
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
      randomInterval: document.getElementById('game-random-interval'),
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
  manual: {
    toast: document.getElementById('manual-toast'),
    inputs: {
      dacCode: document.getElementById('cfg-dac-code'),
      dacPd: document.getElementById('cfg-dac-pd'),
      pwm0: document.getElementById('cfg-pwm0'),
      pwm1: document.getElementById('cfg-pwm1'),
      pwm2: document.getElementById('cfg-pwm2'),
      pwm3: document.getElementById('cfg-pwm3'),
      bleSwing: document.getElementById('cfg-ble-swing'),
      bleVibrate: document.getElementById('cfg-ble-vibrate'),
    },
    pwmValues: {
      pwm0: document.getElementById('cfg-pwm0-value'),
      pwm1: document.getElementById('cfg-pwm1-value'),
      pwm2: document.getElementById('cfg-pwm2-value'),
      pwm3: document.getElementById('cfg-pwm3-value'),
    },
    buttons: {
      save: document.getElementById('btn-manual-save'),
      reset: document.getElementById('btn-manual-reset'),
      bleSwingStop: document.getElementById('btn-ble-swing-stop'),
      bleVibrateStop: document.getElementById('btn-ble-vibrate-stop'),
    },
  },
  system: {
    toast: document.getElementById('system-toast'),
    inputs: {
      sample: document.getElementById('cfg-sample'),
      ws: document.getElementById('cfg-ws'),
      window: document.getElementById('cfg-window'),
      statusLed: document.getElementById('cfg-status-led'),
    },
    buttons: {
      save: document.getElementById('btn-system-save'),
      reset: document.getElementById('btn-system-reset'),
    },
  },
};

let points = [];
let windowSec = 60;
let ws = null;
let reconnectDelay = 500;
let reconnectTimer = null;
let wsLastMessageMs = 0;
let lastPressure = null;
let gameStatus = {};
let gameConfig = {};
let gameRunning = false;
let manualApplyTimer = null;

const WS_STALE_TIMEOUT_MS = 1000;
const WS_WATCHDOG_INTERVAL_MS = 200;
const MANUAL_APPLY_DEBOUNCE_MS = 150;

const GAME_STATE_TEXT = {
  INITIAL_CALM: '乖乖蓄欲期…慢慢涨起来哦',
  MIDDLE:       '忍耐中期…不准太快缴械',
  EDGING:       '边缘颤抖中…敢再往前一步试试？',
  DELAY:        '惩罚冷却…数着秒呼吸，宝贝',
  SUB_CALM:     '被放过后的小喘息…又要开始被玩坏了呢',
};

const SENSOR_PRESSURE_MAX_KPA = 45;
const CHART_Y_BASE_MIN = 0;
const CHART_Y_DYNAMIC_MIN_CAP = 13;
const CHART_Y_MIN_VISIBLE_MAX = 12;
const CHART_Y_FOOTROOM_KPA = 1;
const CHART_Y_HEADROOM_KPA = 2;
const CHART_Y_CRITICAL_MARGIN_KPA = 2.0;
const CHART_Y_SHRINK_RATE = 0.18;

const PWM_KEYS = ['pwm0', 'pwm1', 'pwm2', 'pwm3'];
const CONTROL_DEFAULTS = {
  sampleHz: 25,
  wsHz: 5,
  windowSec: 60,
  dacCode: 0,
  dacPd: 0,
  pwmPermille: [0, 0, 0, 0],
  bleSwing: 0,
  bleVibrate: 0,
  statusLedEnabled: true,
};

let chartYMin = CHART_Y_BASE_MIN;
let chartYMax = SENSOR_PRESSURE_MAX_KPA;

function getGameThresholds() {
  const midRaw = Number(gameConfig.midPressure);
  const critRaw = Number(gameConfig.criticalPressure);
  const mid = Number.isFinite(midRaw) ? midRaw : null;
  const critical = Number.isFinite(critRaw) ? critRaw : null;
  return { mid, critical };
}

function calcChartTargetYMax(visible) {
  let maxPressure = CHART_Y_BASE_MIN;
  for (const p of visible) {
    if (Number.isFinite(p.value) && p.value > maxPressure) {
      maxPressure = p.value;
    }
  }

  const { critical } = getGameThresholds();
  let target = maxPressure + CHART_Y_HEADROOM_KPA;
  if (Number.isFinite(critical)) {
    target = Math.max(target, critical + CHART_Y_CRITICAL_MARGIN_KPA);
  }
  target = Math.max(target, CHART_Y_MIN_VISIBLE_MAX);

  // Keep scaling moderate for this 45kPa sensor, unless critical line requires more.
  if (!(Number.isFinite(critical) && critical > SENSOR_PRESSURE_MAX_KPA)) {
    target = Math.min(target, SENSOR_PRESSURE_MAX_KPA);
  }
  return target;
}

function calcChartTargetYMin(visible) {
  let minPressure = Number.POSITIVE_INFINITY;
  for (const p of visible) {
    if (Number.isFinite(p.value) && p.value < minPressure) {
      minPressure = p.value;
    }
  }
  if (!Number.isFinite(minPressure)) {
    return CHART_Y_BASE_MIN;
  }

  const { critical } = getGameThresholds();
  let target = minPressure - CHART_Y_FOOTROOM_KPA;
  if (Number.isFinite(critical)) {
    target = Math.min(target, critical - CHART_Y_CRITICAL_MARGIN_KPA);
  }
  target = Math.max(target, CHART_Y_BASE_MIN);
  target = Math.min(target, CHART_Y_DYNAMIC_MIN_CAP);
  return target;
}

function smoothChartYMin(target) {
  if (!Number.isFinite(chartYMin) || chartYMin < CHART_Y_BASE_MIN) {
    chartYMin = target;
    return chartYMin;
  }
  if (target <= chartYMin) {
    chartYMin = target;
    return chartYMin;
  }
  chartYMin += (target - chartYMin) * CHART_Y_SHRINK_RATE;
  if (Math.abs(chartYMin - target) < 0.1) {
    chartYMin = target;
  }
  return chartYMin;
}

function smoothChartYMax(target) {
  if (!Number.isFinite(chartYMax) || chartYMax <= CHART_Y_BASE_MIN) {
    chartYMax = target;
    return chartYMax;
  }
  if (target >= chartYMax) {
    chartYMax = target;
    return chartYMax;
  }
  chartYMax += (target - chartYMax) * CHART_Y_SHRINK_RATE;
  if (Math.abs(chartYMax - target) < 0.1) {
    chartYMax = target;
  }
  return chartYMax;
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

function setManualToast(msg, isError = false) {
  if (!el.manual.toast) return;
  el.manual.toast.textContent = msg || '';
  el.manual.toast.style.color = isError ? '#ffb3b3' : 'rgba(230,237,243,0.65)';
}

function setSystemToast(msg, isError = false) {
  if (!el.system.toast) return;
  el.system.toast.textContent = msg || '';
  el.system.toast.style.color = isError ? '#ffb3b3' : 'rgba(230,237,243,0.65)';
}

function parseIntOr(value, fallback) {
  const n = parseInt(value, 10);
  return Number.isFinite(n) ? n : fallback;
}

function parseFloatOr(value, fallback) {
  const n = parseFloat(value);
  return Number.isFinite(n) ? n : fallback;
}

function setManualPwmValue(key, value) {
  const v = Math.max(0, Math.min(100, parseFloatOr(value, 0)));
  const field = el.manual.inputs[key];
  const view = el.manual.pwmValues[key];
  if (field) field.value = v.toFixed(1);
  if (view) view.textContent = `${v.toFixed(1)}%`;
}

function setBleSelectValue(select, value) {
  if (!select) return;
  const v = parseInt(value, 10);
  if (Number.isFinite(v) && v >= 1 && v <= 9) {
    select.value = String(v);
  } else {
    select.value = '';
  }
}

function clampPercent(value) {
  return Math.max(0, Math.min(100, value));
}

function updateGamePwmRangeView(key, minValue, maxValue) {
  const minOut = document.getElementById(`game-${key}-min-value`);
  const maxOut = document.getElementById(`game-${key}-value`);
  const fill = document.getElementById(`game-${key}-fill`);
  if (minOut) minOut.value = minValue.toFixed(1);
  if (maxOut) maxOut.value = maxValue.toFixed(1);
  if (fill) {
    fill.style.left = `${minValue}%`;
    fill.style.width = `${Math.max(0, maxValue - minValue)}%`;
  }
}

function syncGamePwmRange(key, changed = '') {
  const minInput = el.game.inputs[`${key}Min`];
  const maxInput = el.game.inputs[key];
  if (!minInput || !maxInput) return;

  let minValue = clampPercent(parseFloatOr(minInput.value, 0));
  let maxValue = clampPercent(parseFloatOr(maxInput.value, 0));
  if (minValue > maxValue) {
    if (changed === 'min') {
      maxValue = minValue;
    } else {
      minValue = maxValue;
    }
  }

  minInput.value = minValue.toFixed(1);
  maxInput.value = maxValue.toFixed(1);
  updateGamePwmRangeView(key, minValue, maxValue);
}

function setGamePwmRangeValue(key, minValue, maxValue) {
  const minInput = el.game.inputs[`${key}Min`];
  const maxInput = el.game.inputs[key];
  if (minInput) minInput.value = clampPercent(minValue).toFixed(1);
  if (maxInput) maxInput.value = clampPercent(maxValue).toFixed(1);
  syncGamePwmRange(key);
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
    const voltage = data.dac.voltage ?? 0;
    el.dac.textContent = `${voltage.toFixed(3)} V`;
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
  el.system.inputs.sample.value = cfg.sample_hz ?? CONTROL_DEFAULTS.sampleHz;
  el.system.inputs.ws.value = cfg.ws_hz ?? CONTROL_DEFAULTS.wsHz;
  el.system.inputs.window.value = cfg.window_sec ?? CONTROL_DEFAULTS.windowSec;
  el.system.inputs.statusLed.value = (cfg.status_led_enabled ?? CONTROL_DEFAULTS.statusLedEnabled) ? '1' : '0';
  if (cfg.dac) {
    el.manual.inputs.dacCode.value = cfg.dac.code ?? CONTROL_DEFAULTS.dacCode;
    el.manual.inputs.dacPd.value = cfg.dac.pd_mode ?? CONTROL_DEFAULTS.dacPd;
  }
  if (Array.isArray(cfg.pwm)) {
    setManualPwmValue('pwm0', (cfg.pwm[0] ?? 0) / 10);
    setManualPwmValue('pwm1', (cfg.pwm[1] ?? 0) / 10);
    setManualPwmValue('pwm2', (cfg.pwm[2] ?? 0) / 10);
    setManualPwmValue('pwm3', (cfg.pwm[3] ?? 0) / 10);
  } else {
    PWM_KEYS.forEach((key, idx) => setManualPwmValue(key, (CONTROL_DEFAULTS.pwmPermille[idx] ?? 0) / 10));
  }
  if (cfg.ble) {
    setBleSelectValue(el.manual.inputs.bleSwing, cfg.ble.swing ?? CONTROL_DEFAULTS.bleSwing);
    setBleSelectValue(el.manual.inputs.bleVibrate, cfg.ble.vibrate ?? CONTROL_DEFAULTS.bleVibrate);
  }
  if (typeof cfg.window_sec === 'number') {
    windowSec = cfg.window_sec;
    el.chartMeta.textContent = `最近 ${windowSec} 秒`;
  }
  updatePressureColor(lastPressure);
  renderChart();
}

function setManualEnabled(enabled) {
  Object.values(el.manual.inputs).forEach((input) => {
    if (input) input.disabled = !enabled;
  });
  Object.values(el.manual.buttons).forEach((btn) => {
    if (btn) btn.disabled = !enabled;
  });
}

function collectManualConfig() {
  return {
    dac: {
      code: parseIntOr(el.manual.inputs.dacCode.value, CONTROL_DEFAULTS.dacCode),
      pd_mode: parseIntOr(el.manual.inputs.dacPd.value, CONTROL_DEFAULTS.dacPd),
    },
    pwm: [
      Math.round(parseFloatOr(el.manual.inputs.pwm0.value, 0) * 10),
      Math.round(parseFloatOr(el.manual.inputs.pwm1.value, 0) * 10),
      Math.round(parseFloatOr(el.manual.inputs.pwm2.value, 0) * 10),
      Math.round(parseFloatOr(el.manual.inputs.pwm3.value, 0) * 10),
    ],
    ble: {
      swing: parseIntOr(el.manual.inputs.bleSwing.value, CONTROL_DEFAULTS.bleSwing),
      vibrate: parseIntOr(el.manual.inputs.bleVibrate.value, CONTROL_DEFAULTS.bleVibrate),
    },
  };
}

function collectSystemConfig() {
  return {
    sample_hz: parseIntOr(el.system.inputs.sample.value, CONTROL_DEFAULTS.sampleHz),
    ws_hz: parseIntOr(el.system.inputs.ws.value, CONTROL_DEFAULTS.wsHz),
    window_sec: parseIntOr(el.system.inputs.window.value, CONTROL_DEFAULTS.windowSec),
    status_led_enabled: el.system.inputs.statusLed.value === '1',
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
  el.game.inputs.random.value = cfg.stimulationRampRandomPercent ?? 30;
  el.game.inputs.randomInterval.value = cfg.stimulationRampRandomInterval ?? 1;
  el.game.inputs.increase.value = cfg.intensityGradualIncrease ?? 2;
  el.game.inputs.shockVoltage.value = parseFloatOr(cfg.shockIntensity, 1.2).toFixed(1);
  el.game.inputs.midMin.value = cfg.midMinIntensity ?? 5;
  PWM_KEYS.forEach((key, idx) => {
    const minPermille = Array.isArray(cfg.pwmMinPermille) ? (cfg.pwmMinPermille[idx] ?? 0) : 0;
    const maxPermille = Array.isArray(cfg.pwmMaxPermille) ? (cfg.pwmMaxPermille[idx] ?? 0) : 0;
    setGamePwmRangeValue(key, minPermille / 10, maxPermille / 10);
  });
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
    stimulationRampRandomInterval: parseFloat(el.game.inputs.randomInterval.value),
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
    if (!data.running) {
      el.game.state.textContent = '停止';
    } else if (data.paused) {
      el.game.state.textContent = '暂停';
    } else {
      const k = data.state || '';
      el.game.state.textContent = GAME_STATE_TEXT[k] || k || '--';
    }
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

async function postManualConfig(save = false, quiet = false) {
  if (!quiet) {
    setManualToast('正在提交...');
  }
  try {
    const res = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ...collectManualConfig(), save }),
    });
    const data = await res.json();
    if (!res.ok) {
      throw new Error(data.error || '提交失败');
    }
    updateConfigForm(data);
    if (!quiet) {
      setManualToast(save ? '保存成功' : '已应用');
    }
    setSystemToast('');
  } catch (err) {
    setManualToast(err.message, true);
  }
}

function scheduleManualConfigApply() {
  if (gameRunning) {
    return;
  }
  if (manualApplyTimer) {
    clearTimeout(manualApplyTimer);
  }
  manualApplyTimer = setTimeout(() => {
    manualApplyTimer = null;
    postManualConfig(false, true);
  }, MANUAL_APPLY_DEBOUNCE_MS);
}

async function postSystemConfig(save = true) {
  setSystemToast('正在提交...');
  try {
    const res = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ...collectSystemConfig(), save }),
    });
    const data = await res.json();
    if (!res.ok) {
      throw new Error(data.error || '提交失败');
    }
    updateConfigForm(data);
    setSystemToast(save ? '保存成功' : '已应用');
    setManualToast('');
  } catch (err) {
    setSystemToast(err.message, true);
  }
}

function resetManualConfigToDefaults() {
  el.manual.inputs.dacCode.value = CONTROL_DEFAULTS.dacCode;
  el.manual.inputs.dacPd.value = CONTROL_DEFAULTS.dacPd;
  PWM_KEYS.forEach((key, idx) => {
    const v = (CONTROL_DEFAULTS.pwmPermille[idx] ?? 0) / 10;
    setManualPwmValue(key, v);
  });
  setBleSelectValue(el.manual.inputs.bleSwing, CONTROL_DEFAULTS.bleSwing);
  setBleSelectValue(el.manual.inputs.bleVibrate, CONTROL_DEFAULTS.bleVibrate);
  postManualConfig(false);
}

function resetSystemConfigToDefaults() {
  el.system.inputs.sample.value = CONTROL_DEFAULTS.sampleHz;
  el.system.inputs.ws.value = CONTROL_DEFAULTS.wsHz;
  el.system.inputs.window.value = CONTROL_DEFAULTS.windowSec;
  postSystemConfig(true);
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

  const targetYMin = calcChartTargetYMin(visible);
  const minVal = smoothChartYMin(targetYMin);
  const targetYMax = calcChartTargetYMax(visible);
  const maxVal = smoothChartYMax(targetYMax);
  const range = Math.max(1e-6, maxVal - minVal);

  ctx.strokeStyle = 'rgba(255,255,255,0.08)';
  ctx.lineWidth = 1;
  for (let i = 1; i <= 4; i++) {
    const y = (h / 5) * i;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  const { mid, critical } = getGameThresholds();
  if (Number.isFinite(mid)) {
    const ratio = (mid - minVal) / range;
    const clamped = Math.min(1, Math.max(0, ratio));
    const y = h - clamped * h;
    ctx.save();
    ctx.setLineDash([6, 6]);
    ctx.strokeStyle = 'rgba(251, 191, 36, 0.75)';
    ctx.lineWidth = 1.2;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
    ctx.restore();
  }
  if (Number.isFinite(critical)) {
    const ratio = (critical - minVal) / range;
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
    const ratio = (p.value - minVal) / range;
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
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
    return;
  }
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);

  ws.onopen = () => {
    setConnection(true);
    wsLastMessageMs = Date.now();
    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
    reconnectDelay = 500;
  };

  ws.onclose = () => {
    setConnection(false);
    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
    }
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      connectWs();
    }, reconnectDelay);
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
      wsLastMessageMs = Date.now();
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
    setGameToast('加载配置失败', true);
    setManualToast('加载配置失败', true);
    setSystemToast('加载配置失败', true);
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

function showPage(pageId) {
  el.nav.pages.forEach((page) => {
    page.classList.toggle('is-active', page.id === pageId);
  });
  el.nav.tabs.forEach((tab) => {
    tab.classList.toggle('active', tab.dataset.page === pageId);
  });
  closeHelpPopover();
  if (pageId === 'page-game') {
    requestAnimationFrame(() => chart.resize());
  }
}

function setupPageTabs() {
  el.nav.tabs.forEach((tab) => {
    tab.addEventListener('click', () => {
      const pageId = tab.dataset.page;
      if (!pageId) return;
      showPage(pageId);
    });
  });
}

function setupGamePwmRangeControls() {
  PWM_KEYS.forEach((key) => {
    const minInput = el.game.inputs[`${key}Min`];
    const maxInput = el.game.inputs[key];
    const minValueInput = document.getElementById(`game-${key}-min-value`);
    const maxValueInput = document.getElementById(`game-${key}-value`);
    if (!minInput || !maxInput) return;

    minInput.addEventListener('input', () => {
      syncGamePwmRange(key, 'min');
    });
    maxInput.addEventListener('input', () => {
      syncGamePwmRange(key, 'max');
    });
    if (minValueInput) {
      minValueInput.addEventListener('change', () => {
        minInput.value = minValueInput.value;
        syncGamePwmRange(key, 'min');
      });
    }
    if (maxValueInput) {
      maxValueInput.addEventListener('change', () => {
        maxInput.value = maxValueInput.value;
        syncGamePwmRange(key, 'max');
      });
    }
    syncGamePwmRange(key);
  });
}

function setupManualControls() {
  PWM_KEYS.forEach((key) => {
    const slider = el.manual.inputs[key];
    if (!slider) return;
    slider.addEventListener('input', () => {
      setManualPwmValue(key, slider.value);
      scheduleManualConfigApply();
    });
  });

  ['dacCode', 'dacPd', 'bleSwing', 'bleVibrate'].forEach((key) => {
    const input = el.manual.inputs[key];
    if (!input) return;
    input.addEventListener('change', () => {
      scheduleManualConfigApply();
    });
  });
}

el.manual.buttons.save.addEventListener('click', () => postManualConfig(true));
el.manual.buttons.reset.addEventListener('click', () => resetManualConfigToDefaults());
el.manual.buttons.bleSwingStop.addEventListener('click', () => {
  setBleSelectValue(el.manual.inputs.bleSwing, 0);
  postManualConfig(false);
});
el.manual.buttons.bleVibrateStop.addEventListener('click', () => {
  setBleSelectValue(el.manual.inputs.bleVibrate, 0);
  postManualConfig(false);
});

el.system.buttons.save.addEventListener('click', () => postSystemConfig(true));
el.system.buttons.reset.addEventListener('click', () => resetSystemConfigToDefaults());

el.game.buttons.save.addEventListener('click', () => postGameConfig(true, false));
el.game.buttons.reset.addEventListener('click', () => postGameConfig(true, true));
el.game.buttons.start.addEventListener('click', () => postGameControl('start'));
el.game.buttons.pause.addEventListener('click', () => postGameControl('pause'));
el.game.buttons.stop.addEventListener('click', () => postGameControl('stop'));
el.game.buttons.shock.addEventListener('click', () => postGameControl('shockOnce'));

setupPageTabs();
setupGamePwmRangeControls();
setupManualControls();
setupHelpPopovers();

window.addEventListener('resize', () => {
  if (document.getElementById('page-game').classList.contains('is-active')) {
    chart.resize();
  }
});
chart.resize();
loadInitial();
connectWs();

setInterval(() => {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    return;
  }
  if (Date.now() - wsLastMessageMs > WS_STALE_TIMEOUT_MS) {
    console.warn('ws stale, reconnecting...');
    ws.close();
  }
}, WS_WATCHDOG_INTERVAL_MS);
