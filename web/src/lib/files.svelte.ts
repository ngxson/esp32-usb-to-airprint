// The Files tab's store: images and PDFs kept in IndexedDB (this browser
// only), with thumbnails.

import * as db from './db';
import type { StoredFile } from './db';
import { imageDpi } from './imageinfo';
import { canvasBlob, cachedSource, forgetSource, isPdf, openSource, renderThumb, type Source } from './source';

export type { StoredFile };

// the list is replaced, never mutated: plain objects, as IndexedDB stores them
class Files {
  list = $state.raw<StoredFile[]>([]);
  loaded = $state(false);
  error = $state('');
}
export const files = new Files();

const THUMB = 360;

// other tabs of this browser share the store: tell them when it changes
const channel = typeof BroadcastChannel === 'function' ? new BroadcastChannel('printer-web-files') : null;
if (channel) channel.onmessage = () => loadFiles();
function changed() {
  channel?.postMessage('changed');
}

export async function loadFiles() {
  try {
    const all = await db.allFiles();
    all.sort((a, b) => b.created - a.created);
    files.list = all;
  } catch (e) {
    files.error = `Cannot open the file store: ${(e as Error).message}`;
  }
  files.loaded = true;
}

export function isSupported(type: string, name: string): boolean {
  return type.startsWith('image/') || isPdf(type, name);
}

export interface NewFile {
  id?: string;
  name: string;
  blob: Blob;
  source: StoredFile['source'];
  scanId?: string;
}

// Reads the file (size, pages, dpi), makes its thumbnail and stores it.
// Throws if the browser cannot read it, or (ConstraintError) if the id exists.
export async function addFile(n: NewFile): Promise<StoredFile> {
  const type = isPdf(n.blob.type, n.name) ? 'application/pdf' : n.blob.type || 'image/jpeg';
  const dpi = type === 'application/pdf' ? null : imageDpi(await n.blob.slice(0, 65536).arrayBuffer());
  const src = await openSource(n.blob, type, dpi);
  try {
    const thumb = await canvasBlob(await renderThumb(src, 0, THUMB), 'image/jpeg', 0.8);
    const first = src.pages[0];
    const f: StoredFile = {
      id: n.id || db.newId(n.source === 'scan' ? 'scan' : 'f'),
      name: n.name,
      type,
      size: n.blob.size,
      created: Date.now(),
      source: n.source,
      blob: n.blob,
      thumb,
      pages: src.pages.length,
      width: src.kind === 'image' ? Math.round((first.w * (dpi || 72)) / 72) : Math.round(first.w),
      height: src.kind === 'image' ? Math.round((first.h * (dpi || 72)) / 72) : Math.round(first.h),
      dpi,
      scanId: n.scanId,
    };
    await db.addFile(f);
    files.list = [f, ...files.list.filter((x) => x.id !== f.id)];
    changed();
    return f;
  } finally {
    src.close();
  }
}

export async function removeFile(id: string) {
  await db.deleteFile(id);
  files.list = files.list.filter((f) => f.id !== id);
  const u = thumbs.get(id);
  if (u) URL.revokeObjectURL(u);
  thumbs.delete(id);
  forgetSource(id);
  changed();
}

export async function renameFile(id: string, name: string) {
  const f = files.list.find((x) => x.id === id);
  if (!f || !name.trim()) return;
  const g = { ...f, name: name.trim() };
  await db.putFile(g);
  files.list = files.list.map((x) => (x.id === id ? g : x));
  changed();
}

// A file another tab stored: into this tab's list.
export async function storedFile(id: string): Promise<StoredFile | null> {
  const f = (await db.getFile(id)) ?? null;
  if (f && !files.list.some((x) => x.id === id)) files.list = [f, ...files.list];
  return f;
}

export function findFile(id: string): StoredFile | undefined {
  return files.list.find((f) => f.id === id);
}

const thumbs = new Map<string, string>();

export function thumbUrl(f: StoredFile): string {
  let u = thumbs.get(f.id);
  if (!u && f.thumb) {
    u = URL.createObjectURL(f.thumb);
    thumbs.set(f.id, u);
  }
  return u || '';
}

export function fileSource(f: StoredFile): Promise<Source> {
  return cachedSource(f.id, () => openSource(f.blob, f.type, f.dpi ?? null));
}

export function download(blob: Blob, name: string) {
  const u = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = u;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(u), 30000);
}

export function formatSize(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1048576) return `${Math.round(n / 1024)} KB`;
  return `${(n / 1048576).toFixed(1)} MB`;
}

export function stamp(d = new Date()): string {
  const z = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${z(d.getMonth() + 1)}-${z(d.getDate())} ${z(d.getHours())}.${z(d.getMinutes())}.${z(d.getSeconds())}`;
}
