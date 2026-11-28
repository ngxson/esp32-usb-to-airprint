import { test } from 'node:test';
import assert from 'node:assert/strict';
import { noScannerMessage } from '../src/lib/scanner.ts';

test('no printer attached: check the printer', () => {
  assert.equal(noScannerMessage(false, 'Canon MG2500 series'), 'No scanner: is the printer on and connected?');
  assert.equal(noScannerMessage(undefined), 'No scanner: is the printer on and connected?');
});

test('a printer attached whose scanner is not supported: say so', () => {
  const m = noScannerMessage(true, 'Canon P200 series');
  assert.equal(m, "The Canon P200 series's scanner is not supported; printing works.");
  assert.ok(!m.includes('on and connected'));
});
