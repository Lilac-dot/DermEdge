// DermEdge's image models, running in the visitor's browser. Nothing is uploaded: the model
// files are downloaded once (and cached by the browser), and every photo stays on the device.
//
//   Stage 1  CLIP ViT-B/16 image encoder (models/clip_vision_w8.*.onnx, 8-bit weights, from
//            scripts/export_web_clip.py) + the fine-tuned rash head and per-skin-type offsets
//            (models/heads.json)
//   Stage 2  eczema vs look-alikes head on the same CLIP embedding (heads.json "eczema", from
//            scripts/train_web_heads_v2.py; it replaced a ResNet18 that was at chance on real
//            phone photos). On laptops an occlusion map shows which parts of the photo it relied on.
//   Stage 3  severity head on the same CLIP embedding
// CLIP runs on ONNX Runtime Web: WebGPU where the browser has it, else multithreaded
// WebAssembly (vercel.json sets the cross-origin isolation headers threads need).
// Checked against the Python model: the 8-bit CLIP keeps the embedding within cosine 0.999 of
// fp32 and the same stage-1 decision on 98.7% of 300 SCIN phone photos (scripts/
// check_web_clip_quantized.py).

// Bump when a file in web/models changes, so browsers that cached the old copy fetch the new one
// (also bump every ?v= in local-api.js).
export const MODELS_V = 4;
const CLIP_MEAN = [0.48145466, 0.4578275, 0.40821073], CLIP_STD = [0.26862954, 0.26130258, 0.27577711];
const CLIP_PARTS = 2, CACHE = 'dermedge-models';
let clip, text, heads, ready, backend = '';

function report(onProgress, msg, frac) { if (onProgress) onProgress(msg, frac); }

// Phones get WebAssembly, not WebGPU: mobile GPUs can build the session and then fail or stall on
// the first run, and phone browsers have far less memory to fall back with.
const MOBILE = (navigator.userAgentData && navigator.userAgentData.mobile) || /Android|iPhone|iPad|iPod|Mobile/i.test(navigator.userAgent);
// Smaller ONNX Runtime memory footprint on WebAssembly (no pre-reserved arena or memory patterns).
const WASM_OPTS = { executionProviders: ['wasm'], enableCpuMemArena: false, enableMemPattern: false };

// Fetches a model file through the Cache API, so later visits read it from disk instead of the
// network (the HTTP cache may evict files this large). onBytes(n) reports bytes as they arrive.
// With a plain Content-Length the bytes go straight into one buffer, so a phone holds one copy
// of the file while it downloads instead of two.
async function fetchModel(url, onBytes) {
  let cache = null;
  try { cache = await caches.open(CACHE); } catch (e) { cache = null; }
  const hit = cache && await cache.match(url);
  if (hit) { const b = await hit.arrayBuffer(); onBytes(b.byteLength); return new Uint8Array(b); }
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  const len = res.headers.get('Content-Encoding') ? 0 : +res.headers.get('Content-Length') || 0;
  const reader = res.body.getReader(), chunks = [];
  let out = len ? new Uint8Array(len) : null, n = 0, w = 0;  // w: bytes written into out
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    if (out && !chunks.length && w + value.length <= len) { out.set(value, w); w += value.length; } else chunks.push(value);
    n += value.length; onBytes(value.length);
  }
  if (!out || n !== len) {  // no usable length (or it was wrong): join the pieces instead
    const all = new Uint8Array(n);
    if (out) all.set(out.subarray(0, w));
    let o = w; for (const c of chunks) { all.set(c, o); o += c.length; }
    out = all;
  }
  if (cache) await cache.put(url, new Response(out)).catch(() => {});
  return out;
}

async function dropOldModels() {  // cached files from earlier MODELS_V
  try {
    const cache = await caches.open(CACHE);
    for (const req of await cache.keys()) if (!req.url.includes(`v=${MODELS_V}`)) cache.delete(req);
  } catch (e) { /* no Cache API (private window): nothing to clean */ }
}

async function loadClip(onProgress) {
  const TOTAL = 89.6e6;  // both parts, for the progress bar before their sizes are known
  let got = 0;
  const onBytes = n => {
    got += n;
    report(onProgress, `Loading the skin model: ${Math.round(got / 1e6)} of ${Math.round(TOTAL / 1e6)} MB`, Math.min(got / TOTAL, 0.99));
  };
  const parts = await Promise.all([...Array(CLIP_PARTS).keys()].map(i => fetchModel(`/models/clip_vision_w8.${i}.onnx?v=${MODELS_V}`, onBytes)));
  const bytes = new Uint8Array(parts.reduce((a, p) => a + p.length, 0));
  let o = 0; for (const p of parts) { bytes.set(p, o); o += p.length; }
  report(onProgress, 'Starting the skin model…', 0.99);
  let gpu = false;
  if (!MOBILE) try { gpu = !!(navigator.gpu && await navigator.gpu.requestAdapter()); } catch (e) { gpu = false; }
  if (gpu) {
    // A test run on a blank image, so a GPU that builds the session but can't run it still falls
    // back to WebAssembly here rather than failing the first check.
    try {
      clip = await window.ort.InferenceSession.create(bytes, { executionProviders: ['webgpu'] });
      await clip.run({ pixel_values: new window.ort.Tensor('float32', new Float32Array(3 * 224 * 224), [1, 3, 224, 224]) });
      backend = 'WebGPU'; return;
    } catch (e) {
      console.warn('WebGPU CLIP failed, falling back to WebAssembly', e);
      if (clip) clip.release().catch(() => {});
      clip = null;
    }
  }
  clip = await window.ort.InferenceSession.create(bytes, WASM_OPTS);
  backend = `WebAssembly, ${window.crossOriginIsolated ? window.ort.env.wasm.numThreads || 'multi' : 1} thread(s)`;
}

// A failed load (a dropped connection, the phone running short of memory) is forgotten, so a
// later check tries again instead of failing until the app is restarted; the live camera's
// checks within 20 s of a failure get the same error rather than restarting the download.
let failed = null;
export function init(onProgress) {
  if (!ready && failed && Date.now() - failed.at < 20000) return Promise.reject(failed.error);
  if (!ready) ready = (async () => {
    report(onProgress, 'Loading DermEdge models…', 0);
    window.ort.env.wasm.wasmPaths = 'https://cdn.jsdelivr.net/npm/onnxruntime-web@1.20.1/dist/';
    dropOldModels();
    const clipReady = loadClip(onProgress);
    [text, heads] = await Promise.all(['clip_text', 'heads'].map(n => fetch(`/models/${n}.json?v=${MODELS_V}`).then(r => r.json())));
    await clipReady;
    report(onProgress, `Models ready (${backend})`, 1);
  })().catch(e => {
    ready = null; clip = null;
    failed = { at: Date.now(), error: new Error(`the skin model didn't load (${e.message || e}). Try again, or reopen the app.`) };
    throw failed.error;
  });
  return ready;
}

// ONNX Runtime sessions can't run twice at once ("Session already started"), and the webcam's
// live check fires while an Analyze is still running, so model runs go through one queue.
let queue = Promise.resolve();
function serial(fn) { const run = queue.then(fn, fn); queue = run.catch(() => {}); return run; }

const dot = (a, b) => { let s = 0; for (let i = 0; i < a.length; i++) s += a[i] * b[i]; return s; };
const sigmoid = z => 1 / (1 + Math.exp(-z));

export async function bitmap(blob) { return createImageBitmap(blob, { imageOrientation: 'from-image' }); }

// CLIP preprocessing (openai/clip-vit-base-patch16): shortest side to 224, centre crop 224,
// then normalise with CLIP's mean and std.
function clipPixels(bmp) {
  const s = 224 / Math.min(bmp.width, bmp.height), w = Math.round(bmp.width * s), h = Math.round(bmp.height * s);
  const c = new OffscreenCanvas(224, 224), g = c.getContext('2d');
  g.imageSmoothingEnabled = true; g.imageSmoothingQuality = 'high';
  g.drawImage(bmp, (224 - w) / 2, (224 - h) / 2, w, h);
  const d = g.getImageData(0, 0, 224, 224).data, x = new Float32Array(3 * 224 * 224);
  for (let i = 0; i < 224 * 224; i++) for (let ch = 0; ch < 3; ch++) x[ch * 50176 + i] = (d[4 * i + ch] / 255 - CLIP_MEAN[ch]) / CLIP_STD[ch];
  return x;
}

async function embedPixels(x) {
  const out = await serial(() => clip.run({ pixel_values: new window.ort.Tensor('float32', x, [1, 3, 224, 224]) }));
  const e = Array.from(out.image_embeds.data), n = Math.hypot(...e);
  return e.map(v => v / n);
}

async function clipEmbedding(bmp) { return embedPixels(clipPixels(bmp)); }

const eczemaLogit = e => dot(e, heads.eczema.w) + heads.eczema.b;

// Occlusion map for stage 2: cover one cell of an n x n grid at a time with CLIP's mean colour
// (0 after normalising) and see how far the eczema-vs-look-alike score moves away from its
// answer. Cells that move it most are what the decision rested on. The n*n covered copies go
// through CLIP as one batch. Laptops only: a 4 x 4 grid on WebGPU, 3 x 3 on WebAssembly.
async function occlusionMap(x, base, n = 4) {
  const dir = base >= 0 ? 1 : -1, cell = 224 / n, N = 3 * 50176, batch = new Float32Array(n * n * N);
  for (let k = 0; k < n * n; k++) {
    const gy = Math.floor(k / n), gx = k % n, o = k * N;
    batch.set(x, o);
    for (let ch = 0; ch < 3; ch++) for (let r = Math.round(gy * cell); r < Math.round((gy + 1) * cell); r++)
      batch.fill(0, o + ch * 50176 + r * 224 + Math.round(gx * cell), o + ch * 50176 + r * 224 + Math.round((gx + 1) * cell));
  }
  const out = await serial(() => clip.run({ pixel_values: new window.ort.Tensor('float32', batch, [n * n, 3, 224, 224]) }));
  const d = out.image_embeds.data, cam = new Float32Array(n * n);
  for (let k = 0; k < n * n; k++) {
    const e = Array.from(d.subarray(k * 512, (k + 1) * 512)), m = Math.hypot(...e);
    cam[k] = Math.max(0, dir * (base - eczemaLogit(e.map(v => v / m))));
  }
  const mx = Math.max(...cam) || 1;
  return { n, v: cam.map(v => v / mx) };
}

function fstGroup(f) { return !f ? null : f <= 2 ? 'I-II' : f <= 4 ? 'III-IV' : 'V-VI'; }

function stage1(e, fitzpatrick) {
  // "No skin" must beat every skin prompt (normal, rash, body parts) by 4 logit points, so a hand
  // or arm in a busy webcam frame still counts as skin (app/pipeline.py NO_SKIN_MARGIN).
  const [normal, rash, noSkin, body] = ['normal', 'rash', 'no_skin', 'body'].map(k => 100 * dot(e, text[k]));
  const off = heads.rash.fst_offsets[fstGroup(fitzpatrick)] || 0;
  return { p_rash: sigmoid(dot(e, heads.rash.w) + heads.rash.b + off), skin: noSkin <= Math.max(normal, rash, body) + 4,
           calibrated_for: fstGroup(fitzpatrick) };
}

export async function screen(blob, fitzpatrick) {
  await init();
  const s = stage1(await clipEmbedding(await bitmap(blob)), fitzpatrick);
  return { p_rash: +s.p_rash.toFixed(3), skin: s.skin };
}

// sharpness = variance of the 3x3 Laplacian on a 256-px greyscale copy; brightness = mean grey
export function quality(bmp) {
  const s = 256 / Math.max(bmp.width, bmp.height), w = Math.max(1, Math.round(bmp.width * s)), h = Math.max(1, Math.round(bmp.height * s));
  const c = new OffscreenCanvas(w, h), g = c.getContext('2d'); g.drawImage(bmp, 0, 0, w, h);
  const d = g.getImageData(0, 0, w, h).data, y = new Float32Array(w * h);
  let sum = 0;
  for (let i = 0; i < w * h; i++) { y[i] = 0.299 * d[4 * i] + 0.587 * d[4 * i + 1] + 0.114 * d[4 * i + 2]; sum += y[i]; }
  const lap = [];
  for (let r = 1; r < h - 1; r++) for (let q = 1; q < w - 1; q++) {
    const i = r * w + q; lap.push(y[i - 1] + y[i + 1] + y[i - w] + y[i + w] - 4 * y[i]);
  }
  const m = lap.reduce((a, b) => a + b, 0) / lap.length;
  const sharpness = lap.reduce((a, b) => a + (b - m) ** 2, 0) / lap.length, brightness = sum / (w * h);
  const issues = brightness < 40 ? ['too_dark'] : brightness > 225 ? ['too_bright'] : sharpness < 15 ? ['blurry'] : [];
  return { ok: !issues.length, issues, sharpness: +sharpness.toFixed(1), brightness: +brightness.toFixed(1) };
}

function jet(v) {  // matplotlib-like jet colormap, v in 0..1
  const r = Math.min(1, Math.max(0, 1.5 - Math.abs(4 * v - 3))), g = Math.min(1, Math.max(0, 1.5 - Math.abs(4 * v - 2))),
        b = Math.min(1, Math.max(0, 1.5 - Math.abs(4 * v - 1)));
  return [r * 255, g * 255, b * 255];
}

export function toDataURL(bmp, size = 512, cam = null) {
  const s = size / Math.max(bmp.width, bmp.height), w = Math.round(bmp.width * Math.min(1, s)), h = Math.round(bmp.height * Math.min(1, s));
  const c = document.createElement('canvas'); c.width = w; c.height = h;
  const g = c.getContext('2d'); g.drawImage(bmp, 0, 0, w, h);
  if (cam) {  // over the centre square CLIP saw
    const m = document.createElement('canvas'); m.width = m.height = cam.n;
    const mg = m.getContext('2d'), id = mg.createImageData(cam.n, cam.n);
    cam.v.forEach((v, i) => { const [r, gg, b] = jet(v); id.data.set([r, gg, b, 255 * 0.55 * v], 4 * i); });
    mg.putImageData(id, 0, 0);
    const side = Math.min(w, h);
    g.imageSmoothingEnabled = true; g.imageSmoothingQuality = 'high'; g.drawImage(m, (w - side) / 2, (h - side) / 2, side, side);
  }
  return c.toDataURL('image/jpeg', 0.85);
}

export async function analyze(blob, fitzpatrick) {
  await init();
  const bmp = await bitmap(blob);
  const x = clipPixels(bmp), e = await embedPixels(x), s = stage1(e, fitzpatrick);
  const res = { p_rash: +s.p_rash.toFixed(3), skin: s.skin, calibrated_for: s.calibrated_for, p_eczema: null, severity: null };
  let cam = null;
  if (s.skin && s.p_rash >= 0.5) {
    const z = eczemaLogit(e), p = sigmoid(z);
    res.p_eczema = +p.toFixed(3);
    if (!MOBILE) cam = await occlusionMap(x, z, backend === 'WebGPU' ? 4 : 3);
    if (p >= 0.5) {
      const score = Math.min(12, Math.max(0, dot(e, heads.severity.w) + heads.severity.b));
      const band = heads.severity.classes[score < heads.severity.band_edges[0] ? 0 : score < heads.severity.band_edges[1] ? 1 : 2];
      res.severity = { score: +score.toFixed(1), band };
    }
  }
  return { res, bmp, cam };
}

export function fairness() { return { before: heads.fairness.before, after: heads.fairness.after, offsets: heads.rash.fst_offsets }; }
export function loadedHeads() { return heads; }
