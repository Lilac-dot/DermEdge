// DermEdge's image models, running in the visitor's browser. Nothing is uploaded: the model
// files are downloaded once (and cached by the browser), and every photo stays on the device.
//
//   Stage 1  CLIP ViT-B/16 image encoder (models/clip_vision_w8.*.onnx, 8-bit weights, from
//            scripts/export_web_clip.py) + the fine-tuned rash head and per-skin-type offsets
//            (models/heads.json)
//   Stage 2  ResNet18 eczema vs 7 look-alikes (models/stage2.onnx) with a class activation map
//            from its layer4 features
//   Stage 3  severity head on the same CLIP embedding
// All three run on ONNX Runtime Web: WebGPU where the browser has it, else multithreaded
// WebAssembly (vercel.json sets the cross-origin isolation headers threads need).
// Checked against the Python models: the 8-bit CLIP keeps the embedding within cosine 0.999 of
// fp32 and the same stage-1 decision on 98.7% of 300 SCIN phone photos (scripts/
// check_web_clip_quantized.py); the ONNX ResNet18 matches PyTorch exactly.

// Bump when a file in web/models changes, so browsers that cached the old copy fetch the new one
// (also bump every ?v= in local-api.js).
export const MODELS_V = 3;
const MEAN = [0.485, 0.456, 0.406], STD = [0.229, 0.224, 0.225];
const CLIP_MEAN = [0.48145466, 0.4578275, 0.40821073], CLIP_STD = [0.26862954, 0.26130258, 0.27577711];
const CLIP_PARTS = 2, CACHE = 'dermedge-models';
let clip, stage2, stage2Ready, text, heads, fc, ready, backend = '';

function report(onProgress, msg, frac) { if (onProgress) onProgress(msg, frac); }

// Fetches a model file through the Cache API, so later visits read it from disk instead of the
// network (the HTTP cache may evict files this large). onBytes(n) reports bytes as they arrive.
async function fetchModel(url, onBytes) {
  let cache = null;
  try { cache = await caches.open(CACHE); } catch (e) { cache = null; }
  const hit = cache && await cache.match(url);
  if (hit) { const b = await hit.arrayBuffer(); onBytes(b.byteLength); return new Uint8Array(b); }
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  const reader = res.body.getReader(), chunks = [];
  let n = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value); n += value.length; onBytes(value.length);
  }
  const out = new Uint8Array(n);
  let o = 0; for (const c of chunks) { out.set(c, o); o += c.length; }
  if (cache) cache.put(url, new Response(out)).catch(() => {});
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
  try { gpu = !!(navigator.gpu && await navigator.gpu.requestAdapter()); } catch (e) { gpu = false; }
  if (gpu) {
    try { clip = await window.ort.InferenceSession.create(bytes, { executionProviders: ['webgpu'] }); backend = 'WebGPU'; return; }
    catch (e) { console.warn('WebGPU CLIP failed, falling back to WebAssembly', e); }
  }
  clip = await window.ort.InferenceSession.create(bytes, { executionProviders: ['wasm'] });
  backend = `WebAssembly, ${window.crossOriginIsolated ? window.ort.env.wasm.numThreads || 'multi' : 1} thread(s)`;
}

// Stage 2 is only needed once a photo screens positive, so it downloads in the background
// while CLIP loads instead of in front of it. Its session is created only after CLIP's: two
// sessions starting at once both try to initialise ONNX Runtime's WebAssembly ("multiple calls
// to 'initWasm()'"), which knocks CLIP off WebGPU.
let clipReady;
function loadStage2() {
  if (!stage2Ready) stage2Ready = Promise.all([fetchModel(`/models/stage2.onnx?v=${MODELS_V}`, () => {}), clipReady])
    .then(([b]) => window.ort.InferenceSession.create(b, { executionProviders: ['wasm'] }))
    .then(s => { stage2 = s; }, e => { stage2Ready = null; throw e; });
  return stage2Ready;
}

export function init(onProgress) {
  if (!ready) ready = (async () => {
    report(onProgress, 'Loading DermEdge models…', 0);
    window.ort.env.wasm.wasmPaths = 'https://cdn.jsdelivr.net/npm/onnxruntime-web@1.20.1/dist/';
    dropOldModels();
    clipReady = loadClip(onProgress);
    loadStage2().catch(e => console.warn('stage 2 preload failed; retrying on first use', e));
    [text, heads, fc] = await Promise.all(['clip_text', 'heads', 'stage2_fc'].map(n => fetch(`/models/${n}.json?v=${MODELS_V}`).then(r => r.json())));
    await clipReady;
    report(onProgress, `Models ready (${backend})`, 1);
  })();
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
  return new window.ort.Tensor('float32', x, [1, 3, 224, 224]);
}

async function clipEmbedding(bmp) {
  const out = await serial(() => clip.run({ pixel_values: clipPixels(bmp) }));
  const e = Array.from(out.image_embeds.data), n = Math.hypot(...e);
  return e.map(x => x / n);
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

async function stage2Run(bmp) {
  const c = new OffscreenCanvas(224, 224), g = c.getContext('2d'); g.drawImage(bmp, 0, 0, 224, 224);
  const d = g.getImageData(0, 0, 224, 224).data, x = new Float32Array(3 * 224 * 224);
  for (let i = 0; i < 224 * 224; i++) for (let ch = 0; ch < 3; ch++) x[ch * 50176 + i] = (d[4 * i + ch] / 255 - MEAN[ch]) / STD[ch];
  const out = await serial(() => stage2.run({ image: new window.ort.Tensor('float32', x, [1, 3, 224, 224]) }));
  const [l0, l1] = out.logits.data, p = 1 / (1 + Math.exp(l0 - l1));
  const f = out.features.data, dir = p >= 0.5 ? 1 : -1, cam = new Float32Array(49);
  for (let i = 0; i < 49; i++) { let s = 0; for (let ch = 0; ch < 512; ch++) s += fc.w[ch] * f[ch * 49 + i]; cam[i] = Math.max(0, dir * s); }
  const mx = Math.max(...cam) || 1;
  return { p, cam: cam.map(v => v / mx) };
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
  if (cam) {
    const m = document.createElement('canvas'); m.width = m.height = 7;
    const mg = m.getContext('2d'), id = mg.createImageData(7, 7);
    cam.forEach((v, i) => { const [r, gg, b] = jet(v); id.data.set([r, gg, b, 255 * 0.55 * v], 4 * i); });
    mg.putImageData(id, 0, 0);
    g.imageSmoothingEnabled = true; g.imageSmoothingQuality = 'high'; g.drawImage(m, 0, 0, w, h);
  }
  return c.toDataURL('image/jpeg', 0.85);
}

export async function analyze(blob, fitzpatrick) {
  await init();
  const bmp = await bitmap(blob);
  const e = await clipEmbedding(bmp), s = stage1(e, fitzpatrick);
  const res = { p_rash: +s.p_rash.toFixed(3), skin: s.skin, calibrated_for: s.calibrated_for, p_eczema: null, severity: null };
  let cam = null;
  if (s.skin && s.p_rash >= 0.5) {
    await loadStage2();
    const r = await stage2Run(bmp); res.p_eczema = +r.p.toFixed(3); cam = r.cam;
    if (r.p >= 0.5) {
      const score = Math.min(12, Math.max(0, dot(e, heads.severity.w) + heads.severity.b));
      const band = heads.severity.classes[score < heads.severity.band_edges[0] ? 0 : score < heads.severity.band_edges[1] ? 1 : 2];
      res.severity = { score: +score.toFixed(1), band };
    }
  }
  return { res, bmp, cam };
}

export function fairness() { return { before: heads.fairness.before, after: heads.fairness.after, offsets: heads.rash.fst_offsets }; }
export function loadedHeads() { return heads; }
