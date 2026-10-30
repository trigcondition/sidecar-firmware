// pi/ui/sidecar_insfx.h - the Pi display's INSERT FX page (docs/INSERT-FX.md): one Sidecar voice's two insert slots (the
// TYPE, P1..P4 as dials with the TYPE's labels and values), its sends to the three FX buses, and the buses' own chains.
// The voice shown follows the Rytm's selected track (as VOICE does) until a voice key is tapped; tapping a slot's TYPE
// steps it to the next TYPE (the console's "ins=Vn:S:type:T", run by the caller). LVGL only (builds on the Mac too,
// pi/ui/host); both themes, OLED the default. The caller hands it a view ~10 times a second while it shows
// (sidecar_insfx_set); it redraws only what changed.
#ifndef SIDECAR_INSFX_H
#define SIDECAR_INSFX_H

#ifdef __cplusplus
extern "C" {
#endif

#define SIF_VOICES	8
#define SIF_CHAINS	11		// V1..V8, B1..B3
#define SIF_BUSES	3

typedef struct
{
	int		type;			// 0 OFF .. 8 RINGMOD, 9 AMP
	char		name[8];		// "FILTER"
	char		label[4][5];		// "CUT" ..
	char		val[4][8];		// "1.6K" ..
	unsigned char	v[4];			// 0..127 as it plays (a lock / an LFO included)
} sidecar_insfx_slot_t;

typedef struct
{
	int			valid;
	int			voice;			// the Rytm's selected track's voice (0..7), -1 unknown
	int			track;			// that track (0..11), -1 unknown
	sidecar_insfx_slot_t	slot[SIF_CHAINS][2];
	unsigned char		send[SIF_VOICES][SIF_BUSES];
	char			bus_fx[2][12];		// bus 1 / 2: the send FX's TYPE ("OFF")
	unsigned char		bus_post[SIF_BUSES];	// 1: returns POST (0/1), 0 PRE (2/3)
	// the OUT route (core/fx/out.h, docs/STEREO.md): each voice's route (0 TRK, 1 PRE, 2 POST), LEV / PAN / WID as
	// they play (0..127) and their texts, its pair's peaks as sent (0..127 of the pair's ceiling; PRE / POST only)
	unsigned char		out_route[SIF_VOICES];
	unsigned char		out_v[SIF_VOICES][3];
	char			out_val[SIF_VOICES][3][6];
	unsigned char		out_peak[SIF_VOICES][2];
	// ins1112: each chain's POST EQ (the stage after both slots), ready made: "" when it is flat, else
	// "HPF 420  LPF 6.2K  LOW +3.0DB  HIGH FLAT". Buses 1 and 2 are the send FX returns.
	char			peq[SIF_CHAINS][56];
	unsigned		gen;			// changes with anything above
} sidecar_insfx_view_t;

typedef void (*sidecar_insfx_command_t) (const char *cmd);
void sidecar_insfx_create (void *parent, int y0);	// hidden until shown
void sidecar_insfx_set_command (sidecar_insfx_command_t fn);
void sidecar_insfx_show (int on);
int sidecar_insfx_shown (void);
void sidecar_insfx_set (const sidecar_insfx_view_t *v);	// copied; cheap when gen is unchanged
int sidecar_insfx_voice (void);				// the voice shown now

#ifdef __cplusplus
}
#endif

#endif
