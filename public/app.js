// Bitaxe Deck – Frontend
// Datenquellen laufen über den lokalen Server (server.js):
//   /ext/coingecko/...  /ext/mempool/...  (Cache)   /bitaxe/system/info?host=IP  (CORS-Proxy)

const $ = (id) => document.getElementById(id);
const LOCALE = 'de-DE';
const TWO32 = 4294967296;
const HALVING_INTERVAL = 210000;

// ---------------------------------------------------------------- Zustand

const store = {
  get(key, fallback) {
    try { const v = localStorage.getItem(key); return v == null ? fallback : JSON.parse(v); } catch { return fallback; }
  },
  set(key, value) {
    try { localStorage.setItem(key, JSON.stringify(value)); } catch { /* Speicher voll/gesperrt */ }
  },
  del(key) { try { localStorage.removeItem(key); } catch {} },
};

const settings = Object.assign(
  { host: '', interval: 5000, demo: false, kwh: 0.35, watt: '' },
  store.get('bd.settings', {}),
);
const saveSettings = () => store.set('bd.settings', settings);

const state = {
  usd: null, eur: null, chg24: null,
  height: null, difficulty: null, netHash: null, blockTime: 600_000,
  avgFees: 0, blocks: [],
  miner: null, minerMode: 'demo', minerError: '', lastOnline: 0, failCount: 0,
  priceDays: 1, histHours: 1,
};

// ---------------------------------------------------------------- Formatierung

const nf = (d = 0, opts = {}) => new Intl.NumberFormat(LOCALE, { minimumFractionDigits: d, maximumFractionDigits: d, ...opts });
const fmt = (n, d = 0) => (n == null || !isFinite(n) ? '—' : nf(d).format(n));
const fmtUsd = (n, d = 0) => (n == null ? '—' : '$' + fmt(n, d));
const fmtEur = (n, d = 2) => (n == null ? '—' : fmt(n, d) + ' €');

function fmtUnit(value, units, base = 1000) {
  if (value == null || !isFinite(value)) return { v: '—', u: '' };
  let i = 0;
  while (Math.abs(value) >= base && i < units.length - 1) { value /= base; i++; }
  const d = value >= 100 ? 1 : value >= 10 ? 2 : 2;
  return { v: fmt(value, d), u: units[i], raw: value };
}
const HASH_UNITS = ['H/s', 'kH/s', 'MH/s', 'GH/s', 'TH/s', 'PH/s', 'EH/s', 'ZH/s'];
const DIFF_UNITS = ['', 'K', 'M', 'G', 'T', 'P', 'E'];
const fmtHash = (hps) => { const r = fmtUnit(hps, HASH_UNITS); return `${r.v} ${r.u}`; };
const fmtDiff = (d) => { const r = fmtUnit(d, DIFF_UNITS); return `${r.v}${r.u ? ' ' + r.u : ''}`; };

function parseDiff(v) {
  if (v == null || v === '') return null;
  if (typeof v === 'number') return v;
  const m = String(v).trim().match(/^([\d.,]+)\s*([kKMGTPE]?)/);
  if (!m) return null;
  const mult = { '': 1, k: 1e3, K: 1e3, M: 1e6, G: 1e9, T: 1e12, P: 1e15, E: 1e18 }[m[2]];
  return parseFloat(m[1].replace(',', '.')) * mult;
}

function fmtDuration(sec, short = false) {
  if (sec == null || !isFinite(sec)) return '—';
  sec = Math.max(0, sec);
  const y = Math.floor(sec / 31557600);
  if (y >= 1) return `${fmt(y)} ${y === 1 ? 'Jahr' : 'Jahre'}`;
  const d = Math.floor(sec / 86400), h = Math.floor((sec % 86400) / 3600), m = Math.floor((sec % 3600) / 60);
  if (d > 0) return short ? `${d}T ${h}h` : `${d}T ${h}h ${m}m`;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${Math.floor(sec % 60)}s`;
  return `${Math.floor(sec)}s`;
}
function fmtAgo(tsSec) {
  const s = Date.now() / 1000 - tsSec;
  if (s < 60) return 'gerade eben';
  if (s < 3600) return `vor ${Math.floor(s / 60)} Min`;
  if (s < 86400) return `vor ${Math.floor(s / 3600)} Std`;
  return `vor ${Math.floor(s / 86400)} T`;
}
function fmtProb(p) {
  if (p == null || !isFinite(p)) return '—';
  const pct = p * 100;
  if (pct >= 10) return fmt(pct, 1) + ' %';
  if (pct >= 0.01) return fmt(pct, 3) + ' %';
  return new Intl.NumberFormat(LOCALE, { maximumSignificantDigits: 3 }).format(pct) + ' %';
}
function fmtOneIn(p) {
  if (!p || !isFinite(p)) return '—';
  const n = 1 / p;
  return '1 : ' + new Intl.NumberFormat(LOCALE, { notation: n >= 1e6 ? 'compact' : 'standard', maximumFractionDigits: n >= 1e6 ? 1 : 0 }).format(n);
}

// Zählt Zahlen weich hoch/runter
function tween(el, to, format, { flash = false } = {}) {
  if (!el) return;
  if (to == null || !isFinite(to)) { el.textContent = '—'; el._v = null; return; }
  const from = el._v;
  el._v = to;
  if (flash && from != null && from !== to) {
    el.classList.remove('flash-up', 'flash-down');
    void el.offsetWidth;
    el.classList.add(to > from ? 'flash-up' : 'flash-down');
  }
  if (from == null || matchMedia('(prefers-reduced-motion: reduce)').matches) { el.textContent = format(to); return; }
  const start = performance.now(), dur = 700;
  cancelAnimationFrame(el._raf);
  const step = (now) => {
    const k = Math.min(1, (now - start) / dur), e = 1 - Math.pow(1 - k, 3);
    el.textContent = format(from + (to - from) * e);
    if (k < 1) el._raf = requestAnimationFrame(step);
  };
  el._raf = requestAnimationFrame(step);
}

function setChg(el, pct) {
  el.classList.remove('up', 'down');
  if (pct == null || !isFinite(pct)) { el.textContent = '—'; return; }
  el.classList.add(pct >= 0 ? 'up' : 'down');
  el.textContent = `${pct >= 0 ? '▲' : '▼'} ${fmt(Math.abs(pct), 2)} %`;
}

function toast(msg, ms = 3200) {
  const t = $('toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(t._h);
  t._h = setTimeout(() => t.classList.remove('show'), ms);
}

async function getJson(url, timeout = 12000) {
  const r = await fetch(url, { signal: AbortSignal.timeout(timeout) });
  const text = await r.text();
  let data;
  try { data = JSON.parse(text); } catch { data = text; }
  if (!r.ok) throw new Error((data && data.error) || `HTTP ${r.status}`);
  return data;
}

function setPill(id, cls, label) {
  const p = $(id);
  p.className = 'pill ' + cls;
  p.querySelector('span').textContent = label;
}

// ---------------------------------------------------------------- Charts

const css = getComputedStyle(document.documentElement);
const tok = (name) => css.getPropertyValue(name).trim();
const C = {
  accent: tok('--violet-400'),
  accentStrong: tok('--violet-500'),
  accent2: tok('--accent-2'),
  cyan: tok('--cyan'),
  btc: tok('--btc'),
  red: tok('--red'),
  green: tok('--green'),
  temp2: tok('--temp-2'),
  text2: tok('--text-2'),
  text3: tok('--text-3'),
  tooltipBg: tok('--panel-solid'),
};
const hasChart = typeof window.Chart !== 'undefined';

if (hasChart) {
  Chart.defaults.font.family = "'JetBrains Mono', monospace";
  Chart.defaults.font.size = 11;
  Chart.defaults.color = C.text3;
  Chart.defaults.animation.duration = 700;
  Chart.defaults.animation.easing = 'easeOutCubic';
}

function gradient(ctx, color, h = 320) {
  const g = ctx.createLinearGradient(0, 0, 0, h);
  g.addColorStop(0, color + '70');
  g.addColorStop(0.55, color + '1c');
  g.addColorStop(1, color + '00');
  return g;
}

// Horizontaler Farbverlauf für Chart-Linien (scriptable borderColor)
function strokeGradient(stops, fallback) {
  return (context) => {
    const { ctx, chartArea } = context.chart;
    if (!chartArea) return fallback;
    const g = ctx.createLinearGradient(chartArea.left, 0, chartArea.right, 0);
    stops.forEach((c, i) => g.addColorStop(i / (stops.length - 1), c));
    return g;
  };
}
const STROKE_UP = () => strokeGradient([C.accentStrong, C.accent, C.accent2], C.accent);
const STROKE_DOWN = () => strokeGradient(['#ff8a5c', C.red, '#d94bb0'], C.red);
const STROKE_DEMO = () => strokeGradient([C.accent2, '#f0a8ff'], C.accent2);

// Fadenkreuz beim Hovern
const crosshair = {
  id: 'crosshair',
  afterDatasetsDraw(chart) {
    const a = chart.tooltip?.getActiveElements?.();
    if (!a || !a.length) return;
    const { ctx, chartArea: { top, bottom, left, right } } = chart;
    const { x, y } = a[0].element;
    ctx.save();
    ctx.setLineDash([3, 4]);
    ctx.strokeStyle = 'rgba(255,255,255,.25)';
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(x, top); ctx.lineTo(x, bottom); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(right, y); ctx.stroke();
    ctx.setLineDash([]);
    const bc = chart.data.datasets[a[0].datasetIndex].borderColor;
    ctx.fillStyle = typeof bc === 'string' ? bc : C.accent;
    ctx.shadowColor = ctx.fillStyle; ctx.shadowBlur = 12;
    ctx.beginPath(); ctx.arc(x, y, 4.5, 0, Math.PI * 2); ctx.fill();
    ctx.restore();
  },
};

const tooltipBase = {
  backgroundColor: C.tooltipBg,
  borderColor: 'rgba(255,255,255,.12)',
  borderWidth: 1,
  padding: 10,
  cornerRadius: 8,
  titleColor: C.text2,
  bodyColor: '#fff',
  displayColors: false,
  titleFont: { weight: '500' },
  bodyFont: { size: 13, weight: '600' },
};

function timeTick(rangeMs) {
  return (v) => {
    const d = new Date(v);
    if (rangeMs <= 2 * 86400e3) return d.toLocaleTimeString(LOCALE, { hour: '2-digit', minute: '2-digit' });
    if (rangeMs <= 60 * 86400e3) return d.toLocaleDateString(LOCALE, { day: '2-digit', month: '2-digit' });
    return d.toLocaleDateString(LOCALE, { month: 'short', year: '2-digit' });
  };
}

let priceChart, sparkChart, minerChart;

function initCharts() {
  if (!hasChart) {
    for (const id of ['price-msg', 'miner-msg']) { $(id).textContent = 'Chart-Bibliothek konnte nicht geladen werden (offline?).'; $(id).classList.add('show'); }
    return;
  }
  const pctx = $('chart-price').getContext('2d');
  priceChart = new Chart(pctx, {
    type: 'line',
    data: { datasets: [{ data: [], borderColor: STROKE_UP(), backgroundColor: gradient(pctx, C.accentStrong), fill: true, borderWidth: 2, pointRadius: 0, pointHitRadius: 12, tension: 0.25 }] },
    options: {
      responsive: true, maintainAspectRatio: false, parsing: false, normalized: true,
      interaction: { mode: 'index', intersect: false },
      plugins: {
        legend: { display: false },
        tooltip: {
          ...tooltipBase,
          callbacks: {
            title: (it) => new Date(it[0].parsed.x).toLocaleString(LOCALE, { dateStyle: 'medium', timeStyle: state.priceDays > 90 ? undefined : 'short' }),
            label: (it) => fmtUsd(it.parsed.y, it.parsed.y < 1000 ? 2 : 0),
          },
        },
      },
      scales: {
        x: { type: 'linear', grid: { display: false }, border: { display: false }, ticks: { maxTicksLimit: 7, maxRotation: 0 } },
        y: { position: 'right', grid: { color: 'rgba(255,255,255,.05)' }, border: { display: false }, ticks: { maxTicksLimit: 6, callback: (v) => '$' + nf(0, { notation: 'compact' }).format(v) } },
      },
    },
    plugins: [crosshair],
  });

  const sctx = $('spark-price').getContext('2d');
  sparkChart = new Chart(sctx, {
    type: 'line',
    data: { datasets: [{ data: [], borderColor: STROKE_UP(), backgroundColor: gradient(sctx, C.accentStrong, 60), fill: true, borderWidth: 1.5, pointRadius: 0, tension: 0.3 }] },
    options: {
      responsive: true, maintainAspectRatio: false, parsing: false, animation: false, events: [],
      plugins: { legend: { display: false }, tooltip: { enabled: false } },
      scales: { x: { type: 'linear', display: false }, y: { display: false } },
    },
  });

  const mctx = $('chart-miner').getContext('2d');
  minerChart = new Chart(mctx, {
    type: 'line',
    data: {
      datasets: [
        { label: 'Hashrate', data: [], yAxisID: 'y', borderColor: STROKE_UP(), backgroundColor: gradient(mctx, C.accentStrong, 240), fill: true, borderWidth: 2, pointRadius: 0, pointHitRadius: 10, tension: 0.3 },
        { label: 'ASIC-Temp', data: [], yAxisID: 'y1', borderColor: C.btc, borderWidth: 1.5, pointRadius: 0, pointHitRadius: 10, tension: 0.3, borderDash: [] },
        { label: 'VR-Temp', data: [], yAxisID: 'y1', borderColor: C.temp2, borderWidth: 1, pointRadius: 0, tension: 0.3, borderDash: [4, 4] },
      ],
    },
    options: {
      responsive: true, maintainAspectRatio: false, parsing: false, normalized: true,
      interaction: { mode: 'index', intersect: false },
      plugins: {
        legend: { display: true, position: 'top', align: 'end', labels: { boxWidth: 10, boxHeight: 2, color: C.text2 } },
        tooltip: {
          ...tooltipBase, displayColors: true, bodyFont: { size: 12, weight: '500' },
          callbacks: {
            title: (it) => new Date(it[0].parsed.x).toLocaleTimeString(LOCALE),
            label: (it) => it.datasetIndex === 0 ? ` ${fmt(it.parsed.y, 3)} TH/s` : ` ${it.dataset.label}: ${fmt(it.parsed.y, 1)} °C`,
          },
        },
      },
      scales: {
        x: { type: 'linear', grid: { display: false }, border: { display: false }, ticks: { maxTicksLimit: 7, maxRotation: 0, callback: timeTick(3600e3) } },
        y: { position: 'left', grid: { color: 'rgba(255,255,255,.05)' }, border: { display: false }, ticks: { maxTicksLimit: 5, callback: (v) => fmt(v, 2) + ' TH' } },
        y1: { position: 'right', grid: { display: false }, border: { display: false }, ticks: { maxTicksLimit: 5, callback: (v) => fmt(v, 0) + '°' } },
      },
    },
    plugins: [crosshair],
  });
}

// ---------------------------------------------------------------- Markt

async function loadPrice() {
  try {
    const d = await getJson('/ext/coingecko/simple/price?ids=bitcoin&vs_currencies=usd,eur&include_24hr_change=true');
    state.usd = d.bitcoin.usd; state.eur = d.bitcoin.eur; state.chg24 = d.bitcoin.usd_24h_change;
    setPill('pill-api', 'ok', 'Live');
  } catch {
    try { // Fallback: mempool.space
      const p = await getJson('/ext/mempool/v1/prices');
      state.usd = p.USD; state.eur = p.EUR;
      setPill('pill-api', 'warn', 'Fallback');
    } catch {
      setPill('pill-api', 'err', 'Markt offline');
      return;
    }
  }
  tween($('price-usd'), state.usd, (v) => fmtUsd(v), { flash: true });
  $('price-eur').textContent = fmtEur(state.eur, 0);
  setChg($('price-chg'), state.chg24);
  document.title = `${fmtUsd(state.usd)} · Bitaxe Deck`;
  renderConverterFromSats();
  renderCalcs();
  stamp();
}

const priceCache = new Map();
async function loadPriceChart(days = state.priceDays) {
  state.priceDays = days;
  const msg = $('price-msg');
  if (!priceChart) return;
  try {
    let pts = priceCache.get(days);
    if (!pts || Date.now() - pts.t > 5 * 60e3) {
      const d = await getJson(`/ext/coingecko/coins/bitcoin/market_chart?vs_currency=usd&days=${days}`);
      pts = { t: Date.now(), data: d.prices.map(([x, y]) => ({ x, y })) };
      priceCache.set(days, pts);
    }
    const data = pts.data;
    if (days !== state.priceDays) return; // inzwischen umgeschaltet
    msg.classList.remove('show');
    const first = data[0].y, last = data[data.length - 1].y;
    const up = last >= first;
    const color = up ? C.accent : C.red;
    const ds = priceChart.data.datasets[0];
    ds.data = data;
    ds.borderColor = up ? STROKE_UP() : STROKE_DOWN();
    ds.backgroundColor = gradient(priceChart.ctx, up ? C.accentStrong : C.red);
    const range = data[data.length - 1].x - data[0].x;
    priceChart.options.scales.x.min = data[0].x;
    priceChart.options.scales.x.max = data[data.length - 1].x;
    priceChart.options.scales.x.ticks.callback = timeTick(range);
    priceChart.update();
    const ys = data.map((p) => p.y);
    const chg = ((last - first) / first) * 100;
    $('range-hilo').innerHTML = `Hoch ${fmtUsd(Math.max(...ys))} · Tief ${fmtUsd(Math.min(...ys))} · <span style="color:${up ? C.green : C.red}">${chg >= 0 ? '+' : ''}${fmt(chg, 2)} %</span>`;
    if (days === 1 && sparkChart) {
      sparkChart.data.datasets[0].data = data;
      sparkChart.data.datasets[0].borderColor = up ? STROKE_UP() : STROKE_DOWN();
      sparkChart.data.datasets[0].backgroundColor = gradient(sparkChart.ctx, up ? C.accentStrong : C.red, 60);
      sparkChart.update();
    }
  } catch (e) {
    msg.textContent = `Preisdaten nicht verfügbar (${e.message}). Neuer Versuch in Kürze …`;
    msg.classList.add('show');
    setTimeout(() => loadPriceChart(state.priceDays), 20_000);
  }
}

// ---------------------------------------------------------------- Netzwerk

async function loadNetwork() {
  const [blocks, hr, adj, fees] = await Promise.allSettled([
    getJson('/ext/mempool/v1/blocks'),
    getJson('/ext/mempool/v1/mining/hashrate/3d'),
    getJson('/ext/mempool/v1/difficulty-adjustment'),
    getJson('/ext/mempool/v1/fees/recommended'),
  ]);

  if (blocks.status === 'fulfilled' && Array.isArray(blocks.value)) {
    const b = blocks.value;
    state.blocks = b;
    state.height = b[0].height;
    state.difficulty = state.difficulty || b[0].difficulty;
    const feeBlocks = b.filter((x) => x.extras?.totalFees != null);
    state.avgFees = feeBlocks.length ? feeBlocks.reduce((s, x) => s + x.extras.totalFees, 0) / feeBlocks.length : 0;
    tween($('block-height'), state.height, (v) => fmt(Math.round(v)), { flash: true });
    renderBlocks();
  }
  if (hr.status === 'fulfilled') {
    state.netHash = hr.value.currentHashrate;
    state.difficulty = hr.value.currentDifficulty || state.difficulty;
    $('net-hash').textContent = fmtHash(state.netHash);
  }
  if (state.difficulty) $('net-diff').textContent = fmtDiff(state.difficulty);
  if (adj.status === 'fulfilled') {
    const a = adj.value;
    state.blockTime = a.timeAvg || 600_000;
    setChg($('diff-next'), a.difficultyChange);
    $('ring-diff').style.setProperty('--p', a.progressPercent.toFixed(2));
    $('diff-pct').textContent = fmt(a.progressPercent, 1) + ' %';
    $('diff-remaining').textContent = `${fmt(a.remainingBlocks)} Blöcke · ${fmtDuration(a.remainingTime / 1000, true)}`;
    $('diff-date').textContent = new Date(a.estimatedRetargetDate).toLocaleString(LOCALE, { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' });
    $('diff-avg').textContent = `${Math.floor(state.blockTime / 60000)}:${String(Math.round((state.blockTime % 60000) / 1000)).padStart(2, '0')} Min`;
  }
  if (fees.status === 'fulfilled') {
    const f = fees.value;
    $('fee-fast').textContent = fmt(f.fastestFee);
    $('fee-half').textContent = fmt(f.halfHourFee);
    $('fee-hour').textContent = fmt(f.hourFee);
    $('fee-eco').textContent = fmt(f.economyFee);
  }
  const ok = [blocks, hr, adj, fees].filter((x) => x.status === 'fulfilled').length;
  if (ok === 0) setPill('pill-api', 'err', 'Netz offline');
  renderBlockAge();
  renderHalving();
  renderCalcs();
  stamp();
}

function renderBlocks() {
  const wrap = $('blocks');
  const max = 4_000_000;
  wrap.innerHTML = state.blocks.slice(0, 15).map((b, i) => {
    const e = b.extras || {};
    const range = e.feeRange ? `${fmt(e.feeRange[0], 1)} – ${fmt(e.feeRange[e.feeRange.length - 1], 0)} sat/vB` : '';
    const fill = Math.min(100, ((b.weight || 0) / max) * 100);
    return `<a class="block" style="--i:${i}" href="https://mempool.space/block/${b.id}" target="_blank" rel="noopener" title="Block ${b.height}">
      <div class="h">${fmt(b.height)}</div>
      <div class="fee">~${fmt(e.medianFee ?? 0, 1)} sat/vB</div>
      <div class="range">${range}</div>
      <div class="meta"><span>${fmt(b.tx_count)} Tx</span><span>${e.reward ? fmt(e.reward / 1e8, 3) + ' ₿' : ''}</span></div>
      <div class="meta"><span>${fmtAgo(b.timestamp)}</span><span>${fmt((b.size || 0) / 1e6, 2)} MB</span></div>
      <div class="pool">⛏ ${escapeHtml(e.pool?.name || 'Unbekannt')}</div>
      <div class="fill" style="width:${fill}%"></div>
    </a>`;
  }).join('');
}
const escapeHtml = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

function renderBlockAge() {
  if (!state.blocks.length) return;
  $('block-age').textContent = `Letzter Block ${fmtAgo(state.blocks[0].timestamp)}`;
}

function subsidyAt(height) { return 50 / Math.pow(2, Math.floor(height / HALVING_INTERVAL)); }

function renderHalving() {
  if (state.height == null) return;
  const next = (Math.floor(state.height / HALVING_INTERVAL) + 1) * HALVING_INTERVAL;
  const remaining = next - state.height;
  state.halvingTarget = Date.now() + remaining * state.blockTime;
  $('halving-block').textContent = `#${fmt(next)}`;
  $('halving-bar').style.width = (((state.height % HALVING_INTERVAL) / HALVING_INTERVAL) * 100).toFixed(2) + '%';
  $('halving-sub').textContent = `${fmt(remaining)} Blöcke · Belohnung ${fmt(subsidyAt(state.height), 3)} → ${fmt(subsidyAt(next), 4)} ₿ · ca. ${new Date(state.halvingTarget).toLocaleDateString(LOCALE, { month: 'long', year: 'numeric' })}`;
  tickHalving();
}
function tickHalving() {
  if (!state.halvingTarget) return;
  let s = Math.max(0, Math.floor((state.halvingTarget - Date.now()) / 1000));
  const parts = [Math.floor(s / 86400), Math.floor((s % 86400) / 3600), Math.floor((s % 3600) / 60), s % 60];
  const els = $('halving-countdown').querySelectorAll('b');
  parts.forEach((v, i) => { const t = i === 0 ? fmt(v) : String(v).padStart(2, '0'); if (els[i].textContent !== t) els[i].textContent = t; });
}

// ---------------------------------------------------------------- Bitaxe

const histKey = () => `bd.hist.${settings.host || 'none'}`;
let history = store.get(histKey(), []);
let lastSampleAt = history.length ? history[history.length - 1].t : 0;

function recordSample(m) {
  const now = Date.now();
  if (now - lastSampleAt < 15_000) return; // höchstens alle 15 s speichern
  lastSampleAt = now;
  history.push({ t: now, h: m.hashRate, tc: m.temp, vr: m.vrTemp, p: m.power });
  const cutoff = now - 24 * 3600e3;
  while (history.length && history[0].t < cutoff) history.shift();
  store.set(histKey(), history);
}

// Simulierter Gamma 601 (BM1370) für den Demo-Modus
let demoT = 0;
function demoSample(t = Date.now()) {
  const s = t / 1000;
  const wobble = Math.sin(s / 420) * 30 + Math.sin(s / 97) * 14 + (Math.random() - 0.5) * 22;
  const temp = 61 + Math.sin(s / 600) * 1.6 + (Math.random() - 0.5) * 0.6;
  return {
    demo: true,
    hostname: 'bitaxe-demo', ASICModel: 'BM1370', boardVersion: '601', version: 'v2.x (Demo)',
    hashRate: 1205 + wobble, expectedHashrate: 1200, power: 17.8 + Math.sin(s / 90) * 0.5,
    voltage: 5080, current: 3500, temp, vrTemp: temp + 6.5,
    bestDiff: 412_800_000, bestSessionDiff: 38_400_000,
    sharesAccepted: 18_432 + Math.floor(s / 9) % 100_000, sharesRejected: 21,
    uptimeSeconds: 3 * 86400 + 4 * 3600 + (demoT += settings.interval / 1000),
    fanspeed: 46, fanrpm: 4120, frequency: 525, coreVoltage: 1150, coreVoltageActual: 1146,
    wifiRSSI: -54, stratumURL: 'solo.ckpool.org', stratumPort: 3333,
    stratumUser: 'bc1q…demo.bitaxe', poolDifficulty: 1000,
  };
}
function demoHistory() {
  const now = Date.now(), out = [];
  for (let t = now - 24 * 3600e3; t <= now; t += 60e3) {
    const m = demoSample(t);
    out.push({ t, h: m.hashRate, tc: m.temp, vr: m.vrTemp, p: m.power });
  }
  return out;
}
let demoHist = null;

async function fetchMiner(host) {
  return getJson(`/bitaxe/system/info?host=${encodeURIComponent(host)}`, 6000);
}

async function pollMiner() {
  clearTimeout(pollMiner._t);
  if (settings.demo || !settings.host) {
    state.minerMode = 'demo';
    state.miner = demoSample();
  } else {
    try {
      const m = await fetchMiner(settings.host);
      if (typeof m !== 'object' || m.hashRate == null) throw new Error('Unerwartete Antwort – ist das ein AxeOS-Gerät?');
      const before = state.miner && !state.miner.demo ? state.miner : null;
      state.miner = m; state.minerMode = 'online'; state.failCount = 0; state.lastOnline = Date.now();
      recordSample(m);
      if (m.blockFound || (before && parseDiff(m.bestDiff) > parseDiff(before.bestDiff) && parseDiff(m.bestDiff) >= (state.difficulty || Infinity))) {
        toast('🎉 BLOCK GEFUNDEN! Herzlichen Glückwunsch!', 15000);
      } else if (before && parseDiff(m.bestSessionDiff) > parseDiff(before.bestSessionDiff) && parseDiff(m.bestSessionDiff) === parseDiff(m.bestDiff)) {
        toast(`⚡ Neue Best Difficulty: ${fmtDiff(parseDiff(m.bestDiff))}`);
      }
    } catch (e) {
      state.failCount++;
      state.minerError = e.message;
      // Einzelne Aussetzer überbrücken, danach klar als offline markieren
      if (state.failCount >= 2 || !state.miner || state.miner.demo) {
        state.minerMode = 'offline';
        state.miner = demoSample();
      }
    }
  }
  renderMiner();
  renderMinerChart();
  renderCalcs();
  pollMiner._t = setTimeout(pollMiner, settings.interval);
}

function renderMiner() {
  const m = state.miner;
  if (!m) return;
  const card = document.querySelector('.miner');
  const mode = state.minerMode;
  card.classList.toggle('is-demo', mode !== 'online');

  const badge = $('miner-badge');
  const banner = $('miner-banner');
  if (mode === 'online') {
    badge.className = 'badge ok'; badge.textContent = '● Online';
    setPill('pill-miner', 'ok', 'Bitaxe online');
    banner.hidden = true;
  } else if (mode === 'offline') {
    badge.className = 'badge err'; badge.textContent = 'Offline';
    setPill('pill-miner', 'err', 'Bitaxe offline');
    banner.hidden = false; banner.className = 'banner err';
    banner.innerHTML = `<span><b>Gerät unter ${escapeHtml(settings.host)} nicht erreichbar</b> – ${escapeHtml(state.minerError)}.${state.lastOnline ? ` Zuletzt online ${fmtAgo(state.lastOnline / 1000)}.` : ''} Bis dahin siehst du <b>Demo-Werte</b>.</span><button class="btn ghost" data-open-settings>Adresse prüfen</button>`;
  } else {
    badge.className = 'badge demo'; badge.textContent = 'Demo';
    setPill('pill-miner', 'demo', 'Demo');
    banner.hidden = false; banner.className = 'banner';
    banner.innerHTML = settings.demo
      ? `<span>Demo-Modus aktiv – alle Miner-Werte sind <b>simuliert</b>.</span><button class="btn ghost" data-open-settings>Einstellungen</button>`
      : `<span>Noch kein Bitaxe eingetragen – angezeigt werden <b>simulierte Demo-Werte</b>.</span><button class="btn primary" data-open-settings>IP eintragen</button>`;
  }

  const isGamma = /BM1370/i.test(m.ASICModel || '');
  $('miner-name').textContent = isGamma ? `Bitaxe Gamma ${m.boardVersion || '601'}` : (m.hostname || 'Bitaxe');
  $('miner-meta').textContent = [m.hostname, m.ASICModel, m.boardVersion && `Board ${m.boardVersion}`, m.version && `AxeOS ${m.version}`].filter(Boolean).join(' · ');

  const gh = Number(m.hashRate) || 0;
  const th = gh / 1000;
  tween($('m-hash'), th, (v) => fmt(v, v >= 10 ? 1 : 2));
  $('m-hash-exp').textContent = m.expectedHashrate ? `Soll ${fmt(m.expectedHashrate / 1000, 2)} TH/s` : '';
  const gmax = (m.expectedHashrate || 1200) * 1.3;
  $('gauge-hash').style.setProperty('--p', Math.min(100, (gh / gmax) * 100).toFixed(1));

  tween($('m-temp'), m.temp, (v) => fmt(v, 1) + ' °C');
  $('m-temp-bar').style.width = Math.min(100, ((m.temp || 0) / 80) * 100) + '%';
  if (m.vrTemp) { tween($('m-vrtemp'), m.vrTemp, (v) => fmt(v, 1) + ' °C'); $('m-vrtemp-bar').style.width = Math.min(100, (m.vrTemp / 100) * 100) + '%'; }
  else $('m-vrtemp').textContent = '—';
  tween($('m-power'), m.power, (v) => fmt(v, 1) + ' W');
  $('m-power-sub').textContent = m.voltage ? `${fmt(m.voltage / 1000, 2)} V · ${fmt((m.current || 0) / 1000, 2)} A` : '';
  tween($('m-eff'), th > 0 ? m.power / th : null, (v) => fmt(v, 1));

  const best = parseDiff(m.bestDiff), sess = parseDiff(m.bestSessionDiff);
  $('m-best').textContent = best != null ? fmtDiff(best) : '—';
  $('m-best-session').textContent = sess != null ? `Session ${fmtDiff(sess)}` : '';
  tween($('m-shares'), m.sharesAccepted, (v) => fmt(Math.round(v)));
  const rej = m.sharesRejected || 0, total = (m.sharesAccepted || 0) + rej;
  $('m-rejected').textContent = `${fmt(rej)} abgelehnt · ${fmt(total ? (rej / total) * 100 : 0, 2)} %`;
  $('m-uptime').textContent = fmtDuration(m.uptimeSeconds, true);
  $('m-wifi').textContent = m.wifiRSSI != null ? `WLAN ${m.wifiRSSI} dBm` : '';
  $('m-fan').textContent = m.fanspeed != null ? `${fmt(m.fanspeed)} %` : '—';
  $('m-fanrpm').textContent = m.fanrpm != null ? `${fmt(m.fanrpm)} U/min` : '';
  $('m-freq').textContent = fmt(m.frequency);
  $('m-cv').textContent = m.coreVoltageActual ? `${fmt(m.coreVoltageActual)} mV` : (m.coreVoltage ? `${fmt(m.coreVoltage)} mV` : '—');
  $('m-cv-sub').textContent = m.coreVoltage ? `Soll ${fmt(m.coreVoltage)} mV` : '';

  const fallback = m.isUsingFallbackStratum;
  const url = fallback ? m.fallbackStratumURL : m.stratumURL;
  const port = fallback ? m.fallbackStratumPort : m.stratumPort;
  $('m-pool').textContent = url ? `${url}${port ? ':' + port : ''}${fallback ? ' (Fallback)' : ''}` : '—';
  $('m-worker').textContent = (fallback ? m.fallbackStratumUser : m.stratumUser) || '—';
  $('m-worker').title = $('m-worker').textContent;
  $('m-pooldiff').textContent = m.poolDifficulty ? fmtDiff(m.poolDifficulty) : '—';
}

function renderMinerChart() {
  if (!minerChart) return;
  const useReal = !!settings.host && !settings.demo && (state.minerMode === 'online' || history.length > 1);
  let data = useReal ? history : (demoHist ||= demoHistory());
  if (!useReal && state.miner?.demo) {
    const last = data[data.length - 1];
    if (Date.now() - last.t > 60e3) { const m = demoSample(); data.push({ t: Date.now(), h: m.hashRate, tc: m.temp, vr: m.vrTemp }); data.shift(); }
  }
  const cutoff = Date.now() - state.histHours * 3600e3;
  const pts = data.filter((p) => p.t >= cutoff);
  const [h, t, v] = minerChart.data.datasets;
  h.data = pts.map((p) => ({ x: p.t, y: p.h / 1000 }));
  t.data = pts.map((p) => ({ x: p.t, y: p.tc }));
  v.data = pts.filter((p) => p.vr).map((p) => ({ x: p.t, y: p.vr }));
  const demo = !useReal;
  h.borderColor = demo ? STROKE_DEMO() : STROKE_UP();
  h.backgroundColor = gradient(minerChart.ctx, demo ? C.accent2 : C.accentStrong, 240);
  minerChart.options.scales.x.min = pts.length > 1 ? Math.max(cutoff, pts[0].t) : cutoff;
  minerChart.options.scales.x.max = Date.now();
  minerChart.options.scales.x.ticks.callback = timeTick(state.histHours * 3600e3);
  minerChart.update('none');

  const avg = pts.length ? pts.reduce((s, p) => s + p.h, 0) / pts.length / 1000 : null;
  $('hist-info').textContent = demo
    ? '· Demo-Verlauf'
    : `· ${fmt(history.length)} Messpunkte · Ø ${fmt(avg, 3)} TH/s`;
  const msg = $('miner-msg');
  if (useReal && pts.length < 2) { msg.textContent = 'Sammle Messwerte … (alle 15 s ein Punkt, gespeichert in diesem Browser)'; msg.classList.add('show'); }
  else msg.classList.remove('show');
}

// ---------------------------------------------------------------- Rechner

function renderCalcs() {
  const m = state.miner;
  const D = state.difficulty;
  const gh = m ? Number(m.hashRate) || 0 : 0;
  const H = gh * 1e9;
  const isDemo = state.minerMode !== 'online';
  $('lot-hash').textContent = `${fmt(gh / 1000, 2)} TH/s${isDemo ? ' · Demo' : ''}`;

  let yearCost = null, costDay = null;
  const watts = settings.watt !== '' && settings.watt != null ? Number(settings.watt) : (m ? m.power : null);
  $('in-watt').placeholder = m?.power ? `${fmt(m.power, 1)} (Gerät)` : 'vom Gerät';
  if (watts != null && isFinite(watts)) {
    const kwhDay = (watts * 24) / 1000;
    costDay = kwhDay * settings.kwh;
    yearCost = costDay * 365.25;
    $('cost-day').textContent = fmtEur(costDay);
    $('cost-month').textContent = fmtEur(costDay * 30.44);
    $('cost-year').textContent = fmtEur(yearCost);
    $('kwh-day').textContent = `${fmt(kwhDay, 2)} kWh`;
    $('kwh-month').textContent = `${fmt(kwhDay * 30.44, 1)} kWh`;
    $('kwh-year').textContent = `${fmt(kwhDay * 365.25, 0)} kWh`;
    const th = gh / 1000;
    $('eff-wth').textContent = th > 0 ? `${fmt(watts / th, 2)} W/TH` : '—';
    $('eff-th-day').textContent = th > 0 ? fmtEur(costDay / th, 3) : '—';
  }

  if (!D || !H) return;
  const lambda = H / (D * TWO32); // Blöcke pro Sekunde
  const P = (sec) => -Math.expm1(-lambda * sec);
  const day = 86400, year = 365.25 * day;
  $('lot-day').textContent = fmtProb(P(day)); $('lot-day-1in').textContent = fmtOneIn(P(day));
  $('lot-week').textContent = fmtProb(P(7 * day)); $('lot-week-1in').textContent = fmtOneIn(P(7 * day));
  $('lot-month').textContent = fmtProb(P(year / 12)); $('lot-month-1in').textContent = fmtOneIn(P(year / 12));
  $('lot-year').textContent = fmtProb(P(year)); $('lot-year-1in').textContent = fmtOneIn(P(year));
  $('lot-eta').textContent = fmtDuration(1 / lambda);

  const subsidySats = subsidyAt(state.height || 0) * 1e8;
  const rewardSats = subsidySats + (state.avgFees || 0);
  const rewardBtc = rewardSats / 1e8;
  $('lot-reward').textContent = `${fmt(rewardBtc, 3)} ₿${state.usd ? ' ≈ ' + fmtUsd(rewardBtc * state.usd) : ''}`;
  const evSats = lambda * day * rewardSats;
  $('lot-ev').textContent = `${fmt(evSats, 1)} sats`;
  $('ev-eur').textContent = state.eur ? `${fmtEur((evSats / 1e8) * state.eur, 3)}${costDay != null ? ` vs. ${fmtEur(costDay)} Strom` : ''}` : `${fmt(evSats, 1)} sats`;

  if (yearCost != null) {
    const lambdaNeeded = -Math.log(0.99) / year;
    const factor = lambdaNeeded / lambda;
    $('ticket').textContent = `${fmtEur(yearCost * factor, 0)} / Jahr (${fmt(factor, 0)}× Bitaxe)`;
  }

  const best = parseDiff(m?.bestDiff);
  if (best) {
    const pct = Math.min(100, (Math.log10(best) / Math.log10(D)) * 100);
    $('best-bar').style.width = pct.toFixed(1) + '%';
    $('best-sub').textContent = `Best ${fmtDiff(best)} · Netz ${fmtDiff(D)} · ${best >= D ? 'BLOCK! 🎉' : `noch ${fmtOneIn(best / D).replace('1 : ', '')}× entfernt`} (log)`;
  }
}

// Sats-Umrechner
const parseNum = (s) => { const n = parseFloat(String(s).replace(/\s/g, '').replace(/\.(?=\d{3}(\D|$))/g, '').replace(',', '.')); return isFinite(n) ? n : null; };
let convSats = 100000;
function renderConverterFromSats(except) {
  const btc = convSats / 1e8;
  if (except !== 'sats') $('cv-sats').value = nf(0).format(Math.round(convSats));
  if (except !== 'btc') $('cv-btc').value = btc.toFixed(8).replace('.', ',');
  if (except !== 'usd') $('cv-usd').value = state.usd ? nf(2).format(btc * state.usd) : '';
  if (except !== 'eur') $('cv-eur').value = state.eur ? nf(2).format(btc * state.eur) : '';
  if (state.usd) $('moscow-usd').textContent = fmt(1e8 / state.usd);
  if (state.eur) $('moscow-eur').textContent = fmt(1e8 / state.eur);
}
function bindConverter() {
  const map = { 'cv-sats': ['sats', (n) => n], 'cv-btc': ['btc', (n) => n * 1e8], 'cv-usd': ['usd', (n) => (state.usd ? (n / state.usd) * 1e8 : null)], 'cv-eur': ['eur', (n) => (state.eur ? (n / state.eur) * 1e8 : null)] };
  for (const [id, [key, toSats]] of Object.entries(map)) {
    $(id).addEventListener('input', (e) => {
      const n = parseNum(e.target.value);
      if (n == null) return;
      const s = toSats(n);
      if (s == null) return;
      convSats = s;
      renderConverterFromSats(key);
    });
    $(id).addEventListener('blur', () => renderConverterFromSats());
  }
}

// ---------------------------------------------------------------- UI-Verdrahtung

function stamp() { $('last-update').textContent = 'Aktualisiert ' + new Date().toLocaleTimeString(LOCALE); }

function bindUi() {
  // Maus-Glow auf Karten
  document.addEventListener('pointermove', (e) => {
    const card = e.target.closest?.('.card');
    if (!card) return;
    const r = card.getBoundingClientRect();
    card.style.setProperty('--mx', `${e.clientX - r.left}px`);
    card.style.setProperty('--my', `${e.clientY - r.top}px`);
  }, { passive: true });

  $('price-range').addEventListener('click', (e) => {
    const b = e.target.closest('button'); if (!b) return;
    $('price-range').querySelectorAll('button').forEach((x) => x.classList.toggle('on', x === b));
    loadPriceChart(Number(b.dataset.days));
  });
  $('hist-range').addEventListener('click', (e) => {
    const b = e.target.closest('button'); if (!b) return;
    $('hist-range').querySelectorAll('button').forEach((x) => x.classList.toggle('on', x === b));
    state.histHours = Number(b.dataset.h);
    renderMinerChart();
  });

  // Rechner-Eingaben
  $('in-kwh').value = settings.kwh;
  $('in-watt').value = settings.watt ?? '';
  $('in-kwh').addEventListener('input', (e) => { const n = parseNum(e.target.value); if (n != null) { settings.kwh = n; saveSettings(); renderCalcs(); } });
  $('in-watt').addEventListener('input', (e) => { settings.watt = e.target.value === '' ? '' : parseNum(e.target.value); saveSettings(); renderCalcs(); });

  // Einstellungen
  const dlg = $('dlg-settings');
  const open = () => {
    $('set-host').value = settings.host;
    $('set-interval').value = String(settings.interval);
    $('set-demo').checked = settings.demo;
    $('test-result').textContent = '';
    dlg.showModal();
    setTimeout(() => $('set-host').focus(), 50);
  };
  $('btn-settings').addEventListener('click', open);
  $('btn-miner-config').addEventListener('click', open);
  document.addEventListener('click', (e) => { if (e.target.closest('[data-open-settings]')) open(); });

  $('btn-test').addEventListener('click', async () => {
    const host = $('set-host').value.trim();
    const out = $('test-result');
    if (!host) { out.className = 'mono tiny err-text'; out.textContent = 'Bitte Adresse eingeben'; return; }
    out.className = 'mono tiny'; out.textContent = 'Teste …';
    try {
      const m = await fetchMiner(host);
      if (m.hashRate == null) throw new Error('Keine AxeOS-Antwort');
      out.className = 'mono tiny ok-text';
      out.textContent = `✓ ${m.hostname || 'Bitaxe'} · ${m.ASICModel || ''} · ${fmt(m.hashRate / 1000, 2)} TH/s`;
    } catch (err) {
      out.className = 'mono tiny err-text'; out.textContent = '✕ ' + err.message;
    }
  });

  $('btn-clear').addEventListener('click', () => {
    store.del(histKey()); history = []; lastSampleAt = 0;
    renderMinerChart(); toast('Verlauf gelöscht');
  });

  dlg.addEventListener('close', () => {
    if (dlg.returnValue !== 'save') return;
    const host = $('set-host').value.trim().replace(/^https?:\/\//, '').replace(/\/+$/, '');
    const changed = host !== settings.host;
    settings.host = host;
    settings.interval = Number($('set-interval').value);
    settings.demo = $('set-demo').checked;
    saveSettings();
    if (changed) {
      history = store.get(histKey(), []);
      lastSampleAt = history.length ? history[history.length - 1].t : 0;
      state.failCount = 0; state.lastOnline = 0; state.miner = null;
    }
    toast(settings.demo ? 'Demo-Modus aktiv' : host ? `Verbinde mit ${host} …` : 'Keine Adresse – Demo-Modus');
    pollMiner();
  });

  // Aktiver Tab beim Scrollen
  const tabs = [...document.querySelectorAll('.tab')];
  const io = new IntersectionObserver((entries) => {
    for (const en of entries) if (en.isIntersecting) tabs.forEach((t) => t.classList.toggle('active', t.getAttribute('href') === '#' + en.target.id));
  }, { rootMargin: '-40% 0px -55% 0px' });
  ['overview', 'miner', 'tools', 'network'].forEach((id) => io.observe($(id)));

  // Uhr + Countdown
  setInterval(() => {
    $('clock').textContent = new Date().toLocaleTimeString(LOCALE);
    tickHalving();
  }, 1000);
  setInterval(renderBlockAge, 15_000);
}

// ---------------------------------------------------------------- Start

async function boot() {
  if (location.protocol === 'file:') {
    document.body.insertAdjacentHTML('afterbegin', '<div class="banner err" style="margin:12px">Bitte über den lokalen Server öffnen: <code>npm start</code> → http://127.0.0.1:8420</div>');
  }
  initCharts();
  bindUi();
  bindConverter();
  if (!settings.host) {
    try { const c = await getJson('/config', 3000); if (c.defaultHost) { settings.host = c.defaultHost; saveSettings(); history = store.get(histKey(), []); } } catch {}
  }
  pollMiner();
  loadPrice();
  loadPriceChart(1);
  loadNetwork();
  setInterval(loadPrice, 60_000);
  setInterval(loadNetwork, 30_000);
  setInterval(() => { priceCache.delete(state.priceDays); loadPriceChart(state.priceDays); }, 5 * 60_000);
}

boot();
