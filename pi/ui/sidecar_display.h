// pi/ui/sidecar_display.h - the Pi display for Circle (docs/pi-display.md): LVGL on the firmware's frame buffer (the
// DSI display if the firmware reports one, else HDMI; on the Pi 5 the DSI display through Circle's addon/rp1dsi), touch from Circle's USB HID digitizer driver (priority), the
// DSI display's firmware touch buffer, or a USB mouse (mouse1), bound whenever they appear (plug-and-play), and
// the screen of sidecar_ui.h. All calls from one core; the touch events arrive on core 0 (USB IRQ) and are handed
// over through a few aligned words. No timer IRQ handler: LVGL's tick reads CTimer::GetClockTicks64.
#ifndef SIDECAR_DISPLAY_H
#define SIDECAR_DISPLAY_H

#include <circle/bcmframebuffer.h>
#include <circle/device.h>
#include <circle/types.h>
#include "sidecar_ui.h"
#include "touch_cal.h"

class CSidecarDisplay
{
public:
	CSidecarDisplay (unsigned nWidth = 0, unsigned nHeight = 0);	// 0: the firmware's display size (config.txt / EDID)

	boolean Initialize (void);		// FALSE: no frame buffer (no display at boot); then Update does nothing

	// The UI's size (docs/pi-display.md "Screen sizes"): 800x480 or 1024x600, the frame buffer's when it is one of them,
	// else 800x480 centred 1:1 on it with black borders (Pi 4), or scaled up to it (Pi 5's HDMI). SetUiSize before
	// Initialize overrides that (res= in /.rytm/display.txt or cmdline.txt; 0, 0: automatic). The HDMI display's EDID
	// (its preferred mode) is logged and, on the Pi 5, picks 1024x600 for a 1024x600 panel.
	void SetUiSize (unsigned nWidth, unsigned nHeight)	{ m_nUiReqW = nWidth; m_nUiReqH = nHeight; }

	// the boot screen (sidecar_splash.h), before Initialize: the frame buffer set up and the wordmark drawn (FALSE: no
	// frame buffer), then one bar step per boot stage; Initialize's LVGL home screen replaces it
	static const unsigned SplashSteps = 8;
	boolean Splash (void);
	void SplashStep (unsigned nStep);
	unsigned GetFrameWidth (void)		{ return m_bFrameBuffer ? m_nFbWidth : 0; }
	unsigned GetFrameHeight (void)		{ return m_bFrameBuffer ? m_nFbHeight : 0; }

	// lv_timer_handler at most every 10 ms (LVGL redraws dirty areas only, at up to 30 fps), touch1/mouse1 looked
	// up when bPlugAndPlayUpdated; returns at once otherwise
	void Update (boolean bPlugAndPlayUpdated);
	void SetStatus (const sidecar_ui_status_t &rStatus);	// applied at the next Update's redraw

	// the settings page's controls call this with console command lines ("notes=1"); on the thread that calls Update
	void SetCommandHandler (sidecar_ui_command_t pHandler, void *pParam)	{ sidecar_ui_set_command (pHandler, pParam); }

	boolean IsOn (void) const		{ return m_bOn; }
	unsigned GetWidth (void) const		{ return m_nWidth; }
	unsigned GetHeight (void) const		{ return m_nHeight; }
	const char *GetInput (void) const;	// "touch (USB)", "touch (DSI)", "mouse1" or "none"
	// the touch diagnostics for the periodic log line: events (down/move/up) and USB reports since the last call, the last
	// raw x/y and pressed state, and the HID endpoint's counters (circle-hid.patch); "" without a USB touch screen
	const char *TakeTouchDiag (void);
	// the bound input's contacts: 0 none, 1 single touch (no pinch: e.g. an absolute-pointer USB panel, a mouse),
	// 2+ multi-touch; from C: sidecar_touch_contacts() (0 before the display is up)
	unsigned GetTouchContacts (void) const;
	// the 4-point calibration (touch_cal.h, "touchcal=" in /.rytm/display.txt): raw -> the UI's pixels; 0: none (then
	// cmdline touchscreen=, else the USB descriptor's logical range). It wins over both.
	void SetTouchCal (const touch_cal_t *pCal);
	static unsigned TouchContacts (void)	{ return s_pThis ? s_pThis->GetTouchContacts () : 0; }
	unsigned TakeMaxUpdateUs (void);	// the longest Update (a redraw) since the last call
	unsigned PeekMaxUpdateUs (void) const	{ return m_nMaxUs; }	// the same, not reset (the STATUS page)

	// The screensaver: after nMinutes (0: never) without a touch or a display command the screen goes black (the
	// frame buffer filled black, LVGL not run, the firmware's blank-screen call); the next touch only wakes it (its
	// press reaches no control). Activity: a display command (the caller calls Activity). Logged when it blanks/wakes.
	void SetScreensaver (unsigned nMinutes);
	unsigned GetScreensaver (void) const	{ return m_nSaverMin; }
	boolean IsBlank (void) const		{ return m_bBlank; }
	void Activity (void);

private:
	boolean OpenFrameBuffer (void);
	void OpenDsiTouch (void);
	void BindInput (void);
	void Blank (boolean bOn);
	void Put (const CDisplay::TArea &rArea, const u16 *pPixels);	// UI pixels to the frame buffer (centred, or scaled on the Pi 5)
	void ReadEdid (void);						// HDMI: the display's preferred mode (m_nEdidW/H), logged
	void ChooseUi (void);						// the UI's size and place (Initialize)
#if RASPPI >= 5
	boolean OpenRp1Dsi (void);					// the DSI display through addon/rp1dsi (OpenFrameBuffer)
	void ScaleSetup (unsigned nWidth, unsigned nHeight);		// the UI's size and the scaler (OpenFrameBuffer)
#endif
	friend struct TSidecarDisplayHooks;	// the LVGL and Circle callbacks (sidecar_display.cpp)

private:
	unsigned	m_nReqWidth, m_nReqHeight;
	unsigned	m_nUiReqW, m_nUiReqH;	// SetUiSize (res=), 0: automatic
	unsigned	m_nEdidW, m_nEdidH;	// the HDMI display's preferred mode (EDID), 0: not read
	unsigned	m_nOffX, m_nOffY;	// Pi 4: the UI's top left in the frame buffer (centred), else 0
	CBcmFrameBuffer	*m_pFB;			// on the DSI display when the firmware reports one (OpenFrameBuffer)
	CDisplay	*m_pOut;		// what is drawn on: m_pFB, or the Pi 5's rp1dsi display (m_pRPiTouch)
	unsigned	m_nDisplay;		// the firmware's display number of m_pFB
	boolean		m_bDsiSeen;		// the firmware reported a DSI display
	boolean		m_bDsiUnknown;		// the firmware did not answer the display id
	boolean		m_bFrameBuffer;		// m_pOut initialized (Splash or Initialize)
	boolean		m_bRp1Tried;		// Pi 5: the DSI display looked for (once)
	boolean		m_bOn;
	unsigned	m_nWidth, m_nHeight;	// the UI's (LVGL's) size: the frame buffer's, or on the Pi 5 a smaller one
	unsigned	m_nFbWidth, m_nFbHeight;	// the frame buffer's
	boolean		m_bScaled;		// Pi 5: the UI scaled up to the frame buffer (Put)
	u16		*m_pXMap;		// scaled: the UI column of each frame buffer column
	u16		*m_pScaleBuf;		// scaled: ScaleRows frame buffer rows, DMA'd to the frame buffer
	u16		*m_pSplash;		// scaled: the boot screen at the UI's size (freed by Initialize)
	static const unsigned ScaleRows = 64;
	u16		*m_pBuffer;
	u16		*m_pDotBuf;		// the OLED theme: DotRows UI rows of dots expanded to screen pixels (Flush)
	static const unsigned DotRows = 16;
	unsigned	m_nLastUpdate;
	unsigned	m_nMaxUs;
	CDevice *volatile m_pTouch;
	CDevice *volatile m_pMouse;
	class CRPiTouchScreen *m_pRPiTouch;	// Pi 4: the DSI display's touch (the firmware's buffer); Pi 5: the rp1dsi display; or 0
	CDevice		*m_pDsiTouch;		// its touchN device
	unsigned	m_nDsiInvert;		// cmdline dsitouch_inv: 1 x, 2 y
	const char	*m_pSource;		// the touch source last logged
	const unsigned	*m_pCalibration;	// cmdline touchscreen=minx,maxx,miny,maxy, or 0: auto (raw = pixels, or 12/15-bit)
	unsigned	m_nRawMax;		// auto: 0 (raw = pixels), 4095 or 32767
	unsigned	m_nLogMaxX, m_nLogMaxY;	// the USB touch's logical X/Y maxima (its descriptor), 0: not known
	boolean		m_bTouchCal;		// m_TouchCal set (SetTouchCal)
	touch_cal_t	m_TouchCal;
	volatile u32	m_nDownSeq;		// fingers down so far (core 0); PointerRead shows a press LVGL missed
	u32		m_nReadDownSeq;		// the last one PointerRead saw
	u32		m_nShownDownSeq;	// the last one Update showed (the readout, the calibration)
	boolean		m_bLoggedDown;		// the log's touch: down / up lines
	unsigned	m_nUsbContacts;		// the USB touch's contacts a report (1: single touch), 0: not known

	// core 0 (USB IRQ) -> the UI core
	volatile u32	m_nPointer;		// pressed << 31 | y << 16 | x (screen)
	volatile u32	m_nRaw;			// raw y << 16 | raw x
	volatile u32	m_nTouchSeq;		// bumped per event: the readout's change
	volatile u32	m_nEvents;		// all touch events (a counter that is never reset)
	volatile u32	m_nPointer2;		// the second finger: pressed << 31 | y << 16 | x (screen)
	volatile u32	m_nTouchSeq2;		// bumped per second-finger event
	u32		m_nDiagEvents, m_nDiagReports, m_nDiagResets;
	char		m_DiagText[384];	// the touch diagnostics line (reenum and its flags made it longer)

	void Map (unsigned nRawX, unsigned nRawY, int *pX, int *pY);

	u32		m_nShownSeq;
	u32		m_nShownSeq2;
	unsigned	m_nSaverMin;
	u64		m_nLastActive;		// CTimer::GetClockTicks64 of the last touch or command
	boolean		m_bBlank;
	volatile boolean m_bSwallow;		// the waking press: the pointer reads released until the finger is up
	sidecar_ui_status_t m_Status;
	boolean		m_bStatusNew;

	static CSidecarDisplay *s_pThis;
};

extern "C" unsigned sidecar_touch_contacts (void);	// CSidecarDisplay::TouchContacts ()

#endif
