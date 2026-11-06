// web-next/src/lib/coalesce.ts - commands from a dial or a fader: while one value of a control is on its way, only the
// newest next one is kept and sent when the first is answered. Turning a dial by keyboard (a command per key press) or
// dragging then never runs into the Sidecar's 20 requests a second, and the last value always arrives. The control is
// the command without its trailing value ("ins=V1:eq:hpf:40" -> "ins=V1:eq:hpf"). Pure (coalesce.test.ts).

/** "ins=V1:eq:hpf:40" -> "ins=V1:eq:hpf"; a command without a numeric tail is its own control. */
export function controlOf(c: string): string {
  const m = /^(.*[:=])-?\d+$/.exec(c);
  return m ? m[1].slice(0, -1) : c;
}

export function coalescer(send: (c: string) => Promise<void>, onError?: (e: unknown) => void) {
  const busy = new Set<string>();
  const next = new Map<string, string>();
  async function run(key: string, c: string): Promise<void> {
    busy.add(key);
    try {
      await send(c);
    } catch (e) {
      onError?.(e);
    }
    const n = next.get(key);
    next.delete(key);
    if (n !== undefined) return run(key, n);
    busy.delete(key);
  }
  return (c: string): Promise<void> => {
    const key = controlOf(c);
    if (busy.has(key)) {
      next.set(key, c); // replaces an older waiting value
      return Promise.resolve();
    }
    return run(key, c);
  };
}
