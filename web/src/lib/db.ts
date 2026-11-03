// IndexedDB: the browser's file store. Everything stays on this device.
//   files  {id, name, type, size, created, source, blob, thumb, ...}
//   kv     small values (the Process workspace, imported scan ids)

import { t } from './i18n.ts';

const DB_NAME = 'printer-web';
const VERSION = 1;

export interface StoredFile {
  id: string;
  name: string;
  type: string; // MIME
  size: number;
  created: number; // ms
  source: 'upload' | 'scan' | 'process';
  blob: Blob;
  thumb?: Blob;
  pages?: number; // PDF
  width?: number; // image pixels / first PDF page in points
  height?: number;
  dpi?: number | null; // image resolution if known
  scanId?: string;
}

// Keep the message lazy: persistent errors must follow later language changes.
class FileStoreBlockedError extends Error {
  override get message() {
    return t('the file store is open in an older tab; close it and reload');
  }
}

let dbp: Promise<IDBDatabase> | null = null;

function open(): Promise<IDBDatabase> {
  dbp ||= new Promise((resolve, reject) => {
    const r = indexedDB.open(DB_NAME, VERSION);
    r.onupgradeneeded = () => {
      const db = r.result;
      if (!db.objectStoreNames.contains('files')) db.createObjectStore('files', { keyPath: 'id' });
      if (!db.objectStoreNames.contains('kv')) db.createObjectStore('kv');
    };
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
    r.onblocked = () => reject(new FileStoreBlockedError());
  });
  return dbp;
}

function req<T>(r: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}

async function store(name: 'files' | 'kv', mode: IDBTransactionMode): Promise<IDBObjectStore> {
  return (await open()).transaction(name, mode).objectStore(name);
}

export async function allFiles(): Promise<StoredFile[]> {
  return req((await store('files', 'readonly')).getAll() as IDBRequest<StoredFile[]>);
}

export async function getFile(id: string): Promise<StoredFile | undefined> {
  return req((await store('files', 'readonly')).get(id) as IDBRequest<StoredFile | undefined>);
}

// add: fails (ConstraintError) if the id exists, which deduplicates scans
// imported by several tabs at once
export async function addFile(f: StoredFile): Promise<void> {
  await req((await store('files', 'readwrite')).add(f));
}

export async function putFile(f: StoredFile): Promise<void> {
  await req((await store('files', 'readwrite')).put(f));
}

export async function deleteFile(id: string): Promise<void> {
  await req((await store('files', 'readwrite')).delete(id));
}

export async function kvGet<T>(key: string): Promise<T | undefined> {
  return req((await store('kv', 'readonly')).get(key) as IDBRequest<T | undefined>);
}

export async function kvSet<T>(key: string, value: T): Promise<void> {
  await req((await store('kv', 'readwrite')).put(value, key));
}

export function newId(prefix = 'f'): string {
  // crypto.randomUUID needs a secure context, which a LAN http:// page is not
  return `${prefix}-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}
