// web-next/src/lib/usePrefs.ts - the theme and the text size, per device (localStorage; later also per user on the
// Pi). Applied as data-theme / data-text-size on <html> (src/styles/tokens.css). Storage may be blocked (private
// mode, a webview): everything still works, it just is not remembered.
import { ref, watch } from "vue";

export type Theme = "standard" | "contrast";
export type TextSize = 100 | 125 | 150;
export const THEMES: Theme[] = ["standard", "contrast"];
export const TEXT_SIZES: TextSize[] = [100, 125, 150];

const KEY_THEME = "scNextTheme";
const KEY_SIZE = "scNextTextSize";

function read(k: string): string | null {
  try {
    return localStorage.getItem(k);
  } catch {
    return null;
  }
}
function write(k: string, v: string): void {
  try {
    localStorage.setItem(k, v);
  } catch {
    /* not remembered */
  }
}

/** The stored theme, else high contrast when the system asks for more contrast. */
export function initialTheme(stored = read(KEY_THEME), moreContrast = matchMedia?.("(prefers-contrast: more)").matches): Theme {
  if (stored === "standard" || stored === "contrast") return stored;
  return moreContrast ? "contrast" : "standard";
}
export function initialTextSize(stored = read(KEY_SIZE)): TextSize {
  const n = Number(stored);
  return (TEXT_SIZES as number[]).includes(n) ? (n as TextSize) : 100;
}

const theme = ref<Theme>("standard");
const textSize = ref<TextSize>(100);
let started = false;

export function usePrefs(root: HTMLElement = document.documentElement) {
  if (!started) {
    started = true;
    theme.value = initialTheme();
    textSize.value = initialTextSize();
    watch(
      [theme, textSize],
      ([t, s]) => {
        root.dataset.theme = t;
        root.dataset.textSize = String(s);
        write(KEY_THEME, t);
        write(KEY_SIZE, String(s));
      },
      { immediate: true },
    );
  }
  return { theme, textSize };
}
