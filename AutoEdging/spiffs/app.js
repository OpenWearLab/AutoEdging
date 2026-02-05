const el = {
  conn: document.getElementById('conn'),
  pressure: document.getElementById('pressure'),
  temp: document.getElementById('temp'),
  dac: document.getElementById('dac'),
  pwm: document.getElementById('pwm'),
  ble: document.getElementById('ble'),
  chartMeta: document.getElementById('chart-meta'),
  toast: document.getElementById('toast'),
  inputs: {
    threshold: document.getElementById('cfg-threshold'),
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
    apply: document.getElementById('btn-apply'),
    save: document.getElementById('btn-save'),
    reset: document.getElementById('btn-reset'),
  },
};

let points = [];
let windowSec = 60;
let ws = null;
let reconnectDelay = 500;

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
  }
  if (typeof data.temp_c === 'number') {
    el.temp.textContent = data.temp_c.toFixed(1) + ' ℃';
  }
  if (data.dac) {
    const code = data.dac.code ?? data.dac_code ?? 0;
    const voltage = data.dac.voltage ?? 0;
    el.dac.textContent = `code ${code}, ${voltage.toFixed(3)} V`;
  }
  if (Array.isArray(data.pwm)) {
    el.pwm.textContent = data.pwm.map(v => Math.round(v)).join(' / ');
  }
  if (data.ble) {
    el.ble.textContent = `swing ${data.ble.swing}, vibrate ${data.ble.vibrate}`;
  }
}

function updateConfigForm(cfg) {
  if (!cfg) return;
  el.inputs.threshold.value = cfg.pressure_threshold_kpa ?? 0;
  el.inputs.sample.value = cfg.sample_hz ?? 25;
  el.inputs.ws.value = cfg.ws_hz ?? 5;
  el.inputs.window.value = cfg.window_sec ?? 60;
  if (cfg.dac) {
    el.inputs.dacCode.value = cfg.dac.code ?? 0;
    el.inputs.dacPd.value = cfg.dac.pd_mode ?? 0;
  }
  if (Array.isArray(cfg.pwm)) {
    el.inputs.pwm0.value = cfg.pwm[0] ?? 0;
    el.inputs.pwm1.value = cfg.pwm[1] ?? 0;
    el.inputs.pwm2.value = cfg.pwm[2] ?? 0;
    el.inputs.pwm3.value = cfg.pwm[3] ?? 0;
  }
  if (cfg.ble) {
    el.inputs.bleSwing.value = cfg.ble.swing ?? 0;
    el.inputs.bleVibrate.value = cfg.ble.vibrate ?? 0;
  }
  if (typeof cfg.window_sec === 'number') {
    windowSec = cfg.window_sec;
    el.chartMeta.textContent = `最近 ${windowSec} 秒`;
  }
}

function collectConfig() {
  return {
    pressure_threshold_kpa: parseFloat(el.inputs.threshold.value),
    sample_hz: parseInt(el.inputs.sample.value, 10),
    ws_hz: parseInt(el.inputs.ws.value, 10),
    window_sec: parseInt(el.inputs.window.value, 10),
    dac: {
      code: parseInt(el.inputs.dacCode.value, 10),
      pd_mode: parseInt(el.inputs.dacPd.value, 10),
    },
    pwm: [
      parseInt(el.inputs.pwm0.value, 10),
      parseInt(el.inputs.pwm1.value, 10),
      parseInt(el.inputs.pwm2.value, 10),
      parseInt(el.inputs.pwm3.value, 10),
    ],
    ble: {
      swing: parseInt(el.inputs.bleSwing.value, 10),
      vibrate: parseInt(el.inputs.bleVibrate.value, 10),
    },
  };
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

  let minVal = visible[0].value;
  let maxVal = visible[0].value;
  for (const p of visible) {
    if (p.value < minVal) minVal = p.value;
    if (p.value > maxVal) maxVal = p.value;
  }
  if (minVal === maxVal) {
    minVal -= 1;
    maxVal += 1;
  }
  const pad = (maxVal - minVal) * 0.1;
  minVal -= pad;
  maxVal += pad;

  ctx.strokeStyle = 'rgba(255,255,255,0.08)';
  ctx.lineWidth = 1;
  for (let i = 1; i <= 4; i++) {
    const y = (h / 5) * i;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  ctx.strokeStyle = '#00d2ff';
  ctx.lineWidth = 2;
  ctx.beginPath();
  visible.forEach((p, idx) => {
    const x = ((p.ts - tMin) / (tMax - tMin)) * w;
    const y = h - ((p.value - minVal) / (maxVal - minVal)) * h;
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
    const [cfgRes, stRes] = await Promise.all([
      fetch('/api/config'),
      fetch('/api/status'),
    ]);
    const cfg = await cfgRes.json();
    updateConfigForm(cfg);
    const st = await stRes.json();
    updateStatus(st);
  } catch (err) {
    setToast('加载配置失败', true);
  }
}

el.buttons.apply.addEventListener('click', () => postConfig(false, false));
el.buttons.save.addEventListener('click', () => postConfig(true, false));
el.buttons.reset.addEventListener('click', () => postConfig(true, true));

window.addEventListener('resize', () => chart.resize());

chart.resize();
loadInitial();
connectWs();
