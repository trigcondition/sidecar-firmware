// web-next/src/lib/useSidecar.ts - the client and the pairing as Vue state: one connection for the whole app,
// created in App.vue and handed to the pages with provide / inject.
import { inject, onScopeDispose, provide, ref, shallowRef, type InjectionKey, type Ref, type ShallowRef } from "vue";
import { SidecarClient, type ClientState } from "./client";
import { Api } from "./api";
import { Pairing } from "./pairing";
import type { Hello, Levels, Status } from "./protocol";
import type { SeqPos } from "./seq";

export function wsUrl(loc: { protocol: string; host: string } = location): string {
  return `${loc.protocol === "https:" ? "wss:" : "ws:"}//${loc.host}/ws`;
}

export interface Sidecar {
  client: SidecarClient;
  state: Ref<ClientState>;
  hello: ShallowRef<Hello | null>;
  paired: Ref<boolean>;
  status: ShallowRef<Status | null>;
  levels: ShallowRef<Levels | null>;
  /** the play head, pattern and tempo ("seqpos"; the header's pill), when the build serves it */
  seqpos: ShallowRef<SeqPos | null>;
  lastError: Ref<string>;
  pair: (code: string) => Promise<void>;
  /** the HTTP endpoints without a topic (src/lib/api.ts), with this browser's pairing token */
  api: Api;
  pairing: Pairing;
  /** a command; resolves when the Sidecar took it, rejects with its refusal (RequestError) */
  cmd: (c: string) => Promise<void>;
  /** the pairing gate (GET /pair/status): "on", "fixed", "off"; "" not known yet */
  gate: Ref<string>;
  /** this page reaches the Sidecar over the USB-C cable (its network, docs/usb-network.md): trusted, no pairing */
  trusted: Ref<boolean>;
}

const KEY: InjectionKey<Sidecar> = Symbol.for("sidecar"); // the same across hot updates: a new copy of this module finds App's connection

export function provideSidecar(url = wsUrl()): Sidecar {
  const pairing = new Pairing();
  // the session token is reused across reconnects (each resume makes a new session on the Sidecar, which keeps a
  // limited number); a fresh one only when the Sidecar refused it
  const client = new SidecarClient({
    url,
    getToken: async () => pairing.current ?? (await pairing.resume().catch(() => null)),
    refreshToken: () => pairing.resume().catch(() => null),
  });
  const trusted = ref(false);
  const s: Sidecar = {
    client,
    state: ref<ClientState>(client.state),
    hello: shallowRef<Hello | null>(null),
    paired: ref(false),
    status: shallowRef<Status | null>(null),
    levels: shallowRef<Levels | null>(null),
    seqpos: shallowRef<SeqPos | null>(null),
    lastError: ref(""),
    gate: ref(""),
    trusted,
    // on the trusted USB-C cable the header still goes (any value): the Sidecar wants it there, another site cannot send it
    api: new Api({ token: () => pairing.current ?? (trusted.value ? "usb" : null), onPairLost: () => void client.authenticate().catch(() => false) }),
    pairing,
    async cmd(c: string) {
      await client.command(c);
    },
    async pair(code: string) {
      await pairing.pair(code);
      // paired either way; if the socket is down right now, the token goes out when it is back
      await client.authenticate().catch(() => false);
    },
  };
  let offLevels: (() => void) | null = null;
  let offPos: (() => void) | null = null;
  const offs = [
    client.on("state", (v) => (s.state.value = v)),
    client.on("hello", (h) => {
      s.hello.value = h;
      // levels only from a server that serves them (the Pi's later topic; the mock has it)
      if (h.topics.includes("levels") && !offLevels) offLevels = client.subscribe<Levels>("levels", (l) => (s.levels.value = l), { ms: 40 });
      if (h.topics.includes("seqpos") && !offPos) offPos = client.subscribe<SeqPos>("seqpos", (p) => (s.seqpos.value = p), { ms: 250 });
    }),
    client.on("paired", (p) => (s.paired.value = p)),
    client.on("error", (e) => (s.lastError.value = `${e.code} ${e.msg}`)),
    client.subscribe<Status>("status", (v) => (s.status.value = v)),
  ];
  client.connect();

  // While unpaired (the old page's 5 s tick): ask the gate. Off, or this page on the USB-C cable (trusted): the
  // Sidecar takes any token. Otherwise try this
  // browser's saved pairing again (paired elsewhere meanwhile, a reboot). Once paired: the page's clock to the
  // Sidecar (POST /time, gated, as the old page after pairing), once per pairing.
  let timeSent = false;
  async function recheck() {
    if (s.state.value !== "open" || s.paired.value) return;
    try {
      const ps = await s.api.json<{ gate?: string; trusted?: boolean }>("/pair/status");
      s.gate.value = ps.gate ?? "";
      s.trusted.value = ps.trusted === true;
      if (ps.gate === "off" || ps.trusted === true) await client.authenticateAs("");
      else if (pairing.saved()) await client.authenticate();
    } catch {
      /* the next tick */
    }
  }
  const pairTick = setInterval(recheck, 5000);
  client.on("hello", () => void recheck());
  client.on("paired", (p) => {
    if (!p) return void (timeSent = false);
    if (timeSent) return;
    timeSent = true;
    void s.api.post("/time", { epoch: Math.floor(Date.now() / 1000), tz: -new Date().getTimezoneOffset() }).catch(() => (timeSent = false));
  });
  onScopeDispose(() => {
    clearInterval(pairTick);
    offs.forEach((f) => f());
    offLevels?.();
    offPos?.();
    client.close();
  });
  provide(KEY, s);
  return s;
}

export function useSidecar(): Sidecar {
  const s = inject(KEY);
  if (!s) throw new Error("useSidecar outside App (provideSidecar)");
  return s;
}
