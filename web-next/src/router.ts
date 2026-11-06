// web-next/src/router.ts - the app's pages. Hash history: the Pi serves /next/ as static files and needs no
// fallback route for /next/mix etc.
import { createRouter, createWebHashHistory, type RouteRecordRaw } from "vue-router";
import Home from "./pages/Home.vue";

export const routes: RouteRecordRaw[] = [
  { path: "/", name: "home", component: Home, meta: { title: "Home" } },
  { path: "/voices", name: "voices", component: () => import("./pages/Voices.vue"), meta: { title: "Voices" } },
  { path: "/mix", name: "mix", component: () => import("./pages/Mix.vue"), meta: { title: "Mix" } },
  { path: "/morph", name: "morph", component: () => import("./pages/Morph.vue"), meta: { title: "Morph" } },
  { path: "/seq", name: "seq", component: () => import("./pages/Seq.vue"), meta: { title: "Seq" } },
  { path: "/rec", name: "rec", component: () => import("./pages/Rec.vue"), meta: { title: "Rec" } },
  { path: "/library", name: "library", component: () => import("./pages/Library.vue"), meta: { title: "Library" } },
  { path: "/settings", name: "settings", component: () => import("./pages/Settings.vue"), meta: { title: "Settings" } },
  { path: "/:rest(.*)*", redirect: "/" },
];

export const router = createRouter({ history: createWebHashHistory(), routes });

// the page title follows the route, and focus moves to the new page's heading (keyboard and screen readers)
router.afterEach((to) => {
  document.title = `Sidecar · ${String(to.meta.title ?? "")}`;
  requestAnimationFrame(() => document.querySelector<HTMLElement>("main h1")?.focus({ preventScroll: true }));
});
