<script lang="ts">
  import ScanLine from '@lucide/svelte/icons/scan-line';
  import CircleStop from '@lucide/svelte/icons/circle-stop';
  import Printer from '@lucide/svelte/icons/printer';
  import Download from '@lucide/svelte/icons/download';
  import Layers from '@lucide/svelte/icons/layers';
  import LoaderCircle from '@lucide/svelte/icons/loader-circle';
  import { device } from '../lib/device.svelte';
  import { download, files, formatSize, thumbUrl } from '../lib/files.svelte';
  import { addFilesToWork } from '../lib/process.svelte';
  import {
    cancelScan,
    scanProgress,
    scanUi,
    sourceLabel,
    startKept,
    startStream,
    type ScanPaper,
  } from '../lib/scan.svelte';
  import { go, openPrint, openViewer, toast } from '../lib/ui.svelte';
  import { defaultPaper } from '../lib/layout';

  const saved = (() => {
    try {
      return JSON.parse(localStorage.getItem('scanSettings.v2') || '{}');
    } catch {
      return {};
    }
  })();
  let paper = $state<ScanPaper>(saved.paper || defaultPaper());
  let dpi = $state<number>(saved.dpi || 150);
  let starting = $state(false);

  $effect(() => {
    try {
      localStorage.setItem('scanSettings.v2', JSON.stringify({ paper, dpi }));
    } catch {
      /* private mode */
    }
  });

  const st = $derived(device.scan);
  const scanning = $derived(st?.state === 'scanning' || scanUi.streaming);
  const progress = $derived(scanProgress(st));
  const recent = $derived(files.list.filter((f) => f.source === 'scan').slice(0, 8));
  const aspect = $derived(paper === 'card' ? 2400 / 1700 : paper === 'letter' ? 8.5 / 11 : 210 / 297);
  const printerBusy = $derived(device.printer?.state === 'processing');

  const statusText = $derived.by(() => {
    if (!st) return '';
    if (scanning) {
      const who = st.source && st.source !== 'web' && st.source !== 'stream' ? ` · started from ${sourceLabel(st.source)}` : '';
      if (!st.lines) return `Warming up and calibrating…${who}`;
      return `Scanning ${Math.round(progress * 100)}% · ${formatSize(scanUi.received || st.bytes)}${who}`;
    }
    if (st.state === 'failed') return `Scan failed: ${st.error}`;
    if (st.state === 'canceled') return 'Canceled';
    if (st.state === 'done') return `Done: ${st.width} × ${st.height} pixels at ${st.dpi} dpi in ${Math.round(st.elapsed)} s`;
    return '';
  });

  async function scan() {
    starting = true;
    try {
      if (dpi >= 600) {
        const p = startStream(paper, dpi);
        starting = false;
        await p;
      } else {
        await startKept(paper, dpi);
      }
    } catch (e) {
      toast((e as Error).message, 'error');
    } finally {
      starting = false;
    }
  }
</script>

<div class="layout">
  <section class="card card-pad settings">
    <h2>Scan</h2>
    <div class="field">
      <span class="label">Original</span>
      <div class="seg">
        <button class:on={paper === 'a4'} onclick={() => (paper = 'a4')} disabled={scanning}>A4</button>
        <button class:on={paper === 'letter'} onclick={() => (paper = 'letter')} disabled={scanning}>Letter</button>
        <button class:on={paper === 'card'} onclick={() => (paper = 'card')} disabled={scanning}>ID card</button>
      </div>
      {#if paper === 'card'}
        <p class="muted small">Put the card in the corner of the glass, by the arrow. Only that corner is scanned, which is quick.</p>
      {/if}
    </div>
    <div class="field">
      <span class="label">Resolution</span>
      <div class="seg">
        {#each [150, 300, 600] as d}
          <button class:on={dpi === d} onclick={() => (dpi = d)} disabled={scanning}>{d} dpi</button>
        {/each}
      </div>
      <p class="muted small">
        {#if dpi === 150}Quick, for reading on screen.{:else if dpi === 300}Documents and copies.{:else}Photos. Slow, and only this browser receives it: keep this page open.{/if}
      </p>
    </div>
    <div class="go">
      {#if scanning}
        <button class="btn big danger" onclick={cancelScan}><CircleStop size={18} /> Cancel</button>
      {:else}
        <button
          class="btn primary big"
          onclick={scan}
          disabled={starting || !device.online || !st?.scanner || printerBusy}
        >
          {#if starting}<LoaderCircle size={18} class="spin" />{:else}<ScanLine size={18} />{/if} Scan
        </button>
      {/if}
    </div>
    {#if device.loaded && !st?.scanner}
      <p class="err small">No scanner: is the printer on and connected?</p>
    {:else if printerBusy}
      <p class="muted small">The printer is printing; scan when it is done.</p>
    {/if}
    {#if st?.button}
      <p class="muted small hint">
        The button on the printer box scans too ({st.button.paper === 'letter' ? 'Letter' : 'A4'}, {st.button.dpi} dpi).
        Every open page receives the scan.
      </p>
    {/if}
  </section>

  <section class="card preview-card">
    <div class="paper" style:aspect-ratio={aspect} style:width={`min(100%, 520px, calc(70vh * ${aspect}))`}>
      {#if scanUi.preview && (scanning || st?.state === 'done' || scanUi.lastFile)}
        <img src={scanUi.preview} alt="Scanned page" />
      {:else}
        <div class="placeholder muted">
          <ScanLine size={36} strokeWidth={1.4} />
          <span>{scanning ? 'Warming up…' : 'The page appears here while it is scanned'}</span>
        </div>
      {/if}
      {#if scanning}
        <div class="beam" style:top={`${progress * 100}%`}></div>
      {/if}
    </div>
    <div class="status">
      {#if scanning}
        <div class="bar"><div style:width={`${Math.max(2, progress * 100)}%`}></div></div>
      {/if}
      <p class:err={st?.state === 'failed'} class="small">{statusText}</p>
      {#if scanUi.error}<p class="err small">{scanUi.error}</p>{/if}
      {#if scanUi.lastFile && !scanning}
        {@const f = scanUi.lastFile}
        <div class="row wrap actions">
          <button class="btn primary" onclick={() => openPrint(f)}><Printer size={17} /> Print a copy</button>
          <button
            class="btn"
            onclick={async () => {
              await addFilesToWork([f]);
              toast('Added to Process', 'ok', [{ label: 'Open', run: () => go('process') }]);
            }}><Layers size={17} /> Process</button
          >
          <button class="btn ghost" onclick={() => download(f.blob, f.name)}><Download size={17} /> Download</button>
        </div>
      {/if}
    </div>
  </section>

  {#if recent.length}
    <section class="recent">
      <div class="row"><h2 class="grow">Recent scans</h2><a href="#/files" class="small">All files</a></div>
      <div class="strip">
        {#each recent as f (f.id)}
          <button class="thumb" onclick={() => openViewer(f)} title={f.name}>
            <img src={thumbUrl(f)} alt={f.name} loading="lazy" />
          </button>
        {/each}
      </div>
    </section>
  {/if}
</div>

<style>
  .layout {
    display: grid;
    grid-template-columns: minmax(280px, 360px) 1fr;
    gap: 18px;
    align-items: start;
  }
  .settings {
    display: grid;
    gap: 18px;
  }
  .settings h2 {
    margin-bottom: -4px;
  }
  .go .btn {
    width: 100%;
  }
  .hint {
    border-top: 1px solid var(--border);
    padding-top: 14px;
  }
  .preview-card {
    padding: 20px;
    display: grid;
    gap: 14px;
    justify-items: center;
  }
  .paper {
    position: relative;
    background: var(--paper);
    border-radius: 6px;
    box-shadow:
      0 0 0 1px var(--border),
      var(--shadow);
    overflow: hidden;
    display: grid;
  }
  .paper img {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    object-fit: contain;
  }
  .placeholder {
    display: grid;
    place-items: center;
    align-content: center;
    gap: 10px;
    text-align: center;
    padding: 20px;
    color: #8a909c;
  }
  .beam {
    position: absolute;
    left: 0;
    right: 0;
    height: 3px;
    background: linear-gradient(90deg, transparent, var(--accent), transparent);
    box-shadow: 0 0 14px var(--accent);
    transition: top 0.9s linear;
  }
  .status {
    width: min(100%, 520px);
    display: grid;
    gap: 8px;
  }
  .actions {
    margin-top: 4px;
  }
  .recent {
    grid-column: 1 / -1;
  }
  .strip {
    display: flex;
    gap: 10px;
    overflow-x: auto;
    padding: 10px 2px 4px;
  }
  .thumb {
    flex: none;
    width: 96px;
    height: 128px;
    padding: 0;
    border: 1px solid var(--border);
    border-radius: 8px;
    background: var(--paper);
    overflow: hidden;
    cursor: pointer;
  }
  .thumb img {
    width: 100%;
    height: 100%;
    object-fit: contain;
  }
  @media (max-width: 900px) {
    .layout {
      grid-template-columns: 1fr;
    }
  }
</style>
