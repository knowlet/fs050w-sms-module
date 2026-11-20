// Each invocation performs at most one action POST. A send is never retried.
async function api(operation, body) {
  try {
    if (operation === "inbox") {
      const data = check(await transaction("/action/sms_get_sms_list", {page:1, pageSize:64}));
      return {...FS050WSmsData.inboxRows(data), deleteOne:data.delete_one === 1};
    }
    if (operation.startsWith("message?")) {
      const query = new URLSearchParams(operation.slice(8));
      const id = Number(query.get("id"));
      const storage = Number(query.get("storage"));
      if (![0,1].includes(storage) || !Number.isInteger(id) || id < 0) throw new Error("簡訊位置無效。");
      return await readSlot({id, storage});
    }
    if (operation === "delete") {
      const {id, storage} = body.item;
      const current = await readSlot({id, storage});
      if (current.pdu !== body.detail.pdu) throw new Error("這個位置的簡訊已變更。請重新整理後再選取要刪除的訊息。");
      // The OEM delete route has no response buffer. The module accepts this
      // exact marked command on the read route so success can be confirmed.
      const reply = await transaction("/action/sms_read_sms_info", {fs050w_sms_delete_one:1, id, storage});
      if (reply.retcode === 103) {
        const failure = reply.data || {};
        if (failure.module === "sms" && failure.msgid === 11011 &&
            Number.isInteger(failure.rc) && Number.isInteger(failure.err)) {
          throw new Error(`數據機未能刪除簡訊（rc ${failure.rc}，err ${failure.err}）。請重新整理收件匣。`);
        }
        if (failure.reason === "invalid_delete_request") {
          throw new Error("簡訊刪除請求未通過模組檢查。請重新整理收件匣。");
        }
        throw new Error("刪除未完成（回碼 103）。請確認設備已安裝最新簡訊模組。");
      }
      const result = check(reply);
      if (result.module !== "sms" || result.msgid !== 11011 || result.deleted !== 1 ||
          result.storage !== storage || result.id !== id || result.rc !== 0 || result.err !== 0) {
        throw new Error("未收到明確的刪除結果。請重新整理收件匣後再操作。");
      }
      return {ok:true};
    }
    if (operation === "send") {
      const result = check(await transaction("/action/sms_send_sms_info", {Number:body.number, Content:body.text}));
      if ((result.accepted ?? 1) !== 1 || (result.rc ?? 0) !== 0 || (result.err ?? 0) !== 0) throw new Error("簡訊模組未完成發送。");
      return {ok:true};
    }
    throw new Error("未知的簡訊操作。");
  } catch (error) {
    if (error instanceof LoginRequired) {
      switchView(false);
      notice("login-notice", loginMessage);
      el("login").focus();
    }
    throw error;
  }
}

async function readSlot({id, storage}) {
  const raw = check(await transaction("/action/sms_read_sms_info", {id, storage}));
  if (raw.id !== id || raw.storage !== storage) throw new Error("路由器回傳的簡訊位置與選取項目不同。");
  const detail = FS050WSmsData.extractMessage(raw);
  // The prefix before byte 44 contains modem metadata, including mutable flags.
  // Compare the full declared PDU, which includes sender, timestamp and body.
  const full = raw.hex.toLowerCase();
  const length = Number.parseInt(full.slice(88,96).match(/../g).reverse().join(""), 16);
  return {...detail, pdu:full.slice(96, 96 + 2*length)};
}
