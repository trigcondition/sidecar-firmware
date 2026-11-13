// web-next/src/lib/usbnet.test.ts - Settings › USB-C's network rows from /status.
import { describe, expect, it } from "vitest";
import { usbNetRows } from "./usbnet";

const onCable = { net: "usb", usbNet: { function: true, link: true, setting: "auto", ip: "172.31.77.1", computer: "172.31.77.2", trusted: true, late: false, why: "a computer started its USB-C network" } };

describe("usbNetRows", () => {
  it("shows nothing without the network function (an older build, usbnet=off)", () => {
    expect(usbNetRows(null)).toBeNull();
    expect(usbNetRows({ net: "wifi" })).toBeNull();
    expect(usbNetRows({ net: "wifi", usbNet: { function: false } })).toBeNull();
  });

  it("on the cable: the addresses, the link and the trust", () => {
    const r = Object.fromEntries(usbNetRows(onCable)!);
    expect(r.Network).toBe("this USB-C cable");
    expect(r.Address).toBe("172.31.77.1 · the computer 172.31.77.2");
    expect(r.Link).toBe("up");
    expect(r.Pairing).toBe("not needed on the cable");
    expect(r.Setting).toBe("net=auto · a computer started its USB-C network");
  });

  it("on the cable with usbtrust=0 and the link down", () => {
    const r = Object.fromEntries(usbNetRows({ net: "usb", usbNet: { ...onCable.usbNet, link: false, trusted: false } })!);
    expect(r.Link).toMatch(/^down/);
    expect(r.Pairing).toMatch(/usbtrust=0/);
  });

  it("on Wi-Fi: the cable's network is off this boot, no cable addresses", () => {
    const r = Object.fromEntries(usbNetRows({ net: "wifi", usbNet: { ...onCable.usbNet, trusted: false, setting: "wifi", why: "" } })!);
    expect(r.Network).toMatch(/^Wi-Fi this boot/);
    expect(r.Address).toBeUndefined();
    expect(r.Setting).toBe("net=wifi");
  });
});
