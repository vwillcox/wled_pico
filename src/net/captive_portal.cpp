#include "captive_portal.h"

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <LittleFS.h>
#include <WiFi.h>

namespace {

constexpr uint16_t kDnsPort = 53;

// Talks to the real /json/state API and /ws live channel (see
// src/api/json_api.h) rather than one-off endpoints, so anything driving
// this page could equally well be curl or a real WLED app hitting the
// same routes. /update (src/api/ota.h) handles the firmware upload form.
const char kIndexHtml[] PROGMEM = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>wled_pico</title>
<style>
  body { font-family: sans-serif; background: #111; color: #eee; padding: 1.5rem; max-width: 420px; margin: 0 auto; }
  h1 { font-size: 1.2rem; }
  h2 { font-size: 0.95rem; color: #ccc; margin-top: 2rem; border-top: 1px solid #333; padding-top: 1rem; }
  label { display: block; margin-top: 1rem; font-size: 0.85rem; color: #aaa; }
  input[type=range] { width: 100%; }
  select, input[type=color], input[type=file] { width: 100%; padding: 0.4rem; margin-top: 0.25rem; }
  .row { display: flex; gap: 1rem; }
  .row > div { flex: 1; }
  button { margin-top: 1rem; width: 100%; padding: 0.6rem; font-size: 1rem; background: #333; color: #eee; border: 1px solid #555; border-radius: 4px; }
  .presets { display: flex; flex-wrap: wrap; gap: 0.5rem; margin-top: 0.5rem; }
  .presets button { width: auto; flex: 1 0 20%; margin-top: 0; }
  #status { font-size: 0.8rem; color: #888; margin-top: 0.5rem; }
</style>
</head>
<body>
<h1>wled_pico</h1>
<div id="status">connecting…</div>

<button id="onoff">On / Off</button>

<label>Brightness <span id="briVal"></span></label>
<input type="range" min="1" max="255" id="bri">

<label>Effect</label>
<select id="fx"></select>

<label>Palette</label>
<select id="palette"></select>

<label>Speed <span id="sxVal"></span></label>
<input type="range" min="0" max="255" id="sx">

<label>Intensity <span id="ixVal"></span></label>
<input type="range" min="0" max="255" id="ix">

<div class="row">
  <div>
    <label>Custom 1 <span id="c1Val"></span></label>
    <input type="range" min="0" max="255" id="c1">
  </div>
  <div>
    <label>Custom 2 <span id="c2Val"></span></label>
    <input type="range" min="0" max="255" id="c2">
  </div>
  <div>
    <label>Custom 3 <span id="c3Val"></span></label>
    <input type="range" min="0" max="255" id="c3">
  </div>
</div>

<div class="row" style="margin-top:1rem;">
  <label style="display:flex;align-items:center;gap:0.4rem;margin-top:0;">
    <input type="checkbox" id="o1" style="width:auto;"> Option 1
  </label>
  <label style="display:flex;align-items:center;gap:0.4rem;margin-top:0;">
    <input type="checkbox" id="o2" style="width:auto;"> Option 2
  </label>
  <label style="display:flex;align-items:center;gap:0.4rem;margin-top:0;">
    <input type="checkbox" id="o3" style="width:auto;"> Option 3
  </label>
</div>
<p style="font-size:0.75rem;color:#888;margin-top:0.4rem;">
  "Custom"/"Option" meanings vary per effect - same as real WLED's generic
  sliders/checkboxes (e.g. Custom 3 is ball count on Bouncing Balls, ghost
  count on PacMan; Option 1 flips a color mode on Percent). Check the
  per-effect comment in <code>src/effects/</code> if one seems to do
  nothing - some effects don't use all of them.
</p>

<div class="row">
  <div>
    <label>Primary</label>
    <input type="color" id="col0" value="#ff0000">
  </div>
  <div>
    <label>Secondary</label>
    <input type="color" id="col1" value="#000000">
  </div>
  <div>
    <label>Tertiary</label>
    <input type="color" id="col2" value="#000000">
  </div>
</div>

<label>Scroll text (for the "Scrolling Text" effect)</label>
<input type="text" id="scrollText" maxlength="32">

<h2>Custom image</h2>
<p style="font-size:0.8rem;color:#888;">
  Any image file - it's resized to 32x32 right here in the browser (stretched
  to fill, not cropped) before uploading, so this device never has to decode
  a JPEG/PNG/GIF itself. Animated GIFs play back too, up to 40 frames -
  Chrome/Edge/desktop Firefox decode every real frame and its exact timing;
  other browsers (older Firefox, Firefox on Android, Safari) instead play
  the GIF natively for a few seconds and record what's on screen, which
  looks right for most animations but won't perfectly match an unusual
  loop point or exact per-frame timing. Select the "Image" effect above to
  display it.
</p>
<input type="file" id="imageFile" accept="image/*">
<div id="imageStatus" style="font-size:0.8rem;color:#888;margin-top:0.5rem;"></div>

<h2>Join WiFi</h2>
<p style="font-size:0.8rem;color:#888;">
  Drops this device's own WLED-Pico-Setup AP to join an existing network
  instead, and remembers it - it'll auto-rejoin on every future boot until
  you hit Forget below. <strong>This page's own connection will drop the
  moment you hit Connect</strong> (the AP it's connected through is what's
  going away); if the join fails the AP comes back within ~25s but your
  phone won't auto-rejoin it, so you'll need to reselect WLED-Pico-Setup in
  WiFi settings.
</p>
<label>SSID</label>
<input type="text" id="staSsid">
<label>Password</label>
<input type="password" id="staPass">
<button id="staConnect">Connect</button>
<button id="staForget">Forget saved WiFi</button>
<div id="staStatus" style="font-size:0.8rem;color:#888;margin-top:0.5rem;"></div>

<h2>Presets</h2>
<div class="presets" id="presetLoad"></div>
<div class="presets" id="presetSave"></div>

<h2>Firmware update</h2>
<form id="otaForm">
  <input type="file" id="otaFile" name="update" accept=".bin">
  <button type="submit">Upload &amp; reboot</button>
</form>

<script>
let state = null;
let ws = null;
let suppressEcho = false;

function hexToRgb(hex) {
  const n = parseInt(hex.slice(1), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}
function rgbToHex([r, g, b]) {
  return '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0')).join('');
}

function applyToUI(s) {
  state = s;
  suppressEcho = true;
  document.getElementById('bri').value = s.bri;
  document.getElementById('briVal').textContent = s.bri;
  const seg = s.seg[0];
  document.getElementById('fx').value = seg.fx;
  document.getElementById('palette').value = seg.pal;
  document.getElementById('sx').value = seg.sx;
  document.getElementById('sxVal').textContent = seg.sx;
  document.getElementById('ix').value = seg.ix;
  document.getElementById('ixVal').textContent = seg.ix;
  document.getElementById('c1').value = seg.c1;
  document.getElementById('c1Val').textContent = seg.c1;
  document.getElementById('c2').value = seg.c2;
  document.getElementById('c2Val').textContent = seg.c2;
  document.getElementById('c3').value = seg.c3;
  document.getElementById('c3Val').textContent = seg.c3;
  document.getElementById('o1').checked = seg.o1;
  document.getElementById('o2').checked = seg.o2;
  document.getElementById('o3').checked = seg.o3;
  document.getElementById('col0').value = rgbToHex(seg.col[0]);
  document.getElementById('col1').value = rgbToHex(seg.col[1]);
  document.getElementById('col2').value = rgbToHex(seg.col[2]);
  document.getElementById('scrollText').value = seg.n;
  suppressEcho = false;
}

function post(partial) {
  // Fire-and-forget over the WebSocket when it's up (that's the live
  // channel every other connected client also gets pushed on); falls back
  // to a plain POST if it's not connected yet.
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(partial));
  } else {
    fetch('/json/state', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(partial),
    }).then(r => r.json()).then(applyToUI);
  }
}

function connectWs() {
  ws = new WebSocket('ws://' + location.host + '/ws');
  ws.onopen = () => { document.getElementById('status').textContent = 'connected'; };
  ws.onclose = () => {
    document.getElementById('status').textContent = 'disconnected, retrying…';
    setTimeout(connectWs, 1500);
  };
  ws.onmessage = ev => { if (!suppressEcho) applyToUI(JSON.parse(ev.data)); };
}

document.getElementById('onoff').addEventListener('click', () => post({ on: !state.on }));
document.getElementById('bri').addEventListener('input', e => {
  document.getElementById('briVal').textContent = e.target.value;
  post({ bri: parseInt(e.target.value) });
});
document.getElementById('fx').addEventListener('change', e => post({ seg: [{ fx: parseInt(e.target.value) }] }));
document.getElementById('sx').addEventListener('input', e => {
  document.getElementById('sxVal').textContent = e.target.value;
  post({ seg: [{ sx: parseInt(e.target.value) }] });
});
document.getElementById('ix').addEventListener('input', e => {
  document.getElementById('ixVal').textContent = e.target.value;
  post({ seg: [{ ix: parseInt(e.target.value) }] });
});
document.getElementById('palette').addEventListener('change', e => post({ seg: [{ pal: parseInt(e.target.value) }] }));
document.getElementById('c1').addEventListener('input', e => {
  document.getElementById('c1Val').textContent = e.target.value;
  post({ seg: [{ c1: parseInt(e.target.value) }] });
});
document.getElementById('c2').addEventListener('input', e => {
  document.getElementById('c2Val').textContent = e.target.value;
  post({ seg: [{ c2: parseInt(e.target.value) }] });
});
document.getElementById('c3').addEventListener('input', e => {
  document.getElementById('c3Val').textContent = e.target.value;
  post({ seg: [{ c3: parseInt(e.target.value) }] });
});
document.getElementById('o1').addEventListener('change', e => post({ seg: [{ o1: e.target.checked }] }));
document.getElementById('o2').addEventListener('change', e => post({ seg: [{ o2: e.target.checked }] }));
document.getElementById('o3').addEventListener('change', e => post({ seg: [{ o3: e.target.checked }] }));
document.getElementById('col0').addEventListener('change', e => post({ seg: [{ col: [hexToRgb(e.target.value)] }] }));
document.getElementById('col1').addEventListener('change', e => post({ seg: [{ col: [null, hexToRgb(e.target.value)] }] }));
document.getElementById('col2').addEventListener('change', e => post({ seg: [{ col: [null, null, hexToRgb(e.target.value)] }] }));
document.getElementById('scrollText').addEventListener('change', e => post({ seg: [{ n: e.target.value }] }));

const loadDiv = document.getElementById('presetLoad');
const saveDiv = document.getElementById('presetSave');
for (let i = 1; i <= 8; i++) {
  const l = document.createElement('button');
  l.textContent = 'Load ' + i;
  l.addEventListener('click', () => post({ ps: i }));
  loadDiv.appendChild(l);

  const s = document.createElement('button');
  s.textContent = 'Save ' + i;
  s.addEventListener('click', () => post({ psave: i }));
  saveDiv.appendChild(s);
}

// Draws `source` (an ImageBitmap or a decoded ImageDecoder VideoFrame) onto
// a 32x32 canvas and returns its pixels as a plain RGB Uint8Array (3072
// bytes) - the browser does all the actual image-format decoding via
// createImageBitmap()/ImageDecoder before this ever runs, so this step
// itself is just a resize + RGBA-to-RGB strip.
function toRgb32x32(source) {
  const canvas = document.createElement('canvas');
  canvas.width = 32;
  canvas.height = 32;
  const ctx = canvas.getContext('2d');
  ctx.drawImage(source, 0, 0, 32, 32);
  const rgba = ctx.getImageData(0, 0, 32, 32).data;
  const rgb = new Uint8Array(32 * 32 * 3);
  for (let i = 0, j = 0; i < rgba.length; i += 4, j += 3) {
    rgb[j] = rgba[i]; rgb[j + 1] = rgba[i + 1]; rgb[j + 2] = rgba[i + 2];
  }
  return rgb;
}

// Decodes every frame of an animated image (GIF) via the WebCodecs
// ImageDecoder API - this is what does the actual GIF/LZW decoding, not
// this firmware. Returns [{delayMs, rgb}, ...], or null if this browser
// doesn't support ImageDecoder, the file isn't multi-frame, or decoding
// otherwise fails (caller falls back to a single still frame either way).
// kMaxFramesClient must match effects::kMaxFrames (image_data.h) - kept in
// sync by comment, not code.
async function decodeAnimatedFrames(file) {
  const kMaxFramesClient = 40;
  if (!('ImageDecoder' in window)) return null;
  let decoder;
  try {
    const buf = await file.arrayBuffer();
    decoder = new ImageDecoder({ data: buf, type: file.type || 'image/gif' });
    await decoder.tracks.ready;
    const track = decoder.tracks.selectedTrack;
    const frameCount = track ? track.frameCount : 1;
    if (!frameCount || frameCount <= 1) return null;
    const n = Math.min(frameCount, kMaxFramesClient);
    const frames = [];
    for (let i = 0; i < n; i++) {
      const { image } = await decoder.decode({ frameIndex: i });
      const rgb = toRgb32x32(image);
      const delayMs = Math.max(20, Math.round((image.duration || 100000) / 1000));
      image.close();
      frames.push({ delayMs, rgb });
    }
    return frames;
  } catch (err) {
    return null;  // fall back to the still-frame path
  } finally {
    if (decoder) decoder.close();
  }
}

// Universal animation fallback for browsers without ImageDecoder (notably
// Firefox for Android, which has no WebCodecs support at all, and any
// older browser) - every browser can animate a GIF/animated-WEBP natively
// in an <img>, so instead of decoding the file ourselves, this plays it
// off-screen and samples its rendered output over time via canvas,
// collapsing consecutive identical samples into one longer-delay frame.
// This can't recover the source's exact frame count/timing or detect its
// natural loop point - it's a fixed-duration recording, not a decode -
// but it plays back recognizably in literally any browser with no feature
// dependency at all. Returns null (falls through to the plain still-frame
// path) if the file never appears to change across the sampling window.
async function decodeAnimatedFramesBySampling(file) {
  const kMaxFramesClient = 40;
  const sampleIntervalMs = 60;
  const maxSampleDurationMs = 3000;
  const url = URL.createObjectURL(file);
  const img = document.createElement('img');
  try {
    img.src = url;
    await new Promise((resolve, reject) => { img.onload = resolve; img.onerror = reject; });
    // Must actually be rendered (not display:none) for most browsers to
    // keep animating it - parked off-screen instead of made invisible.
    img.style.position = 'fixed';
    img.style.left = '-9999px';
    document.body.appendChild(img);

    const frames = [];
    const start = performance.now();
    while (performance.now() - start < maxSampleDurationMs && frames.length < kMaxFramesClient) {
      const rgb = toRgb32x32(img);
      const last = frames[frames.length - 1];
      if (last && last.rgb.every((v, i) => v === rgb[i])) {
        last.delayMs += sampleIntervalMs;
      } else {
        frames.push({ delayMs: sampleIntervalMs, rgb });
      }
      await new Promise(r => setTimeout(r, sampleIntervalMs));
    }
    return frames.length > 1 ? frames : null;
  } catch (err) {
    return null;
  } finally {
    if (img.parentNode) img.parentNode.removeChild(img);
    URL.revokeObjectURL(url);
  }
}

document.getElementById('imageFile').addEventListener('change', async e => {
  const file = e.target.files[0];
  if (!file) return;
  const imgStatus = document.getElementById('imageStatus');
  try {
    imgStatus.textContent = 'decoding…';
    let frames = await decodeAnimatedFrames(file);
    // Only worth a 3-second sampling pass for formats that can actually
    // animate - skip it for a plain JPEG/PNG/etc, which would just burn
    // the wait and correctly find nothing.
    if (!frames && (file.type === 'image/gif' || file.type === 'image/webp')) {
      imgStatus.textContent = 'detecting animation…';
      frames = await decodeAnimatedFramesBySampling(file);
    }
    if (!frames) {
      const bitmap = await createImageBitmap(file);
      frames = [{ delayMs: 0, rgb: toRgb32x32(bitmap) }];
    }

    const header = new Uint8Array(2 + frames.length * 2);
    header[0] = frames.length & 0xFF;
    header[1] = (frames.length >> 8) & 0xFF;
    frames.forEach((f, i) => {
      header[2 + i * 2] = f.delayMs & 0xFF;
      header[2 + i * 2 + 1] = (f.delayMs >> 8) & 0xFF;
    });
    const payload = new Uint8Array(header.length + frames.length * 32 * 32 * 3);
    payload.set(header, 0);
    let off = header.length;
    for (const f of frames) { payload.set(f.rgb, off); off += f.rgb.length; }

    imgStatus.textContent = `uploading ${frames.length} frame${frames.length > 1 ? 's' : ''}…`;
    await fetch('/image', { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: payload });
    imgStatus.textContent = 'done - select the Image effect above to view it';
  } catch (err) {
    imgStatus.textContent = 'failed: ' + err;
  }
});

document.getElementById('otaForm').addEventListener('submit', async e => {
  e.preventDefault();
  const file = document.getElementById('otaFile').files[0];
  if (!file) return;
  const status = document.getElementById('status');
  status.textContent = 'uploading firmware…';
  const body = new FormData();
  body.append('update', file);
  try {
    const res = await fetch('/update', { method: 'POST', body });
    status.textContent = await res.text();
  } catch (err) {
    status.textContent = 'upload failed: device is rebooting or connection dropped';
  }
});

let staPoll = null;
async function refreshStaStatus() {
  const el = document.getElementById('staStatus');
  try {
    const s = await (await fetch('/wifi')).json();
    if (s.connected) {
      el.textContent = 'connected as ' + s.ip + ' (this page just lost its own connection - that\'s expected, the AP just dropped)';
      clearInterval(staPoll);
    } else if (s.failed) {
      el.textContent = 'failed to connect - falling back to this AP. Reconnect your phone to WLED-Pico-Setup.';
      clearInterval(staPoll);
    } else if (s.connecting) {
      el.textContent = 'connecting… (this page will lose its connection any moment - that\'s expected while the device switches off its own AP to try joining the other network)';
    } else {
      el.textContent = '';
    }
  } catch (err) {
    // Expected: the AP just went down because the device is now trying to
    // join the other network, so this page's own connection just dropped.
    el.textContent = 'lost connection to the device (expected - it just dropped its own AP to attempt the join). Reconnect to WLED-Pico-Setup to check the result, or find it on your other network.';
    clearInterval(staPoll);
  }
}
document.getElementById('staConnect').addEventListener('click', async () => {
  document.getElementById('staStatus').textContent = 'connecting…';
  try {
    await fetch('/wifi', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        ssid: document.getElementById('staSsid').value,
        pass: document.getElementById('staPass').value,
      }),
    });
  } catch (err) { /* connection may already be dropping by the time this returns */ }
  clearInterval(staPoll);
  staPoll = setInterval(refreshStaStatus, 1000);
});
document.getElementById('staForget').addEventListener('click', async () => {
  document.getElementById('staStatus').textContent = 'forgetting saved WiFi…';
  try {
    await fetch('/wifi/forget', { method: 'POST' });
  } catch (err) { /* AP is coming back up, connection may blip */ }
  document.getElementById('staStatus').textContent = 'forgotten - back on WLED-Pico-Setup';
});
refreshStaStatus();

// Effect IDs now match real WLED's numbering (see src/effects/effects.h),
// so /json/eff is a dense array naming every real WLED effect whether or
// not this firmware actually implements it (audio-reactive/particle-
// system effects and 2D scrolling text don't - see README.md's "Effects
// library" section). Cross-reference /json/implemented (not a real WLED
// endpoint, just for this page) so the dropdown only offers ones that'll
// actually render something, instead of silently falling back to a flat
// color when picked.
// Palettes (unlike effects) are all fully implemented - every one of the
// 72 real WLED palettes this firmware ports has a working
// color_from_palette() - so /json/pal needs no implemented-filtering.
Promise.all([
  fetch('/json/eff').then(r => r.json()),
  fetch('/json/implemented').then(r => r.json()),
  fetch('/json/pal').then(r => r.json()),
]).then(([names, implemented, paletteNames]) => {
  const fx = document.getElementById('fx');
  names.forEach((n, i) => {
    if (!implemented[i]) return;
    const opt = document.createElement('option');
    opt.value = i;
    opt.textContent = n;
    fx.appendChild(opt);
  });
  const palette = document.getElementById('palette');
  paletteNames.forEach((n, i) => {
    const opt = document.createElement('option');
    opt.value = i;
    opt.textContent = n;
    palette.appendChild(opt);
  });
  connectWs();
  fetch('/json/state').then(r => r.json()).then(applyToUI);
});
</script>
</body>
</html>
)HTML";

}  // namespace

void CaptivePortal::begin(const char *ap_ssid) {
  ap_ssid_ = ap_ssid;
  LittleFS.begin();  // idempotent - PresetStore also mounts this, fine either order

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid_);

  // Wildcard: every hostname resolves to us, which is what makes phones/
  // laptops pop their "sign in to network" captive portal prompt.
  dns_.start(kDnsPort, "*", WiFi.softAPIP());

  server_.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", kIndexHtml);
  });

  server_.on("/wifi", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    bool connected = !sta_failed_ && WiFi.status() == WL_CONNECTED;
    root["connected"] = connected;
    root["connecting"] = sta_attempted_ && !sta_failed_ && !connected;
    root["failed"] = sta_failed_;
    root["ip"] = connected ? WiFi.localIP().toString() : "";
    // Raw wl_status_t (see WiFi.h): 0=idle 1=no-ssid-avail 3=connected
    // 4=connect-failed 5=connection-lost 6=disconnected 7=scan-completed.
    // Exposed because there's no other way to see why a join failed - this
    // device has no accessible serial console right now.
    root["statusCode"] = static_cast<int>(WiFi.status());
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
  });

  // Dropping the AP here (not WIFI_AP_STA) is deliberate - see the header
  // comment on this class. loop_tick() restores the AP if this doesn't
  // connect within kStaConnectTimeoutMs. WiFi.disconnect() before each
  // attempt/fallback matters: without it, WiFi.status() can keep reporting
  // a stale WL_CONNECTED from a previous attempt after the radio's already
  // switched back to AP-only, which is exactly what happened here first.
  auto *wifi_post = new AsyncCallbackJsonWebHandler(
      "/wifi", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObjectConst root = json.as<JsonObjectConst>();
        const char *ssid = root["ssid"] | "";
        const char *pass = root["pass"] | "";
        if (ssid[0] != '\0') join(ssid, pass);
        request->send(200, "text/plain", "ok");
      });
  wifi_post->setMethod(HTTP_POST);
  server_.addHandler(wifi_post);

  server_.on("/wifi/forget", HTTP_POST, [this](AsyncWebServerRequest *request) {
    LittleFS.remove(kCredsPath);
    request->send(200, "text/plain", "ok");
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid_);
    dns_.start(kDnsPort, "*", WiFi.softAPIP());
    sta_attempted_ = false;
    sta_failed_ = false;
  });

  // Anything else (including the OS's own captive-portal probe URLs, e.g.
  // /generate_204, /hotspot-detect.html) gets the same page back. JsonApi's
  // /json/* routes and Ota's /update are registered separately and take
  // priority over this, since AsyncWebServer only falls through to
  // onNotFound when nothing else matched.
  server_.onNotFound([](AsyncWebServerRequest *request) {
    request->send(200, "text/html", kIndexHtml);
  });

  load_and_join_saved_credentials();
}

bool CaptivePortal::load_and_join_saved_credentials() {
  File f = LittleFS.open(kCredsPath, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  const char *ssid = doc["ssid"] | "";
  const char *pass = doc["pass"] | "";
  if (ssid[0] == '\0') return false;

  Serial.printf("[wifi] found saved credentials for \"%s\", auto-joining\n", ssid);
  join(ssid, pass);
  return true;
}

void CaptivePortal::save_credentials(const char *ssid, const char *pass) {
  JsonDocument doc;
  doc["ssid"] = ssid;
  doc["pass"] = pass;
  File f = LittleFS.open(kCredsPath, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

void CaptivePortal::join(const char *ssid, const char *pass) {
  save_credentials(ssid, pass);

  Serial.printf("[wifi] join: disconnecting...\n");
  WiFi.disconnect();
  Serial.printf("[wifi] join: switching to STA mode...\n");
  WiFi.mode(WIFI_STA);
  Serial.printf("[wifi] join: calling WiFi.begin(\"%s\", ...)\n", ssid);
  WiFi.begin(ssid, pass);
  Serial.printf("[wifi] join: WiFi.begin() returned, status=%d\n", static_cast<int>(WiFi.status()));

  sta_attempted_ = true;
  sta_failed_ = false;
  sta_attempt_start_ms_ = millis();
}

void CaptivePortal::loop_tick() {
  dns_.processNextRequest();

  if (sta_attempted_ && !sta_failed_) {
    static uint32_t last_status_print = 0;
    if (millis() - last_status_print > 1000) {
      last_status_print = millis();
      Serial.printf("[wifi] status=%d elapsed=%lums", static_cast<int>(WiFi.status()),
                    static_cast<unsigned long>(millis() - sta_attempt_start_ms_));
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf(" ip=%s", WiFi.localIP().toString().c_str());
      }
      Serial.printf("\n");
    }
  }

  if (sta_attempted_ && !sta_failed_ && WiFi.status() != WL_CONNECTED &&
      millis() - sta_attempt_start_ms_ > kStaConnectTimeoutMs) {
    Serial.printf("[wifi] join: timed out (status=%d), falling back to AP\n",
                  static_cast<int>(WiFi.status()));
    sta_failed_ = true;
    WiFi.disconnect();  // clear STA state so status() doesn't keep reporting stale WL_CONNECTED
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid_);
    dns_.start(kDnsPort, "*", WiFi.softAPIP());
    Serial.printf("[wifi] join: AP restored, IP=%s\n", WiFi.softAPIP().toString().c_str());
  }
}
