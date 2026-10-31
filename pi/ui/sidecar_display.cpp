// pi/ui/sidecar_display.cpp - see sidecar_display.h. Based on Circle's addon/lvgl/lvgl.cpp (GPL-3.0, R. Stange),
// changed: the touch screen and mouse are bound late (they enumerate after USB's plug-and-play), raw touch values are
// scaled here (Circle's USB touch driver reports the HID logical range unscaled), synchronous flushes, a tick callback,
// the firmware's DSI display preferred and its touch (CRPiTouchScreen) as a second touch source; on the Pi 5 the DSI
// display driven by Circle's addon/rp1dsi instead (docs/pi-display.md, docs/pi5.md).
#include "sidecar_display.h"
#include "sidecar_seqed.h"		// the second finger: the SEQ page's pinch
#include "sidecar_splash.h"
#include "sidecar_dot.h"		// the OLED theme's dot expansion (Flush)
#include "lvgl.h"
#include <circle/bcmpropertytags.h>
#include <circle/devicenameservice.h>
#include <circle/synchronize.h>
#include <circle/input/mouse.h>
#if RASPPI >= 5
#include <rp1dsi/rpitouchscreen.h>		// the Pi 5's DSI display and its touch (Circle's addon/rp1dsi, GPL-3.0)
#include <circle/i2cmaster.h>
#include <circle/interrupt.h>
#else
#include <circle/input/rpitouchscreen.h>	// the firmware's DSI touch buffer (Pi 4)
#endif
#include <circle/input/touchscreen.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/new.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/usb/usbtouchscreen.h>
#include <circle/util.h>

LOGMODULE ("display");

CSidecarDisplay *CSidecarDisplay::s_pThis = 0;

struct TSidecarDisplayHooks
{
	static void Flush (lv_display_t *pDisplay, const lv_area_t *pArea, u8 *pBuffer);
	static void PointerRead (lv_indev_t *pIndev, lv_indev_data_t *pData);
	static u32 Tick (void);
	static void Log (lv_log_level_t Level, const char *pMsg);
	static void TouchHandler (TTouchScreenEvent Event, unsigned nID, unsigned nPosX, unsigned nPosY);
	static void DsiTouchHandler (TTouchScreenEvent Event, unsigned nID, unsigned nPosX, unsigned nPosY);
	static void MouseHandler (TMouseEvent Event, unsigned nButtons, unsigned nPosX, unsigned nPosY, int nWheel);
	static void TouchRemoved (CDevice *pDevice, void *pContext);
	static void MouseRemoved (CDevice *pDevice, void *pContext);
};
typedef TSidecarDisplayHooks H;

CSidecarDisplay::CSidecarDisplay (unsigned nWidth, unsigned nHeight)
:	m_nReqWidth (nWidth),
	m_nReqHeight (nHeight),
	m_nUiReqW (0),
	m_nUiReqH (0),
	m_nEdidW (0),
	m_nEdidH (0),
	m_nOffX (0),
	m_nOffY (0),
	m_pFB (0),
	m_pOut (0),
	m_nDisplay (0),
	m_bDsiSeen (FALSE),
	m_bDsiUnknown (FALSE),
	m_bFrameBuffer (FALSE),
	m_bRp1Tried (FALSE),
	m_bOn (FALSE),
	m_nWidth (0),
	m_nHeight (0),
	m_nFbWidth (0),
	m_nFbHeight (0),
	m_bScaled (FALSE),
	m_pXMap (0),
	m_pScaleBuf (0),
	m_pSplash (0),
	m_pBuffer (0),
	m_pDotBuf (0),
	m_nLastUpdate (0),
	m_nMaxUs (0),
	m_pTouch (0),
	m_pMouse (0),
	m_pRPiTouch (0),
	m_pDsiTouch (0),
	m_nDsiInvert (0),
	m_pSource (0),
	m_pCalibration (0),
	m_nRawMax (0),
	m_nLogMaxX (0),
	m_nLogMaxY (0),
	m_bTouchCal (FALSE),
	m_TouchCal (),
	m_nDownSeq (0),
	m_nReadDownSeq (0),
	m_nShownDownSeq (0),
	m_bLoggedDown (FALSE),
	m_nUsbContacts (0),
	m_nPointer (0),
	m_nRaw (0),
	m_nTouchSeq (0),
	m_nEvents (0),
	m_nPointer2 (0),
	m_nTouchSeq2 (0),
	m_nDiagEvents (0),
	m_nDiagReports (0),
	m_nDiagResets (0),
	m_nShownSeq (0),
	m_nShownSeq2 (0),
	m_nSaverMin (60),
	m_nLastActive (0),
	m_bBlank (FALSE),
	m_bSwallow (FALSE),
	m_bStatusNew (FALSE)
{
	memset (&m_Status, 0, sizeof m_Status);
	for (unsigned c = 0; c < SIDECAR_UI_CORES; c++) m_Status.load[c] = -1;
	s_pThis = this;
#if RASPPI >= 5
	m_nDsiDepth = 16;
	m_pConv = 0;
	m_nRot = 0;
	m_pYMap = 0;
#endif
}

// The firmware's displays (Pi 4: max_framebuffers=2 in config.txt gives each its frame buffer): "get display id" per
// display number -> 0 the DSI LCD (the official 7" display or a compatible), 2 HDMI0, 7 HDMI1, 3 composite
#define PROPTAG_GET_DISPLAY_ID	0x00040016
static const unsigned DisplayIdDsi = 0;

static const char *DisplayName (unsigned nId)
{
	switch (nId)
	{
	case 0:	return "DSI";
	case 1:	return "DSI-aux";
	case 2:	return "HDMI0";
	case 3:	return "composite";
	case 7:	return "HDMI1";
	case ~0u: return "?";
	default: return "other";
	}
}

// the frame buffer, once: on the DSI display if the firmware reports one (cmdline dsi=0: display 0, as before), and the
// firmware's report logged: "display: firmware 2 display(s): 0=HDMI0 1=DSI -> frame buffer on 1 (DSI) 800x480"
boolean CSidecarDisplay::OpenFrameBuffer (void)
{
	if (m_bFrameBuffer) return TRUE;
	if (m_pFB) return FALSE;				// tried, failed
#if RASPPI >= 5
	if (!m_bRp1Tried)					// the DSI display first (once), else HDMI
	{
		m_bRp1Tried = TRUE;
		if (OpenRp1Dsi ()) return TRUE;
	}
#endif
	unsigned nDisplays = CBcmFrameBuffer::GetNumDisplays ();
#if RASPPI >= 5
	// The Pi 5's firmware sets up HDMI itself at power-on and reports 0 displays when it saw none then (measured on
	// the Pi 5 with the Waveshare HDMI LCD: "firmware 0 display(s)"); Circle then makes no frame buffer. Not marked
	// as tried, so Initialize asks again (Circle asks the firmware anew while it answers 0). The fix is in config.txt:
	// hdmi_force_hotplug=1 (docs/pi5.md, "HDMI display").
	if (nDisplays == 0)
	{
		CBcmPropertyTags Tags;
		TPropertyTagDisplayDimensions Dim;
		boolean bDim = Tags.GetTag (PROPTAG_GET_DISPLAY_DIMENSIONS, &Dim, sizeof Dim);
		LOGWARN ("firmware reports no display (dimensions %s %ux%u): HDMI0 (next to USB-C), on before power-on, "
			 "config.txt hdmi_force_hotplug=1", bDim ? "answered" : "not answered",
			 bDim ? Dim.nWidth : 0, bDim ? Dim.nHeight : 0);
		return FALSE;
	}
	// The frame buffer's size is the firmware's (Circle on the Pi 5 cannot set it; measured: the Waveshare LCD gets
	// 1920x1080, config.txt's framebuffer_width/height and hdmi_cvt ignored). cmdline.txt's width=/height= are the
	// UI's size instead (ScaleSetup), so they are not passed to Circle.
	unsigned nUiWidth = m_nReqWidth, nUiHeight = m_nReqHeight;
	m_nReqWidth = m_nReqHeight = 0;
#endif
	boolean bPreferDsi = CKernelOptions::Get ()->GetAppOptionDecimal ("dsi", 1) != 0;
	CString List;
	m_nDisplay = 0;
	m_bDsiSeen = FALSE;
	boolean bIdAnswered = FALSE;
	for (unsigned i = 0; i < nDisplays && i < 8; i++)
	{
		CBcmPropertyTags Tags;
		TPropertyTagSimple Tag;
		Tag.nValue = i;
		unsigned nId = ~0u;
		if (Tags.GetTag (PROPTAG_GET_DISPLAY_ID, &Tag, sizeof Tag, 4))
		{
			nId = Tag.nValue;
			bIdAnswered = TRUE;
		}
		if (nId == DisplayIdDsi)
		{
			if (!m_bDsiSeen && bPreferDsi) m_nDisplay = i;
			m_bDsiSeen = TRUE;
		}
		CString Item;
		Item.Format (" %u=%s", i, DisplayName (nId));
		List.Append (Item);
	}
	m_bDsiUnknown = !bIdAnswered;
	m_pFB = new CBcmFrameBuffer (m_nReqWidth, m_nReqHeight, 16, 0, 0, m_nDisplay);
	m_bFrameBuffer = m_pFB->Initialize ();
	m_pOut = m_pFB;
	LOGNOTE ("firmware %u display(s):%s%s -> frame buffer on %u %ux%u%s", nDisplays, (const char *) List,
		 bIdAnswered ? "" : " (display id not answered)", m_nDisplay,
		 m_bFrameBuffer ? m_pFB->GetWidth () : 0, m_bFrameBuffer ? m_pFB->GetHeight () : 0,
		 m_bFrameBuffer ? "" : " FAILED");
	if (m_bFrameBuffer)
	{
		m_nFbWidth = m_nWidth = m_pFB->GetWidth ();
		m_nFbHeight = m_nHeight = m_pFB->GetHeight ();
		if (!(m_bDsiSeen && bPreferDsi)) ReadEdid ();	// on HDMI (not the DSI display)
#if RASPPI >= 5
		ScaleSetup (nUiWidth, nUiHeight);
#endif
	}
	return m_bFrameBuffer;
}

// The HDMI display's EDID (the firmware's "get EDID block" property, block 0: HDMI0's): its first detailed timing is
// the preferred mode, the monitor name descriptor its name. Logged: "display: EDID: <name> preferred 1024x600 (60 Hz)"
// (a 1024x600 panel such as the GeeekPi EP-0194 should say so; docs/pi-display.md). Pi 5: the firmware's answer is
// *inferred* (the same property; not yet seen on hardware).
void CSidecarDisplay::ReadEdid (void)
{
	CBcmPropertyTags Tags;
	TPropertyTagEDIDBlock Edid;
	Edid.nBlockNumber = EDID_FIRST_BLOCK;
	if (!Tags.GetTag (PROPTAG_GET_EDID_BLOCK, &Edid, sizeof Edid, 4) || Edid.nStatus != EDID_STATUS_SUCCESS
	    || Edid.Block[0] != 0x00 || Edid.Block[1] != 0xFF || Edid.Block[7] != 0x00)
	{
		LOGNOTE ("EDID: not available (forced hotplug without a display, or the firmware did not answer)");
		return;
	}
	const u8 *b = Edid.Block;
	char Name[14] = "";
	for (unsigned d = 72; d <= 108; d += 18)		// the descriptors after the first: 0xFC is the monitor name
		if (b[d] == 0 && b[d + 1] == 0 && b[d + 3] == 0xFC)
		{
			unsigned n = 0;
			for (; n < 13 && b[d + 5 + n] != 0x0A && b[d + 5 + n] >= 0x20 && b[d + 5 + n] < 0x7F; n++) Name[n] = (char) b[d + 5 + n];
			while (n > 0 && Name[n - 1] == ' ') n--;
			Name[n] = 0;
		}
	unsigned nClock = b[54] | b[55] << 8;			// the first detailed timing: 10 kHz units, 0: none
	if (nClock == 0)
	{
		LOGNOTE ("EDID: %s, no detailed timing", Name[0] ? Name : "(no name)");
		return;
	}
	unsigned W = b[56] | (b[58] & 0xF0) << 4, H = b[59] | (b[61] & 0xF0) << 4;
	unsigned HT = W + (b[57] | (b[58] & 0x0F) << 8), VT = H + (b[60] | (b[61] & 0x0F) << 8);
	unsigned nHz = HT && VT ? (unsigned) ((u64) nClock * 10000 / (HT * VT)) : 0;
	m_nEdidW = W;
	m_nEdidH = H;
	LOGNOTE ("EDID: %s preferred %ux%u (%u Hz)%s", Name[0] ? Name : "(no name)", W, H, nHz,
		 W == m_nFbWidth && H == m_nFbHeight ? ", the frame buffer's size"
		 : W == 1024 && H == 600 ? ": for 1024x600 set config.txt hdmi_cvt=1024 600 60 6 0 0 0 (docs/pi-display.md)" : "");
}

// The UI's size and its place in the frame buffer: res= (SetUiSize) if it fits, else the frame buffer's size when it is
// 800x480 or 1024x600 (the two layouts, sidecar_theme.h), else 800x480 centred 1:1 with black borders (no scaling: no
// blur). Pi 5 on scaled HDMI: ScaleSetup chose already (res= redoes it). A frame buffer smaller than 800x480: its own size.
void CSidecarDisplay::ChooseUi (void)
{
	const unsigned FW = m_nFbWidth, FH = m_nFbHeight;
	unsigned W = m_nUiReqW, H = m_nUiReqH;
	const boolean bReq = W != 0 && H != 0;
#if RASPPI >= 5
	if (m_bScaled || (m_pOut == m_pFB && (FW > 1024 || FH > 600)))	// through the scaler: its choice, or res=
	{
		if (bReq && (W != m_nWidth || H != m_nHeight)) ScaleSetup (W, H);
		return;
	}
#endif
	if (bReq && (W > FW || H > FH))
	{
		LOGWARN ("res=%ux%u does not fit the frame buffer's %ux%u: automatic", W, H, FW, FH);
		W = H = 0;
	}
	if (W == 0 || H == 0)
	{
		const boolean bExact = (FW == 800 && FH == 480) || (FW == 1024 && FH == 600);
		W = bExact || FW < 800 || FH < 480 ? FW : 800;
		H = bExact || FW < 800 || FH < 480 ? FH : 480;
	}
	m_nWidth = W;
	m_nHeight = H;
	m_nOffX = (FW - W) / 2;
	m_nOffY = (FH - H) / 2;
	if (m_nOffX || m_nOffY)
	{
		CDisplay::TArea All = {0, FW - 1, 0, FH - 1};	// the borders black once (the boot screen filled the frame buffer)
		u16 *pRow = new (HEAP_DMA30) u16[FW * 16];		// as the draw buffers: SetArea may DMA from it
		if (pRow)
		{
			memset (pRow, 0, FW * 16 * sizeof (u16));
			for (unsigned y = 0; y < FH; y += 16)
			{
				All.y1 = y;
				All.y2 = (y + 16 < FH ? y + 16 : FH) - 1;
				m_pOut->SetArea (All, pRow);
			}
			delete [] pRow;
		}
	}
	LOGNOTE ("UI %ux%u%s on the frame buffer's %ux%u%s (res=800x480|1024x600 in /.rytm/display.txt or cmdline.txt)", W, H,
		 m_nOffX || m_nOffY ? " centred" : "", FW, FH, bReq ? ", res= given" : "");
}

// UI pixels (rArea in the UI's coordinates, pPixels its rows, the area's width each) to the frame buffer: as they are,
// or on the Pi 5 scaled up nearest-neighbour to the whole frame buffer (non-uniform: an 800x480 UI on 1920x1080 is
// x2.4 y2.25, which a 5:3 panel showing a 16:9 signal stretches back). Frame buffer column X shows UI column
// X * W / FW (m_pXMap), row Y row Y * H / FH; the area's frame buffer rows are those that map into it, so
// neighbouring areas meet without gaps. Each UI row is scaled once, its repeats copied, ScaleRows rows per DMA.
void CSidecarDisplay::Put (const CDisplay::TArea &rArea, const u16 *pPixels)
{
#if RASPPI >= 5
	if (m_nRot)
	{
		PutRotated (rArea, pPixels);
		return;
	}
#endif
	if (!m_bScaled)
	{
		if (m_nOffX || m_nOffY)				// centred: moved to the UI's place
		{
			CDisplay::TArea A = {rArea.x1 + m_nOffX, rArea.x2 + m_nOffX, rArea.y1 + m_nOffY, rArea.y2 + m_nOffY};
#if RASPPI >= 5
			if (m_pOut != m_pFB)
			{
				DsiSetArea (A, pPixels);	// rp1dsi: at its depth
				return;
			}
#endif
			m_pOut->SetArea (A, pPixels);
			return;
		}
#if RASPPI >= 5
		if (m_pOut != m_pFB)
		{
			DsiSetArea (rArea, pPixels);		// rp1dsi: at its depth
			return;
		}
#endif
		m_pOut->SetArea (rArea, pPixels);
		return;
	}
	const unsigned W = m_nWidth, H = m_nHeight, FW = m_nFbWidth, FH = m_nFbHeight;
	const unsigned X1 = (rArea.x1 * FW + W - 1) / W, X2 = ((rArea.x2 + 1) * FW + W - 1) / W - 1;
	const unsigned Y1 = (rArea.y1 * FH + H - 1) / H, Y2 = ((rArea.y2 + 1) * FH + H - 1) / H - 1;
	if (X2 < X1 || Y2 < Y1 || X2 >= FW || Y2 >= FH) return;
	const unsigned nOut = X2 - X1 + 1, nStride = rArea.x2 - rArea.x1 + 1;
	const u16 *pMap = m_pXMap + X1;
	for (unsigned Y = Y1; Y <= Y2; )
	{
		unsigned n = Y2 - Y + 1 < ScaleRows ? Y2 - Y + 1 : ScaleRows;
		u16 *pOut = m_pScaleBuf;
		unsigned nPrev = ~0u;
		for (unsigned k = 0; k < n; k++, pOut += nOut)
		{
			unsigned sy = (Y + k) * H / FH;
			if (sy == nPrev)
			{
				memcpy (pOut, pOut - nOut, nOut * sizeof (u16));
				continue;
			}
			nPrev = sy;
			const u16 *pIn = pPixels + (sy - rArea.y1) * nStride;
			const unsigned x1 = rArea.x1;
			for (unsigned i = 0; i < nOut; i++) pOut[i] = pIn[pMap[i] - x1];
		}
		CDisplay::TArea Out = {X1, X2, Y, Y + n - 1};
		m_pFB->SetArea (Out, m_pScaleBuf);
		Y += n;
	}
}

#if RASPPI >= 5
// The Pi 5's DSI display (docs/pi5.md "DSI display"): Circle's addon/rp1dsi drives the official 7" display v1 (an ATTiny
// regulator at I2C 0x45, the TC358762 DSI bridge, an FT5x06 touch at 0x38) and v2 itself: the RP1's DSI host, no firmware.
// cmdline.txt: dsi=0 (HDMI only), dsi_port=0|1 (CAM/DISP 0 or 1; default: 0, then 1), dsi_rotate=180 (the picture and the
// touch turned, for an upside-down mount), dsitouch_inv=1|2|3 (x, y, both mirrored). Each port's display regulator is
// read first (its id logged); with none on either port both ports' I2C buses are scanned and the answering addresses
// logged (another panel: docs/pi-display.md "If the DSI screen is not firmware-compatible"). If the display does not
// start with its touch (a touch controller rp1dsi does not know), it is started again without touch.
static const unsigned DsiI2CBus[2] = {6, 4};		// rp1dsi's CRPiTouchScreen: DISP0 RP1 I2C6 (GPIO38/39), DISP1 I2C4 (GPIO40/41)

static int DsiReadReg (CI2CMaster &rI2C, u8 uchAddr, u8 uchReg)	// the register's byte, or -1 (no answer)
{
	u8 b;
	if (rI2C.Write (uchAddr, &uchReg, 1) != 1) return -1;
	CTimer::SimpleusDelay (5000);			// as rp1dsi's GetRegID
	return rI2C.Read (uchAddr, &b, 1) == 1 ? b : -1;
}

static int DsiRegulatorId (unsigned nPort)
{
	CI2CMaster I2C (DsiI2CBus[nPort]);
	return I2C.Initialize () ? DsiReadReg (I2C, 0x45, 0x01) : -1;
}

static void DsiScan (unsigned nPort)
{
	CI2CMaster I2C (DsiI2CBus[nPort]);
	if (!I2C.Initialize ()) return;
	CString List;
	for (unsigned a = 0x08; a < 0x78; a++)
	{
		u8 b;
		if (I2C.Read ((u8) a, &b, 1) != 1) continue;
		CString Item;
		Item.Format (" 0x%02X", a);
		List.Append (Item);
	}
	LOGNOTE ("DSI: DISP%u I2C%u answers at:%s", nPort, DsiI2CBus[nPort], List.GetLength () ? (const char *) List : " none");
}

static CDevice *DsiTouchDevice (CDevice *pBefore[8], boolean bNew)	// !bNew: touch1..8 into pBefore; bNew: the one new since
{
	CDeviceNameService *pNames = CDeviceNameService::Get ();
	for (unsigned i = 0; i < 8; i++)
	{
		CString Name;
		Name.Format ("touch%u", i + 1);
		CDevice *pDev = pNames->GetDevice (Name, FALSE);
		if (!bNew) pBefore[i] = pDev;
		else if (pDev && pDev != pBefore[i]) return pDev;
	}
	return 0;
}

boolean CSidecarDisplay::OpenRp1Dsi (void)
{
	CKernelOptions *pOpt = CKernelOptions::Get ();
	if (pOpt->GetAppOptionDecimal ("dsi", 1) == 0)
	{
		LOGNOTE ("DSI: off (dsi=0), HDMI");
		return FALSE;
	}
	unsigned nWant = pOpt->GetAppOptionDecimal ("dsi_port", 2);	// 2: both, DISP0 first
	unsigned nRotOpt = pOpt->GetAppOptionDecimal ("dsi_rotate", 0);
	unsigned nRotate = nRotOpt == 180 ? 180 : 0;			// v1: rp1dsi turns it; a portrait panel: RotateSetup
	// the Touch Display 2's DSI video mode (circle-rp1dsi.patch): sync events as Linux, or dsi_mode=pulse (rp1dsi's
	// v1 flags, as Sidecar 0.2.0 drove it)
	g_nRPiTouchScreen2Panel = pOpt->GetAppOptionDecimal ("dsi_panel", 0);	// 5 or 7: the Touch Display 2's size forced
										// (0: from the regulator's id)
	const char *pMode = pOpt->GetAppOptionString ("dsi_mode", "events");
	if (strcmp (pMode, "pulse") == 0)
		g_ulRPiTouchScreen2ModeFlags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE | MIPI_DSI_MODE_LPM
					       | MIPI_DSI_MODE_VIDEO_HSE;
	unsigned nInv = pOpt->GetAppOptionDecimal ("dsitouch_inv", 0);
	m_nDsiInvert = nInv <= 3 ? nInv : 0;
	boolean bAny = FALSE;
	for (unsigned nPort = 0; nPort < 2; nPort++)
	{
		if (nWant < 2 && nPort != nWant) continue;
		int nId = DsiRegulatorId (nPort);
		if (nId < 0)
		{
			LOGNOTE ("DSI: DISP%u: no display regulator at I2C%u 0x45", nPort, DsiI2CBus[nPort]);
			continue;
		}
		bAny = TRUE;
		unsigned k = nId & 0x0F;
		LOGNOTE ("DSI: DISP%u: display regulator id 0x%02X (%s)", nPort, nId,
			 k == 0x01 || k == 0x04 ? "Touch Display 2, 7\"" : k == 0x08 || k == 0x09 ? "Touch Display 2, 5\""
			 : "official 7\" v1 or compatible: TC358762, FT5x06");
		for (unsigned nTry = 0; nTry < 2; nTry++)		// with touch, then without
		{
			boolean bTouch = nTry == 0;
			CDevice *pBefore[8];
			DsiTouchDevice (pBefore, FALSE);
			m_nDsiDepth = pOpt->GetAppOptionDecimal ("dsi_depth", 16) == 32 ? 32 : 16;	// the panel's frame buffer
			CRPiTouchScreen *pDsi = new CRPiTouchScreen (CInterruptSystem::Get (), m_nDsiDepth, nPort, bTouch);
			pDsi->SetRotation (nRotate);
			if (!pDsi->Initialize ())
			{
				LOGWARN ("DSI: DISP%u: not started%s (the rp1touch/rp1dsi lines above say why)", nPort,
					 bTouch ? " with touch" : "");
				delete pDsi;
				continue;
			}
			m_pRPiTouch = pDsi;
			m_pOut = pDsi;
			m_bFrameBuffer = TRUE;
			m_nFbWidth = m_nWidth = pDsi->GetWidth ();
			m_nFbHeight = m_nHeight = pDsi->GetHeight ();
			if (m_nFbHeight > m_nFbWidth)					// Touch Display 2: portrait
			{
				if (nRotOpt != 0 && nRotOpt != 90 && nRotOpt != 270)
					LOGWARN ("DSI: dsi_rotate=%u is not supported on a portrait panel (90 or 270): using 90", nRotOpt);
				RotateSetup (nRotOpt == 270 ? 270 : 90);
			}
			CDevice *pTouch = bTouch ? DsiTouchDevice (pBefore, TRUE) : 0;
			if (pTouch)
			{
				m_pDsiTouch = pTouch;
				((CTouchScreenDevice *) pTouch)->Setup (pDsi);
				((CTouchScreenDevice *) pTouch)->RegisterEventHandler (H::DsiTouchHandler);
			}
			LOGNOTE ("DSI: DISP%u %ux%u (rp1dsi, %s), rotation %u, touch %s%s", nPort, m_nFbWidth,
				 m_nFbHeight, m_nRot ? "UI turned" : "no scaling", m_nRot ? m_nRot : nRotate,
				 pTouch ? (m_nRot ? "GT911 polled" : "FT5x06 polled") : "none",
				 pTouch && m_nDsiInvert ? ", inverted" : "");
			return TRUE;
		}
	}
	if (!bAny)
	{
		for (unsigned nPort = 0; nPort < 2; nPort++)
			if (nWant >= 2 || nPort == nWant) DsiScan (nPort);
		LOGNOTE ("DSI: no display found, HDMI");
	}
	return FALSE;
}

// The UI's size on the Pi 5: cmdline.txt's width= and height= if both are given (or res=, ChooseUi calls it again), else
// when the frame buffer is bigger than 1024x600 the layout the panel suits: 1024x600 if the HDMI display's EDID prefers
// 1024x600 (ReadEdid), else 800x480; else the frame buffer's. Never bigger than the frame buffer (no downscaling); equal
// to it: no scaling. Called again: the scaler redone (its buffers kept).
void CSidecarDisplay::ScaleSetup (unsigned nWidth, unsigned nHeight)
{
	const unsigned FW = m_nFbWidth, FH = m_nFbHeight;
	if (nWidth == 0 || nHeight == 0)			// a 1024x600 panel (its EDID) gets the 1024x600 layout
	{
		boolean bBig = FW > 1024 || FH > 600, b1024 = m_nEdidW == 1024 && m_nEdidH == 600;
		nWidth = bBig ? (b1024 ? 1024 : 800) : FW;
		nHeight = bBig ? (b1024 ? 600 : 480) : FH;
	}
	if (nWidth > FW) nWidth = FW;
	if (nHeight > FH) nHeight = FH;
	m_nWidth = FW;						// unscaled until the scaler is set up again
	m_nHeight = FH;
	m_bScaled = FALSE;
	if (nWidth < 320 || nHeight < 240 || (nWidth == FW && nHeight == FH)) return;
	if (!m_pXMap) m_pXMap = new u16[FW];
	if (!m_pScaleBuf) m_pScaleBuf = new (HEAP_DMA30) u16[FW * ScaleRows];
	if (!m_pXMap || !m_pScaleBuf)
	{
		LOGWARN ("no memory for the scaler: the UI at %ux%u", FW, FH);
		return;
	}
	for (unsigned X = 0; X < FW; X++) m_pXMap[X] = X * nWidth / FW;
	m_nWidth = nWidth;
	m_nHeight = nHeight;
	m_bScaled = TRUE;
	LOGNOTE ("UI %ux%u scaled to the frame buffer's %ux%u (x %u.%02u, y %u.%02u; res=800x480|1024x600 sets the UI's size)",
		 nWidth, nHeight, FW, FH, FW / nWidth, FW * 100 / nWidth % 100, FH / nHeight, FH * 100 / nHeight % 100);
}

// The panel indices k (0..N-1) whose UI index ((bFlip ? N-1-k : k) * S / N) lies in a1..a2 (N >= S: never empty)
static void Span (unsigned a1, unsigned a2, unsigned N, unsigned S, boolean bFlip, unsigned *p1, unsigned *p2)
{
	unsigned k1 = (a1 * N + S - 1) / S, k2 = ((a2 + 1) * N + S - 1) / S - 1;
	if (bFlip)
	{
		unsigned t = N - 1 - k2;
		k2 = N - 1 - k1;
		k1 = t;
	}
	*p1 = k1;
	*p2 = k2;
}

// The Touch Display 2 (rp1dsi v2: a 720x1280 portrait panel): the 800x480 UI turned a quarter (dsi_rotate=90, the
// default: its top at the panel's right edge; 270: at its left) and stretched to the whole panel, non-uniform as the
// HDMI scaler (x1.60 along, x1.50 across). The UI's size is fixed at 800x480 here: cmdline.txt's width=/height= (ScaleSetup,
// HDMI) are not read. Panel column X shows UI row m_pXMap[X], panel row Y UI column m_pYMap[Y].
void CSidecarDisplay::RotateSetup (unsigned nRot)
{
	const unsigned PW = m_nFbWidth, PH = m_nFbHeight, W = 800, H = 480;
	m_pXMap = new u16[PW];
	m_pYMap = new u16[PH];
	m_pScaleBuf = new (HEAP_DMA30) u16[PW * ScaleRows];
	if (!m_pXMap || !m_pYMap || !m_pScaleBuf)
	{
		LOGWARN ("DSI: no memory to turn the UI: %ux%u portrait", PW, PH);
		return;
	}
	for (unsigned X = 0; X < PW; X++) m_pXMap[X] = (nRot == 90 ? PW - 1 - X : X) * H / PW;
	for (unsigned Y = 0; Y < PH; Y++) m_pYMap[Y] = (nRot == 90 ? Y : PH - 1 - Y) * W / PH;
	m_nWidth = W;
	m_nHeight = H;
	m_nRot = nRot;
	LOGNOTE ("DSI: UI %ux%u turned %u and stretched to the panel's %ux%u (dsi_rotate=90|270)", W, H, nRot, PW, PH);
}

// RGB565 pixels (rArea's, row after row) to the rp1dsi display: as they are, or with dsi_depth=32 as ARGB8888
// (the frame buffer format Linux uses on the Touch Display 2), converted in bands of m_pConv's size
void CSidecarDisplay::DsiSetArea (const CDisplay::TArea &rArea, const u16 *pPixels)
{
	if (m_nDsiDepth != 32)
	{
		m_pOut->SetArea (rArea, pPixels);
		return;
	}
	static const unsigned ConvPixels = 720 * 64;
	if (!m_pConv) m_pConv = new u32[ConvPixels];
	if (!m_pConv) return;
	const unsigned nWidth = rArea.x2 - rArea.x1 + 1;
	unsigned nRows = ConvPixels / nWidth;
	if (nRows == 0) return;
	for (unsigned Y = rArea.y1; Y <= rArea.y2; Y += nRows)
	{
		unsigned n = rArea.y2 - Y + 1 < nRows ? rArea.y2 - Y + 1 : nRows;
		const u16 *p = pPixels + (Y - rArea.y1) * nWidth;
		for (unsigned i = 0; i < n * nWidth; i++)
		{
			u32 c = p[i], r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
			m_pConv[i] = 0xFF000000 | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
		}
		CDisplay::TArea Band = {rArea.x1, rArea.x2, Y, Y + n - 1};
		m_pOut->SetArea (Band, m_pConv);
	}
}

// Put on the turned panel: the panel rows and columns that show rArea (the inverse of the maps), ScaleRows panel rows
// per SetArea, each panel row one UI column read down the area's rows
void CSidecarDisplay::PutRotated (const CDisplay::TArea &rArea, const u16 *pPixels)
{
	const unsigned PW = m_nFbWidth, PH = m_nFbHeight, nStride = rArea.x2 - rArea.x1 + 1;
	unsigned X1, X2, Y1, Y2;
	Span (rArea.y1, rArea.y2, PW, m_nHeight, m_nRot == 90, &X1, &X2);	// panel columns <- UI rows
	Span (rArea.x1, rArea.x2, PH, m_nWidth, m_nRot == 270, &Y1, &Y2);	// panel rows <- UI columns
	if (X2 < X1 || Y2 < Y1 || X2 >= PW || Y2 >= PH) return;
	const unsigned nOut = X2 - X1 + 1;
	for (unsigned Y = Y1; Y <= Y2; )
	{
		unsigned n = Y2 - Y + 1 < ScaleRows ? Y2 - Y + 1 : ScaleRows;
		u16 *pOut = m_pScaleBuf;
		unsigned nPrev = ~0u;
		for (unsigned k = 0; k < n; k++, pOut += nOut)
		{
			unsigned u = m_pYMap[Y + k];			// this panel row's UI column
			if (u == nPrev)
			{
				memcpy (pOut, pOut - nOut, nOut * sizeof (u16));
				continue;
			}
			nPrev = u;
			const u16 *pIn = pPixels + (u - rArea.x1);
			for (unsigned i = 0; i < nOut; i++) pOut[i] = pIn[(m_pXMap[X1 + i] - rArea.y1) * nStride];
		}
		CDisplay::TArea Out = {X1, X2, Y, Y + n - 1};
		DsiSetArea (Out, m_pScaleBuf);			// the rp1dsi display (m_pFB is 0 there)
		Y += n;
	}
}
#endif

boolean CSidecarDisplay::Splash (void)
{
	if (!OpenFrameBuffer ()) return FALSE;
	if (m_bScaled || m_pOut != m_pFB)			// scaled, or the Pi 5's DSI (no frame buffer pointer)
	{
		m_pSplash = new u16[m_nWidth * m_nHeight];	// drawn at the UI's size, scaled as the UI is
		if (!m_pSplash) return TRUE;
		sidecar_splash_draw (m_pSplash, m_nWidth, m_nWidth, m_nHeight);
		CDisplay::TArea Area = {0, m_nWidth - 1, 0, m_nHeight - 1};
		Put (Area, m_pSplash);
		return TRUE;
	}
	sidecar_splash_draw ((u16 *) (uintptr) m_pFB->GetBuffer (), m_pFB->GetPitch () / 2,
			     m_pFB->GetWidth (), m_pFB->GetHeight ());
	return TRUE;
}

void CSidecarDisplay::SplashStep (unsigned nStep)
{
	if (!m_bFrameBuffer || m_bOn) return;
	if (m_bScaled || m_pOut != m_pFB)
	{
		if (!m_pSplash) return;
		sidecar_splash_progress (m_pSplash, m_nWidth, m_nWidth, m_nHeight, nStep, SplashSteps);
		CDisplay::TArea Area = {0, m_nWidth - 1, m_nHeight / 2, m_nHeight - 1};	// the bar is in the lower half
		Put (Area, m_pSplash + m_nWidth * (m_nHeight / 2));
		return;
	}
	sidecar_splash_progress ((u16 *) (uintptr) m_pFB->GetBuffer (), m_pFB->GetPitch () / 2,
				 m_pFB->GetWidth (), m_pFB->GetHeight (), nStep, SplashSteps);
}

boolean CSidecarDisplay::Initialize (void)
{
	if (!OpenFrameBuffer ())
	{
		LOGWARN ("no frame buffer: the display is off");
		return FALSE;
	}
	delete [] m_pSplash;					// LVGL's screen replaces it
	m_pSplash = 0;
	ChooseUi ();

	lv_init ();
	lv_tick_set_cb (H::Tick);
	lv_log_register_print_cb (H::Log);

	const unsigned nPixels = m_nWidth * 48;			// a 48-line band: 77 KB at 800 wide, 98 KB at 1024
	m_pBuffer = new (HEAP_DMA30) u16[nPixels];
	if (!m_pBuffer) return FALSE;
	m_pDotBuf = new (HEAP_DMA30) u16[m_nWidth * DotRows * SIDECAR_DOT];	// 51 KB at 800 wide, 66 KB at 1024
	if (!m_pDotBuf) return FALSE;

	lv_display_t *pDisplay = lv_display_create (m_nWidth, m_nHeight);
	lv_display_set_flush_cb (pDisplay, H::Flush);
	lv_display_set_buffers (pDisplay, m_pBuffer, 0, nPixels * sizeof (u16), LV_DISPLAY_RENDER_MODE_PARTIAL);

	lv_indev_t *pIndev = lv_indev_create ();
	lv_indev_set_type (pIndev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb (pIndev, H::PointerRead);

	const unsigned *pCal = CKernelOptions::Get ()->GetTouchScreen ();
	if (pCal && pCal[0] < pCal[1] && pCal[2] < pCal[3]) m_pCalibration = pCal;

	sidecar_ui_create ();
	int ox, oy;						// the layout centred in a bigger UI (width=/height=): its border black
	sidecar_ui_origin (&ox, &oy);
	if (ox || oy)
	{
		memset (m_pBuffer, 0, m_nWidth * 40 * sizeof (u16));
		for (unsigned y = 0; y < m_nHeight; y += 40)
		{
			CDisplay::TArea A = {0, m_nWidth - 1, y, (y + 40 < m_nHeight ? y + 40 : m_nHeight) - 1};
			Put (A, m_pBuffer);
		}
	}
	m_bOn = TRUE;
	Activity ();
	LOGNOTE ("%ux%u, LVGL %d.%d", m_nWidth, m_nHeight, LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);
	OpenDsiTouch ();
	BindInput ();
	return TRUE;
}

// The official 7" display's touch (and compatibles'): the firmware reads the FT5406 over the display's I2C and keeps
// the points in a buffer that CRPiTouchScreen polls (its CTouchScreenDevice's Update, from Update below). Created when
// the firmware reports a DSI display (or does not answer the display id: then it is tried), never with dsi=0.
// Coordinates are the panel's 0..799 / 0..479; cmdline dsitouch_inv=1 (x), 2 (y), 3 (both) flips them.
void CSidecarDisplay::OpenDsiTouch (void)
{
	if (CKernelOptions::Get ()->GetAppOptionDecimal ("dsi", 1) == 0) return;
#if RASPPI >= 5
	if (!m_pRPiTouch) LOGNOTE ("touch: no DSI display, no DSI touch");	// else set up with the display (OpenRp1Dsi)
#else
	if (!m_bDsiSeen && !m_bDsiUnknown)
	{
		LOGNOTE ("touch: no DSI display reported, no firmware touch");
		return;
	}
	unsigned nInv = CKernelOptions::Get ()->GetAppOptionDecimal ("dsitouch_inv", 0);
	m_nDsiInvert = nInv <= 3 ? nInv : 0;
	CDeviceNameService *pNames = CDeviceNameService::Get ();
	CDevice *pBefore[8];					// the touch devices so far: the new one is the DSI's
	for (unsigned i = 0; i < 8; i++)
	{
		CString Name;
		Name.Format ("touch%u", i + 1);
		pBefore[i] = pNames->GetDevice (Name, FALSE);
	}
	CRPiTouchScreen *pRPi = new CRPiTouchScreen;
	if (!pRPi->Initialize ())
	{
		LOGNOTE ("touch: DSI (firmware) not available");
		delete pRPi;
		return;
	}
	m_pRPiTouch = pRPi;
	for (unsigned i = 0; i < 8 && !m_pDsiTouch; i++)
	{
		CString Name;
		Name.Format ("touch%u", i + 1);
		CDevice *pDev = pNames->GetDevice (Name, FALSE);
		if (pDev && pDev != pBefore[i]) m_pDsiTouch = pDev;
	}
	if (!m_pDsiTouch) return;
	CTouchScreenDevice *pTouch = (CTouchScreenDevice *) (CDevice *) m_pDsiTouch;
	pTouch->Setup (m_pFB);
	pTouch->RegisterEventHandler (H::DsiTouchHandler);
	LOGNOTE ("touch: DSI (firmware) touch buffer set up%s", m_nDsiInvert ? ", inverted" : "");
#endif
}

const char *CSidecarDisplay::GetInput (void) const
{
	return m_pTouch ? "touch (USB)" : m_pDsiTouch ? "touch (DSI)" : m_pMouse ? "mouse1" : "none";
}

// The USB touch screen (any touchN but the DSI's) has priority: while it is bound the DSI's events are dropped
void CSidecarDisplay::BindInput (void)
{
	CDeviceNameService *pNames = CDeviceNameService::Get ();
	CUSBTouchScreenDevice *pBest = CUSBTouchScreenDevice::Get ();	// the USB touch with the most contacts
	CDevice *pPreferred = pBest ? pBest->GetTouchDevice () : 0;		// (a panel's digitizer over its pointer)
	for (unsigned i = 1; i <= 8 && !m_pTouch; i++)
	{
		CString Name;
		Name.Format ("touch%u", i);
		CTouchScreenDevice *pTouch = (CTouchScreenDevice *) pNames->GetDevice (Name, FALSE);
		if (pTouch && (CDevice *) pTouch != m_pDsiTouch && (!pPreferred || (CDevice *) pTouch == pPreferred))
		{
			pTouch->Setup (m_pOut);		// no SetCalibration: raw values (Map scales them)
			pTouch->RegisterEventHandler (H::TouchHandler);
			pTouch->RegisterRemovedHandler (H::TouchRemoved, this);
			m_pTouch = pTouch;
			CUSBTouchScreenDevice *pUSB = CUSBTouchScreenDevice::Get ();	// its raw range (circle-touch.patch)
			if (!pUSB || !pUSB->GetLogicalMax (&m_nLogMaxX, &m_nLogMaxY)) m_nLogMaxX = m_nLogMaxY = 0;
			m_nUsbContacts = pUSB ? pUSB->GetMaxContacts () : 0;
			LOGNOTE ("touch screen bound (%s), raw range %s%u x %u, %s", (const char *) Name,
				 m_nLogMaxX ? "0.." : "auto ", m_nLogMaxX, m_nLogMaxY,
				 m_nUsbContacts == 1 ? "single touch" : "multi-touch");
		}
	}
#if RASPPI >= 5
	const char *pSource = m_pTouch ? "USB" : m_pDsiTouch ? "DSI (rp1dsi)" : 0;
#else
	const char *pSource = m_pTouch ? "USB" : m_pDsiTouch ? "DSI (firmware)" : 0;
#endif
	if (pSource != m_pSource)
	{
		m_pSource = pSource;
		LOGNOTE ("touch: %s", pSource ? pSource : "none");
	}
	if (!m_pTouch && !m_pDsiTouch && !m_pMouse)
	{
		CMouseDevice *pMouse = (CMouseDevice *) pNames->GetDevice ("mouse1", FALSE);
		if (pMouse && pMouse->Setup (m_pOut, FALSE))
		{
			pMouse->RegisterEventHandler (H::MouseHandler);
			pMouse->RegisterRemovedHandler (H::MouseRemoved, this);
			m_pMouse = pMouse;
			LOGNOTE ("mouse bound (mouse1)");
		}
	}
}

void CSidecarDisplay::SetScreensaver (unsigned nMinutes)
{
	m_nSaverMin = nMinutes;
	Activity ();
}

void CSidecarDisplay::Activity (void)
{
	m_nLastActive = CTimer::GetClockTicks64 ();
}

// The firmware's "blank screen" property (0x00040002): black, and with hdmi_blanking=1 in config.txt the HDMI output off
// (a monitor's power saving; whether a panel's backlight follows is the panel's)
#define PROPTAG_BLANK_SCREEN	0x00040002

void CSidecarDisplay::Blank (boolean bOn)
{
	m_bBlank = bOn;
	if (bOn)
	{
		CDisplay::TArea Area;				// black, row by row out of the (zeroed) draw buffer
		memset (m_pBuffer, 0, m_nWidth * 40 * sizeof (u16));
		for (unsigned y = 0; y < m_nHeight; y += 40)
		{
			Area.x1 = 0; Area.x2 = m_nWidth - 1;
			Area.y1 = y; Area.y2 = (y + 40 < m_nHeight ? y + 40 : m_nHeight) - 1;
			Put (Area, m_pBuffer);
		}
	}
	boolean bOK;
#if RASPPI >= 5
	if (m_pOut != m_pFB)					// the Pi 5's DSI display: its backlight off (rp1dsi's 200 back on)
		bOK = ((CRPiTouchScreen *) m_pOut)->SetBacklightBrightness (bOn ? 0 : 200);
	else
#endif
	{
		CBcmPropertyTags Tags;
		TPropertyTagSimple Tag;
		Tag.nValue = bOn ? 1 : 0;
		bOK = Tags.GetTag (PROPTAG_BLANK_SCREEN, &Tag, sizeof Tag, 4);
	}
	if (bOn) LOGNOTE ("screensaver: blank after %u min without a touch (%s blank %s)", m_nSaverMin, m_pOut != m_pFB ? "backlight" : "firmware", bOK ? "ok" : "refused");
	else
	{
		lv_obj_invalidate (lv_screen_active ());	// everything drawn again
		LOGNOTE ("screensaver: woken by a touch");
	}
}

void CSidecarDisplay::Update (boolean bPlugAndPlayUpdated)
{
	if (!m_bOn) return;
	if (bPlugAndPlayUpdated) BindInput ();

	unsigned nNow = CTimer::GetClockTicks ();
	if (nNow - m_nLastUpdate < 10 * CLOCKHZ / 1000) return;
	m_nLastUpdate = nNow;

	// The USB touch screen's recovery first: Poll resets and resubmits a halted report endpoint, and when that has
	// been getting nowhere it re-enumerates the whole device (main loop: it issues xHCI commands). A re-enumeration
	// deletes the panel's touchN device - Circle's removed handler clears m_pTouch - and makes a new one, so re-bind
	// before anything is polled through the old pointers. GetReEnumCount is the signal to use: the new device can
	// land on the freed object's address, so comparing the CUSBTouchScreenDevice pointers would not see it.
	CUSBTouchScreenDevice *pUSBTouch = CUSBTouchScreenDevice::Get ();
	if (pUSBTouch)
	{
		unsigned nReEnums = CUSBHIDDevice::GetReEnumCount ();
		pUSBTouch->Poll ();			// pUSBTouch and m_pTouch may both be gone when this returns
		if (CUSBHIDDevice::GetReEnumCount () != nReEnums) BindInput ();	// the new panel's touchN, or none
	}

	if (m_pMouse) ((CMouseDevice *) m_pMouse)->UpdateCursor ();	// polls the mouse (cursor off)
	if (m_pTouch) ((CTouchScreenDevice *) m_pTouch)->Update ();
	if (m_pDsiTouch) ((CTouchScreenDevice *) m_pDsiTouch)->Update ();	// reads the firmware's touch buffer

	u32 nSeq = m_nTouchSeq;
	boolean bDown = (m_nPointer >> 31) != 0;
	if (m_bSwallow && !bDown) m_bSwallow = FALSE;	// the waking finger is up: touches reach the controls again
	if (nSeq != m_nShownSeq) Activity ();
	if (m_bBlank)
	{
		if (nSeq != m_nShownSeq && (bDown || m_nDownSeq != m_nShownDownSeq))	// (a tap shorter than an update too)
		{
			m_bSwallow = TRUE;			// this press only wakes the screen
			m_nShownDownSeq = m_nDownSeq;
			Blank (FALSE);
		}
		else
		{
			m_nShownSeq = nSeq;
			m_nShownDownSeq = m_nDownSeq;
			return;					// black: LVGL not run
		}
	}
	else if (m_nSaverMin && CTimer::GetClockTicks64 () - m_nLastActive >= (u64) m_nSaverMin * 60 * CLOCKHZ)
	{
		m_nShownSeq = nSeq;
		Blank (TRUE);
		return;
	}
	if (nSeq != m_nShownSeq)
	{
		m_nShownSeq = nSeq;
		u32 p = m_nPointer, r = m_nRaw, nDown = m_nDownSeq;
		boolean bNowDown = (p >> 31) != 0;
		if (!bNowDown && nDown != m_nShownDownSeq)		// a tap shorter than one update: its press shown first
		{
			sidecar_ui_touch (1, r & 0xFFFF, r >> 16, p & 0x7FFF, (p >> 16) & 0x7FFF);
			if (!m_bLoggedDown) LOGNOTE ("touch: down raw %u,%u -> %u,%u", r & 0xFFFF, r >> 16, p & 0x7FFF, (p >> 16) & 0x7FFF);
			m_bLoggedDown = TRUE;
		}
		m_nShownDownSeq = nDown;
		if (bNowDown != m_bLoggedDown)			// the raw values of each press and release in the log
		{
			m_bLoggedDown = bNowDown;
			LOGNOTE ("touch: %s raw %u,%u -> %u,%u", bNowDown ? "down" : "up", r & 0xFFFF, r >> 16, p & 0x7FFF, (p >> 16) & 0x7FFF);
		}
		sidecar_ui_touch (p >> 31, r & 0xFFFF, r >> 16, p & 0x7FFF, (p >> 16) & 0x7FFF);
	}
	if (m_bStatusNew)
	{
		m_bStatusNew = FALSE;
		sidecar_ui_status (&m_Status);
	}
	u32 nSeq2 = m_nTouchSeq2;
	if (nSeq2 != m_nShownSeq2)				// the second finger (pinch on the SEQ page)
	{
		m_nShownSeq2 = nSeq2;
		u32 q = m_nPointer2;
		int x2 = (int) (q & 0x7FFF), y2 = (int) ((q >> 16) & 0x7FFF);
		sidecar_ui_to_ui (&x2, &y2);
		sidecar_seqed_touch2 ((q >> 31) && (m_nPointer >> 31), x2, y2);
	}
	lv_timer_handler ();
	unsigned nUs = CTimer::GetClockTicks () - nNow;
	if (nUs > m_nMaxUs) m_nMaxUs = nUs;
}

void CSidecarDisplay::SetTouchCal (const touch_cal_t *pCal)
{
	m_bTouchCal = FALSE;					// (Map on core 0 reads it: off while it changes)
	if (pCal == 0)
	{
		LOGNOTE ("touch: calibration off (%s)", m_pCalibration ? "cmdline touchscreen=" : "the device's range");
		return;
	}
	m_TouchCal = *pCal;
	DataMemBarrier ();
	m_bTouchCal = TRUE;
	LOGNOTE ("touch: calibration x %d..%d, y %d..%d%s", pCal->x0, pCal->x1, pCal->y0, pCal->y1, pCal->swap ? ", axes swapped" : "");
}

// The bound input's contacts: 0 none, 1 single touch (an absolute-pointer USB panel such as the GeeekPi 0416:c169, or a
// mouse: no pinch, the UI may offer double-tap / a zoom slider instead), 2+ multi-touch (USB digitizer, DSI FT5x06)
unsigned CSidecarDisplay::GetTouchContacts (void) const
{
	if (m_pTouch) return m_nUsbContacts ? m_nUsbContacts : 1;
	if (m_pDsiTouch) return 10;
	return m_pMouse ? 1 : 0;
}

extern "C" unsigned sidecar_touch_contacts (void)
{
	return CSidecarDisplay::TouchContacts ();
}

const char *CSidecarDisplay::TakeTouchDiag (void)
{
	m_DiagText[0] = '\0';
	CUSBTouchScreenDevice *pUSB = CUSBTouchScreenDevice::Get ();
	if (!m_pTouch || !pUSB) return m_DiagText;

	CUSBHIDDevice::THIDStats S;
	pUSB->GetHIDStats (&S);
	u32 nEvents = m_nEvents, p = m_nPointer, r = m_nRaw;
	CString Text;
	Text.Format (" | touch: ev +%u rep +%u raw %u,%u %s | hid err %u (xhci %u, last code %u) resub %u reset %u (+%u) fail %u "
		     "ep state %u pend %u ignored %u reenum %u (try %u/%u held %ums key %X)%s%s",
		     nEvents - m_nDiagEvents, S.nReports - m_nDiagReports, r & 0xFFFF, r >> 16, (p >> 31) ? "DOWN" : "up",
		     S.nErrors, S.nXhciErrors, S.nLastCode, S.nResubmits, S.nResets, S.nResets - m_nDiagResets, S.nFailed,
		     S.nState, S.nPending, pUSB->GetIgnored (), S.nReEnums,
		     S.nReEnumTries, S.nReEnumMax, S.nGaveUpHeldMs, S.nReEnumKey,
		     S.bReEnumExhausted ? " REENUM-EXHAUSTED" : "", S.bGaveUp ? " GAVE-UP" : "");
	m_nDiagEvents = nEvents;
	m_nDiagReports = S.nReports;
	m_nDiagResets = S.nResets;
	strncpy (m_DiagText, (const char *) Text, sizeof m_DiagText - 1);
	m_DiagText[sizeof m_DiagText - 1] = '\0';
	return m_DiagText;
}

unsigned CSidecarDisplay::TakeMaxUpdateUs (void)
{
	unsigned n = m_nMaxUs;
	m_nMaxUs = 0;
	return n;
}

void CSidecarDisplay::SetStatus (const sidecar_ui_status_t &rStatus)
{
	m_Status = rStatus;
	m_bStatusNew = TRUE;
}

// LVGL's pixels to the screen. The OLED theme draws at half the size (sidecar_ui_dot): each dot becomes a 2x2 cell, the
// dot and its dimmer grid lines (sidecar_dot_expand, DotRows UI rows at a time), the pattern colours their checkerboards, the brightness
// applied; then Put as before (the Pi 5's scaler on top)
void H::Flush (lv_display_t *pDisplay, const lv_area_t *pArea, u8 *pBuffer)
{
	CSidecarDisplay *p = CSidecarDisplay::s_pThis;
	const int nDot = sidecar_ui_dot ();
	if (nDot == 1 || !p->m_pDotBuf)
	{
		int ox, oy;					// 0, unless LVGL's display is bigger than the layout (width=/height=)
		sidecar_ui_origin (&ox, &oy);
		CDisplay::TArea Area;
		Area.x1 = pArea->x1 + ox;
		Area.x2 = pArea->x2 + ox;
		Area.y1 = pArea->y1 + oy;
		Area.y2 = pArea->y2 + oy;
		p->Put (Area, (const u16 *) pBuffer);		// no completion routine: DMA and wait
		lv_display_flush_ready (pDisplay);
		return;
	}
	int ox, oy;
	sidecar_ui_origin (&ox, &oy);
	const int aw = pArea->x2 - pArea->x1 + 1, ah = pArea->y2 - pArea->y1 + 1, nBright = sidecar_ui_bright ();
	const u16 *pIn = (const u16 *) pBuffer;
	if (pArea->x1 == 0 && pArea->y1 == 0 && ox > 0 && ox <= 2)	// a full redraw: the columns beside the dot grid black
	{
		memset (p->m_pDotBuf, 0, 2 * 120 * sizeof (u16));
		for (unsigned y = 0; y < p->m_nHeight; y += 120)
		{
			unsigned h = p->m_nHeight - y < 120 ? p->m_nHeight - y : 120;
			CDisplay::TArea L = {0, (unsigned) ox - 1, y, y + h - 1};
			CDisplay::TArea R = {p->m_nWidth - (unsigned) ox, p->m_nWidth - 1, y, y + h - 1};
			p->Put (L, p->m_pDotBuf);
			p->Put (R, p->m_pDotBuf);
		}
	}
	for (int y0 = 0; y0 < ah; y0 += (int) CSidecarDisplay::DotRows)
	{
		int n = ah - y0 < (int) CSidecarDisplay::DotRows ? ah - y0 : (int) CSidecarDisplay::DotRows;
		sidecar_dot_expand (pIn + y0 * aw, pArea->x1, pArea->y1 + y0, aw, n, aw, p->m_pDotBuf, aw * nDot, 0, nBright);
		CDisplay::TArea Area;
		Area.x1 = ox + pArea->x1 * nDot;
		Area.x2 = Area.x1 + aw * nDot - 1;
		Area.y1 = oy + (pArea->y1 + y0) * nDot;
		Area.y2 = Area.y1 + n * nDot - 1;
		p->Put (Area, p->m_pDotBuf);
	}
	lv_display_flush_ready (pDisplay);
}

void H::PointerRead (lv_indev_t *pIndev, lv_indev_data_t *pData)
{
	CSidecarDisplay *pThis = CSidecarDisplay::s_pThis;
	u32 p = pThis->m_nPointer;
	u32 nDown = pThis->m_nDownSeq;
	boolean bPressed = (p >> 31) != 0;
	if (!bPressed && nDown != pThis->m_nReadDownSeq) bPressed = TRUE;	// a tap down and up between two reads:
	pThis->m_nReadDownSeq = nDown;					// shown pressed once, released at the next read
	pData->state = bPressed && !pThis->m_bSwallow ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
	int x = (int) (p & 0x7FFF), y = (int) ((p >> 16) & 0x7FFF);
	sidecar_ui_to_ui (&x, &y);					// screen -> the theme's UI pixels (OLED: dots)
	pData->point.x = x;
	pData->point.y = y;
}

void H::Log (lv_log_level_t Level, const char *pMsg)
{
	CLogger::Get ()->Write ("lvgl", Level >= LV_LOG_LEVEL_ERROR ? LogError : LogNotice, "%s", pMsg);
}

u32 H::Tick (void)
{
	return (u32) (CTimer::GetClockTicks64 () / (CLOCKHZ / 1000));
}

// raw -> screen: touchscreen= from cmdline.txt, else the USB touch screen's logical X/Y maxima from its report
// descriptor, else raw = pixels until a value is out of the screen, then a 12-bit (0-4095) or 15-bit (0-32767) range
// The touch panel covers the whole screen: raw -> the frame buffer's pixels (the UI's when scaled), then the centred UI's
// place taken off (Pi 4, a frame buffer of another size: touches on the black border land on the UI's edge).
void CSidecarDisplay::Map (unsigned nRawX, unsigned nRawY, int *pX, int *pY)
{
	if (m_bTouchCal)					// Settings > Display > Calibrate: straight to the UI's pixels
	{
		touch_cal_map (&m_TouchCal, nRawX, nRawY, (int) m_nWidth, (int) m_nHeight, pX, pY);
		return;
	}
	const unsigned SW = m_bScaled ? m_nWidth : m_nFbWidth, SH = m_bScaled ? m_nHeight : m_nFbHeight;
	unsigned x0 = 0, x1 = SW - 1, y0 = 0, y1 = SH - 1;
	if (m_pCalibration)
	{
		x0 = m_pCalibration[0]; x1 = m_pCalibration[1]; y0 = m_pCalibration[2]; y1 = m_pCalibration[3];
	}
	else if (m_nLogMaxX && m_nLogMaxY)			// the descriptor's logical maxima
	{
		x1 = m_nLogMaxX; y1 = m_nLogMaxY;
	}
	else
	{
		if (m_nRawMax < 32767 && (nRawX >= SW || nRawY >= SH))
			m_nRawMax = (nRawX > 4095 || nRawY > 4095) ? 32767 : 4095;
		if (m_nRawMax) x1 = y1 = m_nRawMax;
	}
	int x = (int) ((long) ((int) nRawX - (int) x0) * (SW - 1) / (x1 - x0)) - (int) m_nOffX;
	int y = (int) ((long) ((int) nRawY - (int) y0) * (SH - 1) / (y1 - y0)) - (int) m_nOffY;
	*pX = x < 0 ? 0 : x >= (int) m_nWidth ? m_nWidth - 1 : x;
	*pY = y < 0 ? 0 : y >= (int) m_nHeight ? m_nHeight - 1 : y;
}

void H::TouchHandler (TTouchScreenEvent Event, unsigned nID, unsigned nPosX, unsigned nPosY)
{
	CSidecarDisplay *p = CSidecarDisplay::s_pThis;
	p->m_nEvents = p->m_nEvents + 1;
	if (nID == 1)							// a second finger: the SEQ page's pinch
	{
		int x = 0, y = 0;
		if (Event != TouchScreenEventFingerUp) p->Map (nPosX, nPosY, &x, &y);
		p->m_nPointer2 = (Event == TouchScreenEventFingerUp ? 0 : 1u << 31) | (u32) y << 16 | (u32) x;
		p->m_nTouchSeq2 = p->m_nTouchSeq2 + 1;
		return;
	}
	if (nID != 0) return;						// the first finger only
	u32 nOld = p->m_nPointer;
	if (Event == TouchScreenEventFingerUp)
	{
		p->m_nPointer = nOld & 0x7FFF7FFF;			// released where it was
	}
	else
	{
		int x, y;
		p->Map (nPosX, nPosY, &x, &y);
		p->m_nRaw = (nPosY & 0xFFFF) << 16 | (nPosX & 0xFFFF);
		p->m_nPointer = 1u << 31 | (u32) y << 16 | (u32) x;
		if (Event == TouchScreenEventFingerDown) p->m_nDownSeq = p->m_nDownSeq + 1;
	}
	p->m_nTouchSeq = p->m_nTouchSeq + 1;
}

// the DSI panel's coordinates (Pi 4: 800x480; Pi 5: the panel's) to the screen's, unless a USB touch screen is bound (it has priority)
void H::DsiTouchHandler (TTouchScreenEvent Event, unsigned nID, unsigned nPosX, unsigned nPosY)
{
	CSidecarDisplay *p = CSidecarDisplay::s_pThis;
	if (p->m_pTouch) return;
	p->m_nEvents = p->m_nEvents + 1;
	if (nID > 1) return;						// two fingers (the second: the SEQ page's pinch)
	u32 nOld = p->m_nPointer;
	if (Event == TouchScreenEventFingerUp && nID == 1)
	{
		p->m_nPointer2 = 0;
		p->m_nTouchSeq2 = p->m_nTouchSeq2 + 1;
		return;
	}
	if (Event == TouchScreenEventFingerUp)
	{
		p->m_nPointer = nOld & 0x7FFF7FFF;
	}
	else
	{
#if RASPPI >= 5
		const unsigned PW = p->m_nFbWidth, PH = p->m_nFbHeight;	// rp1dsi: the panel's pixels (the UI's, unless turned)
#else
		const unsigned PW = 800, PH = 480;			// the firmware's touch buffer
#endif
		unsigned x = nPosX < PW ? nPosX : PW - 1, y = nPosY < PH ? nPosY : PH - 1;
		if (p->m_nDsiInvert & 1) x = PW - 1 - x;		// mirrored in the panel's axes
		if (p->m_nDsiInvert & 2) y = PH - 1 - y;
#if RASPPI >= 5
		if (p->m_nRot)						// turned: the inverse of PutRotated's maps
		{
			unsigned u = (p->m_nRot == 90 ? y : PH - 1 - y) * p->m_nWidth / PH;
			unsigned v = (p->m_nRot == 90 ? PW - 1 - x : x) * p->m_nHeight / PW;
			x = u;
			y = v;
		}
		else
#endif
		{
			const unsigned SW = p->m_bScaled ? p->m_nWidth : p->m_nFbWidth, SH = p->m_bScaled ? p->m_nHeight : p->m_nFbHeight;
			int sx = (int) (x * SW / PW) - (int) p->m_nOffX, sy = (int) (y * SH / PH) - (int) p->m_nOffY;
			x = sx < 0 ? 0 : sx >= (int) p->m_nWidth ? p->m_nWidth - 1 : (unsigned) sx;
			y = sy < 0 ? 0 : sy >= (int) p->m_nHeight ? p->m_nHeight - 1 : (unsigned) sy;
		}
		if (nID == 1)
		{
			p->m_nPointer2 = 1u << 31 | (u32) y << 16 | (u32) x;
			p->m_nTouchSeq2 = p->m_nTouchSeq2 + 1;
			return;
		}
		p->m_nRaw = (nPosY & 0xFFFF) << 16 | (nPosX & 0xFFFF);
		p->m_nPointer = 1u << 31 | (u32) y << 16 | (u32) x;
	}
	p->m_nTouchSeq = p->m_nTouchSeq + 1;
}

void H::MouseHandler (TMouseEvent Event, unsigned nButtons, unsigned nPosX, unsigned nPosY, int nWheel)
{
	CSidecarDisplay *p = CSidecarDisplay::s_pThis;
	p->m_nEvents = p->m_nEvents + 1;
	u32 nDown = p->m_nPointer & (1u << 31);
	if (Event == MouseEventMouseDown && (nButtons & MOUSE_BUTTON_LEFT)) nDown = 1u << 31;
	else if (Event == MouseEventMouseUp && (nButtons & MOUSE_BUTTON_LEFT)) nDown = 0;
	p->m_nRaw = (nPosY & 0xFFFF) << 16 | (nPosX & 0xFFFF);
	if (p->m_bScaled)						// the mouse moves in frame buffer pixels
	{
		nPosX = nPosX * p->m_nWidth / p->m_nFbWidth;
		nPosY = nPosY * p->m_nHeight / p->m_nFbHeight;
	}
	else								// centred: the UI's place taken off
	{
		nPosX = nPosX < p->m_nOffX ? 0 : nPosX - p->m_nOffX;
		nPosY = nPosY < p->m_nOffY ? 0 : nPosY - p->m_nOffY;
		if (nPosX >= p->m_nWidth) nPosX = p->m_nWidth - 1;
		if (nPosY >= p->m_nHeight) nPosY = p->m_nHeight - 1;
	}
	p->m_nPointer = nDown | (nPosY & 0x7FFF) << 16 | (nPosX & 0x7FFF);
	p->m_nTouchSeq = p->m_nTouchSeq + 1;
}

void H::TouchRemoved (CDevice *pDevice, void *pContext)
{
	((CSidecarDisplay *) pContext)->m_pTouch = 0;
}

void H::MouseRemoved (CDevice *pDevice, void *pContext)
{
	((CSidecarDisplay *) pContext)->m_pMouse = 0;
}
