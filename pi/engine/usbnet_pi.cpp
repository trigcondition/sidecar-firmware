// pi/engine/usbnet_pi.cpp - the USB-C network on the Pi (usbnet_pi.h, docs/usb-network.md)
#include "usbnet_pi.h"
#include "sidecar_gadget.h"

#include <circle/logger.h>
#include <circle/net/socket.h>
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/sched/scheduler.h>
#include <string.h>

static const char From[] = "usbnet";

// ---- the net device ----------------------------------------------------------------------------------------------

CUsbNetDevice::CUsbNetDevice (CSidecarGadget *pGadget)
:	m_pGadget (pGadget), m_MAC (pGadget->NetPiMac ())
{
	AddNetDevice ();
}

// without the link a frame is dropped at once (TRUE): Circle's queue does not fill up while the computer is away
boolean CUsbNetDevice::IsSendFrameAdvisable (void)
{
	return !m_pGadget->NetLive () || m_pGadget->NetRoom ();
}

boolean CUsbNetDevice::SendFrame (const void *pBuffer, unsigned nLength)
{
	if (!m_pGadget->NetLive ()) return TRUE;
	return m_pGadget->NetSend (pBuffer, nLength);
}

boolean CUsbNetDevice::ReceiveFrame (void *pBuffer, unsigned *pResultLength)
{
	return m_pGadget->NetReceive (pBuffer, pResultLength);
}

boolean CUsbNetDevice::IsLinkUp (void)
{
	return m_pGadget->NetUp ();
}

// ---- the DHCP server ---------------------------------------------------------------------------------------------

CUsbDhcpd::CUsbDhcpd (CNetSubSystem *pNet, const u8 Server[4], const u8 Client[4])
:	m_pNet (pNet)
{
	memcpy (m_Cfg.server, Server, 4);
	memcpy (m_Cfg.client, Client, 4);
	memcpy (m_Cfg.mask, usbnet::kMask, 4);
	m_Cfg.lease_s = 3600;
	SetName ("usbdhcpd");
}

// UDP 67; every answer broadcast (the computer has no address yet, and no ARP answer for the one it is offered).
// One computer, one address: the cable has two ends.
void CUsbDhcpd::Run (void)
{
	while (!m_pNet->IsRunning ()) CScheduler::Get ()->MsSleep (100);
	CSocket Sock (m_pNet, IPPROTO_UDP);
	if (Sock.Bind (67) < 0 || Sock.SetOptionBroadcast (TRUE) < 0)
	{
		CLogger::Get ()->Write (From, LogError, "DHCP server: UDP 67 not bound: the computer needs a manual address");
		return;
	}
	CIPAddress Bcast;
	Bcast.SetBroadcast ();
	static u8 In[FRAME_BUFFER_SIZE];
	static u8 Out[dhcpd::kMaxReply];
	unsigned nAnswers = 0;
	for (;;)
	{
		CIPAddress Sender;
		u16 nPort;
		int n = Sock.ReceiveFrom (In, sizeof In, 0, &Sender, &nPort);
		if (n <= 0 || nPort != 68) continue;
		size_t r = dhcpd::reply (m_Cfg, In, (size_t) n, Out, sizeof Out);
		if (r == 0) continue;
		if (Sock.SendTo (Out, (unsigned) r, MSG_DONTWAIT, Bcast, 68) < 0) continue;
		if (nAnswers++ < 4 && Out[242] == dhcpd::kAck)	// the first few leases, no MAC (option 53 is first in our answers)
			CLogger::Get ()->Write (From, LogNotice, "DHCP: the computer has %u.%u.%u.%u", m_Cfg.client[0], m_Cfg.client[1],
						m_Cfg.client[2], m_Cfg.client[3]);
	}
}

// ---- the stack's interface and the trust rule --------------------------------------------------------------------

namespace usbnet_pi {

static usbnet::Iface s_Iface = usbnet::kIfNone;
static boolean s_bTrust = FALSE;
static u8 s_IP[4], s_Client[4];
static const char *s_pWhy = "";
static usbnet::Net s_Net = usbnet::kAuto;

void Use (usbnet::Iface Iface, const usbnet::Options &rOpt, const char *pWhy)
{
	s_Iface = Iface;
	s_bTrust = rOpt.trust;
	s_Net = rOpt.net;
	memcpy (s_IP, rOpt.ip, 4);
	usbnet::client_ip (rOpt.ip, s_Client);
	s_pWhy = pWhy ? pWhy : "";
}

boolean Trusted (void)
{
	return usbnet::trusted (s_Iface, s_bTrust);
}

usbnet::Iface Iface (void)
{
	return s_Iface;
}

void Status (CString *pOut, boolean bFunction, boolean bLinkUp)
{
	static const char *const Names[] = {"none", "ethernet", "wifi", "usb"};
	CString s;
	s.Format (",\"net\":\"%s\",\"usbNet\":{\"function\":%s,\"link\":%s,\"setting\":\"%s\",\"ip\":\"%u.%u.%u.%u\","
		  "\"computer\":\"%u.%u.%u.%u\",\"trusted\":%s,\"why\":\"%s\"}",
		  Names[s_Iface <= usbnet::kIfUsb ? s_Iface : 0], bFunction ? "true" : "false", bLinkUp ? "true" : "false",
		  usbnet::net_name (s_Net), s_IP[0], s_IP[1], s_IP[2], s_IP[3], s_Client[0], s_Client[1], s_Client[2], s_Client[3],
		  Trusted () ? "true" : "false", s_pWhy);
	pOut->Append (s);
}

}	// namespace usbnet_pi
