/* hle_crypt.c - ADVAPI32 CryptoAPI subset (RSA public-key signature verification with MD5/SHA-1), for DLC.
   notes/DLC.md.  ENV-GATED: BSI_DLC=1 enables it; unset/0 = every function below fails exactly like the
   generated imps.c stub (returns FALSE), i.e. today's behaviour.

   Why: the game accepts a DLC folder (XGame\DLC\<name>) only after FUN_00bb2ff0 verifies that folder's
   signature.bin with CryptoAPI: CryptAcquireContextW(PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) -> CryptImportKey(the
   1024-bit RSA PUBLICKEYBLOB embedded at 0x1217d68) -> CryptCreateHash(CALG_MD5) -> CryptHashData(lower-case DLC
   name, each *.tfc file name + size, <name>__XEngine.ini, <name>__XGame.ini, [DLCD__XEngine.ini]) ->
   CryptVerifySignatureW(signature.bin).  With the stubs CryptAcquireContextW returns FALSE, so every DLC is
   rejected (FUN_00bb3ac0 returns 0 and nothing is installed).  This file implements those calls faithfully: a
   DLC folder copied unmodified from the user's own Steam install verifies; a modified one is rejected, as on
   Windows.  There is deliberately no "always succeed" mode.

   Handles are only valid if created here; CryptAcquireContextA (used by unrelated code at 0xdc76d0/0xdef540)
   is NOT implemented and keeps failing, so those paths are unchanged.
   Log lines (stderr = bsi.log): "[dlc] signature <name>: OK" / "[dlc] signature <name>: FAILED (...)". */
#include "hle.h"
#include <pthread.h>
#include <stdint.h>

#define NTE_BAD_UID        0x80090001u
#define NTE_BAD_HASH       0x80090002u
#define NTE_BAD_KEY        0x80090003u
#define NTE_BAD_SIGNATURE  0x80090006u
#define NTE_BAD_ALGID      0x80090008u
#define NTE_BAD_FLAGS      0x80090009u
#define NTE_BAD_TYPE       0x8009000Au
#define NTE_BAD_HASH_STATE 0x8009000Cu
#define NTE_BAD_KEYSET     0x80090016u
#define ERR_INVALID_PARAM  87u
#define ERR_MORE_DATA      234u
#define CALG_MD5  0x8003u
#define CALG_SHA1 0x8004u
#define CRYPT_NOHASHOID 1u

int dlc_crypt_enabled(void){
  static int v = -1;
  if(v < 0){ const char *e = getenv("BSI_DLC"); v = (e && *e && strcmp(e, "0")) ? 1 : 0;
    if(v) fprintf(stderr, "[dlc] BSI_DLC=1: CryptoAPI subset (RSA verify, MD5/SHA-1) enabled for the DLC signature check\n"); }
  return v;
}

/* ---------------- MD5 (RFC 1321) ---------------- */
typedef struct { uint32_t h[4]; uint64_t n; uint8_t b[64]; } Md5;
static const uint32_t MD5K[64] = {
  0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
  0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
  0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
  0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
  0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
  0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
static const uint8_t MD5R[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
  4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
static uint32_t rol32(uint32_t x, int r){ return (x << r) | (x >> (32 - r)); }
static void md5_block(Md5 *s, const uint8_t *p){
  uint32_t w[16]; for(int i = 0; i < 16; i++) w[i] = p[4*i] | p[4*i+1] << 8 | p[4*i+2] << 16 | (uint32_t)p[4*i+3] << 24;
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
  for(int i = 0; i < 64; i++){ uint32_t f; int g;
    if(i < 16){ f = (b & c) | (~b & d); g = i; } else if(i < 32){ f = (d & b) | (~d & c); g = (5*i + 1) & 15; }
    else if(i < 48){ f = b ^ c ^ d; g = (3*i + 5) & 15; } else { f = c ^ (b | ~d); g = (7*i) & 15; }
    uint32_t t = d; d = c; c = b; b = b + rol32(a + f + MD5K[i] + w[g], MD5R[i]); a = t; }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
}
static void md5_init(Md5 *s){ s->h[0] = 0x67452301; s->h[1] = 0xefcdab89; s->h[2] = 0x98badcfe; s->h[3] = 0x10325476; s->n = 0; }
static void md5_upd(Md5 *s, const uint8_t *p, size_t n){ while(n){ size_t o = s->n & 63, k = 64 - o; if(k > n) k = n; memcpy(s->b + o, p, k); s->n += k; p += k; n -= k; if(!(s->n & 63)) md5_block(s, s->b); } }
static void md5_fin(Md5 s, uint8_t out[16]){ uint64_t bits = s.n * 8; uint8_t z = 0x80; md5_upd(&s, &z, 1); z = 0; while((s.n & 63) != 56) md5_upd(&s, &z, 1);
  uint8_t L[8]; for(int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (8*i)); md5_upd(&s, L, 8);
  for(int i = 0; i < 4; i++) for(int j = 0; j < 4; j++) out[4*i+j] = (uint8_t)(s.h[i] >> (8*j)); }

/* ---------------- SHA-1 (FIPS 180-4) ---------------- */
typedef struct { uint32_t h[5]; uint64_t n; uint8_t b[64]; } Sha1;
static void sha1_block(Sha1 *s, const uint8_t *p){
  uint32_t w[80]; for(int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
  for(int i = 16; i < 80; i++) w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
  for(int i = 0; i < 80; i++){ uint32_t f, k;
    if(i < 20){ f = (b & c) | (~b & d); k = 0x5a827999; } else if(i < 40){ f = b ^ c ^ d; k = 0x6ed9eba1; }
    else if(i < 60){ f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; } else { f = b ^ c ^ d; k = 0xca62c1d6; }
    uint32_t t = rol32(a, 5) + f + e + k + w[i]; e = d; d = c; c = rol32(b, 30); b = a; a = t; }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(Sha1 *s){ s->h[0] = 0x67452301; s->h[1] = 0xefcdab89; s->h[2] = 0x98badcfe; s->h[3] = 0x10325476; s->h[4] = 0xc3d2e1f0; s->n = 0; }
static void sha1_upd(Sha1 *s, const uint8_t *p, size_t n){ while(n){ size_t o = s->n & 63, k = 64 - o; if(k > n) k = n; memcpy(s->b + o, p, k); s->n += k; p += k; n -= k; if(!(s->n & 63)) sha1_block(s, s->b); } }
static void sha1_fin(Sha1 s, uint8_t out[20]){ uint64_t bits = s.n * 8; uint8_t z = 0x80; sha1_upd(&s, &z, 1); z = 0; while((s.n & 63) != 56) sha1_upd(&s, &z, 1);
  uint8_t L[8]; for(int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8*i)); sha1_upd(&s, L, 8);
  for(int i = 0; i < 5; i++) for(int j = 0; j < 4; j++) out[4*i+j] = (uint8_t)(s.h[i] >> (24 - 8*j)); }

/* ---------------- RSA public operation: s^e mod n (Montgomery, 32-bit limbs) ---------------- */
#define RSA_MAXW 128   /* up to 4096-bit moduli */
static void mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *N, uint32_t n0, int nw){
  uint32_t t[RSA_MAXW + 2]; memset(t, 0, sizeof(uint32_t) * (nw + 2));
  for(int i = 0; i < nw; i++){
    uint64_t C = 0;
    for(int j = 0; j < nw; j++){ uint64_t x = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + C; t[j] = (uint32_t)x; C = x >> 32; }
    uint64_t x = (uint64_t)t[nw] + C; t[nw] = (uint32_t)x; t[nw+1] = (uint32_t)(x >> 32);
    uint32_t m = t[0] * n0;
    x = (uint64_t)t[0] + (uint64_t)m * N[0]; C = x >> 32;
    for(int j = 1; j < nw; j++){ x = (uint64_t)t[j] + (uint64_t)m * N[j] + C; t[j-1] = (uint32_t)x; C = x >> 32; }
    x = (uint64_t)t[nw] + C; t[nw-1] = (uint32_t)x; C = x >> 32; t[nw] = t[nw+1] + (uint32_t)C;
  }
  /* t < 2N: subtract N once if t >= N */
  int ge = t[nw] != 0;
  if(!ge){ ge = 1; for(int j = nw - 1; j >= 0; j--){ if(t[j] != N[j]){ ge = t[j] > N[j]; break; } } }
  if(ge){ int64_t br = 0; for(int j = 0; j < nw; j++){ int64_t x = (int64_t)t[j] - N[j] + br; t[j] = (uint32_t)x; br = x >> 32; } }
  memcpy(r, t, sizeof(uint32_t) * nw);
}
/* big-endian byte strings, all of length len (= modulus length); returns 0 if s >= n or bad params */
int dlc_rsa_public(uint8_t *out, const uint8_t *sig, const uint8_t *mod, int len, uint32_t e){
  if(len <= 0 || len > 4 * RSA_MAXW || !(mod[len-1] & 1)) return 0;
  int nw = (len + 3) / 4; uint32_t N[RSA_MAXW] = {0}, S[RSA_MAXW] = {0};
  for(int i = 0; i < len; i++){ N[i/4] |= (uint32_t)mod[len-1-i] << (8*(i%4)); S[i/4] |= (uint32_t)sig[len-1-i] << (8*(i%4)); }
  for(int j = nw - 1; j >= 0; j--){ if(S[j] != N[j]){ if(S[j] > N[j]) return 0; break; } if(j == 0) return 0; }
  uint32_t inv = 1; for(int k = 0; k < 5; k++) inv *= 2 - N[0] * inv;   /* N[0]^-1 mod 2^32 */
  uint32_t n0 = (uint32_t)0 - inv;
  /* R2 = 2^(64*nw) mod N by repeated doubling of 1 */
  uint32_t R2[RSA_MAXW] = {0}; R2[0] = 1;
  for(int it = 0; it < 64 * nw; it++){
    uint32_t c = 0; for(int j = 0; j < nw; j++){ uint32_t v = R2[j]; R2[j] = (v << 1) | c; c = v >> 31; }
    int ge = c != 0; if(!ge){ ge = 1; for(int j = nw - 1; j >= 0; j--){ if(R2[j] != N[j]){ ge = R2[j] > N[j]; break; } } }
    if(ge){ int64_t br = 0; for(int j = 0; j < nw; j++){ int64_t x = (int64_t)R2[j] - N[j] + br; R2[j] = (uint32_t)x; br = x >> 32; } }
  }
  uint32_t one[RSA_MAXW] = {0}; one[0] = 1;
  uint32_t X[RSA_MAXW], A[RSA_MAXW];
  mont_mul(X, S, R2, N, n0, nw);        /* S*R */
  mont_mul(A, one, R2, N, n0, nw);      /* 1*R */
  int top = 31; while(top >= 0 && !(e >> top & 1)) top--;
  if(top < 0) return 0;
  for(int b = top; b >= 0; b--){ mont_mul(A, A, A, N, n0, nw); if(e >> b & 1) mont_mul(A, A, X, N, n0, nw); }
  mont_mul(A, A, one, N, n0, nw);
  for(int i = 0; i < len; i++) out[len-1-i] = (uint8_t)(A[i/4] >> (8*(i%4)));
  return 1;
}
static const uint8_t DI_MD5[]  = {0x30,0x20,0x30,0x0c,0x06,0x08,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x02,0x05,0x05,0x00,0x04,0x10};
static const uint8_t DI_SHA1[] = {0x30,0x21,0x30,0x09,0x06,0x05,0x2b,0x0e,0x03,0x02,0x1a,0x05,0x00,0x04,0x14};
/* PKCS#1 v1.5 type-1 check of the decrypted block em (big-endian, len bytes) */
int dlc_pkcs1_check(const uint8_t *em, int len, const uint8_t *di, int dilen, const uint8_t *h, int hlen){
  int tl = dilen + hlen; if(len < tl + 11) return 0;
  if(em[0] != 0 || em[1] != 1) return 0;
  int ps = len - tl - 3; for(int i = 0; i < ps; i++) if(em[2+i] != 0xff) return 0;
  if(em[2+ps] != 0) return 0;
  if(dilen && memcmp(em + 3 + ps, di, dilen)) return 0;
  return memcmp(em + 3 + ps + dilen, h, hlen) == 0;
}

/* ---------------- handle table ---------------- */
enum { K_FREE, K_PROV, K_KEY, K_HASH };
typedef struct { int kind; uint32_t alg; int done; char label[48];
  Md5 md5; Sha1 sha1; uint8_t mod[512]; int modlen; uint32_t e; } CObj;
#define NOBJ 64
#define HBASE 0x7c0de000u
static CObj OBJ[NOBJ]; static pthread_mutex_t omu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t onew(int kind){ pthread_mutex_lock(&omu); for(int i = 0; i < NOBJ; i++) if(OBJ[i].kind == K_FREE){ memset(&OBJ[i], 0, sizeof OBJ[i]); OBJ[i].kind = kind; pthread_mutex_unlock(&omu); return HBASE + 4 * i; } pthread_mutex_unlock(&omu); return 0; }
static CObj *oget(uint32_t h, int kind){ if(h < HBASE || h >= HBASE + 4 * NOBJ || (h - HBASE) & 3) return 0; CObj *o = &OBJ[(h - HBASE) / 4]; return o->kind == kind ? o : 0; }
static int ofree(uint32_t h, int kind){ CObj *o = oget(h, kind); if(!o) return 0; pthread_mutex_lock(&omu); o->kind = K_FREE; pthread_mutex_unlock(&omu); return 1; }
#define FAIL(n, err) do{ g_last_error = (err); RET_STD(n, 0); }while(0)

/* BOOL CryptAcquireContextW(HCRYPTPROV *phProv, LPCWSTR container, LPCWSTR provider, DWORD provType, DWORD flags) */
HLE(advapi32, CryptAcquireContextW){
  if(!dlc_crypt_enabled()) FAIL(5, NTE_BAD_KEYSET);
  if(!ARG(0)) FAIL(5, ERR_INVALID_PARAM);
  if(ARG(3) != 1 && ARG(3) != 24) FAIL(5, NTE_BAD_TYPE);        /* PROV_RSA_FULL / PROV_RSA_AES */
  uint32_t h = onew(K_PROV); if(!h) FAIL(5, NTE_BAD_KEYSET);
  W32(ARG(0), h); g_last_error = 0; RET_STD(5, 1);
}
/* BOOL CryptReleaseContext(HCRYPTPROV hProv, DWORD flags) */
HLE(advapi32, CryptReleaseContext){ if(!dlc_crypt_enabled() || !ofree(ARG(0), K_PROV)) FAIL(2, NTE_BAD_UID); RET_STD(2, 1); }
/* BOOL CryptImportKey(HCRYPTPROV hProv, const BYTE *pbData, DWORD len, HCRYPTKEY hPubKey, DWORD flags, HCRYPTKEY *phKey)
   only PUBLICKEYBLOB (bType 6) with RSAPUBKEY 'RSA1' */
HLE(advapi32, CryptImportKey){
  if(!dlc_crypt_enabled() || !oget(ARG(0), K_PROV)) FAIL(6, NTE_BAD_UID);
  uint32_t p = ARG(1), n = ARG(2); if(!p || !ARG(5) || n < 20) FAIL(6, ERR_INVALID_PARAM);
  uint8_t btype = R8(p), bver = R8(p + 1); uint32_t magic = R32(p + 8), bits = R32(p + 12), e = R32(p + 16);
  if(btype != 6 || bver != 2 || magic != 0x31415352u /* "RSA1" */ || bits < 512 || bits > 4096 || bits % 8 || n < 20 + bits / 8) FAIL(6, NTE_BAD_TYPE);
  uint32_t h = onew(K_KEY); if(!h) FAIL(6, NTE_BAD_KEY);
  CObj *o = oget(h, K_KEY); o->modlen = (int)(bits / 8); o->e = e;
  for(int i = 0; i < o->modlen; i++) o->mod[o->modlen - 1 - i] = R8(p + 20 + i);   /* little-endian -> big-endian */
  W32(ARG(5), h); g_last_error = 0; RET_STD(6, 1);
}
HLE(advapi32, CryptDestroyKey){ if(!dlc_crypt_enabled() || !ofree(ARG(0), K_KEY)) FAIL(1, NTE_BAD_KEY); RET_STD(1, 1); }
/* BOOL CryptCreateHash(HCRYPTPROV hProv, ALG_ID alg, HCRYPTKEY hKey, DWORD flags, HCRYPTHASH *phHash) */
HLE(advapi32, CryptCreateHash){
  if(!dlc_crypt_enabled() || !oget(ARG(0), K_PROV)) FAIL(5, NTE_BAD_UID);
  uint32_t alg = ARG(1); if(alg != CALG_MD5 && alg != CALG_SHA1) FAIL(5, NTE_BAD_ALGID);
  if(ARG(2)) FAIL(5, NTE_BAD_KEY);                              /* keyed hashes (HMAC/MAC) not supported */
  if(!ARG(4)) FAIL(5, ERR_INVALID_PARAM);
  uint32_t h = onew(K_HASH); if(!h) FAIL(5, NTE_BAD_HASH);
  CObj *o = oget(h, K_HASH); o->alg = alg; if(alg == CALG_MD5) md5_init(&o->md5); else sha1_init(&o->sha1);
  W32(ARG(4), h); g_last_error = 0; RET_STD(5, 1);
}
/* BOOL CryptHashData(HCRYPTHASH h, const BYTE *p, DWORD n, DWORD flags) */
HLE(advapi32, CryptHashData){
  CObj *o = dlc_crypt_enabled() ? oget(ARG(0), K_HASH) : 0; if(!o) FAIL(4, NTE_BAD_HASH);
  if(o->done) FAIL(4, NTE_BAD_HASH_STATE);
  uint32_t p = ARG(1), n = ARG(2); if(n && !p) FAIL(4, ERR_INVALID_PARAM);
  if(!o->label[0] && n && n < sizeof o->label){             /* the first chunk the DLC check hashes is the DLC name */
    int ok = 1; for(uint32_t i = 0; i < n; i++){ uint8_t ch = R8(p + i); if(ch < 32 || ch > 126){ ok = 0; break; } }
    if(ok){ memcpy(o->label, MEMP(p), n); o->label[n] = 0; } }
  if(o->alg == CALG_MD5) md5_upd(&o->md5, MEMP(p), n); else sha1_upd(&o->sha1, MEMP(p), n);
  RET_STD(4, 1);
}
static int hash_value(CObj *o, uint8_t *out){ if(o->alg == CALG_MD5){ md5_fin(o->md5, out); return 16; } sha1_fin(o->sha1, out); return 20; }
/* BOOL CryptGetHashParam(HCRYPTHASH h, DWORD param, BYTE *pb, DWORD *pdwLen, DWORD flags) */
HLE(advapi32, CryptGetHashParam){
  CObj *o = dlc_crypt_enabled() ? oget(ARG(0), K_HASH) : 0; if(!o) FAIL(5, NTE_BAD_HASH);
  uint32_t prm = ARG(1), pb = ARG(2), pl = ARG(3); if(!pl) FAIL(5, ERR_INVALID_PARAM);
  uint8_t v[20]; uint32_t vl;
  if(prm == 1){ vl = 4; memcpy(v, &o->alg, 4); }                             /* HP_ALGID */
  else if(prm == 4){ vl = 4; uint32_t s = o->alg == CALG_MD5 ? 16 : 20; memcpy(v, &s, 4); }  /* HP_HASHSIZE */
  else if(prm == 2){ vl = (uint32_t)hash_value(o, v); }                      /* HP_HASHVAL */
  else FAIL(5, NTE_BAD_TYPE);
  if(!pb){ W32(pl, vl); RET_STD(5, 1); }
  if(R32(pl) < vl){ W32(pl, vl); FAIL(5, ERR_MORE_DATA); }
  memcpy(MEMP(pb), v, vl); W32(pl, vl); if(prm == 2) o->done = 1; RET_STD(5, 1);
}
HLE(advapi32, CryptDestroyHash){ if(!dlc_crypt_enabled() || !ofree(ARG(0), K_HASH)) FAIL(1, NTE_BAD_HASH); RET_STD(1, 1); }
/* BOOL CryptVerifySignatureW(HCRYPTHASH h, const BYTE *sig, DWORD sigLen, HCRYPTKEY hPubKey, LPCWSTR desc, DWORD flags)
   CryptoAPI signatures are little-endian; PKCS#1 v1.5 with the hash's DigestInfo (none with CRYPT_NOHASHOID). */
HLE(advapi32, CryptVerifySignatureW){
  CObj *o = dlc_crypt_enabled() ? oget(ARG(0), K_HASH) : 0; if(!o) FAIL(6, NTE_BAD_HASH);
  CObj *k = oget(ARG(3), K_KEY); if(!k) FAIL(6, NTE_BAD_KEY);
  uint32_t sp = ARG(1), sl = ARG(2), fl = ARG(5); if(!sp) FAIL(6, ERR_INVALID_PARAM);
  if(fl & ~CRYPT_NOHASHOID) FAIL(6, NTE_BAD_FLAGS);
  const char *nm = o->label[0] ? o->label : "?";
  uint8_t hv[20]; int hl = hash_value(o, hv); o->done = 1;
  if((int)sl != k->modlen){ fprintf(stderr, "[dlc] signature %s: FAILED (signature.bin is %u bytes, key is %d)\n", nm, sl, k->modlen); FAIL(6, NTE_BAD_SIGNATURE); }
  uint8_t sig[512], em[512];
  for(uint32_t i = 0; i < sl; i++) sig[sl - 1 - i] = R8(sp + i);
  int ok = dlc_rsa_public(em, sig, k->mod, k->modlen, k->e);
  if(ok){ const uint8_t *di = 0; int dl = 0;
    if(!(fl & CRYPT_NOHASHOID)){ if(o->alg == CALG_MD5){ di = DI_MD5; dl = sizeof DI_MD5; } else { di = DI_SHA1; dl = sizeof DI_SHA1; } }
    ok = dlc_pkcs1_check(em, k->modlen, di, dl, hv, hl); }
  fprintf(stderr, "[dlc] signature %s: %s\n", nm, ok ? "OK (DLC accepted by the game's check)" : "FAILED (files differ from the signed Steam copy - re-copy this DLC folder unmodified)");
  if(!ok) FAIL(6, NTE_BAD_SIGNATURE);
  g_last_error = 0; RET_STD(6, 1);
}
