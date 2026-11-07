// web-next/src/lib/wifi.ts - the Wi-Fi join's input check, the same as the Sidecar's (pi/engine/wifi_conf.cpp
// check_join), so a typo is caught before the Pi restarts for it: a name of 1..32 bytes without control characters; a
// password of 8..63 printable characters or 64 hex digits; an open network only when asked, and then no password.
export function checkJoin(ssid: string, psk: string, open: boolean): string | null {
  const bytes = new TextEncoder().encode(ssid).length;
  if (bytes < 1 || bytes > 32) return "The network name is 1 to 32 bytes.";
  if (/[\u0000-\u001f\u007f]/.test(ssid)) return "The network name has a control character.";
  if (open) return psk ? "An open network takes no password." : null;
  if (/^[0-9a-fA-F]{64}$/.test(psk)) return null;
  if (psk.length < 8 || psk.length > 63) return "The password is 8 to 63 characters (or 64 hex digits).";
  if (!/^[\x20-\x7e]+$/.test(psk)) return "The password has a character that is not printable ASCII.";
  return null;
}

/** Signal bars 0..4 from an RSSI in dBm (the old page's steps). */
export function bars(rssi: number): number {
  return rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
}

export const JOIN_REASON: Record<string, string> = {
  wrong_password: "the password was wrong",
  not_found: "the network was not found",
  no_dhcp: "it connected but got no address (DHCP)",
  not_started: "the Wi-Fi chip did not start",
};
