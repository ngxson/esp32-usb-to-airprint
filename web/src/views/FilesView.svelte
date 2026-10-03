<script lang="ts">
  import Upload from '@lucide/svelte/icons/upload';
  import Printer from '@lucide/svelte/icons/printer';
  import Download from '@lucide/svelte/icons/download';
  import Trash2 from '@lucide/svelte/icons/trash-2';
  import Layers from '@lucide/svelte/icons/layers';
  import Pencil from '@lucide/svelte/icons/pencil';
  import Ellipsis from '@lucide/svelte/icons/ellipsis';
  import Inbox from '@lucide/svelte/icons/inbox';
  import LoaderCircle from '@lucide/svelte/icons/loader-circle';
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
  import { addFilesToWork } from '../lib/process.svelte';
  import { go, openPrint, openViewer, toast } from '../lib/ui.svelte';

  let filter = $state<'all' | 'scan' | 'upload' | 'process'>('all');
  let menu = $state('');
  let uploading = $state(0);
  let dragging = $state(false);
  let input: HTMLInputElement;

  const shown = $derived(filter === 'all' ? files.list : files.list.filter((f) => f.source === filter));
  const total = $derived(files.list.reduce((n, f) => n + f.size, 0));

  async function addAll(list: FileList | File[]) {
    const arr = Array.from(list);
    for (const file of arr) {
      if (!isSupported(file.type, file.name)) {
        toast(`${file.name}: only PDFs and images`, 'error');
        continue;
      }
      uploading++;
      try {
        await addFile({ name: file.name, blob: file, source: 'upload' });
      } catch (e) {
        toast(`${file.name}: ${(e as Error).message}`, 'error');
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
    if (!confirm(`Delete "${f.name}" from this browser?`)) return;
    await removeFile(f.id);
  }

  async function rename(f: StoredFile) {
    const name = prompt('New name', f.name);
    if (name) await renameFile(f.id, name);
  }

  async function toWork(f: StoredFile) {
    const n = await addFilesToWork([f]);
    toast(`${n} page${n === 1 ? '' : 's'} added to Process`, 'ok', [{ label: 'Open', run: () => go('process') }]);
  }

  function when(t: number): string {
    const d = new Date(t);
    const today = new Date().toDateString() === d.toDateString();
    return today ? d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : d.toLocaleDateString();
  }

  function kind(f: StoredFile): string {
    if (f.type === 'application/pdf') return `PDF · ${f.pages} page${f.pages === 1 ? '' : 's'}`;
    return f.dpi ? `${f.source === 'scan' ? 'Scan' : 'Image'} · ${f.dpi} dpi` : 'Image';
  }
</script>

<svelte:window onclick={() => (menu = '')} />

<div
  class="drop"
  class:dragging
  role="region"
  aria-label="Files"
  ondragover={(e) => {
    e.preventDefault();
    dragging = true;
  }}
  ondragleave={(e) => e.currentTarget === e.target && (dragging = false)}
  ondrop={onDrop}
>
  <div class="head row wrap">
    <div class="grow">
      <h1>Files</h1>
      <p class="muted small">Kept in this browser only · {files.list.length} files · {formatSize(total)}</p>
    </div>
    <button class="btn primary" onclick={() => input.click()}>
      {#if uploading}<LoaderCircle size={17} class="spin" />{:else}<Upload size={17} />{/if} Upload
    </button>
    <input bind:this={input} type="file" accept="application/pdf,image/*" multiple hidden onchange={onPick} />
  </div>

  <div class="seg filters">
    {#each [['all', 'All'], ['scan', 'Scans'], ['upload', 'Uploads'], ['process', 'Made here']] as [k, label]}
      <button class:on={filter === k} onclick={() => (filter = k as typeof filter)}>{label}</button>
    {/each}
  </div>

  {#if files.error}
    <p class="err">{files.error}</p>
  {:else if files.loaded && !shown.length}
    <div class="card empty">
      <Inbox size={40} strokeWidth={1.4} />
      <div>
        <b>No files yet</b>
        <p class="small">Upload PDFs or pictures (or drop them here), or scan a page: it shows up here.</p>
      </div>
      <button class="btn" onclick={() => input.click()}><Upload size={17} /> Upload</button>
    </div>
  {:else}
    <ul class="grid">
      {#each shown as f (f.id)}
        <li class="card tile">
          <button class="thumb" onclick={() => openViewer(f)} aria-label="Open {f.name}">
            {#if f.thumb}<img src={thumbUrl(f)} alt="" loading="lazy" />{/if}
            {#if f.type === 'application/pdf'}<span class="badge">PDF</span>{/if}
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
                  aria-label="More actions"
                  onclick={(e) => {
                    e.stopPropagation();
                    menu = menu === f.id ? '' : f.id;
                  }}><Ellipsis size={18} /></button
                >
                {#if menu === f.id}
                  <div class="menu card" role="menu">
                    <button onclick={() => toWork(f)}><Layers size={16} /> Add to Process</button>
                    <button onclick={() => rename(f)}><Pencil size={16} /> Rename</button>
                    <button class="danger" onclick={() => del(f)}><Trash2 size={16} /> Delete</button>
                  </div>
                {/if}
              </div>
            </div>
            <div class="row btns">
              <button class="btn primary grow" onclick={() => openPrint(f)}><Printer size={16} /> Print</button>
              <button class="btn" onclick={() => download(f.blob, f.name)} aria-label="Download"><Download size={16} /></button>
            </div>
          </div>
        </li>
      {/each}
    </ul>
  {/if}
  {#if dragging}
    <div class="overlay"><Upload size={36} /> Drop to add</div>
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
