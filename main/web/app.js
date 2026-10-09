/*
 * Sayfa kartin icinden gelir. Video, tarayicinin MJPEG destegi ile <img> uzerinden akar.
 * Eski kare birikmez: soket hep en yeni kareyi ister.
 */

const view = document.querySelector("#view");
const placeholder = document.querySelector("#placeholder");
const placeholderText = placeholder.querySelector("p");
const modesEl = document.querySelector("#modes");
const ratesEl = document.querySelector("#rates");
const noteEl = document.querySelector("#note");

let latest = null;
let modes = [];
let pickedKey = null;
let streamWanted = false;
let userPaused = false;
let applying = false;
let polling = false;
let formatsReady = false;
let bannerOpened = false;
let retryTimer = 0;
let streamStartedAt = 0;
let ignoreErrorUntil = 0;

function formatFps(x10) {
  return (x10 / 10).toFixed(1);
}

function formatRate(kbps) {
  if (!kbps) return "—";
  if (kbps >= 1000) return (kbps / 1000).toFixed(1) + " Mb/s";
  return Math.round(kbps) + " kb/s";
}

function rssiWord(rssi) {
  if (rssi >= -55) return "Çok iyi";
  if (rssi >= -67) return "İyi";
  if (rssi >= -75) return "Orta";
  return "Zayıf";
}

function cameraError(camera) {
  return camera.error === "mjpeg_yok" || camera.error === "acilamadi";
}

function describe(status) {
  if (!status || !status.camera.connected) {
    return "USB kamerayı kartın Type-A girişine tak.";
  }
  if (status.camera.error === "mjpeg_yok") {
    return "Bu kamera MJPEG vermiyor. MJPEG destekleyen bir USB kamera gerekli.";
  }
  if (status.camera.error === "acilamadi") {
    return "Kamera göründü ama yayın açılamadı. Kabloyu çıkarıp tekrar tak.";
  }
  return "İlk kare yolda…";
}

function setNote(text, bad) {
  noteEl.textContent = text || "";
  noteEl.classList.toggle("bad", Boolean(bad));
}

function setStream(on) {
  if (on) {
    if (streamWanted) return;
    streamWanted = true;
    streamStartedAt = Date.now();
    view.src = "/stream?t=" + Date.now();
    return;
  }
  if (!streamWanted && !view.getAttribute("src")) return;
  streamWanted = false;
  view.classList.remove("live");
  view.removeAttribute("src");
  placeholder.classList.remove("hide");
}

function reloadStream() {
  ignoreErrorUntil = Date.now() + 800;
  streamWanted = true;
  streamStartedAt = Date.now();
  view.classList.remove("live");
  placeholder.classList.remove("hide");
  view.src = "/stream?t=" + Date.now();
}

function groupModes() {
  const map = new Map();
  modes.forEach((mode) => {
    const key = mode.w + "x" + mode.h;
    if (!map.has(key)) map.set(key, []);
    map.get(key).push(mode.fps);
  });
  return map;
}

function pickFps(list) {
  const sorted = [...list].sort((a, b) => a - b);
  const calm = sorted.filter((fps) => fps <= 20);
  return calm.length ? calm[calm.length - 1] : sorted[0];
}

function renderModes() {
  const groups = groupModes();
  if (groups.size === 0) {
    modesEl.innerHTML = "<p class='empty'>Kamera takılınca seçenekler burada belirir.</p>";
    ratesEl.innerHTML = "<p class='empty'>Çözünürlük seçilince dolar.</p>";
    return;
  }

  const liveKey = latest && latest.camera.width
    ? latest.camera.width + "x" + latest.camera.height
    : pickedKey;

  modesEl.innerHTML = "";
  groups.forEach((fpsList, key) => {
    const [w, h] = key.split("x");
    const button = document.createElement("button");
    button.type = "button";
    button.textContent = w + "×" + h;
    button.dataset.w = w;
    button.dataset.h = h;
    button.classList.toggle("on", key === liveKey);
    modesEl.appendChild(button);
  });

  const key = pickedKey || liveKey;
  const fpsList = key ? groups.get(key) : null;
  ratesEl.innerHTML = "";
  if (!fpsList) {
    ratesEl.innerHTML = "<p class='empty'>Çözünürlük seçilince dolar.</p>";
    return;
  }
  const currentFps = latest ? latest.camera.fps_x10 / 10 : 0;
  fpsList.forEach((fps) => {
    const button = document.createElement("button");
    button.type = "button";
    button.textContent = fps + " fps";
    button.dataset.fps = String(fps);
    const sameSize = latest && key === latest.camera.width + "x" + latest.camera.height;
    button.classList.toggle("on", sameSize && Math.abs(currentFps - fps) < 0.6);
    ratesEl.appendChild(button);
  });
}

function renderStatus(status) {
  const wifi = status.wifi;
  const camera = status.camera;
  const hub = status.hub;

  document.querySelector("#ssid").textContent = wifi.ssid || "ağ yok";
  document.querySelector("#ip").textContent = wifi.ip ? "http://" + wifi.ip : "—";
  document.querySelector("#setup-banner").hidden = wifi.mode !== "ap";
  if (wifi.mode === "ap" && !bannerOpened) {
    document.querySelector("#net").open = true;
    bannerOpened = true;
  }

  placeholderText.textContent = describe(status);
  placeholder.classList.toggle("hide", view.classList.contains("live"));
  document.querySelector("#hud-res").textContent = camera.width ? camera.width + "×" + camera.height : "—";
  document.querySelector("#hud-fps").textContent = hub.fps_x10 ? formatFps(hub.fps_x10) + " fps" : "—";
  document.querySelector("#dot").classList.toggle("on", camera.streaming && view.classList.contains("live"));
  document.querySelector("#stat-fps").textContent = hub.fps_x10 ? formatFps(hub.fps_x10) : "—";
  document.querySelector("#stat-rate").textContent = formatRate(hub.kbps);
  document.querySelector("#stat-viewers").textContent = String(hub.viewers);
  document.querySelector("#stat-rssi").textContent = wifi.mode === "ap" ? "AP" : (wifi.connected ? wifi.rssi : "—");
  document.querySelector("#stat-rssi-label").textContent = wifi.mode === "ap"
    ? "Kurulum"
    : (wifi.connected ? rssiWord(wifi.rssi) : "Kopuk");

  const hint = document.querySelector("#hint");
  hint.textContent = hub.kbps > 7000
    ? "Akış yoğun. Daha düşük çözünürlük ev ağında daha akıcı durur."
    : "";

  const canStream = !document.hidden && !userPaused && camera.connected && !cameraError(camera);
  if (camera.streaming && streamWanted) {
    view.classList.add("live");
    placeholder.classList.add("hide");
  }
  if (canStream) {
    if (!streamWanted) setStream(true);
    else if (!view.classList.contains("live") && Date.now() - streamStartedAt > 4000) reloadStream();
  } else if (streamWanted) {
    setStream(false);
  }
  document.querySelector("#dot").classList.toggle("on", camera.streaming && view.classList.contains("live"));
  renderModes();
}

async function loadFormats() {
  const res = await fetch("/api/formats", { cache: "no-store" });
  if (!res.ok) return;
  const data = await res.json();
  modes = data.formats || [];
  formatsReady = true;
  renderModes();
}

async function poll() {
  if (polling) return;
  polling = true;
  try {
    const res = await fetch("/api/status", { cache: "no-store" });
    if (!res.ok) return;
    latest = await res.json();
    if (latest.camera.connected && !formatsReady) {
      await loadFormats();
    }
    if (!latest.camera.connected) formatsReady = false;
    renderStatus(latest);
  } catch (err) {
    placeholderText.textContent = "Kartla bağlantı koptu. Sayfayı yenile.";
  } finally {
    polling = false;
  }
}

async function applyMode(width, height, fps) {
  if (applying) return;
  applying = true;
  document.body.classList.add("busy");
  try {
    const res = await fetch("/api/format", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ width, height, fps })
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok || data.ok === false) {
      throw new Error(data.error || "Çözünürlük değişmedi");
    }
    setNote("");
    reloadStream();
  } catch (err) {
    setNote(err.message || "Çözünürlük değişmedi", true);
  } finally {
    applying = false;
    document.body.classList.remove("busy");
    renderModes();
  }
}

modesEl.addEventListener("click", (event) => {
  const button = event.target.closest("button[data-w]");
  if (!button) return;
  const width = Number(button.dataset.w);
  const height = Number(button.dataset.h);
  pickedKey = width + "x" + height;
  const list = groupModes().get(pickedKey) || [];
  applyMode(width, height, pickFps(list));
});

ratesEl.addEventListener("click", (event) => {
  const button = event.target.closest("button[data-fps]");
  if (!button) return;
  const key = pickedKey || (latest && latest.camera.width
    ? latest.camera.width + "x" + latest.camera.height
    : null);
  if (!key) return;
  const [width, height] = key.split("x").map(Number);
  pickedKey = key;
  applyMode(width, height, Number(button.dataset.fps));
});

document.querySelector("#toggle-play").addEventListener("click", (event) => {
  userPaused = !userPaused;
  event.currentTarget.textContent = userPaused ? "İzle" : "Durdur";
  if (userPaused) setStream(false);
  else if (latest && latest.camera.connected && !cameraError(latest.camera)) setStream(true);
});

document.querySelector("#full").addEventListener("click", toggleFull);
view.addEventListener("dblclick", toggleFull);

function toggleFull() {
  const stage = document.querySelector("#stage");
  const active = document.fullscreenElement || document.webkitFullscreenElement;
  if (active) {
    const exit = document.exitFullscreen || document.webkitExitFullscreen;
    if (exit) exit.call(document);
    return;
  }
  const request = stage.requestFullscreen || stage.webkitRequestFullscreen;
  if (request) request.call(stage);
}

document.querySelector("#shot").addEventListener("click", async () => {
  try {
    const res = await fetch("/snapshot?t=" + Date.now(), { cache: "no-store" });
    if (!res.ok) throw new Error("Hazır kare yok");
    const blob = await res.blob();
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = "p4-kare.jpg";
    link.click();
    URL.revokeObjectURL(url);
  } catch (err) {
    setNote("Fotoğraf alınamadı. Önce görüntünün gelmesini bekle.", true);
  }
});

document.querySelector("#do-scan").addEventListener("click", async () => {
  const button = document.querySelector("#do-scan");
  const select = document.querySelector("#scan");
  button.disabled = true;
  button.textContent = "Taranıyor";
  try {
    const res = await fetch("/api/wifi/scan", { cache: "no-store" });
    const data = await res.json();
    select.innerHTML = "";
    const empty = document.createElement("option");
    empty.value = "";
    empty.textContent = (data.networks || []).length ? "Bir ağ seç" : "Ağ bulunamadı";
    select.appendChild(empty);
    (data.networks || []).forEach((item) => {
      const option = document.createElement("option");
      option.value = item.ssid;
      option.textContent = item.ssid + " · " + item.rssi + " dBm";
      select.appendChild(option);
    });
  } catch (err) {
    setNote("Ağ taraması yapılamadı.", true);
  } finally {
    button.disabled = false;
    button.textContent = "Tara";
  }
});

document.querySelector("#scan").addEventListener("change", (event) => {
  if (event.target.value) document.querySelector("#ssid-input").value = event.target.value;
});

document.querySelector("#toggle-pass").addEventListener("click", (event) => {
  const input = document.querySelector("#pass");
  const show = input.type === "password";
  input.type = show ? "text" : "password";
  event.currentTarget.textContent = show ? "Gizle" : "Göster";
});

document.querySelector("#wifi-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const ssid = document.querySelector("#ssid-input").value.trim();
  const password = document.querySelector("#pass").value;
  if (!ssid) {
    setNote("Ağ adı gerekli.", true);
    return;
  }
  if (password && password.length < 8) {
    setNote("Şifre en az 8 karakter olmalı. Açık ağ için boş bırak.", true);
    return;
  }
  setNote("Kaydediliyor…");
  try {
    const res = await fetch("/api/wifi", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ssid, password })
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok || data.ok === false) throw new Error(data.error || "Kaydedilemedi");
    setNote("Kaydedildi. Kart ev ağına geçiyor. Birazdan http://p4cam.local adresine bak.");
  } catch (err) {
    setNote(err.message || "Kart yeniden başlıyor olabilir. http://p4cam.local adresini dene.", true);
  }
});

document.addEventListener("visibilitychange", () => {
  if (!latest) return;
  if (document.hidden) {
    setStream(false);
    return;
  }
  if (!userPaused && latest.camera.connected && !cameraError(latest.camera)) setStream(true);
});

view.addEventListener("load", () => {
  view.classList.add("live");
  placeholder.classList.add("hide");
});

view.addEventListener("error", () => {
  view.classList.remove("live");
  placeholder.classList.remove("hide");
  if (!streamWanted || Date.now() < ignoreErrorUntil) return;
  clearTimeout(retryTimer);
  retryTimer = setTimeout(() => {
    if (streamWanted) view.src = "/stream?t=" + Date.now();
  }, 1200);
});

poll();
setInterval(poll, 1000);
