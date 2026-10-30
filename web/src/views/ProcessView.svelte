<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  import FolderOpen from '@lucide/svelte/icons/folder-open';
  import Upload from '@lucide/svelte/icons/upload';
  import IdCard from '@lucide/svelte/icons/id-card';
  import FileDown from '@lucide/svelte/icons/file-down';
  import RotateCcw from '@lucide/svelte/icons/rotate-ccw';
  import RotateCw from '@lucide/svelte/icons/rotate-cw';
  import Crop from '@lucide/svelte/icons/crop';
  import ChevronLeft from '@lucide/svelte/icons/chevron-left';
  import ChevronRight from '@lucide/svelte/icons/chevron-right';
  import Trash2 from '@lucide/svelte/icons/trash-2';
  import Pencil from '@lucide/svelte/icons/pencil';
  import Layers from '@lucide/svelte/icons/layers';
  import LoaderCircle from '@lucide/svelte/icons/loader-circle';
  import ItemThumb from '../components/ItemThumb.svelte';
  import CropEditor from '../components/CropEditor.svelte';
  import CardEditor from '../components/CardEditor.svelte';
  import { addFile, download, isSupported } from '../lib/files.svelte';
  import { defaultPaper } from '../lib/layout';
  import {
    addCard,
    addFilesToWork,
    exportPdf,
    rotateRef,
    setItems,
    updateItem,
    work,
    type ExportPaper,
    type Item,
  } from '../lib/process.svelte';
  import { go, openPrint, openViewer, pickFiles, toast } from '../lib/ui.svelte';

  let paper = $state<ExportPaper>(defaultPaper());
  let dpi = $state(200);
  let exporting = $state(false);
  let progress = $state(0);
  let cropping = $state('');
  let carding = $state('');
  let dragFrom = -1;
  let input: HTMLInputElement;

  async function fromFiles() {
    const fs = await pickFiles(t('Add pages from Files'));
    if (fs.length) await addFilesToWork(fs);
  }

  async function upload(e: Event) {
    const el = e.target as HTMLInputElement;
    const added = [];
    for (const file of Array.from(el.files || [])) {
      if (!isSupported(file.type, file.name)) continue;
      try {
        added.push(await addFile({ name: file.name, blob: file, source: 'upload' }));
      } catch (err) {
        toast(t('{name}: {error}', { name: file.name, error: (err as Error).message }), 'error');
      }
    }
    el.value = '';
    if (added.length) await addFilesToWork(added);
  }

  function card() {
    carding = addCard();
  }

  function rotate(it: Item, by: 90 | -90) {
    if (it.kind === 'page') updateItem(it.id, (x) => ({ ...x, ref: rotateRef((x as typeof it).ref, by) }) as Item);
  }

  function moveItem(i: number, to: number) {
    if (to < 0 || to >= work.items.length || to === i) return;
    const items = [...work.items];
    const [x] = items.splice(i, 1);
    items.splice(to, 0, x);
    setItems(items);
  }

  function remove(id: string) {
    setItems(work.items.filter((x) => x.id !== id));
  }

  function clear() {
    if (confirm(t('Remove all pages from Process? Your files stay in Files.'))) setItems([]);
  }

  async function exportIt() {
    exporting = true;
    progress = 0;
    try {
      const f = await exportPdf(work.items, { paper, dpi, fallback: defaultPaper() }, (d, t) => (progress = d / t));
      toast(t('Saved {name} to Files', { name: f.name }), 'ok', [
        { label: t('Print'), run: () => openPrint(f) },
        { label: t('Download'), run: () => download(f.blob, f.name) },
        { label: t('Open'), run: () => openViewer(f) },
      ]);
    } catch (e) {
      toast(t('Export failed: {error}', { error: (e as Error).message }), 'error');
    } finally {
      exporting = false;
    }
  }

  const cropItem = $derived(work.items.find((x) => x.id === cropping && x.kind === 'page') as Extract<Item, { kind: 'page' }> | undefined);
</script>

<div class="head row wrap">
  <div class="grow">
    <h1>{t('Process')}</h1>
    <p class="muted small">{t('Put pages together, turn and crop them, make ID card copies, and save the result as a PDF.')}</p>
  </div>
</div>

<div class="toolbar row wrap">
  <button class="btn" onclick={fromFiles}><FolderOpen size={17} /> {t('Add from Files')}</button>
  <button class="btn" onclick={() => input.click()}><Upload size={17} /> {t('Upload')}</button>
  <button class="btn" onclick={card}><IdCard size={17} /> {t('ID card copy')}</button>
  <input bind:this={input} type="file" accept="application/pdf,image/*" multiple hidden onchange={upload} />
  <span class="grow"></span>
  {#if work.items.length}
    <button class="btn ghost danger" onclick={clear}>{t('Clear')}</button>
  {/if}
</div>

{#if !work.items.length}
  <div class="card empty">
    <Layers size={40} strokeWidth={1.4} />
    <div>
      <b>{t('Nothing here yet')}</b>
      <p class="small">{t('Add scans, pictures or PDF pages from Files, or start an ID card copy: both sides of a card on one sheet.')}</p>
    </div>
    <div class="row wrap" style="justify-content:center">
      <button class="btn primary" onclick={fromFiles}><FolderOpen size={17} /> {t('Add from Files')}</button>
      <button class="btn" onclick={card}><IdCard size={17} /> {t('ID card copy')}</button>
    </div>
  </div>
{:else}
  <ol class="grid">
    {#each work.items as it, i (it.id)}
      <li
        class="card item"
        draggable="true"
        ondragstart={() => (dragFrom = i)}
        ondragover={(e) => e.preventDefault()}
        ondrop={(e) => {
          e.preventDefault();
          moveItem(dragFrom, i);
          dragFrom = -1;
        }}
      >
        <button
          class="preview"
          onclick={() => (it.kind === 'card' ? (carding = it.id) : (cropping = it.id))}
          aria-label={t('Edit page {page}', { page: i + 1 })}
        >
          <ItemThumb item={it} />
          <span class="num">{i + 1}</span>
          {#if it.kind === 'card'}<span class="tag"><IdCard size={13} /> {t('ID card')}</span>{/if}
        </button>
        <div class="tools">
          {#if it.kind === 'page'}
            <button class="icon-btn" onclick={() => rotate(it, -90)} title={t('Rotate left')} aria-label={t('Rotate left')}><RotateCcw size={17} /></button>
            <button class="icon-btn" onclick={() => rotate(it, 90)} title={t('Rotate right')} aria-label={t('Rotate right')}><RotateCw size={17} /></button>
            <button class="icon-btn" onclick={() => (cropping = it.id)} title={t('Crop')} aria-label={t('Crop')}><Crop size={17} /></button>
          {:else}
            <button class="icon-btn" onclick={() => (carding = it.id)} title={t('Edit')} aria-label={t('Edit the card')}><Pencil size={17} /></button>
          {/if}
          <span class="grow"></span>
          <button class="icon-btn" onclick={() => moveItem(i, i - 1)} disabled={i === 0} title={t('Move earlier')} aria-label={t('Move earlier')}><ChevronLeft size={17} /></button>
          <button class="icon-btn" onclick={() => moveItem(i, i + 1)} disabled={i === work.items.length - 1} title={t('Move later')} aria-label={t('Move later')}><ChevronRight size={17} /></button>
          <button class="icon-btn" onclick={() => remove(it.id)} title={t('Remove')} aria-label={t('Remove')}><Trash2 size={17} /></button>
        </div>
      </li>
    {/each}
  </ol>

  <section class="card card-pad export">
    <div class="field">
      <span class="label">{t('Page size')}</span>
      <div class="seg">
        <button class:on={paper === 'a4'} onclick={() => (paper = 'a4')}>{t('A4')}</button>
        <button class:on={paper === 'letter'} onclick={() => (paper = 'letter')}>{t('Letter')}</button>
        <button class:on={paper === 'original'} onclick={() => (paper = 'original')}>{t('As cropped')}</button>
      </div>
    </div>
    <div class="field">
      <span class="label">{t('Quality')}</span>
      <div class="seg">
        <button class:on={dpi === 150} onclick={() => (dpi = 150)}>{t('Small')}</button>
        <button class:on={dpi === 200} onclick={() => (dpi = 200)}>{t('Standard')}</button>
        <button class:on={dpi === 300} onclick={() => (dpi = 300)}>{t('Best')}</button>
      </div>
    </div>
    <div class="go">
      {#if exporting}
        <div class="bar"><div style:width={`${Math.max(3, progress * 100)}%`}></div></div>
      {/if}
      <button class="btn primary big" onclick={exportIt} disabled={exporting}>
        {#if exporting}<LoaderCircle size={18} class="spin" />{:else}<FileDown size={18} />{/if}
        {t('Save as PDF')}
      </button>
      <span class="small muted"><a href="#/files" onclick={() => go('files')}>{t('Saved to Files, ready to print or download.')}</a></span>
    </div>
  </section>
{/if}

{#if cropItem}
  <CropEditor
    ref={cropItem.ref}
    onsave={(r) => updateItem(cropItem.id, (x) => ({ ...x, ref: r }) as Item)}
    onclose={() => (cropping = '')}
  />
{/if}
{#if carding}
  <CardEditor id={carding} onclose={() => (carding = '')} />
{/if}

<style>
  .head {
    margin-bottom: 14px;
  }
  .toolbar {
    margin-bottom: 18px;
  }
  .grid {
    list-style: none;
    margin: 0 0 20px;
    padding: 0;
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(190px, 1fr));
    gap: 16px;
  }
  .item {
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }
  .preview {
    position: relative;
    aspect-ratio: 3 / 4;
    padding: 14px;
    border: 0;
    background: var(--surface-3);
    cursor: pointer;
  }
  .num {
    position: absolute;
    left: 8px;
    top: 8px;
    min-width: 24px;
    height: 24px;
    padding: 0 6px;
    border-radius: 12px;
    background: var(--surface);
    box-shadow: var(--shadow);
    font-size: 0.8rem;
    font-weight: 700;
    display: grid;
    place-items: center;
  }
  .tag {
    position: absolute;
    right: 8px;
    top: 8px;
    display: inline-flex;
    align-items: center;
    gap: 4px;
    padding: 2px 8px;
    border-radius: 12px;
    background: var(--accent);
    color: var(--accent-text);
    font-size: 0.72rem;
    font-weight: 700;
  }
  .tools {
    display: flex;
    align-items: center;
    padding: 4px;
    gap: 0;
  }
  .tools .icon-btn {
    width: 32px;
    height: 32px;
  }
  .export {
    display: flex;
    flex-wrap: wrap;
    gap: 22px;
    align-items: flex-end;
    position: sticky;
    bottom: 12px;
    box-shadow: var(--shadow-lg);
  }
  .go {
    margin-left: auto;
    display: grid;
    gap: 6px;
    justify-items: end;
  }
  .go .bar {
    width: 100%;
  }
  @media (max-width: 760px) {
    .head h1 {
      display: none;
    }
    .grid {
      grid-template-columns: repeat(2, 1fr);
      gap: 10px;
    }
    .export {
      gap: 14px;
      bottom: 6px;
    }
    .go {
      margin-left: 0;
      width: 100%;
      justify-items: stretch;
    }
  }
</style>
