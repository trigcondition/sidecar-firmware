/// <reference types="vite/client" />
declare module "*.vue" {
  import type { DefineComponent } from "vue";
  const component: DefineComponent<object, object, unknown>;
  export default component;
}

/** The build's stamp (vite.config.ts): the release in ../VERSION and the commit it was built from */
declare const __WEB_BUILD__: { version: string; commit: string; dirty: boolean };
