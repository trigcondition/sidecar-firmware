# Zynthian (V5 / V4) as a Sidecar hardware platform

Research 2026-10-20, public sources only. Tags: **[doc]** = stated in an official/public source (URL given), **[sch]** = read by us from the published KiCad schematics (github.com/zynthian/zynthian-hw, V5_main + V5_control; our reading of netlists, not an official statement), **[inf]** = inferred / from memory, verify before relying on it.

## Verdict

Feasible and attractive as a *host hardware* for a bare-metal Circle Sidecar, but not a drop-in:
- The V5.1 is a Pi 5 plus a fully documented, KiCad-published board set. Controls are plain MCP23017 I2C expanders and WS2812 LEDs (no controller MCU, no firmware protocol to reverse). Display is the Pi's own 5" 800x480 DSI panel. Codecs are TI PCM5242/PCM1863 on the Pi I2S bus. All of this is driveable bare-metal.
- The USB-B jack on the case is wired to the Pi 5's USB-C data pins, and the Pi is powered from the DC jack, so the Pi-5 gadget problem we have (USB-C powered => no device mode) is probably solved by the board, with caveats (see 3).
- The Pi's USB-A ports (2x USB3, 2x USB2) are on the rear panel, so the Rytm connects as host. Ethernet and HDMI also exist.
- Blockers: price (395 EUR ex VAT without Pi, often "Not available for sale"), the DSI panel needs our rp1dsi path to match the Zynthian panel, and the engine "lift" list is much shorter than the Zynthian catalogue suggests: most are LV2/JUCE plugins.
- Recommendation: treat Zynthian V5.1 as a supported *target board* (a hardware profile in our Pi build), not as a software base. Do not depend on Zynthian software. Port only the DSP cores listed in 4.

## 1. Hardware

### V5.1 (current, Pi 5)
| Item | Fact | Tag / source |
|---|---|---|
| Pi | Raspberry Pi 5 (BCM2712, 4 or 8 GB, optional extra, not included by default); main board also supports Pi 4 (V5 original shipped with Pi 4) | [doc] https://zynthian.org/technical-specifications ; https://blog.zynthian.org/2024/09/zynthian-v5-1-kit-released/ ; https://blog.zynthian.org/2023/07/zynthian-kit-v5-is-here/ |
| CM | No Compute Module; Pi sits in a 2x20 socket on the main board, thermal block to case | [doc] assembly: https://wiki.zynthian.org/index.php/Assembling_Zynthian_Kit_V5.1 |
| Display | 5" capacitive multi-touch, DSI, 800x480, 5-point touch; touch and display via 24-pin connector (HDMI, GPIO, I2C for touch) | [doc] https://blog.zynthian.org/2023/07/zynthian-kit-v5-is-here/ ; spec page says 5" DSI (no resolution) |
| Controls | 4 rotary encoders with switch, 20 silicone buttons with RGB LEDs | [doc] spec page |
| How read | Control board has 3 x MCP23017 (blog says 2; the V5_control schematic contains 3) on I2C, with per-chip interrupt lines int1..int7 to Pi GPIO; encoders pot1..pot4 a/b/sw. Known: MCP at 0x20 INTA=GPIO5, INTB=GPIO6, further at 0x21/0x22; free GPIO 23/24 are int6/int7 on the EXT header | [doc] blog + https://discourse.zynthian.org/t/v5-kit-available-gpios/10717 ; [sch] V5_control.kicad_sch (3x MCP23017_ML, 5 encoder-switch symbols, global labels pot1-4, int1-7) |
| LEDs | 21 x WS2812B on one data net "LEDS" from main board | [sch] V5_control.kicad_sch; main board label "LEDS" (which Pi GPIO not determined) |
| Control connector | FPC/FFC 20-pin main<->control board: GPIO, SPI, I2C (SPI is presumably the WS2812 drive; unverified) | [doc] blog; usage of SPI [inf] |
| Controller MCU | None. No RP2040/STM32 anywhere in V5_main/V5_control | [sch] (parts lists: only MCP23017, PCM5242, PCM1863, TPA6130A2, SIC478, RTC, WS2812B) |
| Audio | DAC PCM5242 (TI), ADC PCM1863 (TI), headphone amp TPA6130A2 (I2C volume, enable GPIO "HPAMP_ENABLE"). 24-bit up to 192 kHz; dedicated oscillators/LDOs. Line out 2x balanced TRS, line in 2x balanced TRS (-12..+32 dB gain), 1 headphone | [doc] spec page + blog ; [sch] dac.kicad_sch has dedicated ASCO oscillator; BCK/LRCK/DIN/DOUT go to Pi I2S |
| I2C codec addresses | Not published in text. Both chips are I2C-controlled (sda/scl labels on both). Address straps are the ADR pins in dac.kicad_sch; we did not trace them. Read them from the schematic or with an I2C scan on real hardware | [inf] open item |
| I2S format / clocking | Standard Pi I2S (GPIO18-21). Whether Pi or codec is clock master was not determined (BCK/LRCK are inputs to the DAC sheet in the schematic, which suggests Pi is master, but with a separate oscillator for MCLK) | [sch] weak / [inf] |
| MIDI | DIN-5 IN, OUT, THRU (via Pi UART) | [doc] spec page ; [sch] labels MIDI-IN/OUT/THRU |
| USB | Case rear: 2x USB3 + 2x USB2 type A (Pi ports), Ethernet, plus one USB-B "device" connector. (Per user photo of shop.zynthian.org; the spec page itself only lists USB-B) | [doc] spec page ; photo from user |
| CV/Gate | Not on V5; V4 has Zynaptik expansion with CV/gate ADC/DAC footprints (unpopulated); V5 EXT header allows adding ADS1115/MCP4728 | [doc] https://wiki.zynthian.org/index.php/Zynthian_Hardware ; zyncoder README |
| RTC | RV-3018-C7 per blog; the schematic symbol reads RV-3028-C7 (they are both I2C Micro Crystal RTCs; check). CR2032 holder | [doc] blog ; [sch] |
| Power | DC barrel jack 9-48 V, switch, SIC478 buck to 5 V | [doc] spec page ; [sch] psu.kicad_sch |
| Case | Black anodized aluminium, thermal block, laser engraved | [doc] spec page |
| Price/availability | 395 EUR ex VAT (kit without Pi; +110 EUR Pi 5 4 GB, +160 EUR 8 GB; +15 PSU). Shop page showed "Not Available For Sale" (ships from Spain 7-14 days when in stock); V5.1 had been out of stock for stretches | [doc] https://shop.zynthian.org/shop/zynthian-v5-1-kit-383?category=5 ; blog |

### V4 (comparison)
| Item | Fact | Source |
|---|---|---|
| Pi | Pi 4 recommended (Pi 3/3B+ supported, Pi 5 supported by the current software) | [doc] https://wiki.zynthian.org/index.php/Zynthian_Hardware |
| Display | Zynscreen v1.5: 3.5" SPI TFT, XPT2046 resistive touch, an Intel EPM3032A CPLD converts SPI data to the LCD panel (480x320 [inf]) | [doc] same page |
| Controls | Zynscreen MCP23017 port expander: 4 encoders + 4 buttons | [doc] same page |
| Audio | HiFiBerry DAC+ADC (Burr-Brown) stage, TRS stereo line in/out | [doc] same page |
| MIDI/other | Zynaptik board: MIDI IN/OUT/THRU, CV/gate footprints | [doc] same page |
| Status | V4.1 superseded by V5/V5.1; V4 hardware repo folders (ZynScreen_v1.5, Zynaptik_v2/v3) exist | [doc] https://github.com/zynthian/zynthian-hw |

V4 is a worse target: small SPI display behind a CPLD (needs a custom framebuffer driver), only 4 encoders/4 buttons, HiFiBerry-style codec (Circle has support only for generic I2S, we would write the codec init anyway). Use V5.1 only.

## 2. Openness and licences

- Hardware: https://github.com/zynthian/zynthian-hw is CC BY-SA 4.0, with full KiCad projects (V5_main with dac.kicad_sch and psu.kicad_sch, V5_control, V5_zynface, plus dxf/svg case parts) [doc]. Wiki text is CC BY-SA 3.0 [doc]. The case files are at https://github.com/zynthian/zynthian-case [doc].
- Software: zynthian-ui, zynthian-sys, zyncoder are GPL-3.0 [doc] (GitHub pages). zyncoder is the C library that reads the encoders/switches (zyncontrol_v5.c, zynmcp23017.c, zynads1115.c, tpa6130.c, WS driver) and requires wiringPi [doc]. It is the reference for the V5 pin/INT mapping and is GPL-3, i.e. compatible with our Pi binary.
- Controller firmware/protocol: none exists. The "protocol" is direct MCP23017 register access over I2C plus interrupt GPIOs [sch/doc]. A bare-metal driver is straightforward; the exact bit-to-control mapping is in zyncontrol_v5.c (we could not fetch it; the file exists per the repo listing [doc]; fetch it from the repo clone).
- Name/hardware restrictions: no restriction found on running other software on the hardware. "Zynthian" is the project's name; we would not call our product Zynthian or sell a pre-installed "Zynthian" image; the kit is sold by the project as a DIY kit [doc: https://zynthian.org/]. We found no trademark policy text either way [inf: ask jofemodo before shipping bundles].
- CC BY-SA means a derived board needs attribution + same licence; using the finished kit needs nothing.

## 3. Porting effort for the Circle engine

Drivers needed (all bare metal on Pi 5 / RP1):
1. I2C (Circle has it) for 3 x MCP23017, 2 codecs, TPA6130, RTC. Easy. Replace interrupts with polling at 1 kHz if the RP1 GPIO interrupt path is not in Circle yet. ~2-3 days.
2. Encoders/buttons: decode MCP23017 GPIOA/B (port-read with INTCAP) into 4 quadrature encoders + 4 push + 20 buttons. Needs mapping from zyncontrol_v5.c. ~2 days.
3. WS2812B x21: needs a timing-accurate driver. On Pi 5 the RP1 has no Pi-4-style PWM/DMA trick; options are SPI at 2.4-3.2 MHz via a SPI bit-pattern, or RP1 PIO (RP1 has PIO; Circle support [inf] unknown). Which GPIO the LEDS net is on must be read from the main-board schematic. ~2-4 days, highest risk of the control items.
4. Display: Pi DSI 5" 800x480 via our Circle rp1dsi path (already working per project). The panel's DSI init sequence/timing is a Zynthian-specific panel; the spec gives no controller. Risk: medium, depends on whether the panel is a standard Pi DSI touchscreen (touch controller via I2C on the 24-pin connector) [inf]. Touch controller part number not found. ~3-7 days.
5. Audio: PCM5242 + PCM1863 on I2S. Circle has I2S sound (PWM/I2S driver); we add codec init over I2C (reset, format, clock-source, volume). PCM5242 can derive its clock from BCK (PLL) or use the oscillator MCLK; TPA6130 enable GPIO "HPAMP_ENABLE". Requires the register init sequence from the datasheets (TI, public). ~1 week including tuning. Risk: Pi 5 I2S in Circle's RP1 support; also the unknown clock-master wiring (3.).
6. DIN MIDI via UART: easy. RTC: optional.

Reusable from Zynthian: the pin/INT maps, the zyncoder logic as reference (GPL-3 compatible), the case and panel geometry. Not reusable: the UI/engine manager (Python, JACK, Linux).

USB gadget on Pi 5 (key question):
- [doc] The USB-B jack is bridged to the Pi's USB-C: "USB-C port is bridged to the main board's USB type-B connector" (https://blog.zynthian.org/2023/07/zynthian-kit-v5-is-here/) and the spec page calls it "USB type B connector for USB-device operation". The V5.1 kit assembly uses a flat cable (USB-C adapter) plugged into the Pi's USB-C socket (step 3.7.5, https://wiki.zynthian.org/index.php/Assembling_Zynthian_Kit_V5.1). A V5 "minimal upgrade" harness thread confirms D+ = green, D- = white wires (https://discourse.zynthian.org/t/connecting-and-using-zynthian-as-a-midi-device/13077).
- [sch] In V5_main.kicad_sch, USB-B1 D+/D- nets go to the USB-C1 flat-cable connector (Conn_01x22 / SFV20R footprint). The same connector also carries the 5 V rail (USB-C1 pin 2 region has a "5V" label) and the board's 5 V net goes to Pi header pins 2/4 (5V). So the Pi is powered from the barrel jack via SIC478 -> 5 V, apparently both via the 40-pin header and the USB-C VBUS pin. Not stated by Zynthian in words; our reading of the netlist; verify with a multimeter/PD behaviour on real hardware. Developers confirm only that the barrel connector is the sole power input and that a diode on the main board blocks power from USB-B: https://discourse.zynthian.org/t/is-a-v5-1-kit-rpi-5-build-powerable-by-usb/11253 [doc].
- Consequence: the Pi 5 is powered by the board, so Raspberry Pi's "power via header for device mode" advice is met; gadget on the USB-B jack should be usable, and the Rytm can use the Pi USB-A host ports. The Zynthian software exposes only USB MIDI on that port ("No audio transfer on USB-B", forum, April 2026) [doc: discourse 13077], so UAC2 12/12 on it is *untested by Zynthian*, but the hardware path is plain USB 2.0 D+/D-. Risks [inf]: (a) VBUS from the host computer reaches Pi USB-C VBUS only if the flat cable passes it (the diode on the board blocks it, which is what we want); (b) the Pi 5 will not negotiate USB-PD at 5 A from this 5 V rail, so without PSU_MAX_CURRENT the Pi limits USB-A host current (600 mA total, relevant to Rytm bus power; Rytm is self-powered so likely fine) [doc: Pi forum posts in https://discourse.zynthian.org/t/zynthian-4-upgraded-with-raspberry-pi-5/11094?page=2]; (c) USB-C CC pins: the flat cable may not carry CC, in which case the Pi 5 may not know the role (Linux uses dwc2/dwc3 OTG in forced device mode); we handle role in our own Circle driver already.
- Bring-up check list: measure Pi USB-C VBUS with the DC jack on and USB-B unplugged; connect to a computer and see if our gadget enumerates; confirm Rytm on a rear USB-A port enumerates.

Total estimate (one person, Circle already working on Pi 5 with DSI): about 3-5 weeks to a usable V5.1 profile (controls + LEDs + audio + panel + touch), with a board in hand. Without hardware: the schematic gives enough for drivers but codec clocking, WS2812 GPIO and touch controller must be measured. We cannot buy hardware in this research.

## 4. Engines on zynthian.org

Source: https://wiki.zynthian.org/index.php/Engines (engine list) [doc]. Licences below: [doc] where from GitHub API/LICENSE (agent-verified), otherwise [inf]. "Bare-metal port" assumes no Linux, JACK, LV2, threads or filesystem, i.e. lifting DSP classes.

| Engine | Licence | Lift into Circle engine? | Notes |
|---|---|---|---|
| Dexed (DX7 FM) / msfa | GPL-3.0 (Dexed), Apache-2.0 (msfa) [doc: github.com/asb2m10/dexed, github.com/google/music-synthesizer-for-android] | Easy. Engine is in source/msfa, JUCE only in wrapper | 6-op FM, tiny CPU, huge patch library (SysEx). Top pick |
| sfizz (SFZ) | BSD-2-Clause [doc: github.com/sfztools/sfizz] | Medium. Library-first design, but heavy on filesystem (sample streaming), threads, SIMD; we have our own sampler (MULTISAMPLE) so reuse = SFZ parser + region logic only | SFZ is attractive; BSD licence is the most permissive |
| FluidSynth | LGPL-2.1 [doc] | Medium-hard. C, but file I/O, threads, dynamic allocation; SF2 loader reusable; DSP rvoice code lift is feasible | SF2/SF3 playback; General MIDI drums |
| ZynAddSubFX | GPL-2.0 per GitHub (confirm "or later" before mixing with GPL-3) [doc: github.com/zynaddsubfx/zynaddsubfx] | Hard. Large C++ codebase, but engine (DSP/Synth, Effects) separable from FLTK/ NTK GUI and rtosc | PADsynth, additive+subtractive; CPU heavy; best "character" synth. GPL-2-only would be incompatible with GPL-3 |
| setBfree (tonewheel organ) | GPL-2.0 [doc: github.com/pantherb/setBfree] | Medium. C, DSP is mostly self-contained (tonegen, reverb, whirl) | Cheap CPU, great sound. Check "or later" |
| Aeolus (pipe organ) | GPL (version unverified) [doc: kokkinizita.linuxaudio.org/linuxaudio/aeolus/] | Medium-hard. Needs clthreads/zita libs, stop definitions on filesystem | Heavy CPU with many stops; niche |
| Surge XT | GPL-3.0 [doc: github.com/surge-synthesizer/surge] | Hard. C++17, big, uses many libs; SIMD/SSE (sse2neon) | Hybrid flagship; likely too heavy on Pi 4 for multiple instances |
| Vital / Vitalium | GPL-3.0 (Vital) [doc: github.com/mtytel/vital] | Hard. Heavy, wavetable engine SIMD | Pi 5 only, one instance |
| Odin2 | GPL-3.0 [doc: github.com/TheWaveWarden/odin2] | Hard. JUCE-coupled | Modular; CPU heavy |
| OB-Xd | Mixed: "OB-Xd 2" README says freeware, no SPDX; older GPL-3 forks exist; DISTRHO port inherits [doc: github.com/reales/OB-Xd] | Medium for the GPL fork, blocked for the freeware one | Licence unclear: avoid unless the GPL-3 fork is used |
| Helm | GPL-3.0, unmaintained since 2022 [doc: github.com/mtytel/helm] | Medium (own JUCE-free mopo core in mopo/) | Poly synth, moderate CPU |
| LinuxSampler | GPL-2.0 plus a non-commercial additional clause [doc: github.com/linuxsampler/linuxsampler; clause from memory [inf]] | Hard; do not use | Not compatible with GPL-3 plus we want redistribution |
| Pianoteq (Modartt) | Proprietary [doc: wiki page "the only proprietary one"] | Not possible | Excluded |
| MDA JX10 / ePiano / Piano | Original MIT/GPL by Paul Kellett; LV2 port GPL-2+ (some packagers GPL-3) [doc: git.drobilla.net/cgit.cgi/mda.lv2.git/about] | Easy. Tiny self-contained C DSP | JX10 and ePiano are very useful, near-zero CPU |
| Calf (monosynth, organ, wavetable, effects) | LGPL-2.1 [doc: github.com/calf-studio-gear/calf] | Medium. C++ DSP classes in one header set | Good effects: reverb, comp, filters |
| Dragonfly Reverb | GPL-3.0 [doc: github.com/michaelwillis/dragonfly-reverb] | Medium. Freeverb3-derived core | Good hall reverb; CPU moderate |
| Freeverb | Public domain [doc: Jezar original] | Trivial | Cheap fallback reverb |
| Geonkick | GPL-3.0 [doc: github.com/quamplex/geonkick] | Medium | Kick drum synth; fits a Rytm sidecar |
| AMSynth, Nekobi, reMID, MiniOPL3/ADLplug/OPNplug, YC20 (Foo), Sorcer, Padthv1/synthv1/samplv1/drumkv1, TAL-NoiseMaker, Triceratops, Wolpertinger | Mostly GPL-2/3 [inf] | Varies; small ones are easy | FM chip emulators (OPL/OPN) are easy and cheap |
| Effects: ZamComp, Wolf Shaper, LSP, AIDA-X (NAM/RTNeural), DrumGizmo, TAL reverb/filters | GPL-2/3 mostly [inf] | LSP and AIDA-X are heavy; Wolf/Zam easy | |

### Top picks for the Sidecar (sound, CPU fit on Pi 4/5, licence)
1. Dexed/msfa 6-op FM engine: tiny CPU, ~16 voices trivially, GPL-3/Apache, easy lift, strongest sound-per-effort.
2. MDA ePiano + JX10 (+ Piano): near-zero CPU, small C code, GPL-2+ (check version).
3. setBfree tonewheel organ: moderate CPU, characterful, C; GPL-2.0 (check "or later").
4. ZynAddSubFX (ADsynth/PADsynth): the Zynthian signature sound; heavy but fits Pi 5; DSP separable; GPL-2 licence check needed.
5. sfizz regions/parser + our sampler: BSD-2, unlocks SFZ libraries (drums, pianos) with our own streaming layer.
6. Helm core (mopo): GPL-3, poly subtractive with arpeggio features, no JUCE core.
7. Calf + Dragonfly + Freeverb effects: reverb, comp, chorus, phaser, delay (LGPL/GPL-3/PD).
8. Geonkick or OPL/OPN FM chip emulators: fit a drum-machine sidecar.

Skip: Pianoteq (proprietary), LinuxSampler (licence), OB-Xd 2 (freeware), Surge/Vital/Odin2 (CPU and JUCE/SIMD coupling) unless Pi-5-only.

Licence rule of thumb for our GPL-3 Pi binary: GPL-3, GPL-2-or-later, LGPL, BSD, MIT, Apache-2 are OK; GPL-2.0-only is NOT compatible and must be confirmed per file header before lifting code (ZynAddSubFX, setBfree, LinuxSampler flagged).

## Open items to verify on hardware or in the repos
- codec I2C addresses and clock master (trace ADR/MCLK nets in dac.kicad_sch; or i2cdetect on a unit).
- which Pi GPIO carries LEDS (WS2812) and the HPAMP_ENABLE/RST lines (main board schematic).
- exact MCP23017 bit mapping (clone zyncoder/zyncoder/zyncontrol_v5.c).
- DSI panel controller/touch IC.
- Whether USB-C VBUS is wired through the flat cable and CC handling (PD).
- clone zynthian-hw and zyncoder locally for a proper read; this research read only parts via GitHub pages (WebFetch) and the raw KiCad .kicad_sch files in a temporary folder.
