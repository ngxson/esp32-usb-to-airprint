<script lang="ts">
  import { t, i18n } from '../lib/i18n.svelte';
  import X from '@lucide/svelte/icons/x';
  import { dismiss, ui } from '../lib/ui.svelte';
</script>

<div lang={i18n.locale} class="toasts" aria-live="polite">
  {#each ui.toasts as entry (entry.id)}
    <div class="toast {entry.kind}">
      <span class="grow" lang={entry.locale}>{entry.text}</span>
      {#each entry.actions as a}
        <button
          class="btn ghost act"
          lang={entry.locale}
          onclick={() => {
            dismiss(entry.id);
            a.run();
          }}>{a.label}</button
        >
      {/each}
      <button class="icon-btn" aria-label={t('Dismiss')} onclick={() => dismiss(entry.id)}><X size={16} /></button>
    </div>
  {/each}
</div>

<style>
  /* bottom left: dialogs keep their buttons on the right */
  .toasts {
    position: fixed;
    left: 18px;
    bottom: 18px;
    bottom: max(18px, env(safe-area-inset-bottom));
    z-index: 100;
    display: grid;
    gap: 8px;
    width: min(420px, calc(100vw - 24px));
  }
  .toast {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 8px 6px 8px 14px;
    border-radius: 12px;
    background: var(--text);
    color: var(--bg);
    box-shadow: var(--shadow-lg);
    animation: up 0.18s ease-out;
    font-weight: 500;
  }
  .toast.error {
    background: var(--err);
    color: #fff;
  }
  .toast .icon-btn {
    color: inherit;
    opacity: 0.7;
    width: 30px;
    height: 30px;
  }
  .act {
    color: inherit;
    min-height: 30px;
    padding: 0 10px;
    text-decoration: underline;
    text-underline-offset: 3px;
  }
  .act:hover:not(:disabled) {
    background: rgba(127, 127, 127, 0.2);
  }
  @keyframes up {
    from {
      transform: translateY(8px);
      opacity: 0;
    }
  }
  /* phones: at the top, clear of the full-screen dialogs' buttons */
  @media (max-width: 760px) {
    .toasts {
      right: 12px;
      left: 12px;
      bottom: auto;
      top: max(12px, env(safe-area-inset-top));
      width: auto;
    }
  }
</style>
