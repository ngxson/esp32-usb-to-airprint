import { resolveLocale, setLocaleReader, storageKey, type Locale } from './i18n.ts';
export { t } from './i18n.ts';

function initialLocale(): Locale {
  let saved: string | null = null;
  try { saved = localStorage.getItem(storageKey); } catch { /* storage may be disabled */ }
  return resolveLocale(saved, typeof navigator === 'undefined' ? [] : navigator.languages);
}

export const i18n = $state<{ locale: Locale }>({ locale: initialLocale() });
setLocaleReader(() => i18n.locale);

export function setLocale(locale: Locale) {
  i18n.locale = locale;
  try { localStorage.setItem(storageKey, locale); } catch { /* switching still works without storage */ }
}
