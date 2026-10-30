<script lang="ts">
  import { tick } from 'svelte';
  import Languages from '@lucide/svelte/icons/languages';
  import ChevronUp from '@lucide/svelte/icons/chevron-up';
  import Check from '@lucide/svelte/icons/check';
  import { localeConfig, locales, type Locale } from '../lib/i18n';
  import { i18n, setLocale, t } from '../lib/i18n.svelte';

  const id = $props.id();
  let open = $state(false);
  let root: HTMLDivElement;
  let trigger: HTMLButtonElement;

  async function toggle() {
    open = !open;
    if (open) {
      await tick();
      root.querySelector<HTMLInputElement>('input:checked')?.focus();
    }
  }

  function close(restoreFocus = false) {
    open = false;
    if (restoreFocus) trigger.focus();
  }

  function choose(locale: Locale) {
    setLocale(locale);
    close(true);
  }

  function confirm(event: KeyboardEvent, locale: Locale) {
    if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault();
      choose(locale);
    }
  }

  function outside(event: PointerEvent) {
    if (open && event.target instanceof Node && !root.contains(event.target)) close();
  }

  function keydown(event: KeyboardEvent) {
    if (open && event.key === 'Escape') {
      event.preventDefault();
      close(true);
    }
  }
</script>

<svelte:window onpointerdown={outside} onkeydown={keydown} />

<div class="language-picker" bind:this={root} onfocusout={(event) => {
  if (event.relatedTarget instanceof Node && !root.contains(event.relatedTarget)) close();
}}>
  <button
    class="trigger"
    bind:this={trigger}
    aria-label={t('Language')}
    aria-expanded={open}
    aria-controls={id}
    onclick={toggle}
  >
    <Languages size={18} />
    <span class="current" lang={i18n.locale}>{localeConfig[i18n.locale].label}</span>
    <ChevronUp size={16} class={open ? 'chevron expanded' : 'chevron'} />
  </button>

  {#if open}
    <fieldset class="panel" id={id}>
      <legend class="sr-only">{t('Language')}</legend>
      <div class="caption" aria-hidden="true">{t('Language')}</div>
      {#each locales as locale}
        <label class="option" class:selected={locale === i18n.locale}>
          <!-- Arrow keys change the native radio selection without closing the panel.
               A pointer click or Enter/Space commits it and restores trigger focus. -->
          <input
            type="radio"
            name={id}
            value={locale}
            checked={locale === i18n.locale}
            onchange={() => setLocale(locale)}
            onkeydown={(event) => confirm(event, locale)}
            onclick={(event) => { if (event.detail > 0) choose(locale); }}
          />
          <span lang={locale}>{localeConfig[locale].label}</span>
          {#if locale === i18n.locale}<Check size={16} />{/if}
        </label>
      {/each}
    </fieldset>
  {/if}
</div>

<style>
  .language-picker { position: relative; margin-top: 12px; }
  .trigger {
    display: flex; align-items: center; gap: 10px; width: 100%; min-height: 42px;
    padding: 10px 12px; border: 1px solid var(--border); border-radius: 10px;
    background: var(--surface); color: var(--muted); cursor: pointer;
    font: inherit; font-size: 0.85rem; text-align: left;
  }
  .trigger:hover, .trigger[aria-expanded='true'] { background: var(--surface-2); color: var(--text); }
  .current { flex: 1; color: var(--text); }
  .trigger :global(.chevron) { transition: transform 150ms ease; }
  .trigger :global(.expanded) { transform: rotate(180deg); }
  .trigger:focus-visible { outline: 2px solid var(--accent); outline-offset: 3px; }
  .panel {
    position: absolute; inset: auto 0 calc(100% + 8px); z-index: 10;
    min-width: 0; margin: 0; padding: 6px; border: 1px solid var(--border);
    border-radius: 12px; background: var(--surface); box-shadow: var(--shadow);
  }
  .caption { padding: 7px 10px; color: var(--muted); font-size: 0.75rem; }
  .option {
    position: relative; display: flex; align-items: center; justify-content: space-between;
    min-height: 42px; gap: 12px; padding: 10px; border-radius: 7px;
    color: var(--text); font-size: 0.9rem; cursor: pointer;
  }
  .option:hover { background: var(--surface-2); }
  .option.selected { background: var(--accent-soft); color: var(--accent); }
  .option:focus-within { outline: 2px solid var(--accent); outline-offset: -2px; }
  input { position: absolute; inset: 0; width: 100%; height: 100%; margin: 0; opacity: 0; cursor: pointer; }
  .sr-only { position: absolute; width: 1px; height: 1px; padding: 0; margin: -1px; overflow: hidden; clip-path: inset(50%); white-space: nowrap; }
  @media (prefers-reduced-motion: reduce) { .trigger :global(.chevron) { transition: none; } }
</style>
