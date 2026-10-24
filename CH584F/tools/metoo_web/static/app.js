"use strict";

const MAX_NFC_ROWS = 200;
const MAX_LOG_LINES = 400;

const devices = new Map();   // address -> 设备信息
const links = new Map();     // address -> 链路状态

let ws = null;
let wsTimer = null;
let textMax = 200;
let nfcRows = 0;
let logLines = 0;

const $ = (id) => document.getElementById(id);

const ui = {
  wsState: $("ws-state"),
  scanState: $("scan-state"),
  devList: $("dev-list"),
  devCount: $("dev-count"),
  devEmpty: $("dev-empty"),
  nfcBody: $("nfc-body"),
  nfcCount: $("nfc-count"),
  dedupChk: $("dedup-chk"),
  dedupDropped: $("dedup-dropped"),
  rawLog: $("raw-log"),
  rawHint: $("raw-hint"),
};

function escapeHtml(text) {
  return String(text == null ? "" : text)
    .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function nowText() {
  const d = new Date();
  const p = (n, w) => String(n).padStart(w || 2, "0");
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
}

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(obj));
  }
}

// ------------------------------------------------------------------ 日志
function addLog(kind, text) {
  const line = `${nowText()}  [${kind}] ${text}`;
  ui.rawLog.textContent = line + "\n" + ui.rawLog.textContent;
  logLines++;
  if (logLines > MAX_LOG_LINES) {
    const cut = ui.rawLog.textContent.split("\n").slice(0, MAX_LOG_LINES).join("\n");
    ui.rawLog.textContent = cut;
    logLines = MAX_LOG_LINES;
  }
}

// ------------------------------------------------------------------ 渲染
function linkState(address) {
  const link = links.get(address);
  return link ? link.state : "idle";
}

function renderDevices() {
  const list = [...devices.values()];
  ui.devCount.textContent = list.length ? `(${list.length})` : "";
  ui.devEmpty.style.display = list.length ? "none" : "";
  ui.devList.innerHTML = "";

  list.sort((a, b) => {
    const am = (a.name || "").startsWith("MeXsmarEC") ? 0 : 1;
    const bm = (b.name || "").startsWith("MeXsmarEC") ? 0 : 1;
    return am - bm || (a.name || "").localeCompare(b.name || "") ||
           a.address.localeCompare(b.address);
  });

  for (const dev of list) {
    const state = linkState(dev.address);
    const li = document.createElement("li");
    li.className = "dev" + (state === "connected" ? " on" : "");

    const main = document.createElement("div");
    main.className = "dev-main";
    main.innerHTML =
      `<span class="dev-name">${escapeHtml(dev.name || "(无名)")}</span>` +
      `<span class="dev-mac">${escapeHtml(dev.address)}</span>` +
      `<span class="dev-meta">` +
      `${dev.rssi != null ? dev.rssi + " dBm" : ""}` +
      `${state !== "idle" ? " · " + escapeHtml(state) : ""}` +
      `</span>`;

    const btn = document.createElement("button");
    const busy = state === "connecting" || state === "connected" ||
                 state === "reconnecting";
    btn.textContent = busy ? "断开" : "连接";
    btn.onclick = () => send({
      cmd: busy ? "disconnect" : "connect",
      address: dev.address,
    });

    li.append(main, btn);
    ui.devList.appendChild(li);
  }

  renderChannels();
}

function connectedAddresses() {
  return [...links.values()]
    .filter((l) => l.state === "connected")
    .map((l) => l.address);
}

function renderChannels() {
  const online = connectedAddresses();

  for (const id of ["led-via", "write-via", "raw-via"]) {
    const sel = $(id);
    const prev = sel.value;
    sel.innerHTML = "";
    if (!online.length) {
      const opt = document.createElement("option");
      opt.value = "";
      opt.textContent = "（没有已连接的板）";
      sel.appendChild(opt);
      sel.disabled = true;
      continue;
    }
    sel.disabled = false;
    for (const addr of online) {
      const opt = document.createElement("option");
      opt.value = addr;
      opt.textContent = addr;
      sel.appendChild(opt);
    }
    if (online.includes(prev)) sel.value = prev;
  }

  const dl = $("target-list");
  dl.innerHTML = `<option value="FF:FF:FF:FF:FF:FF">广播（全网）</option>`;
  for (const addr of online) {
    const opt = document.createElement("option");
    opt.value = addr;
    opt.textContent = addr;
    dl.appendChild(opt);
  }
}

function addNfcRow(frame) {
  const tr = document.createElement("tr");
  tr.innerHTML =
    `<td class="t">${nowText()}</td>` +
    `<td class="mono">${escapeHtml(frame.node || "")}</td>` +
    `<td class="mono">${escapeHtml(frame.uid || "")}</td>` +
    `<td>${escapeHtml(frame.text || "")}</td>`;
  ui.nfcBody.prepend(tr);

  nfcRows++;
  while (ui.nfcBody.children.length > MAX_NFC_ROWS) {
    ui.nfcBody.lastChild.remove();
  }
  ui.nfcCount.textContent = `${nfcRows} 条`;
}

// ------------------------------------------------------------------ 消息处理
function onMessage(event) {
  let msg;
  try {
    msg = JSON.parse(event.data);
  } catch (err) {
    addLog("告警", "收到非 JSON 消息");
    return;
  }

  switch (msg.ev) {
    case "hello":
      textMax = msg.textMax || 200;
      addLog("信息", `已连接后端，协议版本 ${msg.version}，单帧文本上限 ${textMax} 字节`);
      break;

    case "state":
      devices.clear();
      for (const dev of msg.devices || []) devices.set(dev.address, dev);
      links.clear();
      for (const link of msg.links || []) links.set(link.address, link);
      ui.dedupChk.checked = !!msg.dedup;
      ui.dedupDropped.textContent =
        msg.dedupDropped ? `（已丢弃重复 ${msg.dedupDropped} 帧）` : "";
      ui.scanState.textContent = msg.scanning ? "扫描中 ..." : "";
      renderDevices();
      break;

    case "devices":
      devices.clear();
      for (const dev of msg.devices || []) devices.set(dev.address, dev);
      renderDevices();
      break;

    case "link":
      links.set(msg.address, msg);
      const dev = devices.get(msg.address);
      if (dev) dev.state = msg.state;
      renderDevices();
      addLog("链路", `${msg.address} -> ${msg.state}${msg.detail ? "（" + msg.detail + "）" : ""}`);
      break;

    case "frame":
      addLog("帧", `${msg.address} : ${msg.frame.summary || msg.frame.raw}`);
      if (msg.frame.ok && msg.frame.type === 1) addNfcRow(msg.frame);
      break;

    case "raw":
      addLog(msg.dir === "tx" ? "发送" : "接收", `${msg.address}  ${msg.hex}`);
      break;

    case "log":
      addLog(msg.level === "error" ? "错误" : msg.level === "warn" ? "告警" : "信息", msg.text);
      break;

    default:
      break;
  }
}

// ------------------------------------------------------------------ WebSocket
function setWsState(online) {
  ui.wsState.textContent = online ? "后端已连接" : "后端未连接";
  ui.wsState.className = "pill " + (online ? "on" : "off");
}

function openWs() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/ws`);

  ws.onopen = () => {
    setWsState(true);
    send({ cmd: "state" });
  };
  ws.onmessage = onMessage;
  ws.onerror = () => { try { ws.close(); } catch (err) {} };
  ws.onclose = () => {
    setWsState(false);
    ws = null;
    clearTimeout(wsTimer);
    wsTimer = setTimeout(openWs, 2000);
  };
}

// ------------------------------------------------------------------ 交互
function bindTabs() {
  for (const tab of document.querySelectorAll(".tab")) {
    tab.onclick = () => {
      for (const t of document.querySelectorAll(".tab")) t.classList.remove("active");
      for (const p of document.querySelectorAll(".page")) p.classList.remove("active");
      tab.classList.add("active");
      $(`page-${tab.dataset.tab}`).classList.add("active");
    };
  }
}

function bindActions() {
  $("btn-scan").onclick = () => send({ cmd: "scan", timeout: 6 });

  ui.dedupChk.onchange = () => send({ cmd: "dedup", on: ui.dedupChk.checked });

  $("nfc-clear").onclick = () => {
    ui.nfcBody.innerHTML = "";
    nfcRows = 0;
    ui.nfcCount.textContent = "0 条";
  };

  const ledSend = (on) => send({
    cmd: "led",
    via: $("led-via").value,
    address: $("led-target").value.trim() || "FF:FF:FF:FF:FF:FF",
    on: on,
  });
  $("led-on").onclick = () => ledSend(true);
  $("led-off").onclick = () => ledSend(false);

  const writeLen = () => {
    const bytes = new TextEncoder().encode($("write-text").value).length;
    $("write-len").textContent = `${bytes} / ${textMax} 字节`;
  };
  $("write-text").oninput = writeLen;
  writeLen();

  $("write-send").onclick = () => send({
    cmd: "nfc_write",
    via: $("write-via").value,
    address: $("write-target").value.trim() || "FF:FF:FF:FF:FF:FF",
    text: $("write-text").value,
  });

  $("raw-send").onclick = () => {
    const hex = $("raw-hex").value.trim();
    if (!hex) {
      ui.rawHint.textContent = "先填十六进制帧";
      return;
    }
    ui.rawHint.textContent = "";
    send({ cmd: "raw", via: $("raw-via").value, hex: hex });
  };

  $("log-clear").onclick = () => {
    ui.rawLog.textContent = "";
    logLines = 0;
  };
}

bindTabs();
bindActions();
openWs();
