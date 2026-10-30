<script lang="ts">
  import { t, i18n } from '../lib/i18n.svelte';
  import LoaderCircle from '@lucide/svelte/icons/loader-circle';
  import { layoutItem, renderRef, renderSheet, refSize, type Item } from '../lib/process.svelte';
  import { defaultPaper } from '../lib/layout';
  import { files } from '../lib/files.svelte';

  let { item, max = 280 }: { item: Item; max?: number } = $props();
  let url = $state('');
  let missing = $state(false);
  let busy = $state(true);

  $effect(() => {
    const it = item;
    void files.list; // re-render once the files are loaded
    let alive = true;
    busy = true;
    (async () => {
      let c: HTMLCanvasElement | null = null;
      const k = Math.min(2, devicePixelRatio || 1);
      if (it.kind === 'page') {
        const sz = await refSize(it.ref);
        if (sz) c = await renderRef(it.ref, (max * k) / Math.max(sz.w, sz.h));
      } else {
        const sheet = await layoutItem(it, 'a4', defaultPaper());
        if (sheet) c = await renderSheet(sheet, (max * k) / sheet.h);
      }
      if (!alive) return;
      missing = !c;
      busy = false;
      if (c) {
        c.toBlob((b) => {
          if (!alive || !b) return;
          const old = url;
          url = URL.createObjectURL(b);
          if (old) URL.revokeObjectURL(old);
        }, 'image/jpeg', 0.85);
      }
    })().catch(() => {
      missing = true;
      busy = false;
    });
    return () => {
      alive = false;
    };
  });

  $effect(() => () => {
    if (url) URL.revokeObjectURL(url);
  });
</script>

<div class="thumb" lang={i18n.locale}>
  {#if url}
    <img src={url} alt="" />
  {:else if missing}
    <span class="muted small">{t('File deleted')}</span>
  {:else if busy}
    <LoaderCircle class="spin" />
  {/if}
</div>

<style>
  .thumb {
    display: grid;
    place-items: center;
    width: 100%;
    height: 100%;
  }
  img {
    max-width: 100%;
    max-height: 100%;
    background: var(--paper);
    box-shadow: 0 1px 6px rgba(0, 0, 0, 0.15);
  }
</style>
