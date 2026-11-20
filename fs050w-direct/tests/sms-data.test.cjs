"use strict";
require("../sms-data.js");
const assert = require("node:assert/strict");
const test = require("node:test");
const {extractMessage, inboxRows} = globalThis.FS050WSmsData;
const stamp = "62110211007523", prefix = "000405912143f500";

function record(pdu) {
  const length = Buffer.alloc(4);
  length.writeUInt32LE(pdu.length / 2);
  return {rc:0, err:0, hex:"00".repeat(44) + length.toString("hex") + pdu};
}
function group(tag, ...index) {
  return {tag, index, count:index.length, rc:0, err:0};
}

test("GSM-7 SMS-DELIVER and service timestamp", () => {
  assert.deepEqual(extractMessage(record(prefix + "00" + stamp + "0ae8329bfd4697d9ec37")), {
    sender:"+12345", text:"hellohello", encoding:"GSM-7", received_at:"2026-11-20T11:00:57+08:00"
  });
});
test("modem fixed-size padded response, nonempty SMSC", () => {
  const r = record("07" + "91888888888888" + "040a81" + "0900000000" + "0000" + stamp + "07e639ac06bbd300");
  r.hex = r.hex.padEnd(192 * 2, "0");
  assert.equal(extractMessage(r).text, "fs050w4");
});
test("UCS-2 Chinese and invalid UCS-2", () => {
  assert.equal(extractMessage(record(prefix + "08" + stamp + "044e2d6587")).text, "中文");
  assert.throws(() => extractMessage(record(prefix + "08" + stamp + "014e")), /不完整/);
});
test("segmented message with GSM header fill bits + extension", () => {
  const message = extractMessage(record("004405912143f50000" + stamp + "0e05000322020182c2e186b22903"));
  assert.equal(message.text, "ABC^€");
  assert.deepEqual(message.segment, {reference:34, total:2, part:1});
});
test("segmented UCS-2 + alphanumeric sender", () => {
  const message = extractMessage(record("004406d041e1100008" + stamp + "09060804123402024e2d"));
  assert.equal(message.sender, "ABC");
  assert.equal(message.text, "中");
  assert.deepEqual(message.segment, {reference:0x1234, total:2, part:2});
});
test("truncated modem envelope, non-text, malformed input, MAL error", () => {
  const r = record(prefix + "08" + stamp + "044e2d6587");
  assert.throws(() => extractMessage({...r, hex:r.hex.slice(0,-2)}), /不完整/);
  assert.throws(() => extractMessage(record(prefix + "04" + stamp + "02beef")), /編碼/);
  assert.throws(() => extractMessage({...r, hex:"xx"}), /格式異常/);
  assert.throws(() => extractMessage({...r, err:610}), /無法讀取/);
});
test("partial, duplicate and colliding storage indexes remain distinguishable", () => {
  const result = inboxRows({uim_tags:[group(0),group(1,0),group(3,1)],
    tags:[{...group(0,0), count:3}, group(1,0,3)]});
  assert.deepEqual(result.messages, [{storage:0,id:0,tag:1},{storage:1,id:0,tag:0},{storage:1,id:3,tag:1}]);
  assert.equal(result.warnings.length, 2);
  const missing = inboxRows({tags:[{...group(0),err:610}]});
  assert.deepEqual(missing.messages, []);
  assert.equal(missing.warnings.length, 3);
});
