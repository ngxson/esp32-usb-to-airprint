// App-wide UI state: toasts, the print dialog, the file viewer, the file picker.

import type { StoredFile } from './db';

export interface ToastAction {
  label: string;
  run: () => void;
}

export interface Toast {
  id: number;
  text: string;
  kind: 'info' | 'ok' | 'error';
  actions: ToastAction[];
}

class Ui {
  toasts = $state<Toast[]>([]);
  printFile = $state.raw<StoredFile | null>(null);
  viewFile = $state.raw<StoredFile | null>(null);
  picker = $state.raw<{ title: string; multi: boolean; resolve: (f: StoredFile[]) => void } | null>(null);
}
export const ui = new Ui();

let seq = 0;

export function toast(text: string, kind: Toast['kind'] = 'info', actions: ToastAction[] = [], ms = 5000) {
  const id = ++seq;
  ui.toasts = [...ui.toasts.slice(-3), { id, text, kind, actions }];
  if (ms) setTimeout(() => dismiss(id), actions.length ? ms * 2 : ms);
  return id;
}

export function dismiss(id: number) {
  ui.toasts = ui.toasts.filter((t) => t.id !== id);
}

export function openPrint(f: StoredFile) {
  ui.printFile = f;
}

export function openViewer(f: StoredFile) {
  ui.viewFile = f;
}

// Lets the user choose files from the Files tab; [] if canceled.
export function pickFiles(title: string, multi = true): Promise<StoredFile[]> {
  return new Promise((resolve) => {
    ui.picker = { title, multi, resolve };
  });
}

export function go(route: string) {
  location.hash = '#/' + route;
}
