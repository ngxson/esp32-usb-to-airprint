<script lang="ts">
  import type { Snippet } from 'svelte';
  import X from '@lucide/svelte/icons/x';

  let {
    title,
    onclose,
    wide = false,
    children,
    footer,
  }: { title: string; onclose: () => void; wide?: boolean; children: Snippet; footer?: Snippet } = $props();

  function key(e: KeyboardEvent) {
    if (e.key === 'Escape') onclose();
  }
</script>

<svelte:window onkeydown={key} />

<div class="backdrop" role="presentation" onclick={(e) => e.target === e.currentTarget && onclose()}>
  <div class="modal" class:wide role="dialog" aria-modal="true" aria-label={title}>
    <header>
      <h2 class="grow ellipsis">{title}</h2>
      <button class="icon-btn" aria-label="Close" onclick={onclose}><X size={20} /></button>
    </header>
    <div class="body">
      {@render children()}
    </div>
    {#if footer}
      <footer>{@render footer()}</footer>
    {/if}
  </div>
</div>

<style>
  .backdrop {
    position: fixed;
    inset: 0;
    z-index: 50;
    display: grid;
    place-items: center;
    padding: 24px;
    background: rgba(10, 12, 18, 0.45);
    backdrop-filter: blur(2px);
    animation: fade 0.15s;
  }
  .modal {
    display: flex;
    flex-direction: column;
    width: min(560px, 100%);
    max-height: min(860px, 100%);
    background: var(--surface);
    border-radius: 18px;
    box-shadow: var(--shadow-lg);
    overflow: hidden;
    animation: pop 0.16s ease-out;
  }
  .modal.wide {
    width: min(1060px, 100%);
  }
  header {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 14px 14px 12px 20px;
    border-bottom: 1px solid var(--border);
  }
  .body {
    overflow: auto;
    flex: 1;
    min-height: 0;
  }
  footer {
    display: flex;
    gap: 10px;
    justify-content: flex-end;
    align-items: center;
    padding: 12px 16px;
    border-top: 1px solid var(--border);
    background: var(--surface-2);
    flex-wrap: wrap;
  }
  @keyframes fade {
    from {
      opacity: 0;
    }
  }
  @keyframes pop {
    from {
      transform: scale(0.97);
      opacity: 0;
    }
  }
  @media (max-width: 760px) {
    .backdrop {
      padding: 0;
      place-items: end stretch;
    }
    .modal,
    .modal.wide {
      width: 100%;
      max-height: 100%;
      height: 100%;
      border-radius: 0;
    }
    header {
      padding-top: max(12px, env(safe-area-inset-top));
    }
    footer {
      padding-bottom: max(12px, env(safe-area-inset-bottom));
    }
  }
</style>
