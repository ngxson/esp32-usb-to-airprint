// End-to-end test of the web UI in headless Chrome, against the host build of
// the server (host/airprint_host, emulated scanner, jobs to host/out/job-N.prn):
// upload a PDF and print it, scan, a button scan that two browsers receive,
// Process (rotate, ID card, PDF export) and the phone layout. Screenshots go
// to host/out/ui-*.png.
//
//   BASE=http://localhost:8641 SERVER_PID=<pid> node tests/e2e.mjs
import { chromium } from 'playwright-core';
import assert from 'node:assert/strict';
import { existsSync, readdirSync } from 'node:fs';

const BASE = process.env.BASE || 'http://localhost:8641';
const PID = Number(process.env.SERVER_PID || 0);
const OUT = new URL('../../host/out/', import.meta.url).pathname;
// CUPS's samples (gzipped there; run_tests.sh unpacks them)
const DOC = `${OUT}e2e/document-a4.pdf`;
const PHOTO = `${OUT}e2e/color.jpg`;

const errors = [];
function watch(page, name) {
  page.on('pageerror', (e) => errors.push(`${name}: ${e.message}`));
  page.on('console', (m) => m.type() === 'error' && errors.push(`${name} console: ${m.text()}`));
}
const step = (s) => console.log(`- ${s}`);
const shot = (page, name) => page.screenshot({ path: `${OUT}ui-${name}.png` });
const jobs = () => (existsSync(OUT) ? readdirSync(OUT).filter((f) => /^job-\d+\.prn$/.test(f)) : []);

const browser = await chromium.launch({ channel: 'chrome', headless: true });
let page;
try {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 860 } });
  page = await ctx.newPage();
  watch(page, 'desktop');

  await page.goto(BASE + '/');
  await page.getByText('Online').waitFor();
  await page.getByRole('heading', { name: /Canon MG2500/ }).waitFor();
  await page.getByText('Black').first().waitFor();
  step('printer page: live state over /events');
  await shot(page, 'printer');

  // Files: upload a PDF and a photo
  await page.click('a[href="#/files"]');
  await page.locator('input[type=file][multiple]').setInputFiles([DOC, PHOTO]);
  await page.locator('.tile', { hasText: 'document-a4.pdf' }).waitFor();
  await page.locator('.tile', { hasText: 'color.jpg' }).waitFor();
  await page.locator('.tile img').first().waitFor();
  step('files: PDF and image uploaded, with thumbnails');
  await shot(page, 'files');

  // print the PDF at 300 dpi, pages 1-2, two copies of page... keep it quick: 1-2
  const before = jobs().length;
  await page.locator('.tile', { hasText: 'document-a4.pdf' }).getByRole('button', { name: 'Print' }).click();
  await page.locator('canvas.sheet').waitFor();
  await page.getByPlaceholder(/All \d+ pages/).fill('1-2');
  await page.getByRole('button', { name: 'Fast · 300 dpi' }).click();
  await page.locator('.modal').getByRole('button', { name: 'A4', exact: true }).click();
  await shot(page, 'print-dialog');
  await page.locator('footer').getByRole('button', { name: /^Print/ }).click();
  await page.getByText(/Printed 2 pages of document-a4.pdf/).waitFor({ timeout: 120000 });
  assert.equal(jobs().length, before + 1, 'one job written by the server');
  step('print: 2 pages at 300 dpi sent as URF over IPP');

  // print the photo in colour at 600 dpi, fit
  await page.locator('.tile', { hasText: 'color.jpg' }).getByRole('button', { name: 'Print' }).click();
  await page.locator('canvas.sheet').waitFor();
  await page.getByRole('button', { name: 'Standard · 600 dpi' }).click();
  await page.locator('footer').getByRole('button', { name: /^Print/ }).click();
  await page.getByText(/Printed 1 page of color.jpg/).waitFor({ timeout: 180000 });
  step('print: photo at 600 dpi');

  // scan from the page (kept, 150 dpi)
  await page.click('a[href="#/scanner"]');
  await page.getByRole('button', { name: '150 dpi' }).click();
  await page.getByRole('button', { name: /^A4$/ }).click();
  await page.getByRole('button', { name: /^Scan$/ }).click();
  await page.locator('.beam').waitFor();
  await page.waitForTimeout(1500);
  await shot(page, 'scanning');
  await page.getByText('Scan saved to Files').waitFor({ timeout: 120000 });
  await page.getByRole('button', { name: 'Print a copy' }).waitFor();
  step('scan: live preview, saved to Files');
  await shot(page, 'scanned');

  // the button: a second browser (its own IndexedDB) and a second tab both get it
  const ctx2 = await browser.newContext({ viewport: { width: 1000, height: 800 } });
  const other = await ctx2.newPage();
  watch(other, 'second browser');
  await other.goto(BASE + '/#/scanner');
  await other.getByText('Online').waitFor();
  const tab2 = await ctx.newPage();
  watch(tab2, 'second tab');
  await tab2.goto(BASE + '/#/files');
  await tab2.getByText('Online').waitFor();
  const scanCount = (pg) =>
    pg.evaluate(async () => {
      const all = await new Promise((res) => {
        const r = indexedDB.open('printer-web');
        r.onsuccess = () => {
          const q = r.result.transaction('files').objectStore('files').getAll();
          q.onsuccess = () => res(q.result);
        };
      });
      return all.filter((f) => f.source === 'scan').length;
    });
  const scansBefore = await scanCount(page);
  if (PID) {
    process.kill(PID, 'SIGUSR1');
    await other.getByText(/started from the button/).waitFor({ timeout: 20000 });
    await other.waitForTimeout(1500);
    await shot(other, 'button-scan');
    await other.getByText('Scan saved to Files').waitFor({ timeout: 120000 });
    let counts = 0;
    for (let i = 0; i < 20 && counts <= scansBefore; i++) {
      await page.waitForTimeout(500);
      counts = await scanCount(page);
    }
    await page.waitForTimeout(3000); // a second save would show by now
    counts = await scanCount(page);
    assert.equal(counts, scansBefore + 1, 'two tabs of one browser save the button scan once');
    step('button scan: shown live and saved by a second browser, once per browser');
  }

  // Process: the scan and the PDF, rotate, ID card, export
  await page.bringToFront();
  await page.click('a[href="#/process"]');
  await page.getByRole('button', { name: 'Add from Files' }).first().click();
  await page.locator('.modal .tile', { hasText: 'document-a4.pdf' }).click();
  await page.locator('.modal .tile', { hasText: /Scan \d{4}/ }).first().click();
  await page.getByRole('button', { name: /^Add 2/ }).click();
  await page.locator('.item').nth(3).waitFor();
  await page.locator('.item').first().getByRole('button', { name: 'Rotate right' }).click();
  await page.getByRole('button', { name: 'ID card copy' }).first().click();
  await page.locator('.modal').getByRole('button', { name: 'From Files', exact: true }).first().click();
  await page.locator('.modal .tile', { hasText: 'color.jpg' }).click();
  await page.locator('.slot img').first().waitFor();
  await page.waitForTimeout(800);
  await shot(page, 'card');
  await page.getByRole('button', { name: 'Done' }).click();
  await page.waitForTimeout(1500);
  await shot(page, 'process');
  await page.getByRole('button', { name: 'Save as PDF' }).click();
  await page.getByText(/Saved Document .*\.pdf to Files/).waitFor({ timeout: 120000 });
  step('process: rotated pages + ID card exported as a PDF');

  // the exported PDF opens in the viewer and in the print dialog
  await page.click('a[href="#/files"]');
  await page.locator('.tile', { hasText: /Document \d{4}/ }).first().locator('.thumb').click();
  await page.locator('canvas.page').nth(5).waitFor({ timeout: 30000 });
  await shot(page, 'viewer');
  await page.keyboard.press('Escape');

  // phone
  const phone = await browser.newContext({
    viewport: { width: 390, height: 844 },
    deviceScaleFactor: 2,
    isMobile: true,
    hasTouch: true,
  });
  const mp = await phone.newPage();
  watch(mp, 'phone');
  await mp.goto(BASE + '/');
  await mp.getByText('What would you like to do?').waitFor();
  await mp.getByText('Online').waitFor();
  await shot(mp, 'phone-home');
  await mp.getByRole('link', { name: /Scanner/ }).click();
  await mp.getByRole('button', { name: /^Scan$/ }).waitFor();
  await shot(mp, 'phone-scanner');
  await mp.getByRole('link', { name: 'Back to the menu' }).click();
  await mp.getByText('What would you like to do?').waitFor();
  await mp.getByRole('link', { name: /Printer/ }).click();
  await mp.getByRole('heading', { name: /Canon MG2500/ }).waitFor();
  await shot(mp, 'phone-printer');
  step('phone: menu first, then a section with a back button');

  assert.deepEqual(errors, [], 'no page errors');
  console.log('e2e: all passed');
} catch (e) {
  if (page) await shot(page, 'failure').catch(() => {});
  console.log('errors seen:', errors);
  throw e;
} finally {
  await browser.close();
}
