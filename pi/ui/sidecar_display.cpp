// pi/ui/sidecar_display.cpp - see sidecar_display.h. Based on Circle's addon/lvgl/lvgl.cpp (GPL-3.0, R. Stange),
// changed: the touch screen and mouse are bound late (they enumerate after USB's plug-and-play), raw touch values are
// scaled here (Circle's USB touch driver reports the HID logical range unscaled), synchronous flushes, a tick callback,
// the firmware's DSI display preferred and its touch (CRPiTouchScreen) as a second touch source; on the Pi 5 the DSI
// display driven by Circle's addon/rp1dsi instead (docs/pi-display.md, docs/pi5.md).
#include "sidecar_display.h"
#include "sidecar_splash.h"
#include "lvgl.h"
#include <circle/bcmpropertytags.h>
#include <circle/devicenameservice.h>
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
	m_nPointer (0),
	m_nRaw (0),
	m_nTouchSeq (0),
	m_nEvents (0),
	m_nDiagEvents (0),
	m_nDiagReports (0),
	m_nDiagResets (0),
	m_nShownSeq (0),
	m_nSaverMin (60),
	m_nLastActive (0),
	m_bBlank (FALSE),
	m_bSwallow (FALSE),
	m_bStatusNew (FALSE)
{
	memset (&m_Status, 0, sizeof m_Status);
	for (unsigned c = 0; c < SIDECAR_UI_CORES; c++) m_Status.load[c] = -1;
	s_pThis = this;
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
	if (m_bFrameBuffer)
	{
		m_nFbWidth = m_nWidth = m_pFB->GetWidth ();
		m_nFbHeight = m_nHeight = m_pFB->GetHeight ();
#if RASPPI >= 5
		ScaleSetup (nUiWidth, nUiHeight);
#endif
	}
	LOGNOTE ("firmware %u display(s):%s%s -> frame buffer on %u %ux%u%s", nDisplays, (const char *) List,
		 bIdAnswered ? "" : " (display id not answered)", m_nDisplay,
		 m_bFrameBuffer ? m_pFB->GetWidth () : 0, m_bFrameBuffer ? m_pFB->GetHeight () : 0,
		 m_bFrameBuffer ? "" : " FAILED");
	return m_bFrameBuffer;
}

// UI pixels (rArea in the UI's coordinates, pPixels its rows, the area's width each) to the frame buffer: as they are,
// or on the Pi 5 scaled up nearest-neighbour to the whole frame buffer (non-uniform: an 800x480 UI on 1920x1080 is
// x2.4 y2.25, which a 5:3 panel showing a 16:9 signal stretches back). Frame buffer column X shows UI column
// X * W / FW (m_pXMap), row Y row Y * H / FH; the area's frame buffer rows are those that map into it, so
// neighbouring areas meet without gaps. Each UI row is scaled once, its repeats copied, ScaleRows rows per DMA.
void CSidecarDisplay::Put (const CDisplay::TArea &rArea, const u16 *pPixels)
{
	if (!m_bScaled)
	{
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
	unsigned nRotate = pOpt->GetAppOptionDecimal ("dsi_rotate", 0) == 180 ? 180 : 0;
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
			CRPiTouchScreen *pDsi = new CRPiTouchScreen (CInterruptSystem::Get (), 16, nPort, bTouch);
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
			CDevice *pTouch = bTouch ? DsiTouchDevice (pBefore, TRUE) : 0;
			if (pTouch)
			{
				m_pDsiTouch = pTouch;
				((CTouchScreenDevice *) pTouch)->Setup (pDsi);
				((CTouchScreenDevice *) pTouch)->RegisterEventHandler (H::DsiTouchHandler);
			}
			LOGNOTE ("DSI: DISP%u %ux%u (rp1dsi, no scaling), rotation %u, touch %s%s", nPort, m_nWidth, m_nHeight,
				 nRotate, pTouch ? "FT5x06 polled" : "none", pTouch && m_nDsiInvert ? ", inverted" : "");
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

// The UI's size on the Pi 5: cmdline.txt's width= and height= if both are given, else 800x480 (the size the UI is
// laid out for) when the frame buffer is bigger than 1024x600, else the frame buffer's. Never bigger than the frame
// buffer (no downscaling); equal to it: no scaling.
void CSidecarDisplay::ScaleSetup (unsigned nWidth, unsigned nHeight)
{
	const unsigned FW = m_nFbWidth, FH = m_nFbHeight;
	if (nWidth == 0 || nHeight == 0)
	{
		nWidth = FW > 1024 || FH > 600 ? 800 : FW;
		nHeight = FW > 1024 || FH > 600 ? 480 : FH;
	}
	if (nWidth > FW) nWidth = FW;
	if (nHeight > FH) nHeight = FH;
	if (nWidth < 320 || nHeight < 240 || (nWidth == FW && nHeight == FH)) return;
	m_pXMap = new u16[FW];
	m_pScaleBuf = new (HEAP_DMA30) u16[FW * ScaleRows];
	if (!m_pXMap || !m_pScaleBuf)
	{
		LOGWARN ("no memory for the scaler: the UI at %ux%u", FW, FH);
		return;
	}
	for (unsigned X = 0; X < FW; X++) m_pXMap[X] = X * nWidth / FW;
	m_nWidth = nWidth;
	m_nHeight = nHeight;
	m_bScaled = TRUE;
	LOGNOTE ("UI %ux%u scaled to the frame buffer's %ux%u (x %u.%02u, y %u.%02u; width=/height= in cmdline.txt set the UI's size)",
		 nWidth, nHeight, FW, FH, FW / nWidth, FW * 100 / nWidth % 100, FH / nHeight, FH * 100 / nHeight % 100);
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

	lv_init ();
	lv_tick_set_cb (H::Tick);
	lv_log_register_print_cb (H::Log);

	const unsigned nPixels = m_nWidth * 48;			// a 48-line band: 77 KB at 800 wide
	m_pBuffer = new (HEAP_DMA30) u16[nPixels];
	if (!m_pBuffer) return FALSE;

	lv_display_t *pDisplay = lv_display_create (m_nWidth, m_nHeight);
	lv_display_set_flush_cb (pDisplay, H::Flush);
	lv_display_set_buffers (pDisplay, m_pBuffer, 0, nPixels * sizeof (u16), LV_DISPLAY_RENDER_MODE_PARTIAL);

	lv_indev_t *pIndev = lv_indev_create ();
	lv_indev_set_type (pIndev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb (pIndev, H::PointerRead);

	const unsigned *pCal = CKernelOptions::Get ()->GetTouchScreen ();
	if (pCal && pCal[0] < pCal[1] && pCal[2] < pCal[3]) m_pCalibration = pCal;

	sidecar_ui_create ();
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
	for (unsigned i = 1; i <= 8 && !m_pTouch; i++)
	{
		CString Name;
		Name.Format ("touch%u", i);
		CTouchScreenDevice *pTouch = (CTouchScreenDevice *) pNames->GetDevice (Name, FALSE);
		if (pTouch && (CDevice *) pTouch != m_pDsiTouch)
		{
			pTouch->Setup (m_pOut);		// no SetCalibration: raw values (Map scales them)
			pTouch->RegisterEventHandler (H::TouchHandler);
			pTouch->RegisterRemovedHandler (H::TouchRemoved, this);
			m_pTouch = pTouch;
			LOGNOTE ("touch screen bound (%s)", (const char *) Name);
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

	if (m_pMouse) ((CMouseDevice *) m_pMouse)->UpdateCursor ();	// polls the mouse (cursor off)
	if (m_pTouch) ((CTouchScreenDevice *) m_pTouch)->Update ();
	if (m_pDsiTouch) ((CTouchScreenDevice *) m_pDsiTouch)->Update ();	// reads the firmware's touch buffer
	CUSBTouchScreenDevice *pUSBTouch = CUSBTouchScreenDevice::Get ();
	if (pUSBTouch) pUSBTouch->Poll ();		// resets and resubmits a halted report endpoint (main loop: xHCI commands)

	u32 nSeq = m_nTouchSeq;
	boolean bDown = (m_nPointer >> 31) != 0;
	if (m_bSwallow && !bDown) m_bSwallow = FALSE;	// the waking finger is up: touches reach the controls again
	if (nSeq != m_nShownSeq) Activity ();
	if (m_bBlank)
	{
		if (nSeq != m_nShownSeq && bDown)
		{
			m_bSwallow = TRUE;			// this press only wakes the screen
			Blank (FALSE);
		}
		else
		{
			m_nShownSeq = nSeq;
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
		u32 p = m_nPointer, r = m_nRaw;
		sidecar_ui_touch (p >> 31, r & 0xFFFF, r >> 16, p & 0x7FFF, (p >> 16) & 0x7FFF);
	}
	if (m_bStatusNew)
	{
		m_bStatusNew = FALSE;
		sidecar_ui_status (&m_Status);
	}
	lv_timer_handler ();
	unsigned nUs = CTimer::GetClockTicks () - nNow;
	if (nUs > m_nMaxUs) m_nMaxUs = nUs;
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
		     "ep state %u pend %u ignored %u%s",
		     nEvents - m_nDiagEvents, S.nReports - m_nDiagReports, r & 0xFFFF, r >> 16, (p >> 31) ? "DOWN" : "up",
		     S.nErrors, S.nXhciErrors, S.nLastCode, S.nResubmits, S.nResets, S.nResets - m_nDiagResets, S.nFailed,
		     S.nState, S.nPending, pUSB->GetIgnored (), S.bGaveUp ? " GAVE-UP" : "");
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

void H::Flush (lv_display_t *pDisplay, const lv_area_t *pArea, u8 *pBuffer)
{
	CDisplay::TArea Area;
	Area.x1 = pArea->x1;
	Area.x2 = pArea->x2;
	Area.y1 = pArea->y1;
	Area.y2 = pArea->y2;
	CSidecarDisplay::s_pThis->Put (Area, (const u16 *) pBuffer);		// no completion routine: DMA and wait
	lv_display_flush_ready (pDisplay);
}

void H::PointerRead (lv_indev_t *pIndev, lv_indev_data_t *pData)
{
	u32 p = CSidecarDisplay::s_pThis->m_nPointer;
	pData->state = (p >> 31) && !CSidecarDisplay::s_pThis->m_bSwallow ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
	pData->point.x = p & 0x7FFF;
	pData->point.y = (p >> 16) & 0x7FFF;
}

void H::Log (lv_log_level_t Level, const char *pMsg)
{
	CLogger::Get ()->Write ("lvgl", Level >= LV_LOG_LEVEL_ERROR ? LogError : LogNotice, "%s", pMsg);
}

u32 H::Tick (void)
{
	return (u32) (CTimer::GetClockTicks64 () / (CLOCKHZ / 1000));
}

// raw -> screen: touchscreen= from cmdline.txt, else raw = pixels until a value is out of the screen, then a 12-bit
// (0-4095) or 15-bit (0-32767) range, the usual HID digitizer ranges
void CSidecarDisplay::Map (unsigned nRawX, unsigned nRawY, int *pX, int *pY)
{
	unsigned x0 = 0, x1 = m_nWidth - 1, y0 = 0, y1 = m_nHeight - 1;
	if (m_pCalibration)
	{
		x0 = m_pCalibration[0]; x1 = m_pCalibration[1]; y0 = m_pCalibration[2]; y1 = m_pCalibration[3];
	}
	else
	{
		if (m_nRawMax < 32767 && (nRawX >= m_nWidth || nRawY >= m_nHeight))
			m_nRawMax = (nRawX > 4095 || nRawY > 4095) ? 32767 : 4095;
		if (m_nRawMax) x1 = y1 = m_nRawMax;
	}
	int x = (int) ((long) ((int) nRawX - (int) x0) * (m_nWidth - 1) / (x1 - x0));
	int y = (int) ((long) ((int) nRawY - (int) y0) * (m_nHeight - 1) / (y1 - y0));
	*pX = x < 0 ? 0 : x >= (int) m_nWidth ? m_nWidth - 1 : x;
	*pY = y < 0 ? 0 : y >= (int) m_nHeight ? m_nHeight - 1 : y;
}

void H::TouchHandler (TTouchScreenEvent Event, unsigned nID, unsigned nPosX, unsigned nPosY)
{
	CSidecarDisplay *p = CSidecarDisplay::s_pThis;
	p->m_nEvents = p->m_nEvents + 1;
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
	}
	p->m_nTouchSeq = p->m_nTouchSeq + 1;
}

// the DSI panel's coordinates (Pi 4: 800x480; Pi 5: the panel's) to the screen's, unless a USB touch screen is bound (it has priority)
void H::DsiTouchHandler (TTouchScreenEvent Event, unsigned nID, unsigned nPosX, unsigned nPosY)
{
	CSidecarDisplay *p = CSidecarDisplay::s_pThis;
	if (p->m_pTouch) return;
	p->m_nEvents = p->m_nEvents + 1;
	if (nID != 0) return;						// the first finger only
	u32 nOld = p->m_nPointer;
	if (Event == TouchScreenEventFingerUp)
	{
		p->m_nPointer = nOld & 0x7FFF7FFF;
	}
	else
	{
#if RASPPI >= 5
		const unsigned PW = p->m_nWidth, PH = p->m_nHeight;	// rp1dsi: the panel's pixels, the UI's size
#else
		const unsigned PW = 800, PH = 480;			// the firmware's touch buffer
#endif
		unsigned x = nPosX < PW ? nPosX : PW - 1, y = nPosY < PH ? nPosY : PH - 1;
		if (p->m_nDsiInvert & 1) x = PW - 1 - x;
		if (p->m_nDsiInvert & 2) y = PH - 1 - y;
		x = x * p->m_nWidth / PW;
		y = y * p->m_nHeight / PH;
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
