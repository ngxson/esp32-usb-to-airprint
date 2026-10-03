<script lang="ts">
  import Upload from '@lucide/svelte/icons/upload';
  import Check from '@lucide/svelte/icons/check';
  import Modal from './Modal.svelte';
  import { addFile, files, isSupported, thumbUrl, type StoredFile } from '../lib/files.svelte';
  import { toast } from '../lib/ui.svelte';

  let {
    title,
    multi,
    resolve,
    onclose,
  }: { title: string; multi: boolean; resolve: (f: StoredFile[]) => void; onclose: () => void } = $props();

  let chosen = $state<string[]>([]);
  let input: HTMLInputElement;
  let done = false;

  function finish(list: StoredFile[]) {
    if (done) return;
    done = true;
    resolve(list);
    onclose();
  }

  function toggle(f: StoredFile) {
    if (!multi) return finish([f]);
    chosen = chosen.includes(f.id) ? chosen.filter((x) => x !== f.id) : [...chosen, f.id];
  }

  async function upload(e: Event) {
    const el = e.target as HTMLInputElement;
    const added: StoredFile[] = [];
    for (const file of Array.from(el.files || [])) {
      if (!isSupported(file.type, file.name)) continue;
      try {
        added.push(await addFile({ name: file.name, blob: file, source: 'upload' }));
      } catch (err) {
        toast(`${file.name}: ${(err as Error).message}`, 'error');
      }
    }
    el.value = '';
    if (added.length && !multi) finish(added.slice(0, 1));
    else chosen = [...chosen, ...added.map((f) => f.id)];
  }
</script>

<Modal {title} onclose={() => finish([])} wide>
  <div class="pick">
    {#if !files.list.length}
      <p class="muted empty">No files yet: upload one.</p>
    {/if}
    <ul>
      {#each files.list as f (f.id)}
        <li>
          <button class="tile" class:on={chosen.includes(f.id)} onclick={() => toggle(f)}>
            <img src={thumbUrl(f)} alt="" loading="lazy" />
            <span class="name ellipsis small">{f.name}</span>
            {#if chosen.includes(f.id)}<span class="tick"><Check size={14} /></span>{/if}
          </button>
        </li>
      {/each}
    </ul>
  </div>
  <input bind:this={input} type="file" accept="application/pdf,image/*" multiple={multi} hidden onchange={upload} />
  {#snippet footer()}
    <button class="btn" onclick={() => input.click()}><Upload size={17} /> Upload</button>
    <span class="grow"></span>
    {#if multi}
      <button
        class="btn primary"
        disabled={!chosen.length}
        onclick={() => finish(chosen.map((id) => files.list.find((f) => f.id === id)).filter((f): f is StoredFile => !!f))}
        >Add {chosen.length || ''}</button
      >
    {/if}
  {/snippet}
</Modal>

<style>
  .pick {
    padding: 16px;
  }
  ul {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(120px, 1fr));
    gap: 12px;
  }
  .tile {
    position: relative;
    width: 100%;
    display: grid;
    gap: 6px;
    padding: 8px;
    border: 2px solid transparent;
    border-radius: 12px;
    background: var(--surface-2);
    cursor: pointer;
    text-align: left;
  }
  .tile.on {
    border-color: var(--accent);
    background: var(--accent-soft);
  }
  img {
    width: 100%;
    aspect-ratio: 3 / 4;
    object-fit: contain;
    filter: drop-shadow(0 1px 3px rgba(0, 0, 0, 0.18));
  }
  .tick {
    position: absolute;
    top: 4px;
    right: 4px;
    display: grid;
    place-items: center;
    width: 22px;
    height: 22px;
    border-radius: 50%;
    background: var(--accent);
    color: var(--accent-text);
  }
  .empty {
    padding: 30px;
    text-align: center;
  }
</style>
