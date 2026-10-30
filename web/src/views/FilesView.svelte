<script lang="ts">
  import { t, i18n } from '../lib/i18n.svelte';
  import Upload from '@lucide/svelte/icons/upload';
  import Printer from '@lucide/svelte/icons/printer';
  import Download from '@lucide/svelte/icons/download';
  import Trash2 from '@lucide/svelte/icons/trash-2';
  import Layers from '@lucide/svelte/icons/layers';
  import Pencil from '@lucide/svelte/icons/pencil';
  import Ellipsis from '@lucide/svelte/icons/ellipsis';
  import Inbox from '@lucide/svelte/icons/inbox';
  import LoaderCircle from '@lucide/svelte/icons/loader-circle';
  import SquareCheck from '@lucide/svelte/icons/square-check';
  import FileDown from '@lucide/svelte/icons/file-down';
  import X from '@lucide/svelte/icons/x';
  import Check from '@lucide/svelte/icons/check';
  import {
    addFile,
    download,
    files,
    formatSize,
    isSupported,
    removeFile,
    renameFile,
    thumbUrl,
    type StoredFile,
  } from '../lib/files.svelte';
  import { addFilesToWork, exportPdf, pageItems } from '../lib/process.svelte';
  import { go, openPrint, openViewer, toast } from '../lib/ui.svelte';

  let filter = $state<'all' | 'scan' | 'upload' | 'process'>('all');
  let menu = $state('');
  let uploading = $state(0);
  let dragging = $state(false);
  let input: HTMLInputElement;

  const shown = $derived(filter === 'all' ? files.list : files.list.filter((f) => f.source === filter));

  // bulk actions: files are numbered in the order they were picked, which is
  // the order of the pages in the PDF
  let selecting = $state(false);
  let selected = $state<string[]>([]);
  let busy = $state(false);
  let progress = $state(0);
  const chosen = $derived(
    selected.map((id) => files.list.find((f) => f.id === id)).filter((f): f is StoredFile => !!f),
  );

  function toggle(f: StoredFile) {
    selected = selected.includes(f.id) ? selected.filter((x) => x !== f.id) : [...selected, f.id];
  }

  // grouped by day, newest first, as Google Photos does
  const groups = $derived.by(() => {
    const locale = i18n.locale;
    const out: { key: string; label: string; files: StoredFile[] }[] = [];
    for (const f of shown) {
      const d = new Date(f.created);
      const key = d.toDateString();
      if (out.at(-1)?.key !== key) out.push({ key, label: dayLabel(d, locale), files: [] });
      out.at(-1)!.files.push(f);
    }
    return out;
  });

  function dayLabel(d: Date, locale: string): string {
    const today = new Date();
    const yesterday = new Date(today.getFullYear(), today.getMonth(), today.getDate() - 1);
    if (d.toDateString() === today.toDateString()) return t('Today');
    if (d.toDateString() === yesterday.toDateString()) return t('Yesterday');
    return d.toLocaleDateString(locale, {
      weekday: 'short',
      day: 'numeric',
      month: 'short',
      year: d.getFullYear() === today.getFullYear() ? undefined : 'numeric',
    });
  }

  // the day's check: picks all its files (or drops them when all are picked)
  function toggleDay(fs: StoredFile[]) {
    selecting = true;
    const ids = fs.map((f) => f.id);
    selected = ids.every((id) => selected.includes(id))
      ? selected.filter((id) => !ids.includes(id))
      : [...selected, ...ids.filter((id) => !selected.includes(id))];
  }

  function stopSelecting() {
    selecting = false;
    selected = [];
  }

  function selectAll() {
    const ids = shown.map((f) => f.id).filter((id) => !selected.includes(id));
    selected = ids.length ? [...selected, ...ids] : [];
  }

  async function bulkPdf() {
    busy = true;
    progress = 0;
    try {
      const f = await exportPdf(pageItems(chosen), { paper: 'original', dpi: 200, fallback: 'a4' }, (d, t) => (progress = d / t));
      stopSelecting();
      toast(t('Saved {name} to Files', { name: f.name }), 'ok', [
        { label: t('Print'), run: () => openPrint(f) },
        { label: t('Download'), run: () => download(f.blob, f.name) },
      ]);
    } catch (e) {
      toast(t('Could not make the PDF: {error}', { error: (e as Error).message }), 'error');
    } finally {
      busy = false;
    }
  }

  async function bulkDelete() {
    const n = chosen.length;
    if (!confirm(n === 1 ? t('Delete {count} file from this browser?', { count: n }) : t('Delete {count} files from this browser?', { count: n }))) return;
    busy = true;
    try {
      for (const f of chosen) await removeFile(f.id);
      toast(n === 1 ? t('Deleted {count} file', { count: n }) : t('Deleted {count} files', { count: n }));
      stopSelecting();
    } finally {
      busy = false;
    }
  }
  const total = $derived(files.list.reduce((n, f) => n + f.size, 0));

  async function addAll(list: FileList | File[]) {
    const arr = Array.from(list);
    for (const file of arr) {
      if (!isSupported(file.type, file.name)) {
        toast(t('{name}: only PDFs and images', { name: file.name }), 'error');
        continue;
      }
      uploading++;
      try {
        await addFile({ name: file.name, blob: file, source: 'upload' });
      } catch (e) {
        toast(t('{name}: {error}', { name: file.name, error: (e as Error).message }), 'error');
      } finally {
        uploading--;
      }
    }
  }

  function onPick(e: Event) {
    const el = e.target as HTMLInputElement;
    if (el.files) addAll(el.files);
    el.value = '';
  }

  function onDrop(e: DragEvent) {
    e.preventDefault();
    dragging = false;
    if (e.dataTransfer?.files.length) addAll(e.dataTransfer.files);
  }

  async function del(f: StoredFile) {
    if (!confirm(t('Delete "{name}" from this browser?', { name: f.name }))) return;
    await removeFile(f.id);
  }

  async function rename(f: StoredFile) {
    const name = prompt(t('New name'), f.name);
    if (name) await renameFile(f.id, name);
  }

  async function toWork(f: StoredFile) {
    const n = await addFilesToWork([f]);
    toast(n === 1 ? t('{count} page added to Process', { count: n }) : t('{count} pages added to Process', { count: n }), 'ok', [{ label: t('Open'), run: () => go('process') }]);
  }

  // the day is in the group's heading
  function when(t: number): string {
    return new Date(t).toLocaleTimeString(i18n.locale, { hour: '2-digit', minute: '2-digit' });
  }

  function kind(f: StoredFile): string {
    if (f.type === 'application/pdf') return f.pages === 1 ? t('PDF · {count} page', { count: f.pages ?? 0 }) : t('PDF · {count} pages', { count: f.pages ?? 0 });
    return f.dpi ? (f.source === 'scan' ? t('Scan · {dpi} dpi', { dpi: f.dpi }) : t('Image · {dpi} dpi', { dpi: f.dpi })) : t('Image');
  }
</script>

<svelte:window onclick={() => (menu = '')} />

<div
  class="drop"
  class:dragging
  role="region"
  aria-label={t('Files')}
  ondragover={(e) => {
    e.preventDefault();
    dragging = true;
  }}
  ondragleave={(e) => e.currentTarget === e.target && (dragging = false)}
  ondrop={onDrop}
>
  <div class="head row wrap">
    <div class="grow">
      <h1>{t('Files')}</h1>
      <p class="muted small">{files.list.length === 1 ? t('Kept in this browser only · {count} file · {size}', { count: files.list.length, size: formatSize(total) }) : t('Kept in this browser only · {count} files · {size}', { count: files.list.length, size: formatSize(total) })}</p>
    </div>
    {#if files.list.length}
      <button class="btn" class:on={selecting} onclick={() => (selecting ? stopSelecting() : (selecting = true))}>
        {#if selecting}<X size={17} /> {t('Done')}{:else}<SquareCheck size={17} /> {t('Select')}{/if}
      </button>
    {/if}
    <button class="btn primary" onclick={() => input.click()}>
      {#if uploading}<LoaderCircle size={17} class="spin" />{:else}<Upload size={17} />{/if} {t('Upload')}
    </button>
    <input bind:this={input} type="file" accept="application/pdf,image/*" multiple hidden onchange={onPick} />
  </div>

  <div class="seg filters">
    {#each [['all', t('All')], ['scan', t('Scans')], ['upload', t('Uploads')], ['process', t('Made here')]] as [k, label]}
      <button class:on={filter === k} onclick={() => (filter = k as typeof filter)}>{label}</button>
    {/each}
  </div>

  {#if files.error}
    <p class="err">{t('Cannot open the file store: {message}', { message: files.error.message })}</p>
  {:else if files.loaded && !shown.length}
    <div class="card empty">
      <Inbox size={40} strokeWidth={1.4} />
      <div>
        <b>{t('No files yet')}</b>
        <p class="small">{t('Upload PDFs or pictures (or drop them here), or scan a page: it shows up here.')}</p>
      </div>
      <button class="btn" onclick={() => input.click()}><Upload size={17} /> {t('Upload')}</button>
    </div>
  {:else}
    {#each groups as g (g.key)}
    {@const all = g.files.every((f) => selected.includes(f.id))}
    <section class="day">
    <div class="day-head">
      <button
        class="day-check"
        class:on={selecting && all}
        class:show={selecting}
        onclick={() => toggleDay(g.files)}
        aria-label={all && selecting ? t('Unselect all of {day}', { day: g.label }) : t('Select all of {day}', { day: g.label })}
        title={t('Select the whole day')}><Check size={14} /></button
      >
      <h2>{g.label} <span class="muted small">{g.files.length}</span></h2>
    </div>
    <ul class="grid" class:selecting>
      {#each g.files as f (f.id)}
        {@const n = selected.indexOf(f.id) + 1}
        <li class="card tile" class:picked={n > 0}>
          <button
            class="thumb"
            onclick={() => (selecting ? toggle(f) : openViewer(f))}
            aria-label={selecting ? (n ? t('Unselect {name}', { name: f.name }) : t('Select {name}', { name: f.name })) : t('Open {name}', { name: f.name })}
            aria-pressed={selecting ? n > 0 : undefined}
          >
            {#if f.thumb}<img src={thumbUrl(f)} alt="" loading="lazy" />{/if}
            {#if f.type === 'application/pdf'}<span class="badge">{t('PDF')}</span>{/if}
            {#if selecting}<span class="pick">{n || ''}</span>{/if}
          </button>
          <div class="meta">
            <div class="row">
              <div class="grow">
                <div class="name ellipsis" title={f.name}>{f.name}</div>
                <div class="muted small ellipsis">{kind(f)} · {formatSize(f.size)} · {when(f.created)}</div>
              </div>
              <div class="more">
                <button
                  class="icon-btn"
                  aria-label={t('More actions')}
                  onclick={(e) => {
                    e.stopPropagation();
                    menu = menu === f.id ? '' : f.id;
                  }}><Ellipsis size={18} /></button
                >
                {#if menu === f.id}
                  <div class="menu card" role="menu">
                    <button onclick={() => toWork(f)}><Layers size={16} /> {t('Add to Process')}</button>
                    <button onclick={() => rename(f)}><Pencil size={16} /> {t('Rename')}</button>
                    <button class="danger" onclick={() => del(f)}><Trash2 size={16} /> {t('Delete')}</button>
                  </div>
                {/if}
              </div>
            </div>
            {#if !selecting}
            <div class="row btns">
              <button class="btn primary grow" onclick={() => openPrint(f)}><Printer size={16} /> {t('Print')}</button>
              <button class="btn" onclick={() => download(f.blob, f.name)} aria-label={t('Download')}><Download size={16} /></button>
            </div>
            {/if}
          </div>
        </li>
      {/each}
    </ul>
    </section>
    {/each}
  {/if}
  {#if dragging}
    <div class="overlay"><Upload size={36} /> {t('Drop to add')}</div>
  {/if}
  {#if selecting}
    <div class="bulk card">
      <div class="grow">
        <b>{t('{count} selected', { count: chosen.length })}</b>
        <div class="muted small">
          {chosen.length ? t('The PDF keeps the order you picked them in.') : t('Tap files to pick them.')}
        </div>
        {#if busy && progress}<div class="bar"><div style:width={`${progress * 100}%`}></div></div>{/if}
      </div>
      <button class="btn ghost" onclick={selectAll} disabled={busy}>
        {selected.length && shown.every((f) => selected.includes(f.id)) ? t('None') : t('All')}
      </button>
      <button class="btn danger" onclick={bulkDelete} disabled={!chosen.length || busy}><Trash2 size={17} /> {t('Delete')}</button>
      <button class="btn primary" onclick={bulkPdf} disabled={!chosen.length || busy}>
        {#if busy}<LoaderCircle size={17} class="spin" />{:else}<FileDown size={17} />{/if} {t('Save as PDF')}
      </button>
    </div>
  {/if}
</div>

<style>
  .drop {
    position: relative;
    min-height: 60vh;
  }
  .head {
    margin-bottom: 16px;
  }
  .filters {
    margin-bottom: 18px;
  }
  .grid {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(210px, 1fr));
    gap: 16px;
  }
  .tile {
    display: flex;
    flex-direction: column;
    overflow: visible;
  }
  .thumb {
    position: relative;
    display: block;
    aspect-ratio: 4 / 3.6;
    border: 0;
    padding: 12px;
    background: var(--surface-3);
    border-radius: var(--radius) var(--radius) 0 0;
    cursor: pointer;
    overflow: hidden;
  }
  .thumb img {
    width: 100%;
    height: 100%;
    object-fit: contain;
    filter: drop-shadow(0 1px 4px rgba(0, 0, 0, 0.18));
  }
  .badge {
    position: absolute;
    top: 10px;
    left: 10px;
    background: var(--err);
    color: #fff;
    font-size: 0.7rem;
    font-weight: 700;
    padding: 2px 6px;
    border-radius: 5px;
  }
  .meta {
    padding: 10px 12px 12px;
    display: grid;
    grid-template-columns: minmax(0, 1fr);
    gap: 10px;
  }
  .name {
    font-weight: 600;
  }
  .btns .btn {
    min-height: 34px;
  }
  .more {
    position: relative;
  }
  .menu {
    position: absolute;
    right: 0;
    top: 38px;
    z-index: 10;
    display: grid;
    padding: 6px;
    min-width: 180px;
    box-shadow: var(--shadow-lg);
  }
  .menu button {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 8px 10px;
    border: 0;
    background: none;
    border-radius: 7px;
    cursor: pointer;
    text-align: left;
  }
  .menu button:hover {
    background: var(--surface-3);
  }
  .menu .danger {
    color: var(--err);
  }
  .day + .day {
    margin-top: 26px;
  }
  .day-head {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 12px;
  }
  .day-head h2 {
    font-size: 0.95rem;
  }
  .day-check {
    display: grid;
    place-items: center;
    width: 22px;
    height: 22px;
    padding: 0;
    border-radius: 50%;
    border: 2px solid var(--muted);
    background: transparent;
    color: transparent;
    cursor: pointer;
    opacity: 0.55;
  }
  .day-head:hover .day-check,
  .day-check.show {
    opacity: 1;
  }
  .day-check.on {
    background: var(--accent);
    border-color: var(--accent);
    color: var(--accent-text);
    opacity: 1;
  }
  .pick {
    position: absolute;
    top: 8px;
    right: 8px;
    display: grid;
    place-items: center;
    width: 26px;
    height: 26px;
    border-radius: 50%;
    border: 2px solid var(--muted);
    background: var(--surface);
    font-size: 0.8rem;
    font-weight: 700;
  }
  .picked .pick {
    border-color: var(--accent);
    background: var(--accent);
    color: var(--accent-text);
  }
  .tile.picked {
    outline: 2px solid var(--accent);
    outline-offset: -1px;
  }
  .bulk {
    position: sticky;
    bottom: 12px;
    z-index: 6;
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 10px;
    margin-top: 18px;
    padding: 12px 14px;
    box-shadow: var(--shadow-lg);
  }
  .bulk .bar {
    margin-top: 6px;
  }
  .btn.on {
    background: var(--accent-soft);
    border-color: var(--accent);
  }
  .overlay {
    position: absolute;
    inset: 0;
    display: grid;
    place-items: center;
    align-content: center;
    gap: 10px;
    border: 2px dashed var(--accent);
    border-radius: var(--radius);
    background: color-mix(in srgb, var(--accent-soft) 80%, var(--bg));
    color: var(--accent);
    font-weight: 600;
    pointer-events: none;
  }
  @media (max-width: 760px) {
    .head h1 {
      display: none;
    }
    .grid {
      grid-template-columns: repeat(auto-fill, minmax(150px, 1fr));
      gap: 12px;
    }
  }
</style>
