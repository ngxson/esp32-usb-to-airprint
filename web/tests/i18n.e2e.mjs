// Run against npm run dev: BASE=http://localhost:8631 node tests/i18n.e2e.mjs
import { chromium } from 'playwright-core';
import assert from 'node:assert/strict';
const browser = await chromium.launch({ channel: 'chrome', headless: true });
const base = process.env.BASE || 'http://localhost:8631';
const errors = [];
async function selectLanguage(page, locale) {
  await page.locator('.language-picker .trigger').click();
  await page.getByRole('radio', { name: locale === 'zh-CN' ? '简体中文' : 'English', exact: true }).click();
  await page.locator('.language-picker .panel').waitFor({ state: 'hidden' });
}
async function currentLanguage(page) {
  return (await page.locator('.language-picker .current').innerText()).trim();
}
try {
  const context = await browser.newContext({ locale: 'en-US', viewport: { width: 1280, height: 860 } });
  const page = await context.newPage();
  page.on('pageerror', e => errors.push(e.stack || e.message));
  await page.goto(base);
  await selectLanguage(page, 'zh-CN');
  await page.getByRole('heading', { name: '墨水', exact: true }).waitFor();
  const statusBox = await page.locator('.foot > .pill').boundingBox();
  const pickerBox = await page.locator('.language-picker').boundingBox();
  assert.ok(pickerBox.y >= statusBox.y + statusBox.height, 'language control is below online status');
  await page.locator('.language-picker .trigger').click();
  await page.getByRole('radio', { name: '简体中文', exact: true }).press('ArrowUp');
  assert.equal(await currentLanguage(page), 'English');
  await page.keyboard.press('Escape');
  assert.equal(await page.locator('.language-picker .trigger').evaluate(el => el === document.activeElement), true);
  await selectLanguage(page, 'zh-CN');
  await page.locator('.language-picker .trigger').click();
  await page.getByRole('heading', { name: '墨水', exact: true }).click();
  await page.locator('.language-picker .panel').waitFor({ state: 'hidden' });
  assert.equal(await page.locator('html').getAttribute('lang'), 'zh-CN');
  assert.match(await page.title(), /^打印机 ·/);
  for (const [route, title] of [['scanner', '扫描'], ['files', '文件'], ['process', '页面处理']]) {
    await page.locator(`.sidebar a[href="#/${route}"]`).click();
    await page.getByRole('heading', { name: title, exact: true }).waitFor();
  }
  await page.getByRole('button', { name: '证件复印', exact: true }).first().click();
  await page.locator('.modal').waitFor();
  await page.getByLabel('关闭', { exact: true }).click();
  await page.reload();
  assert.equal(await currentLanguage(page), '简体中文');
  await selectLanguage(page, 'en');
  await page.getByRole('heading', { name: 'Process', exact: true }).waitFor();
  assert.equal(await page.locator('html').getAttribute('lang'), 'en');
  await page.reload();
  assert.equal(await currentLanguage(page), 'English');
  const phone = await browser.newContext({ locale: 'zh-CN', viewport: { width: 390, height: 844 } });
  const mobile = await phone.newPage();
  mobile.on('pageerror', e => errors.push(e.stack || e.message));
  await mobile.goto(base);
  await mobile.getByText('您想做什么？', { exact: true }).waitFor();
  assert.equal(await currentLanguage(mobile), '简体中文');
  assert.ok(await mobile.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'no mobile horizontal overflow');
  await selectLanguage(mobile, 'en');
  await mobile.getByText('What would you like to do?', { exact: true }).waitFor();
  const restricted = await browser.newContext({ locale: 'zh-CN' });
  await restricted.addInitScript(() => {
    Storage.prototype.getItem = () => { throw new DOMException('blocked', 'SecurityError'); };
    Storage.prototype.setItem = () => { throw new DOMException('blocked', 'SecurityError'); };
  });
  const blocked = await restricted.newPage();
  blocked.on('pageerror', e => errors.push(e.stack || e.message));
  await blocked.goto(base);
  await selectLanguage(blocked, 'en');
  await blocked.getByRole('heading', { name: 'Ink', exact: true }).waitFor();
  assert.deepEqual(errors, []);
  console.log('i18n e2e: routes, dialog, live switching, reload persistence, browser detection, mobile and blocked storage passed');
} finally { await browser.close(); }
