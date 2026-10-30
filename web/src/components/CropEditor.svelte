<script lang="ts">
  import { t, i18n } from '../lib/i18n.svelte';
  import RotateCcw from '@lucide/svelte/icons/rotate-ccw';
  import RotateCw from '@lucide/svelte/icons/rotate-cw';
  import WandSparkles from '@lucide/svelte/icons/wand-sparkles';
  import Maximize2 from '@lucide/svelte/icons/maximize-2';
  import LoaderCircle from '@lucide/svelte/icons/loader-circle';
  import Modal from './Modal.svelte';
  import { detectContent, fitAspect } from '../lib/autocrop';
  import { refSize, renderRef, rotateRef, type Crop, type PageRef } from '../lib/process.svelte';
  import { toast } from '../lib/ui.svelte';

  let {
    ref,
    aspect = null,
    title,
    onsave,
    onclose,
  }: {
    ref: PageRef;
    aspect?: number | null; // locked width / height of the crop
    title?: string;
    onsave: (r: PageRef) => void;
    onclose: () => void;
  } = $props();

  // svelte-ignore state_referenced_locally
  let rot = $state(ref.rot);
  // svelte-ignore state_referenced_locally
  let crop = $state<Crop>(ref.crop ?? { x: 0, y: 0, w: 1, h: 1 });
  let url = $state('');
  let imgW = $state(1);
  let imgH = $state(1);
  let canvas: HTMLCanvasElement | null = null;
  let stage = $state<HTMLDivElement>();
  let busy = $state(true);

  // the whole rotated page, about 1200 px, to crop on
  $effect(() => {
    const r = { ...ref, rot, crop: null };
    let alive = true;
    busy = true;
    (async () => {
      const sz = await refSize(r);
      if (!sz || !alive) return;
      const c = await renderRef(r, 1200 / Math.max(sz.w, sz.h));
      if (!c || !alive) return;
      canvas = c;
      imgW = c.width;
      imgH = c.height;
      c.toBlob((b) => {
        if (!b || !alive) return;
        const old = url;
        url = URL.createObjectURL(b);
        if (old) URL.revokeObjectURL(old);
        busy = false;
      }, 'image/jpeg', 0.9);
    })();
    return () => {
      alive = false;
    };
  });

  // the picture fitted in the space available
  let wrapEl = $state<HTMLDivElement>();
  let boxW = $state(600);
  let boxH = $state(600);
  $effect(() => {
    if (!wrapEl) return;
    const ro = new ResizeObserver(() => {
      boxW = wrapEl!.clientWidth - 24;
      boxH = Math.min(window.innerHeight * 0.68, 900);
    });
    ro.observe(wrapEl);
    return () => ro.disconnect();
  });
  const fitScale = $derived(Math.min(boxW / imgW, boxH / imgH));

  $effect(() => () => url && URL.revokeObjectURL(url));

  function enforce(c: Crop): Crop {
    if (!aspect) return c;
    return fitAspect(c, aspect, imgW, imgH);
  }

  let fittedOnce = false;
  $effect(() => {
    // a locked shape: make the initial box match it once the size is known
    if (aspect && !busy && !fittedOnce) {
      fittedOnce = true;
      const ratio = (crop.w * imgW) / (crop.h * imgH);
      if (Math.abs(ratio - aspect) > 0.01) crop = enforce(crop);
    }
  });

  function rotate(by: 90 | -90) {
    const r = rotateRef({ ...ref, rot, crop }, by);
    rot = r.rot;
    crop = r.crop ?? { x: 0, y: 0, w: 1, h: 1 };
    fittedOnce = false;
  }

  function auto() {
    if (!canvas) return;
    // detect on a small copy
    const k = Math.min(1, 500 / Math.max(canvas.width, canvas.height));
    const w = Math.max(1, Math.round(canvas.width * k)),
      h = Math.max(1, Math.round(canvas.height * k));
    const small = document.createElement('canvas');
    small.width = w;
    small.height = h;
    const ctx = small.getContext('2d', { willReadFrequently: true })!;
    ctx.drawImage(canvas, 0, 0, w, h);
    const box = detectContent(ctx.getImageData(0, 0, w, h).data, w, h);
    if (!box) return toast(t('Could not find an object on the page'), 'error');
    // a little margin around it
    const m = 0.006;
    const b = { x: Math.max(0, box.x - m), y: Math.max(0, box.y - m), w: 0, h: 0 };
    b.w = Math.min(1 - b.x, box.w + 2 * m);
    b.h = Math.min(1 - b.y, box.h + 2 * m);
    crop = enforce(b);
  }

  // dragging: the box (move) or a handle
  type Handle = 'move' | 'n' | 's' | 'e' | 'w' | 'nw' | 'ne' | 'sw' | 'se';
  let drag: { h: Handle; x0: number; y0: number; c0: Crop } | null = null;

  function down(e: PointerEvent, h: Handle) {
    e.preventDefault();
    e.stopPropagation();
    (e.currentTarget as Element).setPointerCapture(e.pointerId);
    drag = { h, x0: e.clientX, y0: e.clientY, c0: { ...crop } };
  }

  function move(e: PointerEvent) {
    if (!drag || !stage) return;
    const r = stage.getBoundingClientRect();
    const dx = (e.clientX - drag.x0) / r.width;
    const dy = (e.clientY - drag.y0) / r.height;
    const c0 = drag.c0;
    const min = 0.03;
    if (drag.h === 'move') {
      crop = { ...c0, x: clamp(c0.x + dx, 0, 1 - c0.w), y: clamp(c0.y + dy, 0, 1 - c0.h) };
      return;
    }
    let x0 = c0.x,
      y0 = c0.y,
      x1 = c0.x + c0.w,
      y1 = c0.y + c0.h;
    if (drag.h.includes('w')) x0 = clamp(x0 + dx, 0, x1 - min);
    if (drag.h.includes('e')) x1 = clamp(x1 + dx, x0 + min, 1);
    if (drag.h.includes('n')) y0 = clamp(y0 + dy, 0, y1 - min);
    if (drag.h.includes('s')) y1 = clamp(y1 + dy, y0 + min, 1);
    if (aspect) {
      // keep the shape: the width decides, anchored on the opposite corner
      let w = x1 - x0;
      let h = (w * imgW) / aspect / imgH;
      const maxH = drag.h.includes('n') ? c0.y + c0.h : 1 - c0.y;
      if (h > maxH) {
        h = maxH;
        w = (h * imgH * aspect) / imgW;
      }
      if (drag.h.includes('w')) x0 = x1 - w;
      else x1 = x0 + w;
      if (drag.h.includes('n')) y0 = c0.y + c0.h - h;
      else y0 = c0.y;
      y1 = y0 + h;
    }
    crop = { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
  }

  function up() {
    drag = null;
  }

  function clamp(v: number, a: number, b: number) {
    return Math.min(b, Math.max(a, v));
  }

  function save() {
    const full = crop.x < 0.001 && crop.y < 0.001 && crop.w > 0.998 && crop.h > 0.998;
    onsave({ ...ref, rot, crop: full && !aspect ? null : { ...crop } });
    onclose();
  }

  const handles: Handle[] = $derived(aspect ? ['nw', 'ne', 'sw', 'se'] : ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w']);
</script>

<Modal title={title ?? t('Crop and rotate')} {onclose} wide>
  <div class="wrap" lang={i18n.locale} bind:this={wrapEl} onpointermove={move} onpointerup={up} onpointercancel={up} role="presentation">
    <div class="stage" bind:this={stage} style:width={`${imgW * fitScale}px`} style:height={`${imgH * fitScale}px`}>
      {#if url}<img src={url} alt={t('Page')} draggable="false" />{/if}
      {#if busy}<div class="loading"><LoaderCircle class="spin" /></div>{/if}
      {#if !busy}
        <div
          class="box"
          style:left={`${crop.x * 100}%`}
          style:top={`${crop.y * 100}%`}
          style:width={`${crop.w * 100}%`}
          style:height={`${crop.h * 100}%`}
          onpointerdown={(e) => down(e, 'move')}
          role="presentation"
        >
          {#each handles as h}
            <span class="h {h}" onpointerdown={(e) => down(e, h)} role="presentation"></span>
          {/each}
        </div>
      {/if}
    </div>
  </div>
  {#snippet footer()}
    <div class="row wrap grow">
      <button class="icon-btn" onclick={() => rotate(-90)} aria-label={t('Rotate left')} title={t('Rotate left')}><RotateCcw size={19} /></button>
      <button class="icon-btn" onclick={() => rotate(90)} aria-label={t('Rotate right')} title={t('Rotate right')}><RotateCw size={19} /></button>
      <button class="btn ghost" onclick={auto}><WandSparkles size={17} /> {t('Auto-detect')}</button>
      <button class="btn ghost" onclick={() => (crop = enforce({ x: 0, y: 0, w: 1, h: 1 }))}><Maximize2 size={17} /> {t('Whole page')}</button>
    </div>
    <button class="btn" onclick={onclose}>{t('Cancel')}</button>
    <button class="btn primary" onclick={save}>{t('Done')}</button>
  {/snippet}
</Modal>

<style>
  .wrap {
    display: grid;
    place-items: center;
    padding: 20px;
    background: var(--surface-3);
    touch-action: none;
    min-height: 100%;
  }
  .stage {
    position: relative;
    background: var(--paper);
    user-select: none;
    -webkit-user-select: none;
  }
  img {
    display: block;
    width: 100%;
    height: 100%;
    pointer-events: none;
  }
  .loading {
    position: absolute;
    inset: 0;
    display: grid;
    place-items: center;
  }
  .box {
    position: absolute;
    box-shadow: 0 0 0 9999px rgba(10, 12, 20, 0.5);
    outline: 2px solid #fff;
    cursor: move;
    touch-action: none;
  }
  .h {
    position: absolute;
    width: 22px;
    height: 22px;
    margin: -11px;
    background: #fff;
    border: 2px solid var(--accent);
    border-radius: 50%;
    touch-action: none;
  }
  .nw {
    left: 0;
    top: 0;
    cursor: nwse-resize;
  }
  .ne {
    right: 0;
    top: 0;
    cursor: nesw-resize;
  }
  .sw {
    left: 0;
    bottom: 0;
    cursor: nesw-resize;
  }
  .se {
    right: 0;
    bottom: 0;
    cursor: nwse-resize;
  }
  .n {
    left: 50%;
    top: 0;
    cursor: ns-resize;
  }
  .s {
    left: 50%;
    bottom: 0;
    cursor: ns-resize;
  }
  .e {
    right: 0;
    top: 50%;
    cursor: ew-resize;
  }
  .w {
    left: 0;
    top: 50%;
    cursor: ew-resize;
  }
  @media (max-width: 760px) {
    .wrap {
      padding: 12px;
    }
  }
</style>
