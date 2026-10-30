<script lang="ts">
  import { t, i18n } from '../lib/i18n.svelte';
  import { onDestroy } from 'svelte';
  import Printer from '@lucide/svelte/icons/printer';
  import Download from '@lucide/svelte/icons/download';
  import Layers from '@lucide/svelte/icons/layers';
  import Trash2 from '@lucide/svelte/icons/trash-2';
  import LoaderCircle from '@lucide/svelte/icons/loader-circle';
  import Modal from './Modal.svelte';
  import { download, fileSource, formatSize, removeFile, type StoredFile } from '../lib/files.svelte';
  import { addFilesToWork } from '../lib/process.svelte';
  import { renderPage } from '../lib/source';
  import { go, openPrint, toast } from '../lib/ui.svelte';

  let { file, onclose }: { file: StoredFile; onclose: () => void } = $props();

  // svelte-ignore state_referenced_locally
  const isPdf = file.type === 'application/pdf';
  // svelte-ignore state_referenced_locally
  const url = isPdf ? '' : URL.createObjectURL(file.blob);
  let pagesEl = $state<HTMLDivElement>();
  let error = $state('');
  let loading = $state(isPdf);
  let cancelled = false;

  onDestroy(() => {
    cancelled = true;
    if (url) URL.revokeObjectURL(url);
  });

  $effect(() => {
    if (!isPdf || !pagesEl) return;
    const el = pagesEl;
    (async () => {
      try {
        const src = await fileSource(file);
        const width = Math.min(1600, (el.clientWidth || 600) * Math.min(2, devicePixelRatio || 1));
        for (let i = 0; i < src.pages.length && !cancelled; i++) {
          const c = await renderPage(src, i, 0, width / src.pages[i].w);
          c.className = 'page';
          el.appendChild(c);
          loading = false;
        }
      } catch (e) {
        error = (e as Error).message;
        loading = false;
      }
    })();
  });

  function print() {
    onclose();
    openPrint(file);
  }

  async function toWork() {
    const n = await addFilesToWork([file]);
    toast(n === 1 ? t('{count} page added to Process', { count: n }) : t('{count} pages added to Process', { count: n }), 'ok', [{ label: t('Open'), run: () => go('process') }]);
  }

  async function del() {
    if (!confirm(t('Delete "{name}" from this browser?', { name: file.name }))) return;
    await removeFile(file.id);
    onclose();
  }
</script>

<Modal title={file.name} {onclose} wide>
  <div class="view" lang={i18n.locale}>
    {#if isPdf}
      <div class="pages" bind:this={pagesEl}></div>
      {#if loading}<div class="center"><LoaderCircle class="spin" /></div>{/if}
    {:else}
      <img src={url} alt={file.name} />
    {/if}
    {#if error}<p class="err center">{error}</p>{/if}
  </div>
  {#snippet footer()}
    <span class="grow small muted">
      {formatSize(file.size)}{file.dpi ? ` · ${file.dpi} dpi` : ''}{file.width && !isPdf ? ` · ${file.width} × ${file.height} px` : ''}
    </span>
    <button class="btn ghost danger" onclick={del} aria-label={t('Delete')}><Trash2 size={17} /></button>
    <button class="btn" onclick={toWork}><Layers size={17} /> {t('Process')}</button>
    <button class="btn" onclick={() => download(file.blob, file.name)}><Download size={17} /> {t('Download')}</button>
    <button class="btn primary" onclick={print}><Printer size={17} /> {t('Print')}</button>
  {/snippet}
</Modal>

<style>
  .view {
    background: var(--surface-3);
    padding: 20px;
    min-height: 100%;
  }
  img {
    display: block;
    max-width: 100%;
    margin: 0 auto;
    background: var(--paper);
    box-shadow: var(--shadow);
  }
  .pages {
    display: grid;
    gap: 16px;
    justify-items: center;
  }
  .pages :global(canvas.page) {
    width: 100%;
    max-width: 800px;
    height: auto;
    background: var(--paper);
    box-shadow: var(--shadow);
  }
  .center {
    display: grid;
    place-items: center;
    padding: 30px;
  }
  @media (max-width: 760px) {
    .view {
      padding: 10px;
    }
  }
</style>
