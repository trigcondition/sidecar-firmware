// web-next/src/lib/pending.ts - a setting sent as a command and confirmed by the status, as the old page's "pend"
// (pi/engine/www/index.html): the choice shows at once as pending; when the status reports the new value within
// `timeoutMs` it is "saved", otherwise (or when the command is refused) the old value comes back with a message.
// Framework-free state machine (pending.test.ts); useSetting wraps it for Vue.
import { computed, onScopeDispose, ref, watch, type Ref } from "vue";

export type PendState = "idle" | "pending" | "saved" | "failed";

export interface PendingOptions<T> {
  /** the value the Sidecar reports now (from the status) */
  read: () => T | undefined;
  /** the command line for a value */
  toCmd: (v: T) => string;
  send: (c: string) => Promise<void>;
  timeoutMs?: number;
  savedMs?: number;
  now?: () => number;
}

export class Pending<T> {
  state: PendState = "idle";
  want: T | undefined;
  msg = "";
  private since = 0;
  private savedAt = 0;
  private gen = 0;

  constructor(private readonly o: PendingOptions<T>) {}

  get value(): T | undefined {
    return this.state === "pending" ? this.want : this.o.read();
  }

  async set(v: T): Promise<void> {
    const g = ++this.gen;
    this.want = v;
    this.state = "pending";
    this.msg = "";
    this.since = (this.o.now ?? Date.now)();
    try {
      await this.o.send(this.o.toCmd(v));
    } catch (e) {
      if (g !== this.gen) return;
      this.state = "failed";
      this.msg = `Not taken: ${(e as Error).message}`;
      return;
    }
    this.check();
  }

  /** Call on every status (and on a timer): settles pending into saved or failed, and saved back to idle. */
  check(): void {
    const now = (this.o.now ?? Date.now)();
    const timeout = this.o.timeoutMs ?? 2000;
    if (this.state === "pending") {
      if (same(this.o.read(), this.want)) {
        this.state = "saved";
        this.savedAt = now;
      } else if (now - this.since > timeout) {
        this.state = "failed";
        this.msg = "The Sidecar did not take it; it shows what it has now.";
      }
    } else if (this.state === "saved" && now - this.savedAt > (this.o.savedMs ?? 1500)) this.state = "idle";
  }
}

function same(a: unknown, b: unknown): boolean {
  return a === b || JSON.stringify(a) === JSON.stringify(b);
}

/** Pending as Vue state, re-checked when the status changes and twice a second. */
export function useSetting<T>(status: Ref<unknown>, o: Omit<PendingOptions<T>, "read"> & { read: () => T | undefined }) {
  const p = new Pending<T>(o);
  const tick = ref(0);
  const bump = () => {
    p.check();
    tick.value++;
  };
  watch(status, bump);
  const timer = setInterval(bump, 500);
  onScopeDispose(() => clearInterval(timer));
  const value = computed(() => (tick.value, p.value));
  const state = computed(() => (tick.value, p.state));
  const msg = computed(() => (tick.value, p.msg));
  return {
    value,
    state,
    msg,
    set: async (v: T) => {
      const r = p.set(v);
      tick.value++;
      await r;
      tick.value++;
    },
    stop: () => clearInterval(timer),
  };
}
