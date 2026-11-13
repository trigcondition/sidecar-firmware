// web-next/src/lib/inbox.ts - the drop path: samples dropped (or chosen) on the Files card land in /Samples/Inbox and each
// WAV is taken into the SAMPLES pool (pools.cpp "pool=smp:take:PATH": the shown bank, or the next one with room).
export const INBOX = "/Samples/Inbox";
/** The pool takes WAV only (the card's other audio is for the +WT / library paths). */
export const isPoolAudio = (name: string) => /\.wav$/i.test(name);
export const takeCmd = (path: string) => `pool=smp:take:${path}`;
