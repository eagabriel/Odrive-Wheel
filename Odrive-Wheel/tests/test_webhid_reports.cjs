// Run with: node Odrive-Wheel/tests/test_webhid_reports.cjs
// Execute the actual page builders; no browser or motor is needed.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../tools/odrive-wheel.html'), 'utf8');

for (const match of html.matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script>/gi)) {
  if (!/\bsrc\s*=/.test(match[1]) && !/application\/json/.test(match[1])) {
    new vm.Script(match[2]);
  }
}

function checkSetEffect(bytes, block, type) {
  assert.equal(bytes.length, 17, 'complete Set Effect payload');
  assert.equal(bytes[0], block);
  assert.equal(bytes[1], type);
  const data = new DataView(Uint8Array.from(bytes).buffer);
  assert.equal(bytes[12], 1, 'X enabled without polar direction');
  assert.equal((data.getUint16(13, true) - 18000) / 18000, 1, 'positive unit X');
  assert.equal(-(data.getUint16(15, true) - 18000) / 18000, -0, 'zero Y');
}

let builders = 0;
for (const match of html.matchAll(/const buildSetEffect = ([\s\S]*?=>\s*\[[\s\S]*?\]);/g)) {
  const build = vm.runInNewContext('(' + match[1] + ')');
  for (const type of [1, 4, 8, 9, 11]) {
    const bytes = build(4, type, 1200, 128);
    checkSetEffect(bytes, 4, type);
    assert.equal(bytes[2] | (bytes[3] << 8), 1200);
    assert.equal(bytes[10], 128);
  }
  builders++;
}
assert.equal(builders, 2, 'FFB Test and Performance Test builders covered');

let literals = 0;
for (const match of html.matchAll(/\.sendOutput\(0x01,\s*(\[[\s\S]*?\])\)/g)) {
  const bytes = vm.runInNewContext(match[1], {
    PT_BLOCK: {cf: 1}, PT_TYPE: {cf: 1}, FS_SINE_BLOCK: 4, FS_SINE_TYPE: 4,
  });
  checkSetEffect(bytes, bytes[0], bytes[1]);
  literals++;
}
assert.equal(literals, 6, 'coastdown, sweep and launch reports covered');
console.log('PASS: page JavaScript syntax; 2 builders and 6 direct Set Effect reports');
