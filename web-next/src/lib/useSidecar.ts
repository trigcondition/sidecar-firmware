// web-next/src/lib/useSidecar.ts - the client and the pairing as Vue state: one connection for the whole app,
// created in App.vue and handed to the pages with provide / inject.
import { inject, onScopeDispose, provide, ref, shallowRef, type InjectionKey, type Ref, type ShallowRef } from "vue";
import { SidecarClient, type ClientState } from "./client";
import { Api } from "./api";
import { Pairing } from "./pairing";
import type { Hello, Levels, Status } from "./protocol";

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
  lastError: Ref<string>;
  pair: (code: string) => Promise<void>;
  /** the HTTP endpoints without a topic (src/lib/api.ts), with this browser's pairing token */
  api: Api;
  pairing: Pairing;
  /** a command; resolves when the Sidecar took it, rejects with its refusal (RequestError) */
  cmd: (c: string) => Promise<void>;
}

const KEY: InjectionKey<Sidecar> = Symbol("sidecar");

export function provideSidecar(url = wsUrl()): Sidecar {
  const pairing = new Pairing();
  // the session token is reused across reconnects (each resume makes a new session on the Sidecar, which keeps a
  // limited number); a fresh one only when the Sidecar refused it
  const client = new SidecarClient({
    url,
    getToken: async () => pairing.current ?? (await pairing.resume().catch(() => null)),
    refreshToken: () => pairing.resume().catch(() => null),
  });
  const s: Sidecar = {
    client,
    state: ref<ClientState>(client.state),
    hello: shallowRef<Hello | null>(null),
    paired: ref(false),
    status: shallowRef<Status | null>(null),
    levels: shallowRef<Levels | null>(null),
    lastError: ref(""),
    api: new Api({ token: () => pairing.current, onPairLost: () => void client.authenticate().catch(() => false) }),
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
  const offs = [
    client.on("state", (v) => (s.state.value = v)),
    client.on("hello", (h) => {
      s.hello.value = h;
      // levels only from a server that serves them (the Pi's later topic; the mock has it)
      if (h.topics.includes("levels") && !offLevels) offLevels = client.subscribe<Levels>("levels", (l) => (s.levels.value = l), { ms: 40 });
    }),
    client.on("paired", (p) => (s.paired.value = p)),
    client.on("error", (e) => (s.lastError.value = `${e.code} ${e.msg}`)),
    client.subscribe<Status>("status", (v) => (s.status.value = v)),
  ];
  client.connect();
  onScopeDispose(() => {
    offs.forEach((f) => f());
    offLevels?.();
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
