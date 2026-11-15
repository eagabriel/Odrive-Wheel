// Run with: node Odrive-Wheel/tests/test_overlay_errors.cjs
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const html = fs.readFileSync(path.join(__dirname, '../tools/odrive-wheel.html'), 'utf8');
function source(name) {
  const start = html.indexOf('function ' + name + '(');
  assert(start >= 0, name);
  const open = html.indexOf('{', start);
  let end = open + 1, depth = 1;
  while (depth) {
    if (html[end] === '{') depth++;
    if (html[end] === '}') depth--;
    end++;
  }
  return html.slice(start, end);
}
const el = {textContent: '', hidden: true};
let now = 0;
const context = vm.createContext({
  overlayState: {paused: true, hidErrors: null, pipWin: {document: {getElementById: () => el}}},
  currentLang: 'pt', performance: {now: () => now},
});
vm.runInContext(html.slice(html.indexOf('const ERR_BITS_ODRIVE ='), html.indexOf('function decodeErr(')), context);
for (const name of ['decodeErr', '_ovlCaptureErrors', '_ovlUpdateErrors', '_ovlOnHidReport']) {
  vm.runInContext(source(name), context);
}
const report = new DataView(new ArrayBuffer(49));
function send(data = report, reportId = 1) {
  context._ovlOnHidReport({reportId, data});
  context._ovlUpdateErrors();
}
send();
assert(el.hidden, 'healthy report hides the alert');
report.setUint32(33, 1, true);
report.setUint32(37, 0x80000000, true);
report.setUint32(41, 1, true);
report.setUint32(45, 1, true);
send();
assert(!el.hidden, 'errors display even with charts paused');
assert.match(el.textContent, /Axis 0x00000001/);
assert.match(el.textContent, /Motor 0x80000000/);
assert.match(el.textContent, /Encoder 0x00000001/);
assert.match(el.textContent, /Controller 0x00000001/);
assert.match(el.textContent, /INVALID_STATE/);
now = 2000;
context._ovlUpdateErrors();
assert.match(el.textContent, /última leitura/);
send();
assert(!el.textContent.includes('última leitura'));
report.setUint32(33, 0, true);
report.setUint32(37, 0, true);
report.setUint32(41, 0, true);
report.setUint32(45, 0, true);
send(report, 2);
assert(!el.hidden, 'other report IDs do not clear errors');
send();
assert(el.hidden, 'cleared errors disappear');
send(new DataView(new ArrayBuffer(30)));
assert.match(el.textContent, /indisponíveis/);
context.currentLang = 'en';
context._ovlUpdateErrors();
assert.match(el.textContent, /unavailable/);
context.overlayState.pipWin = null;
assert.doesNotThrow(() => send(new DataView(new ArrayBuffer(0))));
console.log('PASS: live HID errors, four groups, decoding, pause, clear, stale data, old firmware and language');
