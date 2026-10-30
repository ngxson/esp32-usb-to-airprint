<script lang="ts">
  import { t, i18n } from '../lib/i18n.svelte';
  import { onDestroy } from 'svelte';
  import Printer from '@lucide/svelte/icons/printer';
  import ChevronLeft from '@lucide/svelte/icons/chevron-left';
  import ChevronRight from '@lucide/svelte/icons/chevron-right';
  import Minus from '@lucide/svelte/icons/minus';
  import Plus from '@lucide/svelte/icons/plus';
  import LoaderCircle from '@lucide/svelte/icons/loader-circle';
  import Modal from './Modal.svelte';
  import { device, inkSummary } from '../lib/device.svelte';
  import { fileSource, type StoredFile } from '../lib/files.svelte';
  import {
    defaultPaper,
    MARGINS,
    PAPERS,
    parseRanges,
    place,
    type Fit,
    type Orientation,
    type PaperId,
  } from '../lib/layout';
  import { renderJob, sendJob, type PrintOptions } from '../lib/print';
  import { whiteCanvas, type Source } from '../lib/source';
  import { toast } from '../lib/ui.svelte';

  let { file, onclose }: { file: StoredFile; onclose: () => void } = $props();

  const saved = (() => {
    try {
      return JSON.parse(localStorage.getItem('printSettings.v2') || '{}');
    } catch {
      return {};
    }
  })();

  let src = $state.raw<Source | null>(null);
  let loadError = $state('');
  let copies = $state(1);
  let range = $state('');
  let paper = $state<PaperId>(saved.paper || defaultPaper());
  let orientation = $state<Orientation>('auto');
  // a scan prints at its real size (a copy); anything else fits the page
  // svelte-ignore state_referenced_locally
  let fit = $state<Fit>(file.source === 'scan' && file.dpi ? 'actual' : 'fit'); // a scan: a 1:1 copy
  let gray = $state<boolean>(saved.gray ?? false);
  let dpi = $state<300 | 600>(saved.dpi || 600);
  let pageIdx = $state(0);
  let previewEl: HTMLDivElement;

  type Phase = 'setup' | 'render' | 'send' | 'finish';
  let phase = $state<Phase>('setup');
  let frac = $state(0);
  let preparingPage = $state(1);
  let preparingTotal = $state(1);
  const detail = $derived(phase === 'render'
    ? t('Preparing page {page} of {total}', { page: preparingPage, total: preparingTotal })
    : phase === 'finish' ? t('Finishing the last page')
    : t('Sending to the printer'));
  let abort: AbortController | null = null;
  let closed = false;

  // svelte-ignore state_referenced_locally
  fileSource(file).then(
    (s) => (src = s),
    (e) => (loadError = (e as Error).message),
  );

  const count = $derived(src?.pages.length ?? file.pages ?? 1);
  const selected = $derived(parseRanges(range, count));
  const p = $derived(device.printer);
  const blocker = $derived.by(() => {
    if (!device.online) return t('The printer box is not reachable.');
    if (!p?.connected) return t('No printer is connected to the printer box.');
    if (p.state === 'processing') return t('The printer is busy with another job.');
    if (device.scan?.state === 'scanning') return t('The printer is scanning.');
    return '';
  });

  $effect(() => {
    try {
      localStorage.setItem('printSettings.v2', JSON.stringify({ paper, gray, dpi }));
    } catch {
      /* private mode */
    }
  });

  $effect(() => {
    if (pageIdx >= count) pageIdx = Math.max(0, count - 1);
  });

  // preview of the sheet
  let previewToken = 0;
  $effect(() => {
    const s = src;
    const opts = { paper, fit, orientation, pageIdx, gray };
    if (!s || !previewEl) return;
    const token = ++previewToken;
    const P = PAPERS[opts.paper];
    const box = previewEl.getBoundingClientRect();
    const scale = Math.min((box.width || 300) / P.w, (box.height || 420) / P.h) * Math.min(2, devicePixelRatio || 1);
    const ps = s.pages[opts.pageIdx];
    const pl = place(ps.w, ps.h, opts.paper, opts.fit, opts.orientation);
    const c = whiteCanvas(P.w * scale, P.h * scale);
    s.draw(c, opts.pageIdx, pl.rot, pl.scale * scale, pl.x * scale, pl.y * scale).then(() => {
      if (token !== previewToken) return;
      if (opts.gray) {
        const ctx = c.getContext('2d')!;
        ctx.globalCompositeOperation = 'saturation';
        ctx.fillStyle = '#000';
        ctx.fillRect(0, 0, c.width, c.height);
      }
      c.className = 'sheet';
      previewEl.querySelector('canvas')?.remove();
      previewEl.prepend(c);
    });
  });

  async function print() {
    if (!src || !selected?.length) return;
    const o: PrintOptions = { paper, fit, orientation, pages: selected, copies, gray, dpi };
    abort = new AbortController();
    const signal = abort.signal;
    const sheets = selected.length * copies;
    const name = file.name;
    try {
      phase = 'render';
      const doc = await renderJob(
        src,
        o,
        (done, total) => {
          frac = done / total;
          preparingPage = Math.min(total, Math.floor(done) + 1);
          preparingTotal = total;
        },
        signal,
      );
      phase = 'send';
      frac = 0;
      await sendJob(
        doc,
        name,
        (f) => {
          frac = f;
          if (f >= 1) {
            phase = 'finish';
          }
        },
        signal,
      );
      toast(sheets === 1 ? t('Printed {count} page of {name}', { count: sheets, name }) : t('Printed {count} pages of {name}', { count: sheets, name }), 'ok');
      if (!closed) onclose();
    } catch (e) {
      if ((e as Error).name === 'AbortError') {
        toast(t('Print canceled'));
      } else {
        toast(t('Print failed: {error}', { error: (e as Error).message }), 'error', [], 9000);
      }
      phase = 'setup';
    } finally {
      abort = null;
    }
  }

  function close() {
    // a job being sent goes on in the background
    closed = true;
    if (phase === 'render') abort?.abort();
    onclose();
  }

  onDestroy(() => {
    closed = true;
  });

  const marginStyle = $derived.by(() => {
    const P = PAPERS[paper];
    return `left:${(MARGINS.left / P.w) * 100}%;right:${(MARGINS.right / P.w) * 100}%;top:${(MARGINS.top / P.h) * 100}%;bottom:${(MARGINS.bottom / P.h) * 100}%`;
  });
</script>

<Modal title={t('Print {name}', { name: file.name })} onclose={close} wide>
  <div class="layout" lang={i18n.locale}>
    <div class="left">
      <div class="preview" bind:this={previewEl} style:aspect-ratio={`${PAPERS[paper].w} / ${PAPERS[paper].h}`}>
        <div class="margins" style={marginStyle}></div>
        {#if !src && !loadError}<div class="loading"><LoaderCircle class="spin" /></div>{/if}
        {#if loadError}<div class="loading err">{loadError}</div>{/if}
      </div>
      {#if count > 1}
        <div class="pager row">
          <button class="icon-btn" onclick={() => pageIdx--} disabled={pageIdx === 0} aria-label={t('Previous page')}><ChevronLeft /></button>
          <span class="small muted">{t('Page {page} of {count}', { page: pageIdx + 1, count })}</span>
          <button class="icon-btn" onclick={() => pageIdx++} disabled={pageIdx >= count - 1} aria-label={t('Next page')}><ChevronRight /></button>
        </div>
      {/if}
      <p class="small muted center">{t('The dashed line shows what the printer can reach.')}</p>
    </div>

    <div class="opts">
      {#if phase !== 'setup'}
        <div class="progress card card-pad">
          <div class="row"><LoaderCircle size={18} class="spin" /><b>{detail}</b></div>
          <div class="bar"><div style:width={`${Math.max(2, frac * 100)}%`}></div></div>
          <p class="small muted">
            {#if phase === 'render'}
              {t('Pages are prepared here in the browser, then sent.')}
            {:else}
              {t('The printer prints while it receives. You can close this window; progress shows on the Printer page.')}
            {/if}
          </p>
          <button class="btn danger" onclick={() => abort?.abort()}>{t('Cancel')}</button>
        </div>
      {/if}
      <fieldset disabled={phase !== 'setup'}>
        <div class="field">
          <span class="label">{t('Copies')}</span>
          <div class="stepper">
            <button class="icon-btn" onclick={() => (copies = Math.max(1, copies - 1))} aria-label={t('Fewer')}><Minus size={16} /></button>
            <input class="input" type="number" min="1" max="99" bind:value={copies} aria-label={t('Copies')} />
            <button class="icon-btn" onclick={() => (copies = Math.min(99, copies + 1))} aria-label={t('More')}><Plus size={16} /></button>
          </div>
        </div>
        {#if count > 1}
          <div class="field">
            <span class="label">{t('Pages')}</span>
            <input class="input" placeholder={t('All {count} pages, or e.g. 1-3, 5', { count })} bind:value={range} />
            {#if !selected}<span class="err small">{t('Pages 1 to {count}, e.g. "1-3, 5"', { count })}</span>{/if}
          </div>
        {/if}
        <div class="field">
          <span class="label">{t('Paper')}</span>
          <div class="seg">
            <button class:on={paper === 'a4'} onclick={() => (paper = 'a4')}>A4</button>
            <button class:on={paper === 'letter'} onclick={() => (paper = 'letter')}>{t('Letter')}</button>
          </div>
        </div>
        <div class="field">
          <span class="label">{t('Size')}</span>
          <div class="seg">
            <button class:on={fit === 'fit'} onclick={() => (fit = 'fit')}>{t('Fit to page')}</button>
            <button class:on={fit === 'fill'} onclick={() => (fit = 'fill')}>{t('Fill page')}</button>
            <button class:on={fit === 'actual'} onclick={() => (fit = 'actual')}>{t('Actual size')}</button>
          </div>
        </div>
        <div class="field">
          <span class="label">{t('Orientation')}</span>
          <div class="seg">
            <button class:on={orientation === 'auto'} onclick={() => (orientation = 'auto')}>{t('Auto')}</button>
            <button class:on={orientation === 'portrait'} onclick={() => (orientation = 'portrait')}>{t('Portrait')}</button>
            <button class:on={orientation === 'landscape'} onclick={() => (orientation = 'landscape')}>{t('Landscape')}</button>
          </div>
        </div>
        <div class="field">
          <span class="label">{t('Color')}</span>
          <div class="seg">
            <button class:on={!gray} onclick={() => (gray = false)}>{t('Color')}</button>
            <button class:on={gray} onclick={() => (gray = true)}>{t('Black & white')}</button>
          </div>
        </div>
        <div class="field">
          <span class="label">{t('Quality')}</span>
          <div class="seg">
            <button class:on={dpi === 600} onclick={() => (dpi = 600)}>{t('Standard · 600 dpi')}</button>
            <button class:on={dpi === 300} onclick={() => (dpi = 300)}>{t('Fast · 300 dpi')}</button>
          </div>
          <span class="small muted">{t('300 dpi sends a quarter of the data: quicker for photos.')}</span>
        </div>
      </fieldset>
    </div>
  </div>

  {#snippet footer()}
    <div class="grow small">
      {#if blocker}
        <span class="err">{blocker}</span>
      {:else}
        <span class="muted">{p?.name} · {inkSummary(p)}</span>
      {/if}
    </div>
    <button class="btn" onclick={close}>{phase === 'setup' ? t('Cancel') : t('Hide')}</button>
    <button class="btn primary" onclick={print} disabled={!src || !selected?.length || !!blocker || phase !== 'setup'}>
      <Printer size={17} /> {selected && selected.length * copies > 1 ? t('Print {count} pages', { count: selected.length * copies }) : t('Print')}
    </button>
  {/snippet}
</Modal>

<style>
  .layout {
    display: grid;
    grid-template-columns: 1.1fr 1fr;
    gap: 24px;
    padding: 20px;
  }
  .left {
    display: grid;
    gap: 10px;
    align-content: start;
    justify-items: center;
    background: var(--surface-3);
    border-radius: var(--radius);
    padding: 22px;
  }
  .preview {
    position: relative;
    width: 100%;
    max-width: 380px;
    max-height: 62vh;
    background: var(--paper);
    box-shadow: var(--shadow);
  }
  .preview :global(canvas.sheet) {
    display: block;
    width: 100%;
    height: 100%;
  }
  .margins {
    position: absolute;
    border: 1px dashed rgba(63, 86, 224, 0.55);
    pointer-events: none;
  }
  .loading {
    position: absolute;
    inset: 0;
    display: grid;
    place-items: center;
    color: #888;
    padding: 20px;
    text-align: center;
  }
  .pager {
    justify-content: center;
  }
  .center {
    text-align: center;
  }
  .opts {
    display: grid;
    gap: 16px;
    align-content: start;
  }
  fieldset {
    border: 0;
    margin: 0;
    padding: 0;
    display: grid;
    gap: 16px;
    min-width: 0;
  }
  fieldset:disabled {
    opacity: 0.55;
  }
  .stepper {
    display: flex;
    align-items: center;
    gap: 4px;
  }
  .stepper .input {
    width: 70px;
    text-align: center;
  }
  .progress {
    display: grid;
    gap: 10px;
    border-color: var(--accent);
  }
  .progress .btn {
    justify-self: start;
  }
  @media (max-width: 760px) {
    .layout {
      grid-template-columns: 1fr;
      padding: 14px;
      gap: 16px;
    }
    .left {
      padding: 14px;
    }
    .preview {
      max-width: 240px;
    }
  }
</style>
