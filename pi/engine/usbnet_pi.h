// pi/engine/usbnet_pi.h - the USB-C network on the Pi (docs/usb-network.md): Circle's side of it. The engine runs one
// network stack on one device (Circle); at boot the kernel picks the device (usbnet_cfg.h's plan and probe): a computer
// on USB-C whose network driver started gets the USB-C network, else the network of today (Wi-Fi or Ethernet).
//   CUsbNetDevice  the gadget's CDC-NCM function as Circle's net device (type NetDeviceTypeUnknown: CNetSubSystem picks
//                  it by that type, so Circle's network code needs no patch).
//   CUsbDhcpd      the cable's DHCP server: the computer gets the other address of the /24, no router, no DNS.
//   Use / Trusted  which interface the stack runs on, and the trust rule (usbnet::trusted): a request is trusted when it
//                  arrived on the USB-C cable. With one stack on one device every connection arrived on the stack's
//                  interface, so the interface is the stack's, never a source address (that could be spoofed).
#ifndef _usbnet_pi_h
#define _usbnet_pi_h

#include <circle/netdevice.h>
#include <circle/macaddress.h>
#include <circle/sched/task.h>
#include <circle/net/netsubsystem.h>
#include <circle/string.h>
#include <circle/types.h>

#include "usbnet_cfg.h"
#include "dhcpd_lite.h"

class CSidecarGadget;

class CUsbNetDevice : public CNetDevice
{
public:
	CUsbNetDevice (CSidecarGadget *pGadget);	// registers itself (AddNetDevice)

	TNetDeviceType GetType (void) override			{ return NetDeviceTypeUnknown; }
	const CMACAddress *GetMACAddress (void) const override	{ return &m_MAC; }
	boolean IsSendFrameAdvisable (void) override;
	boolean SendFrame (const void *pBuffer, unsigned nLength) override;
	boolean ReceiveFrame (void *pBuffer, unsigned *pResultLength) override;
	boolean IsLinkUp (void) override;
	// NCM hands over every frame the computer sends: no filter to set, the stack filters by destination. Circle's
	// default answers FALSE, and CIGMPHandler asserts on JoinLocalGroup's result the first time the link is up (the
	// Pi 5 halted there on 2026-11-07, the watchdog reset it 15 s later)
	boolean SetMulticastFilter (const u8 Groups[][MAC_ADDRESS_SIZE]) override	{ return TRUE; }

private:
	CSidecarGadget *m_pGadget;
	CMACAddress m_MAC;
};

class CUsbDhcpd : public CTask
{
public:
	CUsbDhcpd (CNetSubSystem *pNet, const u8 Server[4], const u8 Client[4]);
	void Run (void) override;

private:
	CNetSubSystem *m_pNet;
	dhcpd::Config m_Cfg;
};

namespace usbnet_pi {

// the boot's choice, once the stack runs (kernel.cpp): the interface, the trust setting, the cable's addresses
void Use (usbnet::Iface Iface, const usbnet::Options &rOpt, const char *pWhy);

// a request on this stack comes from a trusted interface (the pairing gate is skipped: web.cpp, sdfiles.cpp)
boolean Trusted (void);

usbnet::Iface Iface (void);

// the gadget whose late link the page's button reacts to (kernel.cpp, once); Late: a computer started the USB-C
// network after the boot chose Wi-Fi or Ethernet (CSidecarGadget::NetLate)
void Gadget (CSidecarGadget *pGadget);
boolean Late (void);

// the page's "Reboot onto USB-C" (POST /usbnet/reboot, web.cpp): accepted only while Late; the kernel's loop takes it
boolean RequestReboot (void);
boolean TakeReboot (void);

// /status: ,"net":"usb|wifi|ethernet","usbNet":{...} (no MAC addresses: the computer's is personal enough)
void Status (CString *pOut, boolean bFunction, boolean bLinkUp, boolean bLate);

}	// namespace usbnet_pi

#endif
