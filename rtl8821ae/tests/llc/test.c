#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t UCHAR; typedef uint16_t USHORT; typedef uint32_t ULONG;
#define FAR
static void fmemcpy(UCHAR *d, UCHAR *s, USHORT n) { memcpy(d, s, n); }
#include "block.inc"
static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)
int main(void) {
  UCHAR eth[1600], llc[1600]; USHORT n, is;
  /* RX: IPv4 in RFC 1042 -> Ethernet II (type 0800 + payload) */
  { UCHAR p[] = { 0xAA,0xAA,0x03,0,0,0, 0x08,0x00, 1,2,3,4,5 };
    n = llc_to_eth(p, sizeof p, eth, &is);
    CHECK(n == 7 && !is && eth[12] == 8 && eth[13] == 0 && eth[14] == 1 && eth[18] == 5, "IPv4 SNAP"); }
  /* RX: ARP likewise; AARP and IPX in RFC 1042 stay as 802.3 with the SNAP header; bridge-tunnel is unwrapped */
  { UCHAR p[] = { 0xAA,0xAA,0x03,0,0,0, 0x80,0xF3, 9,9 };
    n = llc_to_eth(p, sizeof p, eth, &is);
    CHECK(is && n == sizeof p + 2 && eth[12] == 0 && eth[13] == sizeof p && eth[14] == 0xAA, "AARP in RFC 1042 stays 802.3"); }
  { UCHAR p[] = { 0xAA,0xAA,0x03,0,0,0xF8, 0x81,0x37, 7,7,7 };
    n = llc_to_eth(p, sizeof p, eth, &is);
    CHECK(!is && n == 5 && eth[12] == 0x81 && eth[13] == 0x37 && eth[14] == 7, "IPX bridge tunnel unwrapped"); }
  /* RX: NetBEUI (LLC F0 F0 03 ...) -> 802.3 frame with the length */
  { UCHAR p[] = { 0xF0,0xF0,0x03, 0x00,0x01, 0xAA,0xBB }; 
    n = llc_to_eth(p, sizeof p, eth, &is);
    CHECK(is && n == sizeof p + 2 && eth[12] == 0 && eth[13] == 7 && eth[14] == 0xF0 && eth[16] == 0x03 && eth[20] == 0xBB, "NetBEUI LLC -> 802.3"); }
  /* RX: STP (42 42 03), too short, too long */
  { UCHAR p[] = { 0x42,0x42,0x03, 0,0,0,0 }; n = llc_to_eth(p, sizeof p, eth, &is); CHECK(is && n == sizeof p + 2 && eth[13] == 7, "STP -> 802.3"); }
  { UCHAR p[2] = { 0xF0, 0xF0 }; CHECK(llc_to_eth(p, 2, eth, &is) == 0, "too short refused"); }
  { memset(llc, 0xF0, sizeof llc); CHECK(llc_to_eth(llc, 1501, eth, &is) == 0, "802.3 longer than 1500 refused"); CHECK(llc_to_eth(llc, 1500, eth, &is) == 1502, "1500 accepted"); }
  /* TX: Ethernet II IPv4 -> SNAP, payload follows the type */
  { UCHAR e[] = { 1,2,3,4,5,6, 7,8,9,10,11,12, 0x08,0x00, 0x45,0,0,20 }; USHORT bl, i2; UCHAR *b = eth_to_llc(e, sizeof e, &bl, &i2);
    CHECK(!i2 && b == e + 6 && bl == sizeof e - 6 && b[0] == 0xAA && b[2] == 3 && b[5] == 0 && b[6] == 8 && b[7] == 0 && b[8] == 0x45, "IPv4 -> SNAP"); }
  { UCHAR e[] = { 1,2,3,4,5,6, 7,8,9,10,11,12, 0x81,0x37, 1,2 }; USHORT bl, i2; UCHAR *b = eth_to_llc(e, sizeof e, &bl, &i2); CHECK(b[5] == 0xF8, "IPX -> bridge tunnel"); }
  { UCHAR e[] = { 1,2,3,4,5,6, 7,8,9,10,11,12, 0x80,0xF3, 1,2 }; USHORT bl, i2; UCHAR *b = eth_to_llc(e, sizeof e, &bl, &i2); CHECK(b[5] == 0xF8, "AARP -> bridge tunnel"); }
  /* TX: 802.3 (length field) -> the LLC header as it is, source address untouched */
  { UCHAR e[] = { 1,2,3,4,5,6, 7,8,9,10,11,12, 0x00,0x07, 0xF0,0xF0,0x03, 0,1,0xAA,0xBB }; USHORT bl, i2; UCHAR *b = eth_to_llc(e, sizeof e, &bl, &i2);
    CHECK(i2 && b == e + 14 && bl == 7 && b[0] == 0xF0 && e[6] == 7, "802.3 -> LLC unchanged"); }
  /* round trip: Ethernet II and 802.3 frames come back identical */
  { UCHAR e[60], b2[80]; USHORT bl, i2, i3; int k; UCHAR *b; for (k = 0; k < 60; k++) e[k] = (UCHAR)k; e[12] = 0x08; e[13] = 0x06;
    UCHAR copy[60]; memcpy(copy, e, 60); b = eth_to_llc(e, 60, &bl, &i2);
    memset(b2, 0, sizeof b2); memcpy(b2 + 12, e, 0); n = llc_to_eth(b, bl, b2, &i3);
    memcpy(b2, copy, 12); CHECK(n + 12 == 60 && !i3 && memcmp(b2, copy, 60) == 0, "ARP round trip"); }
  printf("%s: %d failures\n", fails ? "FAILED" : "PASSED", fails);
  return fails != 0;
}
