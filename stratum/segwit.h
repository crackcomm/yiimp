#include "algos/sha256.h"
#include "json.h"
#include "util.h"
#include <cassert>

static uint32_t bech32_polymod_step(uint32_t pre) {
  uint8_t b = pre >> 25;
  return ((pre & 0x1FFFFFF) << 5) ^ (-((b >> 0) & 1) & 0x3b6a57b2UL) ^
         (-((b >> 1) & 1) & 0x26508e6dUL) ^ (-((b >> 2) & 1) & 0x1ea119faUL) ^
         (-((b >> 3) & 1) & 0x3d4233ddUL) ^ (-((b >> 4) & 1) & 0x2a1462b3UL);
}

static const int8_t bech32_charset_rev[128] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 15, -1, 10, 17, 21, 20, 26, 30, 7,
    5,  -1, -1, -1, -1, -1, -1, -1, 29, -1, 24, 13, 25, 9,  8,  23, -1, 18, 22,
    31, 27, 19, -1, 1,  0,  3,  16, 11, 28, 12, 14, 6,  4,  2,  -1, -1, -1, -1,
    -1, -1, 29, -1, 24, 13, 25, 9,  8,  23, -1, 18, 22, 31, 27, 19, -1, 1,  0,
    3,  16, 11, 28, 12, 14, 6,  4,  2,  -1, -1, -1, -1, -1};

static bool bech32_decode(char *hrp, uint8_t *data, size_t *data_len,
                          const char *input) {
  uint32_t chk = 1;
  size_t i;
  size_t input_len = strlen(input);
  size_t hrp_len;
  int have_lower = 0, have_upper = 0;
  if (input_len < 8 || input_len > 90) {
    return false;
  }
  *data_len = 0;
  while (*data_len < input_len && input[(input_len - 1) - *data_len] != '1') {
    ++(*data_len);
  }
  hrp_len = input_len - (1 + *data_len);
  if (1 + *data_len >= input_len || *data_len < 6) {
    return false;
  }
  *(data_len) -= 6;
  for (i = 0; i < hrp_len; ++i) {
    int ch = input[i];
    if (ch < 33 || ch > 126) {
      return false;
    }
    if (ch >= 'a' && ch <= 'z') {
      have_lower = 1;
    } else if (ch >= 'A' && ch <= 'Z') {
      have_upper = 1;
      ch = (ch - 'A') + 'a';
    }
    hrp[i] = ch;
    chk = bech32_polymod_step(chk) ^ (ch >> 5);
  }
  hrp[i] = 0;
  chk = bech32_polymod_step(chk);
  for (i = 0; i < hrp_len; ++i) {
    chk = bech32_polymod_step(chk) ^ (input[i] & 0x1f);
  }
  ++i;
  while (i < input_len) {
    int v = (input[i] & 0x80) ? -1 : bech32_charset_rev[(int)input[i]];
    if (input[i] >= 'a' && input[i] <= 'z')
      have_lower = 1;
    if (input[i] >= 'A' && input[i] <= 'Z')
      have_upper = 1;
    if (v == -1) {
      return false;
    }
    chk = bech32_polymod_step(chk) ^ v;
    if (i + 6 < input_len) {
      data[i - (1 + hrp_len)] = v;
    }
    ++i;
  }
  if (have_lower && have_upper) {
    return false;
  }
  return chk == 1;
}

static bool convert_bits(uint8_t *out, size_t *outlen, int outbits,
                         const uint8_t *in, size_t inlen, int inbits, int pad) {
  uint32_t val = 0;
  int bits = 0;
  uint32_t maxv = (((uint32_t)1) << outbits) - 1;
  while (inlen--) {
    val = (val << inbits) | *(in++);
    bits += inbits;
    while (bits >= outbits) {
      bits -= outbits;
      out[(*outlen)++] = (val >> bits) & maxv;
    }
  }
  if (pad) {
    if (bits) {
      out[(*outlen)++] = (val << (outbits - bits)) & maxv;
    }
  } else if (((val << (outbits - bits)) & maxv) || bits >= inbits) {
    return false;
  }
  return true;
}

static bool segwit_addr_decode(int *witver, uint8_t *witdata,
                               size_t *witdata_len, const char *addr) {
  uint8_t data[84];
  char hrp_actual[84];
  size_t data_len;
  if (!bech32_decode(hrp_actual, data, &data_len, addr))
    return false;
  if (data_len == 0 || data_len > 65)
    return false;
  if (data[0] > 16)
    return false;
  *witdata_len = 0;
  if (!convert_bits(witdata, witdata_len, 8, data + 1, data_len - 1, 5, 0))
    return false;
  if (*witdata_len < 2 || *witdata_len > 40)
    return false;
  if (data[0] == 0 && *witdata_len != 20 && *witdata_len != 32)
    return false;
  *witver = data[0];
  return true;
}

static size_t bech32_to_script(uint8_t *out, size_t outsz, const char *addr) {
  uint8_t witprog[40];
  size_t witprog_len;
  int witver;

  if (!segwit_addr_decode(&witver, witprog, &witprog_len, addr))
    return 0;
  if (outsz < witprog_len + 2)
    return 0;
  out[0] = witver ? (0x50 + witver) : 0;
  out[1] = witprog_len;
  memcpy(out + 2, witprog, witprog_len);
  return witprog_len + 2;
}

static int b58check(unsigned char *bin, size_t binsz, const char *b58) {
  unsigned char buf[32];
  int i;

  sha256d(buf, bin, binsz - 4);
  if (memcmp(&bin[binsz - 4], buf, 4))
    return -1;

  /* Check number of zeros is correct AFTER verifying checksum
   * (to avoid possibility of accessing the string beyond the end) */
  for (i = 0; bin[i] == '\0' && b58[i] == '1'; ++i)
    ;
  if (bin[i] == '\0' || b58[i] == '1')
    return -3;

  return bin[0];
}

size_t address_to_script(unsigned char *out, size_t outsz, const char *addr) {
  unsigned char addrbin[25];
  int addrver;
  size_t rv;

  if (!base58_decode(addr, (char *)addrbin))
    return bech32_to_script(out, outsz, addr);
  addrver = b58check(addrbin, sizeof(addrbin), addr);
  if (addrver < 0)
    return 0;
  switch (addrver) {
  case 5:   /* Bitcoin script hash */
  case 196: /* Testnet script hash */
    if (outsz < (rv = 23))
      return rv;
    out[0] = 0xa9; /* OP_HASH160 */
    out[1] = 0x14; /* push 20 bytes */
    memcpy(&out[2], &addrbin[1], 20);
    out[22] = 0x87; /* OP_EQUAL */
    return rv;
  default:
    if (outsz < (rv = 25))
      return rv;
    out[0] = 0x76; /* OP_DUP */
    out[1] = 0xa9; /* OP_HASH160 */
    out[2] = 0x14; /* push 20 bytes */
    memcpy(&out[3], &addrbin[1], 20);
    out[23] = 0x88; /* OP_EQUALVERIFY */
    out[24] = 0xac; /* OP_CHECKSIG */
    return rv;
  }
}

void job_pack_segwit_tx(char *output, int64_t amount, const char *addr) {
  static unsigned char temp[34] = {0}; // 4 + 4 + 1 + 25
  static unsigned char pk_script[25] = {0};
  int pk_script_size = address_to_script(pk_script, sizeof(pk_script), addr);
  le32enc((uint32_t *)temp, (uint32_t)amount); // tx out amount
  char hexpk[8] = {0};
  hexlify(hexpk, temp, 4);
  le32enc((uint32_t *)(temp + 4), amount >> 32);
  temp[8] = pk_script_size; // tx out script len
  memcpy(temp + 9, pk_script, pk_script_size);
  char temp_hex[68] = {0};
  hexlify(temp_hex, temp, pk_script_size + 9);
  strcat(output, temp_hex);
}
