<script lang="ts">
  import ScanLine from '@lucide/svelte/icons/scan-line';
  import FolderOpen from '@lucide/svelte/icons/folder-open';
  import Crop from '@lucide/svelte/icons/crop';
  import X from '@lucide/svelte/icons/x';
  import LoaderCircle from '@lucide/svelte/icons/loader-circle';
  import RectangleHorizontal from '@lucide/svelte/icons/rectangle-horizontal';
  import RectangleVertical from '@lucide/svelte/icons/rectangle-vertical';
  import Modal from './Modal.svelte';
  import CropEditor from './CropEditor.svelte';
  import ItemThumb from './ItemThumb.svelte';
  import { detectContent, fitAspect } from '../lib/autocrop';
  import { device } from '../lib/device.svelte';
  import { CARD_MM } from '../lib/layout';
  import { renderRef, refSize, updateItem, work, type Item, type PageRef } from '../lib/process.svelte';
  import { startKept, whenSaved } from '../lib/scan.svelte';
  import type { StoredFile } from '../lib/files.svelte';
  import { pickFiles, toast } from '../lib/ui.svelte';

  type Card = Extract<Item, { kind: 'card' }>;
  let { id, onclose }: { id: string; onclose: () => void } = $props();

  const card = $derived(work.items.find((i) => i.id === id) as Card | undefined);
  const aspect = $derived(card?.vertical ? CARD_MM.h / CARD_MM.w : CARD_MM.w / CARD_MM.h);
  let scanning = $state<'' | 'front' | 'back'>('');
  let editing = $state<'' | 'front' | 'back'>('');

  function set(side: 'front' | 'back', ref: PageRef | null) {
    updateItem(id, (it) => ({ ...(it as Card), [side]: ref }));
  }

  // a crop around the card found on the page, in the card's shape
  async function autoRef(f: StoredFile): Promise<PageRef> {
    const ref: PageRef = { fileId: f.id, page: 0, rot: 0, crop: null };
    const sz = await refSize(ref);
    const c = sz && (await renderRef(ref, 500 / Math.max(sz.w, sz.h)));
    if (!c) return ref;
    let rot = ref.rot;
    const ctx = c.getContext('2d', { willReadFrequently: true })!;
    const box = detectContent(ctx.getImageData(0, 0, c.width, c.height).data, c.width, c.height);
    if (!box) return ref;
    // a card found upright on a landscape layout: turn the page
    const found = (box.w * c.width) / (box.h * c.height);
    let b = box;
    if ((found > 1) !== (aspect > 1)) {
      rot = 90;
      b = { x: 1 - box.y - box.h, y: box.x, w: box.h, h: box.w };
      return { ...ref, rot, crop: fitAspect(b, aspect, c.height, c.width) };
    }
    return { ...ref, crop: fitAspect(b, aspect, c.width, c.height) };
  }

  async function fromFiles(side: 'front' | 'back') {
    const [f] = await pickFiles(`Choose the ${side} of the card`, false);
    if (f) set(side, await autoRef(f));
  }

  async function scan(side: 'front' | 'back') {
    scanning = side;
    try {
      const sid = await startKept('card', 300);
      const f = await whenSaved(sid);
      if (!f) throw new Error(device.scan?.error || 'the scan did not finish');
      set(side, await autoRef(f));
    } catch (e) {
      toast(`Scan failed: ${(e as Error).message}`, 'error');
    } finally {
      scanning = '';
    }
  }

  function turn() {
    updateItem(id, (it) => ({ ...(it as Card), vertical: !(it as Card).vertical }));
  }
</script>

{#if card}
  <Modal title="ID card copy" {onclose} wide>
    <div class="layout">
      <div class="sides">
        <p class="muted small">
          Both sides of a card on one sheet, at the card's real size. Put the card in the corner of the glass by the
          arrow and scan the front, then turn it over and scan the back. Or use pictures from Files.
        </p>
        {#each ['front', 'back'] as const as side}
          {@const ref = card[side]}
          <div class="side card">
            <div class="row">
              <h3 class="grow">{side === 'front' ? 'Front' : 'Back'}</h3>
              {#if ref}
                <button class="icon-btn" onclick={() => (editing = side)} aria-label="Adjust the crop" title="Adjust"><Crop size={18} /></button>
                <button class="icon-btn" onclick={() => set(side, null)} aria-label="Remove" title="Remove"><X size={18} /></button>
              {/if}
            </div>
            <div class="slot" style:aspect-ratio={aspect}>
              {#if scanning === side}
                <div class="center muted"><LoaderCircle class="spin" /> Scanning…</div>
              {:else if ref}
                <ItemThumb item={{ id: `${id}-${side}`, kind: 'page', ref }} max={300} />
              {:else}
                <div class="center muted small">Not set yet</div>
              {/if}
            </div>
            <div class="row wrap">
              <button
                class="btn primary"
                onclick={() => scan(side)}
                disabled={!!scanning || !device.scan?.scanner || device.scan?.state === 'scanning'}
                ><ScanLine size={17} /> Scan {side}</button
              >
              <button class="btn" onclick={() => fromFiles(side)} disabled={!!scanning}><FolderOpen size={17} /> From Files</button>
            </div>
          </div>
        {/each}
      </div>
      <div class="sheet">
        <div class="page"><ItemThumb item={card} max={420} /></div>
        <button class="btn ghost" onclick={turn}>
          {#if card.vertical}<RectangleHorizontal size={17} /> Landscape card{:else}<RectangleVertical size={17} /> Portrait card{/if}
        </button>
      </div>
    </div>
    {#snippet footer()}
      <button class="btn primary" onclick={onclose}>Done</button>
    {/snippet}
  </Modal>

  {#if editing && card[editing]}
    {@const side = editing}
    <CropEditor
      ref={card[side]!}
      {aspect}
      title={`Adjust the ${side}`}
      onsave={(r) => set(side, r)}
      onclose={() => (editing = '')}
    />
  {/if}
{/if}

<style>
  .layout {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 22px;
    padding: 20px;
  }
  .sides {
    display: grid;
    gap: 14px;
    align-content: start;
  }
  .side {
    display: grid;
    gap: 10px;
    padding: 14px;
    box-shadow: none;
  }
  .slot {
    display: grid;
    place-items: center;
    background: var(--surface-3);
    border-radius: 10px;
    padding: 8px;
    max-height: 190px;
    width: 100%;
  }
  .center {
    display: flex;
    gap: 8px;
    align-items: center;
  }
  .sheet {
    display: grid;
    gap: 10px;
    justify-items: center;
    align-content: start;
    background: var(--surface-3);
    border-radius: var(--radius);
    padding: 20px;
  }
  .page {
    width: 100%;
    max-width: 340px;
    aspect-ratio: 210 / 297;
  }
  @media (max-width: 760px) {
    .layout {
      grid-template-columns: 1fr;
      padding: 14px;
    }
    .page {
      max-width: 220px;
    }
  }
</style>
