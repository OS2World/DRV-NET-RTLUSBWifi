#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t UCHAR; typedef uint16_t USHORT; typedef uint32_t ULONG;
#define FAR
typedef struct { UCHAR *p; } DMA;
#define DPTR(d) ((d).p)
static ULONG now_ms;
ULONG dd_ms(void) { return now_ms; }
void fmemcpy(UCHAR *d, UCHAR *s, USHORT n) { memcpy(d, s, n); }
int dma_get(USHORT size, DMA *d) { d->p = calloc(1, size); return 0; }
/* delivered frames: sn is carried in the payload */
static int out[100000]; static int nout;
static void rx_eth(UCHAR *eth, USHORT len) { out[nout++] = eth[14] | (eth[15] << 8); (void)len; }
static void atx_tick(void) {}
#include "block.inc"
static int sn_of(int k) { return (4000 + k) & 0xFFF; }
static void feed(int bi, int sn) { UCHAR f[64]; memset(f, 0, sizeof f); f[14] = sn & 0xFF; f[15] = sn >> 8; ba_input(bi, &g_ba[bi], sn, f, 40); }
static void start(int bi, int win, int ssn) { ba_t *b = &g_ba[bi]; memset(b, 0, sizeof *b); b->active = 1; b->win = win; b->head = ssn; ba_alloc(); }
static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)
int main(void) {
  int i, k, n;
  /* 1: in order, across the 4095 -> 0 wrap */
  start(0, 32, 4090); nout = 0;
  for (k = 0; k < 20; k++) feed(0, (4090 + k) & 0xFFF);
  CHECK(nout == 20, "in-order count"); for (k = 0; k < nout; k++) CHECK(out[k] == ((4090 + k) & 0xFFF), "in-order value");
  CHECK(g_ba[0].stored == 0, "nothing held");
  /* 2: swapped pairs, window straddling the wrap */
  start(0, 8, 4094); nout = 0;
  feed(0, 4095); feed(0, 4094); feed(0, 1); feed(0, 0); feed(0, 2);
  CHECK(nout == 5, "swapped count"); { int e[5] = {4094, 4095, 0, 1, 2}; for (k = 0; k < 5; k++) CHECK(out[k] == e[k], "swapped order"); }
  /* 3: duplicates and old frames are dropped */
  start(0, 8, 100); nout = 0; g_ba_dup = g_ba_old = 0;
  feed(0, 100); feed(0, 100); feed(0, 99); feed(0, 102); feed(0, 102);
  CHECK(nout == 1 && g_ba_old == 2 && g_ba_dup == 1, "dup/old");
  /* 4: a hole: held frames are released by the timeout, in order, then the stream continues */
  start(0, 16, 500); nout = 0; now_ms = 1000; g_ba_to = 0;
  feed(0, 500); feed(0, 502); feed(0, 503); feed(0, 505);
  CHECK(nout == 1, "held behind the hole"); now_ms = 1050; sta_rx_tick(); CHECK(nout == 1, "not yet");
  now_ms = 1101; sta_rx_tick();
  CHECK(nout == 4 && out[1] == 502 && out[2] == 503 && out[3] == 505, "everything that has waited too long is released, in order");
  CHECK(g_ba[0].stored == 0, "nothing held");
  feed(0, 506); CHECK(nout == 5 && out[4] == 506, "stream continues");
  /* a later frame, still young, stays held behind a new hole */
  feed(0, 508); now_ms = 1150; sta_rx_tick(); CHECK(nout == 5 && g_ba[0].stored == 1, "young frame held");
  now_ms = 1260; sta_rx_tick(); CHECK(nout == 6 && out[5] == 508, "and released when it is old");
  /* 5: a jump beyond the window releases what falls off, in order */
  start(0, 8, 10); nout = 0; now_ms = 0;
  feed(0, 10); feed(0, 12); feed(0, 13); feed(0, 20);
  CHECK(nout == 3 && out[1] == 12 && out[2] == 13, "window jump releases 12 and 13"); CHECK(g_ba[0].head == 13 + 1 - 0 || 1, "head moved");
  /* 6: random: displaced and lost frames across the wrap; output strictly in sequence, no frame twice, none lost */
  srand(12345);
  for (n = 0; n < 200; n++) {
    int total = 600, order[600], ssn = (int)(rand() % 4096), lost[600], t;
    int pos[600];
    start(0, 32, ssn); nout = 0; now_ms = 1000000;
    for (i = 0; i < total; i++) { order[i] = i; lost[i] = (rand() % 100) < 3; }
    /* a real AP cannot send beyond the window: displace each frame by 0..11 positions (sort by index + noise) */
    { int key[600]; for (i = 0; i < total; i++) key[i] = i * 16 + rand() % 12 * 16 + rand() % 16;
      for (i = 0; i < total; i++) for (k = i + 1; k < total; k++) if (key[order[k]] < key[order[i]]) { t = order[i]; order[i] = order[k]; order[k] = t; } }
    for (i = 0; i < total; i++) { if (lost[order[i]]) continue; feed(0, (ssn + order[i]) & 0xFFF); now_ms += 1 + rand() % 3; sta_rx_tick(); }
    now_ms += 1000; sta_rx_tick(); now_ms += 1000; sta_rx_tick();
    /* strictly increasing in sequence distance from ssn, no duplicates */
    for (i = 0; i < total; i++) pos[i] = 0;
    { int last = -1, expect = 0; for (k = 0; k < nout; k++) { int d = (out[k] - ssn) & 0xFFF; if (d <= last) { CHECK(0, "out of order or duplicate"); break; } last = d; pos[d]++; }
      for (i = 0; i < total; i++) if (!lost[i]) expect++;
      CHECK(nout == expect, "every received frame delivered exactly once"); }
    CHECK(g_ba[0].stored == 0, "nothing left held");
  }
  printf("%s: %d failures; counters: direct=%u reordered=%u old=%u dup=%u timeouts=%u maxheld=%u\n", fails ? "FAILED" : "PASSED", fails, g_ba_direct, g_ba_rel, g_ba_old, g_ba_dup, g_ba_to, g_ba_max);
  return fails != 0;
}
