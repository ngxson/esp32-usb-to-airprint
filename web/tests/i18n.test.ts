import { test } from 'node:test';
import assert from 'node:assert/strict';
import { messages, resolveLocale, translate } from '../src/lib/i18n.ts';
const placeholders = (s: string) => [...new Set(s.match(/\{\w+\}/g) || [])].sort();
test('translations preserve all named parameters', () => {
  for (const [english, chinese] of Object.entries(messages)) {
    assert.ok(chinese.trim(), `Empty translation: ${english}`);
    assert.deepEqual(placeholders(chinese), placeholders(english), english);
  }
});
test('saved choice wins, browser preferences are negotiated, unknown languages fall back', () => {
  assert.equal(resolveLocale('en', ['zh-CN']), 'en');
  assert.equal(resolveLocale('zh-CN', ['en-US']), 'zh-CN');
  assert.equal(resolveLocale(null, ['fr', 'zh-Hans-CN', 'en']), 'zh-CN');
  assert.equal(resolveLocale(null, ['en-GB', 'zh-CN']), 'en');
  assert.equal(resolveLocale('invalid', ['ja']), 'en');
  assert.equal(resolveLocale(null, []), 'en');
});
test('parameters are literal and English uses the source message', () => {
  assert.equal(translate('en', 'Ready · {ink} ink low', { ink: '$&{ink}' }), 'Ready · $&{ink} ink low');
  assert.equal(translate('zh-CN', 'Scanning · {percent}%', { percent: 42 }), '正在扫描 · 42%');
  assert.equal(translate('en', 'Scanning · {percent}%', { percent: 42 }), 'Scanning · 42%');
});
