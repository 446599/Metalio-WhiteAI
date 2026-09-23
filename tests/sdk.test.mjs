import { test } from 'node:test';
import assert from 'node:assert/strict';
let sequence=0;
const fresh=()=>import(`../sdk.mjs?test=${sequence++}`);

test('self-hosted pinned SDK loads without a CDN and concurrent loads share one request',async()=>{
  const original=globalThis.fetch;let calls=0,signal;
  globalThis.fetch=async(url,options)=>{calls++;signal=options.signal;assert.equal(options.method,'HEAD');assert.match(url,/vendor\/esptool-js-0.7.0/);return new Response(null,{headers:{'content-type':'text/javascript'}});};
  try {const sdk=await fresh();const a=sdk.loadSdk(),b=sdk.loadSdk();assert.equal(a,b);const value=await a;assert.equal(typeof value.ESPLoader,'function');assert.equal(typeof value.Transport,'function');assert.equal(calls,1);assert.equal(signal.aborted,true);}
  finally {globalThis.fetch=original;}
});
test('invalid local MIME fails closed and a later SDK load can retry',async()=>{
  const original=globalThis.fetch;const sdk=await fresh();
  try {
    globalThis.fetch=async()=>new Response('<html>',{headers:{'content-type':'text/html'}});
    await assert.rejects(sdk.loadSdk(),/本地刷机库类型错误/);
    globalThis.fetch=async()=>new Response(null,{headers:{'content-type':'application/javascript'}});
    assert.equal(typeof(await sdk.loadSdk()).ESPLoader,'function');
  } finally {globalThis.fetch=original;}
});
test('local server errors are not silently replaced by a CDN import',async()=>{
  const original=globalThis.fetch;
  try {globalThis.fetch=async()=>new Response(null,{status:503});await assert.rejects((await fresh()).loadSdk(),/HTTP 503/);}
  finally {globalThis.fetch=original;}
});
