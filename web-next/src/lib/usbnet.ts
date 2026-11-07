// web-next/src/lib/usbnet.ts - the USB-C network's rows for Settings › USB-C (docs/usb-network.md), from /status's
// "net" and "usbNet" ({function, link, setting, ip, computer, trusted, late, why}). null: no network function (an
// older build, usbnet=off, OVERBRIDGE mode), nothing to show.
export function usbNetRows(status: unknown): [string, string][] | null {
  const s = (status ?? {}) as Record<string, any>;
  const u = s.usbNet as Record<string, any> | undefined;
  if (!u || u.function !== true) return null;
  const names: Record<string, string> = { wifi: "Wi-Fi", ethernet: "Ethernet", none: "none" };
  const rows: [string, string][] = [];
  if (s.net === "usb") {
    rows.push(["Network", "this USB-C cable"]);
    rows.push(["Address", `${u.ip ?? "–"} · the computer ${u.computer ?? "–"}`]);
    rows.push(["Link", u.link ? "up" : "down (the computer's network driver is not running)"]);
    rows.push(["Pairing", u.trusted ? "not needed on the cable" : "asked on the cable too (usbtrust=0)"]);
  } else {
    rows.push(["Network", `${names[s.net] ?? s.net ?? "–"} this boot: the cable's network is off until a reboot chooses it`]);
  }
  rows.push(["Setting", `net=${u.setting ?? "auto"}${u.why ? ` · ${u.why}` : ""}`]);
  return rows;
}
