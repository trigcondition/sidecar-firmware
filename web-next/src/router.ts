// web-next/src/router.ts - the app's pages. Hash history: the Pi serves /next/ as static files and needs no
// fallback route for /next/mix etc.
import { createRouter, createWebHashHistory, type RouteRecordRaw } from "vue-router";
import Home from "./pages/Home.vue";

// a page of sections (ui/SubPage.vue): /<page> opens its first section, each section its own URL (/library/pool);
// meta.needs hides a section the Sidecar's build lacks (lib/box.ts)
const SubPage = () => import("./ui/SubPage.vue");
type Section = { path: string; title: string; component: RouteRecordRaw["component"]; needs?: string };
function sections(path: string, name: string, title: string, list: Section[]): RouteRecordRaw {
  return {
    path,
    name,
    component: SubPage,
    meta: { title },
    redirect: `${path}/${list[0].path}`,
    children: list.map((c) => ({ path: c.path, component: c.component!, meta: { title: c.title, needs: c.needs } })),
  };
}

export const routes: RouteRecordRaw[] = [
  { path: "/", name: "home", component: Home, meta: { title: "Home" } },
  sections("/voices", "voices", "Voices", [
    { path: "voice", title: "Voice", component: () => import("./pages/voices/VoiceTab.vue") },
    { path: "modulation", title: "Modulation", component: () => import("./pages/voices/ModulationTab.vue") },
    { path: "machines", title: "User machines", component: () => import("./pages/voices/MachinesCard.vue"), needs: "userslots" },
    { path: "presets", title: "Presets", component: () => import("./pages/voices/PresetsCard.vue") },
  ]),
  sections("/mix", "mix", "Mix", [
    { path: "mixer", title: "Mixer", component: () => import("./pages/mix/MixerTab.vue") },
    { path: "master", title: "Master FX", component: () => import("./pages/mix/MasterTab.vue") },
    { path: "sends", title: "Send FX", component: () => import("./pages/mix/SendsTab.vue"), needs: "sendfx" },
    { path: "sidechain", title: "Sidechain", component: () => import("./pages/mix/SidechainTab.vue") },
  ]),
  { path: "/morph", name: "morph", component: () => import("./pages/Morph.vue"), meta: { title: "Morph" } },
  { path: "/seq", name: "seq", component: () => import("./pages/Seq.vue"), meta: { title: "Seq" } },
  sections("/rec", "rec", "Rec", [
    { path: "recorder", title: "Recorder", component: () => import("./pages/rec/RecorderCard.vue") },
    { path: "takes", title: "Takes", component: () => import("./pages/rec/TakesCard.vue") },
    { path: "sample", title: "Sample", component: () => import("./pages/rec/SamplersCard.vue") },
  ]),
  sections("/library", "library", "Library", [
    { path: "pool", title: "Pool", component: () => import("./pages/library/PoolCard.vue"), needs: "pools" },
    { path: "files", title: "Files", component: () => import("./pages/library/FilesCard.vue") },
    { path: "projects", title: "Projects", component: () => import("./pages/library/ProjectsCard.vue"), needs: "project" },
  ]),
  sections("/settings", "settings", "Settings", [
    { path: "look", title: "Look", component: () => import("./pages/settings/LookCard.vue") },
    { path: "audio", title: "Audio", component: () => import("./pages/settings/AudioCard.vue") },
    { path: "usb", title: "USB-C", component: () => import("./pages/settings/UsbCard.vue") },
    { path: "display", title: "Display", component: () => import("./pages/settings/DisplayCard.vue") },
    { path: "storage", title: "Storage", component: () => import("./pages/settings/StorageCard.vue") },
    { path: "wifi", title: "Wi-Fi", component: () => import("./pages/settings/WifiCard.vue") },
    { path: "access", title: "Access", component: () => import("./pages/settings/AccessCard.vue") },
    { path: "bridge", title: "MIDI bridge", component: () => import("./pages/settings/BridgeCard.vue") },
    { path: "about", title: "About", component: () => import("./pages/settings/AboutCard.vue") },
  ]),
  { path: "/:rest(.*)*", redirect: "/" },
];

export const router = createRouter({ history: createWebHashHistory(), routes });

// the page title follows the route, and focus moves to the new page's heading (keyboard and screen readers); with
// ?show=<tile> (the power banner's "Details") to that Home tile instead, scrolled into view
router.afterEach((to, from) => {
  document.title = ["Sidecar", ...to.matched.map((m) => m.meta.title).filter(Boolean)].join(" · ");
  // between sections of one page the focus stays on the section tab just chosen
  if (to.matched.length > 1 && to.matched[0].path === from.matched[0]?.path) return;
  requestAnimationFrame(() => {
    const tile = typeof to.query.show === "string" ? document.getElementById(`tile-${to.query.show}`) : null;
    if (tile) tile.scrollIntoView({ block: "center" });
    (tile ?? document.querySelector<HTMLElement>("main h1"))?.focus({ preventScroll: true });
  });
});
