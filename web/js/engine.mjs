// DermEdge's image models, running in the visitor's browser. Nothing is uploaded: the model
// files are downloaded once (and cached by the browser), and every photo stays on the device.
//
//   Stage 1  CLIP ViT-B/16 image encoder (transformers.js, Xenova/clip-vit-base-patch16) +
//            the fine-tuned rash head and per-skin-type offsets (models/heads.json)
//   Stage 2  ResNet18 eczema vs 7 look-alikes (models/stage2.onnx, ONNX Runtime Web) with a
//            class activation map from its layer4 features
//   Stage 3  severity head on the same CLIP embedding
// Checked against the Python models: CLIP fp16/fp32 gives the same stage-1 decision on 98.5-99%
// of 200 held-out phone photos (AUC 0.931 vs 0.936); the ONNX ResNet18 matches PyTorch exactly.
import { AutoProcessor, CLIPVisionModelWithProjection, RawImage, env } from 'https://cdn.jsdelivr.net/npm/@huggingface/transformers@3.0.2';

env.allowLocalModels = false;
const CLIP_ID = 'Xenova/clip-vit-base-patch16';
const MEAN = [0.485, 0.456, 0.406], STD = [0.229, 0.224, 0.225];
let processor, vision, stage2, text, heads, fc, ready, backend = '';

function report(onProgress, msg, frac) { if (onProgress) onProgress(msg, frac); }

async function loadClip(onProgress) {
  const files = {};
  const progress_callback = p => {
    if (p.status === 'progress' && p.total) {
      files[p.file] = [p.loaded, p.total];
      const [l, t] = Object.values(files).reduce((a, [x, y]) => [a[0] + x, a[1] + y], [0, 0]);
      report(onProgress, `Downloading the skin model (first visit only): ${Math.round(l / 1e6)} of ${Math.round(t / 1e6)} MB`, l / t);
    }
  };
  processor = await AutoProcessor.from_pretrained(CLIP_ID);
  // WebGPU + fp16 (173 MB) where the browser has it; otherwise WebAssembly + fp32 (345 MB).
  let gpu = false;
  try { gpu = !!(navigator.gpu && await navigator.gpu.requestAdapter()); } catch (e) { gpu = false; }
  if (gpu) {
    try {
      vision = await CLIPVisionModelWithProjection.from_pretrained(CLIP_ID, { device: 'webgpu', dtype: 'fp16', progress_callback });
      backend = 'WebGPU'; return;
    } catch (e) { console.warn('WebGPU CLIP failed, falling back to WebAssembly', e); }
  }
  vision = await CLIPVisionModelWithProjection.from_pretrained(CLIP_ID, { device: 'wasm', dtype: 'fp32', progress_callback });
  backend = 'WebAssembly';
}

export function init(onProgress) {
  if (!ready) ready = (async () => {
    report(onProgress, 'Loading DermEdge models…', 0);
    [text, heads, fc] = await Promise.all(['clip_text', 'heads', 'stage2_fc'].map(n => fetch(`/models/${n}.json`).then(r => r.json())));
    window.ort.env.wasm.wasmPaths = 'https://cdn.jsdelivr.net/npm/onnxruntime-web@1.20.1/dist/';
    stage2 = await window.ort.InferenceSession.create('/models/stage2.onnx', { executionProviders: ['wasm'] });
    await loadClip(onProgress);
    report(onProgress, `Models ready (${backend})`, 1);
  })();
  return ready;
}

const dot = (a, b) => { let s = 0; for (let i = 0; i < a.length; i++) s += a[i] * b[i]; return s; };
const sigmoid = z => 1 / (1 + Math.exp(-z));

export async function bitmap(blob) { return createImageBitmap(blob, { imageOrientation: 'from-image' }); }

async function clipEmbedding(blob) {
  const { image_embeds } = await vision(await processor(await RawImage.fromBlob(blob)));
  const e = Array.from(image_embeds.data), n = Math.hypot(...e);
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
  const s = stage1(await clipEmbedding(blob), fitzpatrick);
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
  const out = await stage2.run({ image: new window.ort.Tensor('float32', x, [1, 3, 224, 224]) });
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
  const e = await clipEmbedding(blob), s = stage1(e, fitzpatrick);
  const res = { p_rash: +s.p_rash.toFixed(3), skin: s.skin, calibrated_for: s.calibrated_for, p_eczema: null, severity: null };
  let cam = null;
  if (s.skin && s.p_rash >= 0.5) {
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
