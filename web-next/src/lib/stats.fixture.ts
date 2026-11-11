// web-next/src/lib/stats.fixture.ts - real stats lines (the engine, its idle line, Overbridge, USB-C) for the tests.
export const ENGINE = "12.345 engine: engine: fifo min 0 max 118 target 96 under 3 (restart 0, streams 1) | blocks 3000 max 210 us | cpu c1 41.5 c2 33.0 c3 19.2 % | pass max 260 us | midi 0 B (lost 0) sysex 0 notes 0 | to-host sent 0 B 0 msgs fail 0 dropped 0 | peak 512/1000 | 58 C";
export const IDLE = "15.000 engine: idle: asleep 88 52 61 79 %, wakes 900 1000 1000 1000 a second (cores 0-3), wfe on, ARM clock 2400 MHz, measured 2400, SoC 52 C";
export const OB = "12.400 rytmob: 1000ms IN 1000/s gap 2 lost 0 late 0";
export const SC = "12.401 sidecar: sidecar alt 1/1 · late 0/0/1";
