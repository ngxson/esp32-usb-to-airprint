// Plain TypeScript so the same messages can be used in browser code and Node tests.
import { messages } from './locales/zh-CN.ts';
export { messages };

export type Message = keyof typeof messages;
// One registry drives detection, the picker and catalog selection.
export const localeConfig = {
  en: { label: 'English', language: 'en', messages: null },
  'zh-CN': { label: '简体中文', language: 'zh', messages },
} satisfies Record<string, { label: string; language: string; messages: Record<Message, string> | null }>;
export type Locale = keyof typeof localeConfig;
export const locales = Object.keys(localeConfig) as Locale[];
export const storageKey = 'airprint.language';

export function isLocale(value: string | null): value is Locale {
  return value !== null && Object.prototype.hasOwnProperty.call(localeConfig, value);
}

export function resolveLocale(saved: string | null, languages: readonly string[]): Locale {
  if (isLocale(saved)) return saved;
  for (const language of languages) {
    const match = locales.find((locale) => locale.toLowerCase() === language.toLowerCase())
      ?? locales.find((locale) => localeConfig[locale].language === language.toLowerCase().split('-')[0]);
    if (match) return match;
  }
  return 'en';
}

// The Svelte wrapper supplies its reactive reader; plain Node tests default to English.
let readLocale: () => Locale = () => 'en';
export function setLocaleReader(reader: () => Locale) { readLocale = reader; }

export function translate(locale: Locale, key: Message, params: Record<string, string | number> = {}): string {
  const text = localeConfig[locale].messages?.[key] || key;
  return text.replace(/\{(\w+)\}/g, (placeholder: string, name: string) =>
    Object.prototype.hasOwnProperty.call(params, name) ? String(params[name]) : placeholder);
}

export function t(key: Message, params?: Record<string, string | number>): string {
  return translate(readLocale(), key, params);
}
