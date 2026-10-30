<script lang="ts">
  import { t, i18n } from '../lib/i18n.svelte';
  import type { Message } from '../lib/i18n';
  import Printer from '@lucide/svelte/icons/printer';
  import Upload from '@lucide/svelte/icons/upload';
  import FolderOpen from '@lucide/svelte/icons/folder-open';
  import Lightbulb from '@lucide/svelte/icons/lightbulb';
  import CircleStop from '@lucide/svelte/icons/circle-stop';
  import CircleAlert from '@lucide/svelte/icons/circle-alert';
  import { device, deviceLabel, type Job, type Supply } from '../lib/device.svelte';
  import { cancelJob, identifyPrinter, statusOk, statusText } from '../lib/ipp';
  import { addFile, formatSize, isSupported } from '../lib/files.svelte';
  import { openPrint, pickFiles, toast } from '../lib/ui.svelte';

  const p = $derived(device.printer);
  const active = $derived(p?.jobs.find((j) => j.id === p.active && (j.state === 'processing' || j.state === 'pending')));
  let input: HTMLInputElement;

  const status = $derived.by(() => {
    if (!device.loaded) return { text: t('Connecting…'), tone: '' };
    if (!p) return { text: t('Unavailable'), tone: 'err' };
    if (!p.connected) return { text: t('Not connected'), tone: 'warn' };
    if (p.reasons.some((r) => r.endsWith('-error'))) return { text: deviceLabel(p.message), tone: 'err' };
    if (p.state === 'processing') return { text: t('Printing'), tone: 'busy' };
    if (device.scan?.state === 'scanning') return { text: t('Scanning'), tone: 'busy' };
    return { text: t('Ready'), tone: 'ok' };
  });

  const REASONS: Partial<Record<string, Message>> = {
    'marker-supply-low-warning': 'Ink is running low',
    'marker-supply-empty-error': 'An ink cartridge is empty or missing',
    'media-empty-error': 'Out of paper: load paper in the rear tray',
    'media-jam-error': 'Paper jam',
    'cover-open-error': 'The cover is open',
    'offline-report': 'The printer is not connected',
  };

  function reasonText(r: string): string {
    const key = REASONS[r];
    return key ? t(key) : r;
  }

  function inkColor(s: Supply): string {
    const cs = s.color.match(/#[0-9a-f]{6}/gi) || ['#888'];
    return cs.length > 1 ? `linear-gradient(90deg, ${cs.join(', ')})` : cs[0];
  }

  const STATE: Record<Job['state'], [Message, string]> = {
    pending: ['Waiting', 'busy'],
    held: ['Held', 'warn'],
    processing: ['Printing', 'busy'],
    completed: ['Done', 'ok'],
    canceled: ['Canceled', ''],
    aborted: ['Failed', 'err'],
  };

  // the device's clock is set over the network; without it, no time
  function ago(time: number): string {
    if (time < 1.6e9) return '';
    const seconds = Math.max(0, Date.now() / 1000 - time);
    if (seconds < 60) return t('just now');
    const relative = new Intl.RelativeTimeFormat(i18n.locale, { style: 'short' });
    if (seconds < 3600) return relative.format(-Math.round(seconds / 60), 'minute');
    if (seconds < 86400) return relative.format(-Math.round(seconds / 3600), 'hour');
    return relative.format(-Math.round(seconds / 86400), 'day');
  }

  async function chooseFile() {
    const [f] = await pickFiles(t('Choose a file to print'), false);
    if (f) openPrint(f);
  }

  async function upload(e: Event) {
    const list = (e.target as HTMLInputElement).files;
    const file = list?.[0];
    (e.target as HTMLInputElement).value = '';
    if (!file) return;
    if (!isSupported(file.type, file.name)) return toast(t('Choose a PDF or an image'), 'error');
    try {
      const f = await addFile({ name: file.name, blob: file, source: 'upload' });
      openPrint(f);
    } catch (err) {
      toast(t('Cannot open {name}: {error}', { name: file.name, error: (err as Error).message }), 'error');
    }
  }

  async function identify() {
    try {
      await identifyPrinter();
      toast(t('The light on the printer box flashes blue'), 'ok');
    } catch (e) {
      toast(t('Failed: {error}', { error: (e as Error).message }), 'error');
    }
  }

  async function cancel(id: number) {
    try {
      const r = await cancelJob(id);
      if (!statusOk(r.status)) throw new Error(statusText(r));
      toast(t('Canceling: the sheet is finished and ejected'));
    } catch (e) {
      toast(t('Cannot cancel: {error}', { error: (e as Error).message }), 'error');
    }
  }
</script>

<div class="grid">
  <section class="card hero">
    <div class="art"><Printer size={40} strokeWidth={1.5} /></div>
    <div class="grow">
      <div class="row wrap">
        <h1 class="ellipsis">{p?.name || t('Printer')}</h1>
        <span class="pill {status.tone}"><span class="dot"></span>{status.text}</span>
      </div>
      <p class="muted">{p?.model || ''}{p?.connected && p.message && deviceLabel(p.message) !== status.text ? ` · ${deviceLabel(p.message)}` : ''}</p>
      <div class="actions row wrap">
        <button class="btn primary" onclick={() => input.click()} disabled={!p?.connected}>
          <Upload size={17} /> {t('Upload & print')}
        </button>
        <button class="btn" onclick={chooseFile} disabled={!p?.connected}><FolderOpen size={17} /> {t('Print from Files')}</button>
        <button class="btn ghost" onclick={identify} disabled={!device.online}><Lightbulb size={17} /> {t('Identify')}</button>
      </div>
      <input bind:this={input} type="file" accept="application/pdf,image/*" hidden onchange={upload} />
    </div>
  </section>

  {#if p && p.reasons.length}
    <section class="card card-pad alerts">
      {#each p.reasons as r}
        <div class="row">
          <CircleAlert size={18} class={r.endsWith('-error') ? 'err' : 'warn'} />
          <span>{reasonText(r)}</span>
        </div>
      {/each}
    </section>
  {/if}

  {#if active}
    <section class="card card-pad job">
      <div class="row">
        <div class="grow">
          <h2 class="ellipsis">{active.name}</h2>
          <p class="muted small">
            {active.state === 'pending' ? t('Waiting for the document') : t('Page {page}', { page: active.pages + 1 })} ·
            {active.user ? t('{size} received · from {user}', { size: formatSize(active.bytes), user: active.user }) : t('{size} received', { size: formatSize(active.bytes) })}
          </p>
        </div>
        <button class="btn danger" onclick={() => cancel(active.id)}><CircleStop size={17} /> {t('Cancel')}</button>
      </div>
      <div class="bar indeterminate"><div></div></div>
    </section>
  {/if}

  <section class="card card-pad">
    <h2>{t('Ink')}</h2>
    {#if p?.supplies.length}
      <div class="inks">
        {#each p.supplies as s}
          <div class="ink">
            <div class="row"><span class="grow">{deviceLabel(s.name)}</span><b>{s.level >= 0 ? s.level + '%' : t('unknown')}</b></div>
            <div class="bar">
              <div style:width={(s.level >= 0 ? s.level : 0) + '%'} style:background={inkColor(s)}></div>
            </div>
          </div>
        {/each}
      </div>
      <p class="muted small">{t('Refilled cartridges report "unknown".')}</p>
    {:else}
      <p class="muted">{p?.connected ? t('The printer has not reported its ink yet.') : t('No printer connected.')}</p>
    {/if}
  </section>

  <section class="card card-pad">
    <h2>{t('Recent jobs')}</h2>
    {#if p?.jobs.length}
      <ul class="jobs">
        {#each p.jobs as j (j.id)}
          {@const state = STATE[j.state]}
          {@const label = state ? t(state[0]) : j.state}
          {@const tone = state?.[1] || ''}
          <li class="row">
            <div class="grow">
              <div class="ellipsis">{j.name}</div>
              <div class="muted small">
                {[j.pages === 1 ? t('{count} page', { count: j.pages }) : t('{count} pages', { count: j.pages }), ago(j.time), j.user].filter(Boolean).join(' · ')}
              </div>
            </div>
            <span class="pill {tone}">{label}</span>
          </li>
        {/each}
      </ul>
    {:else}
      <p class="muted">{t('No jobs since the printer box started.')}</p>
    {/if}
  </section>

  <section class="card card-pad tips">
    <h2>{t('Other ways to print')}</h2>
    <p class="muted small">
      {t('iPhone, iPad and Mac: AirPrint (Share → Print). Android: the default print service. The button on the printer box scans a page into Files; press it twice for a test page, hold it to cancel.')}
    </p>
  </section>
</div>

<style>
  .grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 18px;
  }
  .hero,
  .alerts,
  .job {
    grid-column: 1 / -1;
  }
  .hero {
    display: flex;
    gap: 22px;
    padding: 24px;
    align-items: center;
  }
  .art {
    display: grid;
    place-items: center;
    width: 92px;
    height: 92px;
    border-radius: 24px;
    background: var(--accent-soft);
    color: var(--accent);
    flex: none;
  }
  .actions {
    margin-top: 16px;
  }
  h2 {
    margin-bottom: 12px;
  }
  .alerts {
    display: grid;
    gap: 6px;
  }
  .alerts :global(.warn) {
    color: var(--warn);
  }
  .alerts :global(.err) {
    color: var(--err);
  }
  .job h2 {
    margin: 0;
  }
  .job .bar {
    margin-top: 12px;
  }
  .indeterminate > div {
    width: 35%;
    animation: slide 1.4s ease-in-out infinite;
  }
  @keyframes slide {
    from {
      transform: translateX(-100%);
    }
    to {
      transform: translateX(300%);
    }
  }
  .inks {
    display: grid;
    gap: 14px;
    margin-bottom: 12px;
  }
  .ink .bar {
    height: 10px;
    margin-top: 6px;
    border-radius: 5px;
  }
  .ink .bar > div {
    border-radius: 5px;
  }
  .jobs {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
  }
  .jobs li {
    padding: 9px 0;
    border-top: 1px solid var(--border);
  }
  .jobs li:first-child {
    border-top: 0;
    padding-top: 0;
  }
  .tips {
    grid-column: 1 / -1;
  }
  .tips h2 {
    margin-bottom: 6px;
  }
  @media (max-width: 900px) {
    .grid {
      grid-template-columns: 1fr;
    }
    .hero {
      flex-direction: column;
      align-items: flex-start;
      gap: 14px;
      padding: 18px;
    }
    .art {
      width: 60px;
      height: 60px;
      border-radius: 16px;
    }
  }
</style>
