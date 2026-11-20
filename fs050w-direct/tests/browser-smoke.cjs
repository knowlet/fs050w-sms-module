// Run: node tests/browser-smoke.cjs (requires Playwright with a Chromium build).
// Every request to the synthetic router is fulfilled locally, including sends/deletes.
"use strict";
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || "playwright");
const fs = require("node:fs");
const path = require("node:path");
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const html = fs.readFileSync(path.join(__dirname, "../dist/sms.html"), "utf8");
const hostname = "http://192.168.77.1";
const sample = "000405912143f50000621102110075230ae8329bfd4697d9ec37";
function fakePayload(storage, swapped=false) {
  const pdu = Buffer.from(sample, "hex");
  if (swapped) pdu[12] ^= 1;
  const len = Buffer.alloc(4); len.writeUInt32LE(pdu.length);
  return {storage, id:0, rc:0, err:0, hex:Buffer.concat([Buffer.alloc(44), len, pdu]).toString("hex")};
}
function expectedDigest(password, challenge) {
  const sign = s => crypto.createHmac("md5", "0123456789").update(s).digest("hex");
  return sign(crypto.createHash("md5").update("4cc68e3626e5b94602c325f7c4ca5dee:example.com:" + sign(password)).digest("hex") + challenge);
}
(async () => {
  const browser = await chromium.launch({executablePath:process.env.CHROMIUM_PATH || undefined, headless:true});
  try {
    for (const width of [375,1140]) {
      const page = await browser.newPage({viewport:{width,height:800}});
      let logged = false, sends = 0, deletes = 0, swapped = false, removed = false, cap = true, nextCsrf = 0, validCsrf = "", unknownDelete = false;
      const errors = [];
      page.on("pageerror", err => errors.push(err.message));
      await page.route("**/*", async route => {
        const url = new URL(route.request().url());
        assert.equal(url.origin, hostname);
        const req = route.request();
        if (url.pathname === "/sms.html") return route.fulfill({contentType:"text/html; charset=utf-8", body:html});
        if (url.pathname === "/font/FSElliotPro.ttf") return route.fulfill({status:200, body:""});
        if (url.pathname === "/goform/x_csrf_token") {
          validCsrf = String(++nextCsrf);
          return route.fulfill({headers:{"X-Csrf-Token":validCsrf},body:""});
        }
        assert.equal(req.headers()["x-csrf-token"], validCsrf);
        validCsrf = "";
        const body = req.postDataJSON();
        let reply;
        if (url.pathname === "/goform/get_private_key") reply = {retcode:0, prikey:"mock"};
        else if (url.pathname === "/goform/login2") {
          assert.deepEqual(body, {username:"4cc68e3626e5b94602c325f7c4ca5dee", prikey:"mock", password:expectedDigest("pw", "mock")});
          logged = true; reply = {retcode:0};
        } else if (!logged) return route.fulfill({status:302, headers:{location:"/common/home.html#login"}});
        else if (url.pathname === "/action/logout") { logged = false; reply = {retcode:0}; }
        else if (url.pathname === "/action/sms_get_sms_list") {
          const tags = has => [0,1,2,3].map(tag => ({tag,rc:0,err:0,count:tag===1&&has?1:0,index:tag===1&&has?[0]:[]}));
          reply = {retcode:0,data:{tags:tags(true),uim_tags:tags(!removed),delete_one:cap?1:0}};
        } else if (url.pathname === "/action/sms_read_sms_info") {
          if (body.fs050w_sms_delete_one === 1) {
            assert.deepEqual(body,{fs050w_sms_delete_one:1,id:0,storage:0}); deletes++;
            if (unknownDelete) return route.abort("failed");
            removed = true;
            reply = {retcode:0,data:{module:"sms",msgid:11011,id:0,storage:0,deleted:1,rc:0,err:0}};
          } else {
            reply = {retcode:0, data:fakePayload(body.storage, swapped)};
          }
        } else if (url.pathname === "/action/sms_send_sms_info") {
          assert.deepEqual(body,{Number:"0900000000",Content:"sample"});
          sends++; reply = {retcode:0,data:{accepted:1,rc:0,err:0}};
        } else throw Error("Unexpected path: "+url.pathname);
        return route.fulfill({json:reply});
      });
      await page.goto(hostname + "/sms.html");
      await page.waitForFunction(() => !document.getElementById("login").disabled);
      await page.fill("#password","pw");
      await page.click("#login");
      await page.waitForSelector("#messages button");
      assert.equal(await page.locator("#messages button").count(),2);
      for (const store of ["1","0"]) {
        await page.click('[data-filter="'+store+'"]');
        await page.click("#messages button"); await page.waitForSelector("#read-detail:visible");
        assert.equal(await page.textContent("#message-body"),"hellohello");
      }
      const del = () => page.click("#delete");
      await del(); assert.equal(await page.evaluate(() => document.activeElement.id),"keep-message");
      await page.click("#keep-message"); assert.equal(deletes,0);
      await del(); swapped = true;
      await page.click("#confirm-delete"); await page.waitForFunction(()=>document.getElementById("read-notice").textContent.includes("變更"));
      assert.equal(deletes,0);
      swapped = false; unknownDelete = true;
      await del(); await page.click("#confirm-delete");
      await page.waitForFunction(()=>document.getElementById("read-notice").textContent.includes("明確"));
      assert.equal(deletes,1);
      unknownDelete = false;
      await del(); await page.click("#confirm-delete"); await page.waitForSelector("#send-receipt:visible");
      assert.equal(deletes,2); assert.equal(await page.locator("#messages button").count(),0);
      cap=false; await page.click("#refresh"); await page.click('[data-filter="1"]');
      await page.click("#messages button"); await page.waitForSelector("#delete-help:visible");
      assert.equal(await page.isDisabled("#delete"),true);
      await page.click("#compose"); await page.fill("#number","0900000000"); await page.fill("#body","sample"); await page.click("#send");
      await page.waitForFunction(()=>document.getElementById("send-receipt").textContent.includes("接受")); assert.equal(sends,1);
      assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=window.innerWidth),true);
      await page.click("#logout"); await page.waitForSelector("#login-panel:visible");
      assert.deepEqual(errors,[]);
      await page.close();
    }
  } finally { await browser.close(); }
  console.log("Desktop and mobile browser fixtures passed.");
})().catch(err=>{console.error(err);process.exitCode=1;});
