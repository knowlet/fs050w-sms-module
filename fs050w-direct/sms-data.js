// FS050W V2.1.0 SMS IPC reply decoder. Uses Web APIs available on plain HTTP.
(() => {
  "use strict";
  const gsm = "@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞ\x1bÆæßÉ !\"#¤%&'()*+,-./" +
    "0123456789:;<=>?¡ABCDEFGHIJKLMNO" + "PQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmno" + "pqrstuvwxyzäöñüà";
  const extension = {10:"\f",20:"^",40:"{",41:"}",47:"\\",60:"[",61:"~",62:"]",64:"|",101:"€"};

  function inboxRows(data) {
    if (!data || typeof data !== "object" || !("tags" in data || "uim_tags" in data))
      throw new Error("設備沒有提供相容的簡訊清單。");
    const messages = [], warnings = new Set();
    for (const [storage, field, name] of [[0, "uim_tags", "SIM"], [1, "tags", "設備"]]) {
      if (!Array.isArray(data[field])) {
        warnings.add("無法取得" + name + "的收件清單。");
        continue;
      }
      const ids = new Set(), tags = new Set();
      for (const g of data[field]) {
        if (!Number.isInteger(g?.tag)) {
          warnings.add(name + "回傳的分類格式異常。");
          continue;
        }
        if (g.tag !== 0 && g.tag !== 1) continue;
        tags.add(g.tag);
        if (g.rc !== 0 || g.err !== 0) {
          warnings.add("無法讀取" + name + "的收件分類 " + g.tag + "。");
          continue;
        }
        if (!Array.isArray(g.index) || !Number.isInteger(g.count) || g.count < 0) {
          warnings.add(name + "的收件索引格式異常。");
          continue;
        }
        if (g.truncated || g.count !== g.index.length)
          warnings.add(name + "的收件分類 " + g.tag + " 僅列出部分索引，設備回報共 " + g.count + " 封。");
        for (const id of g.index) {
          if (!Number.isInteger(id) || id < 0 || id > 0x7fffffff)
            warnings.add(name + "回傳了無效的簡訊索引。");
          else if (ids.has(id))
            warnings.add(name + "重複列出第 " + id + " 號簡訊，已合併顯示。");
          else {
            ids.add(id);
            messages.push({storage, id, tag:g.tag});
          }
        }
      }
      if (tags.size !== 2) warnings.add(name + "的收件分類不齊全。");
    }
    return {messages, warnings:Array.from(warnings)};
  }

  class Reader {
    constructor(bytes) {
      this.bytes = bytes;
      this.offset = 0;
    }
    take(n) {
      if (this.offset + n > this.bytes.length)
        throw new Error("設備回傳的簡訊不完整，無法顯示全文。");
      const start = this.offset;
      this.offset += n;
      return this.bytes.subarray(start, this.offset);
    }
    byte() { return this.take(1)[0]; }
  }

  function gsmText(bytes, length, start = 0) {
    if (start + length * 7 > bytes.length * 8) throw new Error("簡訊的文字資料不完整。");
    let text = "", escaped = false;
    for (let i = 0; i < length; i++) {
      const bit = start + i * 7, octet = bit >> 3;
      const pair = bytes[octet] | ((bytes[octet + 1] || 0) << 8);
      const code = (pair >> (bit % 8)) & 127;
      if (escaped) {
        if (!(code in extension)) throw new Error("簡訊使用不支援的 GSM 延伸字元。");
        text += extension[code];
        escaped = false;
      } else if (code === 27) escaped = true;
      else text += gsm[code];
    }
    if (escaped) throw new Error("簡訊的延伸字元不完整。");
    return text;
  }

  function senderAddress(pdu) {
    const size = pdu.byte(), type = (pdu.byte() >> 4) & 7;
    const bytes = pdu.take(Math.ceil(size / 2));
    if (type === 5) return gsmText(bytes, Math.floor(size * 4 / 7));
    let value = type === 1 ? "+" : "";
    for (let i = 0; i < size; i++) {
      const nibble = (bytes[i >> 1] >> (4 * (i % 2))) & 15;
      if (nibble === 15) throw new Error("簡訊的寄件號碼格式異常。");
      value += "0123456789*#abc"[nibble];
    }
    return value;
  }

  function timestamp(bytes) {
    const negative = Boolean(bytes[6] & 8);
    const bcd = Array.from(bytes, (v, i) => {
      if (i === 6) v &= 0xf7;
      const tens = v & 15, units = v >> 4;
      return tens > 9 || units > 9 ? NaN : tens * 10 + units;
    });
    if (!bcd.every(Number.isFinite)) return null;
    const [year, month, day, hour, minute, second, quarters] = bcd;
    const fullYear = 2000 + year;
    if (month < 1 || month > 12 || day < 1 || day > new Date(Date.UTC(fullYear, month, 0)).getUTCDate() ||
        hour > 23 || minute > 59 || second > 59 || quarters >= 96) return null;
    const pad = n => String(n).padStart(2, "0");
    const offset = (negative && quarters ? "-" : "+") + pad(Math.floor(quarters / 4)) + ":" + pad(quarters % 4 * 15);
    return fullYear + "-" + pad(month) + "-" + pad(day) + "T" + pad(hour) + ":" + pad(minute) + ":" + pad(second) + offset;
  }

  function encodingOf(dcs) {
    const group = dcs >> 4;
    if (group <= 7 && !(dcs & 0x20)) {
      const coding = (dcs >> 2) & 3;
      if (coding === 0) return "GSM-7";
      if (coding === 2) return "UCS-2";
    } else if (group === 12 || group === 13 || (group === 15 && !(dcs & 4))) return "GSM-7";
    else if (group === 14) return "UCS-2";
    throw new Error("目前無法顯示此簡訊的編碼（DCS 0x" + dcs.toString(16).toUpperCase().padStart(2,"0") + "）。");
  }

  function headerInfo(raw) {
    const data = new Reader(raw), elements = new Reader(data.take(data.byte()));
    let segment;
    while (elements.offset < elements.bytes.length) {
      const kind = elements.byte(), value = elements.take(elements.byte());
      if ((kind === 0x24 || kind === 0x25) && (value.length !== 1 || value[0] !== 0))
        throw new Error("目前無法顯示使用國別字元表的簡訊。");
      if (kind === 4 || kind === 5) throw new Error("這是傳送至應用程式連接埠的簡訊。");
      if (kind === 0 || kind === 8) {
        const total = value[value.length - 2], part = value[value.length - 1];
        if (value.length !== (kind === 0 ? 3 : 4) || part < 1 || part > total)
          throw new Error("分段簡訊的標頭格式異常。");
        segment = {reference: kind === 0 ? value[0] : value[0] * 256 + value[1], total, part};
      }
    }
    return {start:data.offset, segment};
  }

  function extractMessage(data) {
    if (data?.rc !== 0 || data?.err !== 0) throw new Error("設備無法讀取這封簡訊，請重新整理收件匣。");
    if (typeof data.hex !== "string" || !/^(?:\s*[a-f0-9]{2})*\s*$/i.test(data.hex))
      throw new Error("設備回傳的簡訊格式異常。");
    const hex = data.hex.replace(/\s/g, "");
    const bytes = Uint8Array.from(hex.match(/../g) || [], pair => parseInt(pair, 16));
    const envelope = new Reader(bytes);
    envelope.take(44);
    const word = envelope.take(4);
    const size = new DataView(word.buffer, word.byteOffset, 4).getUint32(0, true);
    const pdu = new Reader(envelope.take(size));
    pdu.take(pdu.byte()); // SMSC address
    const flags = pdu.byte();
    if (flags & 3) throw new Error("此記錄不屬於收到的文字簡訊。");
    const sender = senderAddress(pdu);
    pdu.byte(); // TP-PID
    const encoding = encodingOf(pdu.byte()), received_at = timestamp(pdu.take(7));
    const udl = pdu.byte();
    const raw = pdu.take(encoding === "GSM-7" ? Math.ceil(udl * 7 / 8) : udl);
    const {start, segment} = flags & 64 ? headerInfo(raw) : {start:0};
    let text;
    if (encoding === "GSM-7") {
      const headerSeptets = Math.ceil(start * 8 / 7);
      if (headerSeptets > udl) throw new Error("簡訊標頭超過本文長度。");
      text = gsmText(raw, udl - headerSeptets, headerSeptets * 7);
    } else {
      try { text = new TextDecoder("utf-16be", {fatal:true, ignoreBOM:true}).decode(raw.subarray(start)); }
      catch { throw new Error("簡訊中的 UCS-2 文字不完整。"); }
    }
    const result = {sender, text, encoding, received_at};
    if (segment) result.segment = segment;
    return result;
  }

  globalThis.FS050WSmsData = Object.freeze({inboxRows, extractMessage});
})();
