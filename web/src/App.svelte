<script lang="ts">
  import { onMount } from 'svelte';
  import ArrowLeft from '@lucide/svelte/icons/arrow-left';
  import Sidebar from './components/Sidebar.svelte';
  import Toasts from './components/Toasts.svelte';
  import PrintDialog from './components/PrintDialog.svelte';
  import Viewer from './components/Viewer.svelte';
  import FilePicker from './components/FilePicker.svelte';
  import PrinterView from './views/PrinterView.svelte';
  import ScannerView from './views/ScannerView.svelte';
  import FilesView from './views/FilesView.svelte';
  import ProcessView from './views/ProcessView.svelte';
  import { connect, device } from './lib/device.svelte';
  import { loadFiles } from './lib/files.svelte';
  import { loadWork } from './lib/process.svelte';
  import { initScanSync } from './lib/scan.svelte';
  import { ui } from './lib/ui.svelte';

  const ROUTES = ['printer', 'scanner', 'files', 'process'] as const;
  type Route = (typeof ROUTES)[number];
  const TITLES: Record<Route, string> = { printer: 'Printer', scanner: 'Scanner', files: 'Files', process: 'Process' };

  let hash = $state(location.hash);
  let narrow = $state(false);

  const route = $derived.by((): Route | null => {
    const r = hash.replace(/^#\/?/, '').split('/')[0];
    return (ROUTES as readonly string[]).includes(r) ? (r as Route) : null;
  });
  // phones open on the menu; wider screens on the printer
  const current = $derived<Route | null>(route ?? (narrow ? null : 'printer'));

  onMount(() => {
    const mq = matchMedia('(max-width: 760px)');
    narrow = mq.matches;
    mq.addEventListener('change', (e) => (narrow = e.matches));
    const onHash = () => (hash = location.hash);
    addEventListener('hashchange', onHash);
    initScanSync();
    connect();
    loadFiles();
    loadWork();
    return () => removeEventListener('hashchange', onHash);
  });

  $effect(() => {
    const name = device.printer?.name;
    document.title = current ? `${TITLES[current]} · ${name || 'Printer'}` : name || 'Printer';
  });
</script>

<div class="app" class:home={narrow && !current}>
  <Sidebar current={current ?? ''} home={narrow && !current} />
  {#if current}
    <main>
      {#if narrow}
        <header class="topbar">
          <a class="icon-btn" href="#/" aria-label="Back to the menu"><ArrowLeft size={20} /></a>
          <h1>{TITLES[current]}</h1>
        </header>
      {/if}
      <div class="content">
        {#if current === 'printer'}
          <PrinterView />
        {:else if current === 'scanner'}
          <ScannerView />
        {:else if current === 'files'}
          <FilesView />
        {:else}
          <ProcessView />
        {/if}
      </div>
    </main>
  {/if}
</div>

{#if ui.printFile}
  <PrintDialog file={ui.printFile} onclose={() => (ui.printFile = null)} />
{/if}
{#if ui.viewFile}
  <Viewer file={ui.viewFile} onclose={() => (ui.viewFile = null)} />
{/if}
{#if ui.picker}
  <FilePicker {...ui.picker} onclose={() => (ui.picker = null)} />
{/if}
<Toasts />

<style>
  .app {
    display: grid;
    grid-template-columns: var(--sidebar) 1fr;
    height: 100%;
  }
  main {
    min-width: 0;
    height: 100%;
    overflow: auto;
    display: flex;
    flex-direction: column;
  }
  .content {
    width: 100%;
    max-width: 1180px;
    margin: 0 auto;
    padding: 28px 32px 48px;
    flex: 1;
  }
  .topbar {
    position: sticky;
    top: 0;
    z-index: 5;
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 8px 10px;
    padding-top: max(8px, env(safe-area-inset-top));
    background: color-mix(in srgb, var(--bg) 88%, transparent);
    backdrop-filter: blur(10px);
    -webkit-backdrop-filter: blur(10px);
    border-bottom: 1px solid var(--border);
  }
  .topbar h1 {
    font-size: 1.1rem;
  }
  @media (max-width: 760px) {
    .app {
      grid-template-columns: 1fr;
    }
    .app:not(.home) :global(.sidebar) {
      display: none;
    }
    .content {
      padding: 16px 14px 40px;
      padding-bottom: max(40px, env(safe-area-inset-bottom));
    }
  }
</style>
