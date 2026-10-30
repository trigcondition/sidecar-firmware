// core/tests/test_wav_chunks.cpp - the WAV chunk walk (core/wavetable/wav_read.h, which CHOP's, GRANULAR's and WAVETABLE's
// loads share through the pool RAM's pcache): the chunks found wherever they are, whatever precedes fmt / data. The layout
// of a Rytm export that CHOP plays (item 16 of the hardware list): JUNK (52) before fmt, fmt (16), smpl (60, a loop),
// data; plus a plain minimal WAV, an odd-sized chunk (its pad byte), and files that are cut or lie about a size.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "wavetable/wav_read.h"

using namespace core;
using Bytes = std::vector<uint8_t>;

static void u32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
static void u16(Bytes& b, uint16_t v) { b.push_back((uint8_t)v), b.push_back((uint8_t)(v >> 8)); }
static void tag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }
// a chunk: tag, size, bytes, the pad byte when the size is odd
static void chunk(Bytes& b, const char* t, const Bytes& body, uint32_t size_field = ~0u) {
  tag(b, t), u32(b, size_field == ~0u ? (uint32_t)body.size() : size_field);
  b.insert(b.end(), body.begin(), body.end());
  if (body.size() & 1) b.push_back(0);
}
static Bytes fmt16(int ch, int rate, int bits) {
  Bytes f;
  u16(f, 1), u16(f, (uint16_t)ch), u32(f, (uint32_t)rate), u32(f, (uint32_t)(rate * ch * bits / 8)), u16(f, (uint16_t)(ch * bits / 8)),
      u16(f, (uint16_t)bits);
  return f;
}
static Bytes pcm(int frames) {   // 16-bit mono, a ramp
  Bytes d;
  for (int i = 0; i < frames; ++i) u16(d, (uint16_t)(i * 7));
  return d;
}
static Bytes riff(const Bytes& chunks) {
  Bytes b;
  tag(b, "RIFF"), u32(b, (uint32_t)(4 + chunks.size())), tag(b, "WAVE");
  b.insert(b.end(), chunks.begin(), chunks.end());
  return b;
}
static bool parse(const Bytes& b, wavread::Info* a, uint32_t n = 0) {   // (n: how much of it the header read sees)
  return wavread::parse(b.data(), n ? n : (uint32_t)b.size(), a, (uint32_t)b.size());
}

int main() {
  wavread::Info a;

  // 1. the Rytm export: JUNK (52) before fmt, smpl (60, one loop) between fmt and data; 187114 samples, 48 kHz mono 16-bit
  {
    Bytes c;
    chunk(c, "JUNK", Bytes(52, 0));
    chunk(c, "fmt ", fmt16(1, 48000, 16));
    Bytes smpl(60, 0);   // the loop: 36 bytes of header, numLoops = 1 at 28, the loop (cue, type, start, end, fraction, count)
    smpl[28] = 1;
    smpl[36 + 8] = 0, smpl[36 + 12] = 0x40, smpl[36 + 13] = 0x1f;   // start 0, end 8000
    chunk(c, "smpl", smpl);
    chunk(c, "data", pcm(187114));
    Bytes f = riff(c);
    CHECK(f.size() == 12 + 8 + 52 + 8 + 16 + 8 + 60 + 8 + 374228);
    CHECK(parse(f, &a));
    CHECK(a.format == 1 && a.ch == 1 && a.bits == 16 && a.rate == 48000);
    CHECK(a.data_off == 172 && a.data_len == 374228 && a.frames == 187114);
    CHECK(a.frames / (double)a.rate > 3.89 && a.frames / (double)a.rate < 3.91);
    // the pool RAM's header read is a card chunk (32 KB): the same answer from a prefix that holds the header only
    CHECK(parse(f, &a, 4096) && a.frames == 187114 && a.data_off == 172);
    CHECK(wavread::frame_bytes(a) == 2);
    printf("test_wav_chunks: Rytm export layout (JUNK, fmt, smpl, data) PASS: %u Hz, %u ch, %u bit, %u frames, %.2f s\n", a.rate, a.ch,
           a.bits, a.frames, a.frames / (double)a.rate);
  }

  // 2. the plain minimal WAV (fmt, data)
  {
    Bytes c;
    chunk(c, "fmt ", fmt16(2, 44100, 16));
    chunk(c, "data", pcm(1000));
    Bytes f = riff(c);
    CHECK(f.size() == 44 + 2000);
    CHECK(parse(f, &a) && a.ch == 2 && a.rate == 44100 && a.bits == 16 && a.data_off == 44 && a.frames == 500);
    printf("test_wav_chunks: minimal WAV PASS\n");
  }

  // 3. an odd-sized chunk before fmt and another before data: the pad byte is skipped (RIFF), the chunks after it found
  {
    Bytes c;
    chunk(c, "LIST", Bytes(5, 'x'));   // 5 bytes + 1 pad
    chunk(c, "fmt ", fmt16(1, 48000, 16));
    chunk(c, "id3 ", Bytes(33, 'y'));
    chunk(c, "data", pcm(100));
    Bytes f = riff(c);
    CHECK(parse(f, &a) && a.ch == 1 && a.rate == 48000 && a.frames == 100 && a.data_off == 12 + 8 + 6 + 8 + 16 + 8 + 34 + 8);
    // an odd-sized data chunk (one stray byte after the last frame): the whole frames only
    Bytes c2;
    chunk(c2, "fmt ", fmt16(1, 48000, 16));
    Bytes d = pcm(10);
    d.push_back(0x55);
    chunk(c2, "data", d);
    CHECK(parse(riff(c2), &a) && a.frames == 10);
    // the pad byte missing (a writer that forgot it) is not guessed at: the walk lands inside the next chunk and refuses
    Bytes c3;
    tag(c3, "JUNK"), u32(c3, 5), c3.insert(c3.end(), 5, 0);   // no pad
    chunk(c3, "fmt ", fmt16(1, 48000, 16));
    chunk(c3, "data", pcm(10));
    CHECK(!parse(riff(c3), &a));
    printf("test_wav_chunks: odd-sized chunks and the pad byte PASS\n");
  }

  // 4. truncated and malformed files are refused (or cut to what is there) without reading past the end: each case in
  //    a buffer of exactly its length, so a read past it is the sanitizer's, and a wrong answer is the check's
  {
    Bytes c;
    chunk(c, "JUNK", Bytes(52, 0));
    chunk(c, "fmt ", fmt16(1, 48000, 16));
    chunk(c, "data", pcm(1000));
    Bytes f = riff(c);
    for (size_t cut = 0; cut < 12 + 8 + 52 + 8 + 16 + 8; ++cut) {   // anywhere before the data chunk's header ends
      Bytes t(f.begin(), f.begin() + cut);
      CHECK(!parse(t, &a));
    }
    // cut inside the data: the frames that are there
    Bytes t(f.begin(), f.begin() + 12 + 8 + 52 + 8 + 16 + 8 + 400);
    CHECK(parse(t, &a) && a.frames == 200 && a.data_len == 400);
    // a data size that lies (longer than the file)
    Bytes lie = f;
    lie[12 + 8 + 52 + 8 + 16 + 4] = 0xff, lie[12 + 8 + 52 + 8 + 16 + 5] = 0xff, lie[12 + 8 + 52 + 8 + 16 + 6] = 0xff, lie[12 + 8 + 52 + 8 + 16 + 7] = 0x7f;
    CHECK(parse(lie, &a) && a.frames == 1000);
    // a chunk size that runs past the end before the data: refused
    Bytes big;
    chunk(big, "JUNK", Bytes(4, 0), 0xfffffff0u);
    chunk(big, "fmt ", fmt16(1, 48000, 16));
    chunk(big, "data", pcm(10));
    CHECK(!parse(riff(big), &a));
    Bytes big2;   // (a size that wraps the 32-bit position)
    chunk(big2, "JUNK", Bytes(4, 0), 0xfffffff1u);
    chunk(big2, "fmt ", fmt16(1, 48000, 16));
    chunk(big2, "data", pcm(10));
    CHECK(!parse(riff(big2), &a));
    // a fmt chunk too short to be one; fmt missing; data missing; not RIFF / WAVE; zero channels or rate; 12 bytes only
    Bytes shortfmt;
    chunk(shortfmt, "fmt ", Bytes(8, 0));
    chunk(shortfmt, "data", pcm(10));
    CHECK(!parse(riff(shortfmt), &a));
    Bytes nofmt;
    chunk(nofmt, "data", pcm(10));
    CHECK(!parse(riff(nofmt), &a));
    Bytes nodata;
    chunk(nodata, "fmt ", fmt16(1, 48000, 16));
    chunk(nodata, "smpl", Bytes(60, 0));
    CHECK(!parse(riff(nodata), &a));
    Bytes good;
    chunk(good, "fmt ", fmt16(1, 48000, 16));
    chunk(good, "data", pcm(10));
    Bytes notwave = riff(good);
    notwave[8] = 'X';
    CHECK(!parse(notwave, &a));
    Bytes notriff = riff(good);
    notriff[0] = 'X';
    CHECK(!parse(notriff, &a));
    Bytes noch;
    chunk(noch, "fmt ", fmt16(0, 48000, 16));
    chunk(noch, "data", pcm(10));
    CHECK(!parse(riff(noch), &a));
    Bytes norate;
    chunk(norate, "fmt ", fmt16(1, 0, 16));
    chunk(norate, "data", pcm(10));
    CHECK(!parse(riff(norate), &a));
    Bytes odd_bits;
    chunk(odd_bits, "fmt ", fmt16(1, 48000, 12));
    chunk(odd_bits, "data", pcm(10));
    CHECK(!parse(riff(odd_bits), &a));
    Bytes empty = riff(Bytes());   // (one vector: begin and end of the same one - two temporaries were UB)
    Bytes head(empty.begin(), empty.begin() + 12);
    CHECK(!parse(head, &a));
    CHECK(!parse(Bytes(), &a));
    printf("test_wav_chunks: truncated and malformed files PASS\n");
  }

  printf("test_wav_chunks: PASS\n");
  return 0;
}
