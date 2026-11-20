"use strict";
const el = (id) => document.getElementById(id);
const storageNames = ["SIM 卡", "設備"];
const tagNames = ["已讀", "未讀", "已發送", "草稿"];
let connected = false;
let session = 0;
let readRequest = 0;
let filter = "all";
let messages = [];
let canDelete = false;
let selected = null;
let reading = false;
let details = new Map();

function notice(id, message = "") {
  el(id).textContent = message;
}

function switchView(active) {
  connected = active;
  el("workspace").hidden = !active;
  el("logout").hidden = !active;
  el("login-panel").hidden = active;
  el("connection").textContent = active ? "已登入" : "未登入";
  el("connection").classList.toggle("online", active);
  if (!active) {
    session++;
    messages = [];
    details.clear();
    filter = "all";
    clearRead();
    renderList();
    if (el("delete-dialog").open) el("delete-dialog").close();
    if (el("composer").open) el("composer").close();
    el("send-form").reset();
    el("counter").textContent = "0 / 40";
    el("send-receipt").hidden = true;
    notice("send-receipt");
    notice("send-notice");
    notice("inbox-warning");
  }
}
function clearRead() {
  selected = null;
  reading = false;
  readRequest += 1;
  el("read-detail").hidden = true;
  el("read-hint").hidden = false;
  notice("read-hint", "選取一封簡訊，即可閱讀內容。");
  for (const id of ["sender", "location", "message-body", "received-time", "segment", "read-notice"]) notice(id);
  el("received-field").hidden = true;
  el("segment-field").hidden = true;
}

function keyOf(item) {
  return item.storage + "/" + item.id;
}

function renderList() {
  for (const button of document.querySelectorAll("[data-filter]")) {
    button.setAttribute("aria-pressed", String(button.dataset.filter === filter));
  }
  const visible = messages.filter((item) => filter === "all" || String(item.storage) === filter);
  const list = el("messages");
  list.replaceChildren();
  notice("inbox-meta", visible.length ? "共 " + visible.length + " 封簡訊" : "這裡沒有簡訊");
  for (const item of visible) {
    const key = keyOf(item);
    const detail = details.get(key);
    const location = storageNames[item.storage] + " · #" + item.id;
    const row = document.createElement("li");
    const button = document.createElement("button");
    button.className = "entry";
    button.type = "button";
    button.setAttribute("aria-current", String(selected === key));
    const label = document.createElement("span");
    label.className = "entry-main";
    const title = document.createElement("strong");
    title.textContent = detail?.sender || location;
    const meta = document.createElement("small");
    const tag = tagNames[item.tag] || "簡訊";
    meta.textContent = detail?.sender ? location + " · " + tag : tag;
    const arrow = document.createElement("span");
    arrow.className = "entry-arrow";
    arrow.setAttribute("aria-hidden", "true");
    arrow.textContent = "›";
    label.append(title, meta);
    button.append(label, arrow);
    button.addEventListener("click", () => openMessage(item));
    row.append(button);
    list.append(row);
  }
}

async function refresh() {
  const epoch = session;
  el("refresh").disabled = true;
  clearRead();
  details.clear();
  messages = [];
  el("messages").replaceChildren();
  notice("inbox-meta", "正在讀取簡訊…");
  notice("inbox-warning");
  try {
    const data = await api("inbox");
    if (epoch !== session) return;
    if (!Array.isArray(data.messages)) throw new Error("收件匣回應格式有誤。");
    switchView(true);
    el("offline").hidden = true;
    canDelete = data.deleteOne;
    messages = data.messages;
    renderList();
    notice("inbox-warning", (data.warnings || []).join("\n"));
  } catch (error) {
    if (epoch === session) {
      notice("inbox-meta");
      notice(connected ? "inbox-warning" : "login-notice", error.message);
    }
  } finally {
    el("refresh").disabled = false;
  }
}

async function openMessage(item) {
  const key = keyOf(item);
  if (selected === key && reading) return;
  clearRead();
  const request = readRequest;
  selected = key;
  reading = true;
  renderList();
  notice("read-hint", "正在讀取內容…");
  el("read-title").focus({preventScroll:true});
  if (window.matchMedia("(max-width:710px)").matches) el("reader").scrollIntoView({block:"start"});
  try {
    const query = new URLSearchParams({storage:String(item.storage), id:String(item.id)});
    const detail = await api("message?" + query);
    if (request !== readRequest) return;
    details.set(key, detail);
    el("delete").disabled = !canDelete;
    el("delete-help").hidden = canDelete;
    notice("sender", detail.sender || "未提供");
    notice("location", storageNames[item.storage] + " · #" + item.id);
    notice("message-body", detail.text ?? "無法顯示此簡訊的內容。");
    const stamp = Date.parse(detail.received_at || "");
    if (Number.isFinite(stamp)) {
      notice("received-time", new Date(stamp).toLocaleString("zh-TW"));
      el("received-field").hidden = false;
    }
    if (detail.segment) {
      notice("segment", "第 " + detail.segment.part + " 段，共 " + detail.segment.total + " 段");
      el("segment-field").hidden = false;
    }
    if (detail.warning) notice("read-notice", String(detail.warning));
    el("read-hint").hidden = true;
    el("read-detail").hidden = false;
    renderList();
  } catch (error) {
    if (request === readRequest) {
      el("read-hint").hidden = true;
      notice("read-notice", error.message);
    }
  } finally {
    if (request === readRequest) reading = false;
  }
}

for (const button of document.querySelectorAll("[data-filter]")) {
  button.addEventListener("click", () => {
    filter = button.dataset.filter;
    if (selected && filter !== "all" && !selected.startsWith(filter + "/")) clearRead();
    renderList();
  });
}
el("refresh").addEventListener("click", refresh);
function sending(busy) {
  for (const id of ["send", "close-compose", "cancel", "number", "body"]) el(id).disabled = busy;
  el("send").textContent = busy ? "發送中…" : "發送簡訊";
}
el("compose").addEventListener("click", () => {
  notice("send-notice");
  el("composer").showModal();
  el("number").focus();
});
for (const id of ["close-compose", "cancel"]) {
  el(id).addEventListener("click", () => el("composer").close());
}
el("composer").addEventListener("cancel", (event) => {
  if (el("send").disabled) event.preventDefault();
});
el("body").addEventListener("input", () => {
  notice("counter", el("body").value.length + " / 40");
});
el("send-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (el("send").disabled) return;
  const number = el("number").value.trim(), text = el("body").value;
  if (!/^(?:0|\+?886)9[0-9]{8}$/.test(number)) {
    notice("send-notice", "請輸入 09 開頭的十碼手機號碼，或使用 +886 格式。");
    el("number").focus();
    return;
  }
  if (!/^[A-Za-z0-9 ]{1,40}$/.test(text)) {
    notice("send-notice", "請使用英文字母、數字或空格，最多 40 個字元。");
    el("body").focus();
    return;
  }
  const epoch = session;
  sending(true);
  notice("send-notice", "正在等待路由器回覆…");
  el("send-receipt").hidden = true;
  try {
    const result = await api("send", {number, text});
    if (epoch !== session) return;
    if (result.ok !== true) throw new Error("未收到發送確認，請先向收件人核對。");
    el("send-form").reset();
    notice("counter", "0 / 40");
    el("composer").close();
    el("send-receipt").hidden = false;
    notice("send-receipt", "路由器已接受寄往尾碼 " + number.slice(-4) + " 的簡訊。");
  } catch (error) {
    if (epoch === session) notice("send-notice", error.message);
  } finally {
    sending(false);
  }
});

el("retry").addEventListener("click", async () => {
  el("retry").disabled = true;
  try { await refresh(); } finally { el("retry").disabled = false; }
});
el("login-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (el("login").disabled) return;
  const password = el("password").value;
  el("password").value = "";
  el("login").disabled = true;
  el("retry").disabled = true;
  notice("login-notice", "正在登入…");
  try {
    const reply = await transaction("/goform/get_private_key", {});
    check(reply);
    if (typeof reply.prikey !== "string" || !reply.prikey) throw new Error("無法取得原廠登入資料。");
    const outcome = await transaction("/goform/login2", FS050WAuth.passwordResponse(password, reply.prikey));
    if (outcome.retcode === 201) throw new Error("密碼錯誤，請檢查後再登入。");
    if (outcome.retcode === 203) throw new Error("管理登入已被鎖定，請稍後再試。");
    check(outcome);
    switchView(true);
    await refresh();
  } catch (err) {
    notice("login-notice", err.message);
  } finally {
    el("login").disabled = false;
    el("retry").disabled = false;
  }
});
el("logout").addEventListener("click", async () => {
  el("logout").disabled = true;
  try {
    check(await transaction("/action/logout", {}));
    switchView(false);
    notice("login-notice", "已登出。");
  } catch (err) {
    if (err instanceof LoginRequired) {
      switchView(false);
      notice("login-notice", loginMessage);
    } else {
      notice("inbox-warning", err.message);
    }
  } finally {
    el("logout").disabled = false;
  }
});
let deleteTarget;
el("delete").addEventListener("click", () => {
  const item = messages.find(row => keyOf(row) === selected);
  const detail = details.get(selected);
  if (!item || !detail || !canDelete) return;
  deleteTarget = {item, detail};
  notice("delete-source", [storageNames[item.storage] + " · #" + item.id, detail.sender].filter(Boolean).join(" · "));
  notice("delete-preview", detail.text?.slice(0, 100) || "");
  el("delete-dialog").showModal();
  el("keep-message").focus();
});
el("delete-dialog").addEventListener("close", () => {
  deleteTarget = null;
  notice("delete-source");
  notice("delete-preview");
});
el("keep-message").addEventListener("click", () => el("delete-dialog").close());
el("delete-dialog").addEventListener("cancel", event => {
  if (el("confirm-delete").disabled) event.preventDefault();
});
el("confirm-delete").addEventListener("click", async () => {
  if (!deleteTarget || el("confirm-delete").disabled) return;
  const {item, detail} = deleteTarget;
  const epoch = session;
  el("confirm-delete").disabled = el("keep-message").disabled = true;
  try {
    await api("delete", {item, detail});
    if (epoch === session) {
      el("delete-dialog").close();
      await refresh();
      notice("send-receipt", "已刪除 " + storageNames[item.storage] + " 的簡訊 #" + item.id + "。");
      el("send-receipt").hidden = false;
    }
  } catch (error) {
    el("delete-dialog").close();
    if (epoch === session) notice("read-notice", error.message);
  } finally {
    el("confirm-delete").disabled = el("keep-message").disabled = false;
  }
});

function start() {
  switchView(false);
  if (!origin) {
    notice("login-notice", "請在 FS050W 原廠管理頁開啟簡訊書籤，或從設備上的簡訊網址進入。");
    el("login").disabled = el("retry").disabled = true;
    return;
  }
  el("home").href = new URL(SMS_HOME, origin).href;
  el("password").disabled = true;
  el("login").disabled = el("retry").disabled = true;
  refresh().finally(() => {
    el("login").disabled = el("password").disabled = el("retry").disabled = false;
  });
}
start();
