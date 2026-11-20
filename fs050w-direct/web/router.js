"use strict";

const SMS_HOME = "/common/home.html";
function routerOrigin() {
  // document.baseURI inherits the parent URL in a same-origin bookmark frame.
  let url = new URL(document.baseURI);
  if (url.protocol === "about:" && window.opener) {
    try { url = new URL(window.opener.location.href); } catch { return null; }
  }
  if (!/^https?:$/.test(url.protocol) || url.username || url.password) return null;
  const octets = url.hostname.split(".").map(Number);
  if (octets.length !== 4 || octets.some(n => !Number.isInteger(n) || n < 0 || n > 255)) return null;
  const [a, b] = octets;
  return a === 10 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168) ? url.origin : null;
}
const origin = routerOrigin();
class LoginRequired extends Error {}
const loginMessage = "管理登入已失效或尚未登入，請從原廠管理頁登入。";

// The firmware rotates its CSRF token. Serialize token + action transactions.
let pending = Promise.resolve();
function transaction(path, payload) {
  const task = pending.then(() => performRequest(path, payload));
  pending = task.catch(() => {});
  return task;
}

async function http(path, options, timeout) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeout);
  try {
    const res = await fetch(new URL(path, origin), {
      ...options, mode: "same-origin", credentials: "same-origin",
      redirect: "manual", cache: "no-store", signal: controller.signal
    });
    if (res.type === "opaqueredirect" || [301, 302, 401, 403].includes(res.status)) {
      throw new LoginRequired(loginMessage);
    }
    if (!res.ok) throw new Error("路由器回應 HTTP " + res.status + "。");
    // Read the response while its deadline is still running.
    return {headers:res.headers, body:await res.text()};
  } finally {
    clearTimeout(timer);
  }
}

async function performRequest(path, payload) {
  const send = path === "/action/sms_send_sms_info";
  const deleting = path === "/action/sms_del_sms_info" ||
    (path === "/action/sms_read_sms_info" && payload?.fs050w_sms_delete_one === 1);
  const requestHeaders = {"X-Requested-With":"XMLHttpRequest"};
  let actionStarted = false;
  try {
    const tokenResponse = await http("/goform/x_csrf_token", {headers:requestHeaders}, 12000);
    const token = tokenResponse.headers.get("X-Csrf-Token");
    if (!token) throw new Error("未取得管理權杖，請確認已連到 FS050W。");
    requestHeaders["X-Csrf-Token"] = token;
    requestHeaders["Content-Type"] = "application/json";
    actionStarted = true;
    const deadline = send ? 120000 : deleting ? 75000 : 20000;
    const res = await http(path, {method:"POST", headers:requestHeaders, body:JSON.stringify(payload)}, deadline);
    if (/^\s*(<!doctype html|<html)/i.test(res.body)) throw new LoginRequired(loginMessage);
    const reply = JSON.parse(res.body);
    if (!reply || !Number.isInteger(reply.retcode)) throw new SyntaxError();
    return reply;
  } catch (err) {
    if (err instanceof LoginRequired) throw err;
    if (deleting && actionStarted) throw new Error("未收到明確的刪除結果。請重新整理收件匣後再操作。");
    if (send && actionStarted) throw new Error("未收到明確的發送結果。請先與收件人確認，再決定是否重送。");
    if (err instanceof TypeError || err.name === "AbortError") throw new Error("連線未完成，請確認已連上 FS050W 的 Wi-Fi 或 USB 網路。");
    if (err instanceof SyntaxError) throw new Error("無法辨識路由器的回應。");
    throw err;
  }
}

function check(reply) {
  const data = reply.data || reply;
  if (["number_not_allowed", "number_not_own_msisdn"].includes(data.reason)) throw new Error("設備上的測試模組只允許傳送到已設定的門號。");
  if (data.reason === "route_check_failed") throw new Error("數據機的簡訊路由檢查失敗。");
  if (data.err === 610 || reply.retcode === 610) throw new Error("數據機回報 610，請檢查簡訊路由與 LTE 簡訊網域。");
  if (reply.retcode === 109) throw new Error("109：路由器的簡訊服務沒有回應。");
  if (reply.retcode === 201) throw new LoginRequired(loginMessage);
  if (reply.retcode !== 0) throw new Error("路由器回應代碼 " + reply.retcode + "。");
  return data;
}
