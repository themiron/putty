/*
 * sshaes.c - implementation of AES / Rijndael
 * 
 * AES is a flexible algorithm as regards endianness: it has no
 * inherent preference as to which way round you should form words
 * from the input byte stream. It talks endlessly of four-byte
 * _vectors_, but never of 32-bit _words_ - there's no 32-bit
 * addition at all, which would force an endianness by means of
 * which way the carries went. So it would be possible to write a
 * working AES that read words big-endian, and another working one
 * that read them little-endian, just by computing a different set
 * of tables - with no speed drop.
 * 
 * It's therefore tempting to do just that, and remove the overhead
 * of GET_32BIT_MSB_FIRST() et al, allowing every system to use its
 * own endianness-native code; but I decided not to, partly for
 * ease of testing, and mostly because I like the flexibility that
 * allows you to encrypt a non-word-aligned block of memory (which
 * many systems would stop being able to do if I went the
 * endianness-dependent route).
 * 
 * This implementation reads and stores words big-endian, but
 * that's a minor implementation detail. By flipping the endianness
 * of everything in the E0..E3, D0..D3 tables, and substituting
 * GET_32BIT_LSB_FIRST for GET_32BIT_MSB_FIRST, I could create an
 * implementation that worked internally little-endian and gave the
 * same answers at the same speed.
 */

#include <assert.h>
#include <stdlib.h>

#include "ssh.h"

#define MAX_NR 14		       /* max no of rounds */
#define NB 4                          /* no of words in cipher blk */

#define mulby2(x) ( ((x&0x7F) << 1) ^ (x & 0x80 ? 0x1B : 0) )

/*
 * Select appropriate inline keyword for the compiler
 */
#if defined __GNUC__ || defined __clang__
#    define INLINE __inline__
#elif defined (_MSC_VER)
#    define INLINE __forceinline
#else
#    define INLINE
#endif

struct AESContext {
    uint32_t keysched_buf[(MAX_NR + 1) * NB + 3];
    uint32_t invkeysched_buf[(MAX_NR + 1) * NB + 3];
    uint32_t *keysched, *invkeysched;
    uint32_t iv[NB];
    int Nr; /* number of rounds */
    void (*encrypt_cbc)(unsigned char*, int, AESContext*);
    void (*decrypt_cbc)(unsigned char*, int, AESContext*);
    void (*sdctr)(unsigned char*, int, AESContext*);
    bool isNI;
};

static void aes_encrypt_cbc_sw(unsigned char*, int, AESContext*);
static void aes_decrypt_cbc_sw(unsigned char*, int, AESContext*);
static void aes_sdctr_sw(unsigned char*, int, AESContext*);

INLINE static bool supports_aes_ni();
static void aes_setup_ni(AESContext * ctx,
                         const unsigned char *key, int keylen);

INLINE static void aes_encrypt_cbc(unsigned char *blk, int len, AESContext * ctx)
{
    ctx->encrypt_cbc(blk, len, ctx);
}

INLINE static void aes_decrypt_cbc(unsigned char *blk, int len, AESContext * ctx)
{
    ctx->decrypt_cbc(blk, len, ctx);
}

INLINE static void aes_sdctr(unsigned char *blk, int len, AESContext * ctx)
{
    ctx->sdctr(blk, len, ctx);
}

/*
 * SW AES lookup tables
 */
static const unsigned char Sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5,
    0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
    0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a,
    0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
    0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85,
    0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17,
    0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9,
    0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
    0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94,
    0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68,
    0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static const unsigned char Sboxinv[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38,
    0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87,
    0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d,
    0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2,
    0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16,
    0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda,
    0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a,
    0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02,
    0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea,
    0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85,
    0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89,
    0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20,
    0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31,
    0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d,
    0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0,
    0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26,
    0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

static const uint32_t E0[256] = {
    0xc66363a5, 0xf87c7c84, 0xee777799, 0xf67b7b8d,
    0xfff2f20d, 0xd66b6bbd, 0xde6f6fb1, 0x91c5c554,
    0x60303050, 0x02010103, 0xce6767a9, 0x562b2b7d,
    0xe7fefe19, 0xb5d7d762, 0x4dababe6, 0xec76769a,
    0x8fcaca45, 0x1f82829d, 0x89c9c940, 0xfa7d7d87,
    0xeffafa15, 0xb25959eb, 0x8e4747c9, 0xfbf0f00b,
    0x41adadec, 0xb3d4d467, 0x5fa2a2fd, 0x45afafea,
    0x239c9cbf, 0x53a4a4f7, 0xe4727296, 0x9bc0c05b,
    0x75b7b7c2, 0xe1fdfd1c, 0x3d9393ae, 0x4c26266a,
    0x6c36365a, 0x7e3f3f41, 0xf5f7f702, 0x83cccc4f,
    0x6834345c, 0x51a5a5f4, 0xd1e5e534, 0xf9f1f108,
    0xe2717193, 0xabd8d873, 0x62313153, 0x2a15153f,
    0x0804040c, 0x95c7c752, 0x46232365, 0x9dc3c35e,
    0x30181828, 0x379696a1, 0x0a05050f, 0x2f9a9ab5,
    0x0e070709, 0x24121236, 0x1b80809b, 0xdfe2e23d,
    0xcdebeb26, 0x4e272769, 0x7fb2b2cd, 0xea75759f,
    0x1209091b, 0x1d83839e, 0x582c2c74, 0x341a1a2e,
    0x361b1b2d, 0xdc6e6eb2, 0xb45a5aee, 0x5ba0a0fb,
    0xa45252f6, 0x763b3b4d, 0xb7d6d661, 0x7db3b3ce,
    0x5229297b, 0xdde3e33e, 0x5e2f2f71, 0x13848497,
    0xa65353f5, 0xb9d1d168, 0x00000000, 0xc1eded2c,
    0x40202060, 0xe3fcfc1f, 0x79b1b1c8, 0xb65b5bed,
    0xd46a6abe, 0x8dcbcb46, 0x67bebed9, 0x7239394b,
    0x944a4ade, 0x984c4cd4, 0xb05858e8, 0x85cfcf4a,
    0xbbd0d06b, 0xc5efef2a, 0x4faaaae5, 0xedfbfb16,
    0x864343c5, 0x9a4d4dd7, 0x66333355, 0x11858594,
    0x8a4545cf, 0xe9f9f910, 0x04020206, 0xfe7f7f81,
    0xa05050f0, 0x783c3c44, 0x259f9fba, 0x4ba8a8e3,
    0xa25151f3, 0x5da3a3fe, 0x804040c0, 0x058f8f8a,
    0x3f9292ad, 0x219d9dbc, 0x70383848, 0xf1f5f504,
    0x63bcbcdf, 0x77b6b6c1, 0xafdada75, 0x42212163,
    0x20101030, 0xe5ffff1a, 0xfdf3f30e, 0xbfd2d26d,
    0x81cdcd4c, 0x180c0c14, 0x26131335, 0xc3ecec2f,
    0xbe5f5fe1, 0x359797a2, 0x884444cc, 0x2e171739,
    0x93c4c457, 0x55a7a7f2, 0xfc7e7e82, 0x7a3d3d47,
    0xc86464ac, 0xba5d5de7, 0x3219192b, 0xe6737395,
    0xc06060a0, 0x19818198, 0x9e4f4fd1, 0xa3dcdc7f,
    0x44222266, 0x542a2a7e, 0x3b9090ab, 0x0b888883,
    0x8c4646ca, 0xc7eeee29, 0x6bb8b8d3, 0x2814143c,
    0xa7dede79, 0xbc5e5ee2, 0x160b0b1d, 0xaddbdb76,
    0xdbe0e03b, 0x64323256, 0x743a3a4e, 0x140a0a1e,
    0x924949db, 0x0c06060a, 0x4824246c, 0xb85c5ce4,
    0x9fc2c25d, 0xbdd3d36e, 0x43acacef, 0xc46262a6,
    0x399191a8, 0x319595a4, 0xd3e4e437, 0xf279798b,
    0xd5e7e732, 0x8bc8c843, 0x6e373759, 0xda6d6db7,
    0x018d8d8c, 0xb1d5d564, 0x9c4e4ed2, 0x49a9a9e0,
    0xd86c6cb4, 0xac5656fa, 0xf3f4f407, 0xcfeaea25,
    0xca6565af, 0xf47a7a8e, 0x47aeaee9, 0x10080818,
    0x6fbabad5, 0xf0787888, 0x4a25256f, 0x5c2e2e72,
    0x381c1c24, 0x57a6a6f1, 0x73b4b4c7, 0x97c6c651,
    0xcbe8e823, 0xa1dddd7c, 0xe874749c, 0x3e1f1f21,
    0x964b4bdd, 0x61bdbddc, 0x0d8b8b86, 0x0f8a8a85,
    0xe0707090, 0x7c3e3e42, 0x71b5b5c4, 0xcc6666aa,
    0x904848d8, 0x06030305, 0xf7f6f601, 0x1c0e0e12,
    0xc26161a3, 0x6a35355f, 0xae5757f9, 0x69b9b9d0,
    0x17868691, 0x99c1c158, 0x3a1d1d27, 0x279e9eb9,
    0xd9e1e138, 0xebf8f813, 0x2b9898b3, 0x22111133,
    0xd26969bb, 0xa9d9d970, 0x078e8e89, 0x339494a7,
    0x2d9b9bb6, 0x3c1e1e22, 0x15878792, 0xc9e9e920,
    0x87cece49, 0xaa5555ff, 0x50282878, 0xa5dfdf7a,
    0x038c8c8f, 0x59a1a1f8, 0x09898980, 0x1a0d0d17,
    0x65bfbfda, 0xd7e6e631, 0x844242c6, 0xd06868b8,
    0x824141c3, 0x299999b0, 0x5a2d2d77, 0x1e0f0f11,
    0x7bb0b0cb, 0xa85454fc, 0x6dbbbbd6, 0x2c16163a,
};
static const uint32_t E1[256] = {
    0xa5c66363, 0x84f87c7c, 0x99ee7777, 0x8df67b7b,
    0x0dfff2f2, 0xbdd66b6b, 0xb1de6f6f, 0x5491c5c5,
    0x50603030, 0x03020101, 0xa9ce6767, 0x7d562b2b,
    0x19e7fefe, 0x62b5d7d7, 0xe64dabab, 0x9aec7676,
    0x458fcaca, 0x9d1f8282, 0x4089c9c9, 0x87fa7d7d,
    0x15effafa, 0xebb25959, 0xc98e4747, 0x0bfbf0f0,
    0xec41adad, 0x67b3d4d4, 0xfd5fa2a2, 0xea45afaf,
    0xbf239c9c, 0xf753a4a4, 0x96e47272, 0x5b9bc0c0,
    0xc275b7b7, 0x1ce1fdfd, 0xae3d9393, 0x6a4c2626,
    0x5a6c3636, 0x417e3f3f, 0x02f5f7f7, 0x4f83cccc,
    0x5c683434, 0xf451a5a5, 0x34d1e5e5, 0x08f9f1f1,
    0x93e27171, 0x73abd8d8, 0x53623131, 0x3f2a1515,
    0x0c080404, 0x5295c7c7, 0x65462323, 0x5e9dc3c3,
    0x28301818, 0xa1379696, 0x0f0a0505, 0xb52f9a9a,
    0x090e0707, 0x36241212, 0x9b1b8080, 0x3ddfe2e2,
    0x26cdebeb, 0x694e2727, 0xcd7fb2b2, 0x9fea7575,
    0x1b120909, 0x9e1d8383, 0x74582c2c, 0x2e341a1a,
    0x2d361b1b, 0xb2dc6e6e, 0xeeb45a5a, 0xfb5ba0a0,
    0xf6a45252, 0x4d763b3b, 0x61b7d6d6, 0xce7db3b3,
    0x7b522929, 0x3edde3e3, 0x715e2f2f, 0x97138484,
    0xf5a65353, 0x68b9d1d1, 0x00000000, 0x2cc1eded,
    0x60402020, 0x1fe3fcfc, 0xc879b1b1, 0xedb65b5b,
    0xbed46a6a, 0x468dcbcb, 0xd967bebe, 0x4b723939,
    0xde944a4a, 0xd4984c4c, 0xe8b05858, 0x4a85cfcf,
    0x6bbbd0d0, 0x2ac5efef, 0xe54faaaa, 0x16edfbfb,
    0xc5864343, 0xd79a4d4d, 0x55663333, 0x94118585,
    0xcf8a4545, 0x10e9f9f9, 0x06040202, 0x81fe7f7f,
    0xf0a05050, 0x44783c3c, 0xba259f9f, 0xe34ba8a8,
    0xf3a25151, 0xfe5da3a3, 0xc0804040, 0x8a058f8f,
    0xad3f9292, 0xbc219d9d, 0x48703838, 0x04f1f5f5,
    0xdf63bcbc, 0xc177b6b6, 0x75afdada, 0x63422121,
    0x30201010, 0x1ae5ffff, 0x0efdf3f3, 0x6dbfd2d2,
    0x4c81cdcd, 0x14180c0c, 0x35261313, 0x2fc3ecec,
    0xe1be5f5f, 0xa2359797, 0xcc884444, 0x392e1717,
    0x5793c4c4, 0xf255a7a7, 0x82fc7e7e, 0x477a3d3d,
    0xacc86464, 0xe7ba5d5d, 0x2b321919, 0x95e67373,
    0xa0c06060, 0x98198181, 0xd19e4f4f, 0x7fa3dcdc,
    0x66442222, 0x7e542a2a, 0xab3b9090, 0x830b8888,
    0xca8c4646, 0x29c7eeee, 0xd36bb8b8, 0x3c281414,
    0x79a7dede, 0xe2bc5e5e, 0x1d160b0b, 0x76addbdb,
    0x3bdbe0e0, 0x56643232, 0x4e743a3a, 0x1e140a0a,
    0xdb924949, 0x0a0c0606, 0x6c482424, 0xe4b85c5c,
    0x5d9fc2c2, 0x6ebdd3d3, 0xef43acac, 0xa6c46262,
    0xa8399191, 0xa4319595, 0x37d3e4e4, 0x8bf27979,
    0x32d5e7e7, 0x438bc8c8, 0x596e3737, 0xb7da6d6d,
    0x8c018d8d, 0x64b1d5d5, 0xd29c4e4e, 0xe049a9a9,
    0xb4d86c6c, 0xfaac5656, 0x07f3f4f4, 0x25cfeaea,
    0xafca6565, 0x8ef47a7a, 0xe947aeae, 0x18100808,
    0xd56fbaba, 0x88f07878, 0x6f4a2525, 0x725c2e2e,
    0x24381c1c, 0xf157a6a6, 0xc773b4b4, 0x5197c6c6,
    0x23cbe8e8, 0x7ca1dddd, 0x9ce87474, 0x213e1f1f,
    0xdd964b4b, 0xdc61bdbd, 0x860d8b8b, 0x850f8a8a,
    0x90e07070, 0x427c3e3e, 0xc471b5b5, 0xaacc6666,
    0xd8904848, 0x05060303, 0x01f7f6f6, 0x121c0e0e,
    0xa3c26161, 0x5f6a3535, 0xf9ae5757, 0xd069b9b9,
    0x91178686, 0x5899c1c1, 0x273a1d1d, 0xb9279e9e,
    0x38d9e1e1, 0x13ebf8f8, 0xb32b9898, 0x33221111,
    0xbbd26969, 0x70a9d9d9, 0x89078e8e, 0xa7339494,
    0xb62d9b9b, 0x223c1e1e, 0x92158787, 0x20c9e9e9,
    0x4987cece, 0xffaa5555, 0x78502828, 0x7aa5dfdf,
    0x8f038c8c, 0xf859a1a1, 0x80098989, 0x171a0d0d,
    0xda65bfbf, 0x31d7e6e6, 0xc6844242, 0xb8d06868,
    0xc3824141, 0xb0299999, 0x775a2d2d, 0x111e0f0f,
    0xcb7bb0b0, 0xfca85454, 0xd66dbbbb, 0x3a2c1616,
};
static const uint32_t E2[256] = {
    0x63a5c663, 0x7c84f87c, 0x7799ee77, 0x7b8df67b,
    0xf20dfff2, 0x6bbdd66b, 0x6fb1de6f, 0xc55491c5,
    0x30506030, 0x01030201, 0x67a9ce67, 0x2b7d562b,
    0xfe19e7fe, 0xd762b5d7, 0xabe64dab, 0x769aec76,
    0xca458fca, 0x829d1f82, 0xc94089c9, 0x7d87fa7d,
    0xfa15effa, 0x59ebb259, 0x47c98e47, 0xf00bfbf0,
    0xadec41ad, 0xd467b3d4, 0xa2fd5fa2, 0xafea45af,
    0x9cbf239c, 0xa4f753a4, 0x7296e472, 0xc05b9bc0,
    0xb7c275b7, 0xfd1ce1fd, 0x93ae3d93, 0x266a4c26,
    0x365a6c36, 0x3f417e3f, 0xf702f5f7, 0xcc4f83cc,
    0x345c6834, 0xa5f451a5, 0xe534d1e5, 0xf108f9f1,
    0x7193e271, 0xd873abd8, 0x31536231, 0x153f2a15,
    0x040c0804, 0xc75295c7, 0x23654623, 0xc35e9dc3,
    0x18283018, 0x96a13796, 0x050f0a05, 0x9ab52f9a,
    0x07090e07, 0x12362412, 0x809b1b80, 0xe23ddfe2,
    0xeb26cdeb, 0x27694e27, 0xb2cd7fb2, 0x759fea75,
    0x091b1209, 0x839e1d83, 0x2c74582c, 0x1a2e341a,
    0x1b2d361b, 0x6eb2dc6e, 0x5aeeb45a, 0xa0fb5ba0,
    0x52f6a452, 0x3b4d763b, 0xd661b7d6, 0xb3ce7db3,
    0x297b5229, 0xe33edde3, 0x2f715e2f, 0x84971384,
    0x53f5a653, 0xd168b9d1, 0x00000000, 0xed2cc1ed,
    0x20604020, 0xfc1fe3fc, 0xb1c879b1, 0x5bedb65b,
    0x6abed46a, 0xcb468dcb, 0xbed967be, 0x394b7239,
    0x4ade944a, 0x4cd4984c, 0x58e8b058, 0xcf4a85cf,
    0xd06bbbd0, 0xef2ac5ef, 0xaae54faa, 0xfb16edfb,
    0x43c58643, 0x4dd79a4d, 0x33556633, 0x85941185,
    0x45cf8a45, 0xf910e9f9, 0x02060402, 0x7f81fe7f,
    0x50f0a050, 0x3c44783c, 0x9fba259f, 0xa8e34ba8,
    0x51f3a251, 0xa3fe5da3, 0x40c08040, 0x8f8a058f,
    0x92ad3f92, 0x9dbc219d, 0x38487038, 0xf504f1f5,
    0xbcdf63bc, 0xb6c177b6, 0xda75afda, 0x21634221,
    0x10302010, 0xff1ae5ff, 0xf30efdf3, 0xd26dbfd2,
    0xcd4c81cd, 0x0c14180c, 0x13352613, 0xec2fc3ec,
    0x5fe1be5f, 0x97a23597, 0x44cc8844, 0x17392e17,
    0xc45793c4, 0xa7f255a7, 0x7e82fc7e, 0x3d477a3d,
    0x64acc864, 0x5de7ba5d, 0x192b3219, 0x7395e673,
    0x60a0c060, 0x81981981, 0x4fd19e4f, 0xdc7fa3dc,
    0x22664422, 0x2a7e542a, 0x90ab3b90, 0x88830b88,
    0x46ca8c46, 0xee29c7ee, 0xb8d36bb8, 0x143c2814,
    0xde79a7de, 0x5ee2bc5e, 0x0b1d160b, 0xdb76addb,
    0xe03bdbe0, 0x32566432, 0x3a4e743a, 0x0a1e140a,
    0x49db9249, 0x060a0c06, 0x246c4824, 0x5ce4b85c,
    0xc25d9fc2, 0xd36ebdd3, 0xacef43ac, 0x62a6c462,
    0x91a83991, 0x95a43195, 0xe437d3e4, 0x798bf279,
    0xe732d5e7, 0xc8438bc8, 0x37596e37, 0x6db7da6d,
    0x8d8c018d, 0xd564b1d5, 0x4ed29c4e, 0xa9e049a9,
    0x6cb4d86c, 0x56faac56, 0xf407f3f4, 0xea25cfea,
    0x65afca65, 0x7a8ef47a, 0xaee947ae, 0x08181008,
    0xbad56fba, 0x7888f078, 0x256f4a25, 0x2e725c2e,
    0x1c24381c, 0xa6f157a6, 0xb4c773b4, 0xc65197c6,
    0xe823cbe8, 0xdd7ca1dd, 0x749ce874, 0x1f213e1f,
    0x4bdd964b, 0xbddc61bd, 0x8b860d8b, 0x8a850f8a,
    0x7090e070, 0x3e427c3e, 0xb5c471b5, 0x66aacc66,
    0x48d89048, 0x03050603, 0xf601f7f6, 0x0e121c0e,
    0x61a3c261, 0x355f6a35, 0x57f9ae57, 0xb9d069b9,
    0x86911786, 0xc15899c1, 0x1d273a1d, 0x9eb9279e,
    0xe138d9e1, 0xf813ebf8, 0x98b32b98, 0x11332211,
    0x69bbd269, 0xd970a9d9, 0x8e89078e, 0x94a73394,
    0x9bb62d9b, 0x1e223c1e, 0x87921587, 0xe920c9e9,
    0xce4987ce, 0x55ffaa55, 0x28785028, 0xdf7aa5df,
    0x8c8f038c, 0xa1f859a1, 0x89800989, 0x0d171a0d,
    0xbfda65bf, 0xe631d7e6, 0x42c68442, 0x68b8d068,
    0x41c38241, 0x99b02999, 0x2d775a2d, 0x0f111e0f,
    0xb0cb7bb0, 0x54fca854, 0xbbd66dbb, 0x163a2c16,
};
static const uint32_t E3[256] = {
    0x6363a5c6, 0x7c7c84f8, 0x777799ee, 0x7b7b8df6,
    0xf2f20dff, 0x6b6bbdd6, 0x6f6fb1de, 0xc5c55491,
    0x30305060, 0x01010302, 0x6767a9ce, 0x2b2b7d56,
    0xfefe19e7, 0xd7d762b5, 0xababe64d, 0x76769aec,
    0xcaca458f, 0x82829d1f, 0xc9c94089, 0x7d7d87fa,
    0xfafa15ef, 0x5959ebb2, 0x4747c98e, 0xf0f00bfb,
    0xadadec41, 0xd4d467b3, 0xa2a2fd5f, 0xafafea45,
    0x9c9cbf23, 0xa4a4f753, 0x727296e4, 0xc0c05b9b,
    0xb7b7c275, 0xfdfd1ce1, 0x9393ae3d, 0x26266a4c,
    0x36365a6c, 0x3f3f417e, 0xf7f702f5, 0xcccc4f83,
    0x34345c68, 0xa5a5f451, 0xe5e534d1, 0xf1f108f9,
    0x717193e2, 0xd8d873ab, 0x31315362, 0x15153f2a,
    0x04040c08, 0xc7c75295, 0x23236546, 0xc3c35e9d,
    0x18182830, 0x9696a137, 0x05050f0a, 0x9a9ab52f,
    0x0707090e, 0x12123624, 0x80809b1b, 0xe2e23ddf,
    0xebeb26cd, 0x2727694e, 0xb2b2cd7f, 0x75759fea,
    0x09091b12, 0x83839e1d, 0x2c2c7458, 0x1a1a2e34,
    0x1b1b2d36, 0x6e6eb2dc, 0x5a5aeeb4, 0xa0a0fb5b,
    0x5252f6a4, 0x3b3b4d76, 0xd6d661b7, 0xb3b3ce7d,
    0x29297b52, 0xe3e33edd, 0x2f2f715e, 0x84849713,
    0x5353f5a6, 0xd1d168b9, 0x00000000, 0xeded2cc1,
    0x20206040, 0xfcfc1fe3, 0xb1b1c879, 0x5b5bedb6,
    0x6a6abed4, 0xcbcb468d, 0xbebed967, 0x39394b72,
    0x4a4ade94, 0x4c4cd498, 0x5858e8b0, 0xcfcf4a85,
    0xd0d06bbb, 0xefef2ac5, 0xaaaae54f, 0xfbfb16ed,
    0x4343c586, 0x4d4dd79a, 0x33335566, 0x85859411,
    0x4545cf8a, 0xf9f910e9, 0x02020604, 0x7f7f81fe,
    0x5050f0a0, 0x3c3c4478, 0x9f9fba25, 0xa8a8e34b,
    0x5151f3a2, 0xa3a3fe5d, 0x4040c080, 0x8f8f8a05,
    0x9292ad3f, 0x9d9dbc21, 0x38384870, 0xf5f504f1,
    0xbcbcdf63, 0xb6b6c177, 0xdada75af, 0x21216342,
    0x10103020, 0xffff1ae5, 0xf3f30efd, 0xd2d26dbf,
    0xcdcd4c81, 0x0c0c1418, 0x13133526, 0xecec2fc3,
    0x5f5fe1be, 0x9797a235, 0x4444cc88, 0x1717392e,
    0xc4c45793, 0xa7a7f255, 0x7e7e82fc, 0x3d3d477a,
    0x6464acc8, 0x5d5de7ba, 0x19192b32, 0x737395e6,
    0x6060a0c0, 0x81819819, 0x4f4fd19e, 0xdcdc7fa3,
    0x22226644, 0x2a2a7e54, 0x9090ab3b, 0x8888830b,
    0x4646ca8c, 0xeeee29c7, 0xb8b8d36b, 0x14143c28,
    0xdede79a7, 0x5e5ee2bc, 0x0b0b1d16, 0xdbdb76ad,
    0xe0e03bdb, 0x32325664, 0x3a3a4e74, 0x0a0a1e14,
    0x4949db92, 0x06060a0c, 0x24246c48, 0x5c5ce4b8,
    0xc2c25d9f, 0xd3d36ebd, 0xacacef43, 0x6262a6c4,
    0x9191a839, 0x9595a431, 0xe4e437d3, 0x79798bf2,
    0xe7e732d5, 0xc8c8438b, 0x3737596e, 0x6d6db7da,
    0x8d8d8c01, 0xd5d564b1, 0x4e4ed29c, 0xa9a9e049,
    0x6c6cb4d8, 0x5656faac, 0xf4f407f3, 0xeaea25cf,
    0x6565afca, 0x7a7a8ef4, 0xaeaee947, 0x08081810,
    0xbabad56f, 0x787888f0, 0x25256f4a, 0x2e2e725c,
    0x1c1c2438, 0xa6a6f157, 0xb4b4c773, 0xc6c65197,
    0xe8e823cb, 0xdddd7ca1, 0x74749ce8, 0x1f1f213e,
    0x4b4bdd96, 0xbdbddc61, 0x8b8b860d, 0x8a8a850f,
    0x707090e0, 0x3e3e427c, 0xb5b5c471, 0x6666aacc,
    0x4848d890, 0x03030506, 0xf6f601f7, 0x0e0e121c,
    0x6161a3c2, 0x35355f6a, 0x5757f9ae, 0xb9b9d069,
    0x86869117, 0xc1c15899, 0x1d1d273a, 0x9e9eb927,
    0xe1e138d9, 0xf8f813eb, 0x9898b32b, 0x11113322,
    0x6969bbd2, 0xd9d970a9, 0x8e8e8907, 0x9494a733,
    0x9b9bb62d, 0x1e1e223c, 0x87879215, 0xe9e920c9,
    0xcece4987, 0x5555ffaa, 0x28287850, 0xdfdf7aa5,
    0x8c8c8f03, 0xa1a1f859, 0x89898009, 0x0d0d171a,
    0xbfbfda65, 0xe6e631d7, 0x4242c684, 0x6868b8d0,
    0x4141c382, 0x9999b029, 0x2d2d775a, 0x0f0f111e,
    0xb0b0cb7b, 0x5454fca8, 0xbbbbd66d, 0x16163a2c,
};
static const uint32_t D0[256] = {
    0x51f4a750, 0x7e416553, 0x1a17a4c3, 0x3a275e96,
    0x3bab6bcb, 0x1f9d45f1, 0xacfa58ab, 0x4be30393,
    0x2030fa55, 0xad766df6, 0x88cc7691, 0xf5024c25,
    0x4fe5d7fc, 0xc52acbd7, 0x26354480, 0xb562a38f,
    0xdeb15a49, 0x25ba1b67, 0x45ea0e98, 0x5dfec0e1,
    0xc32f7502, 0x814cf012, 0x8d4697a3, 0x6bd3f9c6,
    0x038f5fe7, 0x15929c95, 0xbf6d7aeb, 0x955259da,
    0xd4be832d, 0x587421d3, 0x49e06929, 0x8ec9c844,
    0x75c2896a, 0xf48e7978, 0x99583e6b, 0x27b971dd,
    0xbee14fb6, 0xf088ad17, 0xc920ac66, 0x7dce3ab4,
    0x63df4a18, 0xe51a3182, 0x97513360, 0x62537f45,
    0xb16477e0, 0xbb6bae84, 0xfe81a01c, 0xf9082b94,
    0x70486858, 0x8f45fd19, 0x94de6c87, 0x527bf8b7,
    0xab73d323, 0x724b02e2, 0xe31f8f57, 0x6655ab2a,
    0xb2eb2807, 0x2fb5c203, 0x86c57b9a, 0xd33708a5,
    0x302887f2, 0x23bfa5b2, 0x02036aba, 0xed16825c,
    0x8acf1c2b, 0xa779b492, 0xf307f2f0, 0x4e69e2a1,
    0x65daf4cd, 0x0605bed5, 0xd134621f, 0xc4a6fe8a,
    0x342e539d, 0xa2f355a0, 0x058ae132, 0xa4f6eb75,
    0x0b83ec39, 0x4060efaa, 0x5e719f06, 0xbd6e1051,
    0x3e218af9, 0x96dd063d, 0xdd3e05ae, 0x4de6bd46,
    0x91548db5, 0x71c45d05, 0x0406d46f, 0x605015ff,
    0x1998fb24, 0xd6bde997, 0x894043cc, 0x67d99e77,
    0xb0e842bd, 0x07898b88, 0xe7195b38, 0x79c8eedb,
    0xa17c0a47, 0x7c420fe9, 0xf8841ec9, 0x00000000,
    0x09808683, 0x322bed48, 0x1e1170ac, 0x6c5a724e,
    0xfd0efffb, 0x0f853856, 0x3daed51e, 0x362d3927,
    0x0a0fd964, 0x685ca621, 0x9b5b54d1, 0x24362e3a,
    0x0c0a67b1, 0x9357e70f, 0xb4ee96d2, 0x1b9b919e,
    0x80c0c54f, 0x61dc20a2, 0x5a774b69, 0x1c121a16,
    0xe293ba0a, 0xc0a02ae5, 0x3c22e043, 0x121b171d,
    0x0e090d0b, 0xf28bc7ad, 0x2db6a8b9, 0x141ea9c8,
    0x57f11985, 0xaf75074c, 0xee99ddbb, 0xa37f60fd,
    0xf701269f, 0x5c72f5bc, 0x44663bc5, 0x5bfb7e34,
    0x8b432976, 0xcb23c6dc, 0xb6edfc68, 0xb8e4f163,
    0xd731dcca, 0x42638510, 0x13972240, 0x84c61120,
    0x854a247d, 0xd2bb3df8, 0xaef93211, 0xc729a16d,
    0x1d9e2f4b, 0xdcb230f3, 0x0d8652ec, 0x77c1e3d0,
    0x2bb3166c, 0xa970b999, 0x119448fa, 0x47e96422,
    0xa8fc8cc4, 0xa0f03f1a, 0x567d2cd8, 0x223390ef,
    0x87494ec7, 0xd938d1c1, 0x8ccaa2fe, 0x98d40b36,
    0xa6f581cf, 0xa57ade28, 0xdab78e26, 0x3fadbfa4,
    0x2c3a9de4, 0x5078920d, 0x6a5fcc9b, 0x547e4662,
    0xf68d13c2, 0x90d8b8e8, 0x2e39f75e, 0x82c3aff5,
    0x9f5d80be, 0x69d0937c, 0x6fd52da9, 0xcf2512b3,
    0xc8ac993b, 0x10187da7, 0xe89c636e, 0xdb3bbb7b,
    0xcd267809, 0x6e5918f4, 0xec9ab701, 0x834f9aa8,
    0xe6956e65, 0xaaffe67e, 0x21bccf08, 0xef15e8e6,
    0xbae79bd9, 0x4a6f36ce, 0xea9f09d4, 0x29b07cd6,
    0x31a4b2af, 0x2a3f2331, 0xc6a59430, 0x35a266c0,
    0x744ebc37, 0xfc82caa6, 0xe090d0b0, 0x33a7d815,
    0xf104984a, 0x41ecdaf7, 0x7fcd500e, 0x1791f62f,
    0x764dd68d, 0x43efb04d, 0xccaa4d54, 0xe49604df,
    0x9ed1b5e3, 0x4c6a881b, 0xc12c1fb8, 0x4665517f,
    0x9d5eea04, 0x018c355d, 0xfa877473, 0xfb0b412e,
    0xb3671d5a, 0x92dbd252, 0xe9105633, 0x6dd64713,
    0x9ad7618c, 0x37a10c7a, 0x59f8148e, 0xeb133c89,
    0xcea927ee, 0xb761c935, 0xe11ce5ed, 0x7a47b13c,
    0x9cd2df59, 0x55f2733f, 0x1814ce79, 0x73c737bf,
    0x53f7cdea, 0x5ffdaa5b, 0xdf3d6f14, 0x7844db86,
    0xcaaff381, 0xb968c43e, 0x3824342c, 0xc2a3405f,
    0x161dc372, 0xbce2250c, 0x283c498b, 0xff0d9541,
    0x39a80171, 0x080cb3de, 0xd8b4e49c, 0x6456c190,
    0x7bcb8461, 0xd532b670, 0x486c5c74, 0xd0b85742,
};
static const uint32_t D1[256] = {
    0x5051f4a7, 0x537e4165, 0xc31a17a4, 0x963a275e,
    0xcb3bab6b, 0xf11f9d45, 0xabacfa58, 0x934be303,
    0x552030fa, 0xf6ad766d, 0x9188cc76, 0x25f5024c,
    0xfc4fe5d7, 0xd7c52acb, 0x80263544, 0x8fb562a3,
    0x49deb15a, 0x6725ba1b, 0x9845ea0e, 0xe15dfec0,
    0x02c32f75, 0x12814cf0, 0xa38d4697, 0xc66bd3f9,
    0xe7038f5f, 0x9515929c, 0xebbf6d7a, 0xda955259,
    0x2dd4be83, 0xd3587421, 0x2949e069, 0x448ec9c8,
    0x6a75c289, 0x78f48e79, 0x6b99583e, 0xdd27b971,
    0xb6bee14f, 0x17f088ad, 0x66c920ac, 0xb47dce3a,
    0x1863df4a, 0x82e51a31, 0x60975133, 0x4562537f,
    0xe0b16477, 0x84bb6bae, 0x1cfe81a0, 0x94f9082b,
    0x58704868, 0x198f45fd, 0x8794de6c, 0xb7527bf8,
    0x23ab73d3, 0xe2724b02, 0x57e31f8f, 0x2a6655ab,
    0x07b2eb28, 0x032fb5c2, 0x9a86c57b, 0xa5d33708,
    0xf2302887, 0xb223bfa5, 0xba02036a, 0x5ced1682,
    0x2b8acf1c, 0x92a779b4, 0xf0f307f2, 0xa14e69e2,
    0xcd65daf4, 0xd50605be, 0x1fd13462, 0x8ac4a6fe,
    0x9d342e53, 0xa0a2f355, 0x32058ae1, 0x75a4f6eb,
    0x390b83ec, 0xaa4060ef, 0x065e719f, 0x51bd6e10,
    0xf93e218a, 0x3d96dd06, 0xaedd3e05, 0x464de6bd,
    0xb591548d, 0x0571c45d, 0x6f0406d4, 0xff605015,
    0x241998fb, 0x97d6bde9, 0xcc894043, 0x7767d99e,
    0xbdb0e842, 0x8807898b, 0x38e7195b, 0xdb79c8ee,
    0x47a17c0a, 0xe97c420f, 0xc9f8841e, 0x00000000,
    0x83098086, 0x48322bed, 0xac1e1170, 0x4e6c5a72,
    0xfbfd0eff, 0x560f8538, 0x1e3daed5, 0x27362d39,
    0x640a0fd9, 0x21685ca6, 0xd19b5b54, 0x3a24362e,
    0xb10c0a67, 0x0f9357e7, 0xd2b4ee96, 0x9e1b9b91,
    0x4f80c0c5, 0xa261dc20, 0x695a774b, 0x161c121a,
    0x0ae293ba, 0xe5c0a02a, 0x433c22e0, 0x1d121b17,
    0x0b0e090d, 0xadf28bc7, 0xb92db6a8, 0xc8141ea9,
    0x8557f119, 0x4caf7507, 0xbbee99dd, 0xfda37f60,
    0x9ff70126, 0xbc5c72f5, 0xc544663b, 0x345bfb7e,
    0x768b4329, 0xdccb23c6, 0x68b6edfc, 0x63b8e4f1,
    0xcad731dc, 0x10426385, 0x40139722, 0x2084c611,
    0x7d854a24, 0xf8d2bb3d, 0x11aef932, 0x6dc729a1,
    0x4b1d9e2f, 0xf3dcb230, 0xec0d8652, 0xd077c1e3,
    0x6c2bb316, 0x99a970b9, 0xfa119448, 0x2247e964,
    0xc4a8fc8c, 0x1aa0f03f, 0xd8567d2c, 0xef223390,
    0xc787494e, 0xc1d938d1, 0xfe8ccaa2, 0x3698d40b,
    0xcfa6f581, 0x28a57ade, 0x26dab78e, 0xa43fadbf,
    0xe42c3a9d, 0x0d507892, 0x9b6a5fcc, 0x62547e46,
    0xc2f68d13, 0xe890d8b8, 0x5e2e39f7, 0xf582c3af,
    0xbe9f5d80, 0x7c69d093, 0xa96fd52d, 0xb3cf2512,
    0x3bc8ac99, 0xa710187d, 0x6ee89c63, 0x7bdb3bbb,
    0x09cd2678, 0xf46e5918, 0x01ec9ab7, 0xa8834f9a,
    0x65e6956e, 0x7eaaffe6, 0x0821bccf, 0xe6ef15e8,
    0xd9bae79b, 0xce4a6f36, 0xd4ea9f09, 0xd629b07c,
    0xaf31a4b2, 0x312a3f23, 0x30c6a594, 0xc035a266,
    0x37744ebc, 0xa6fc82ca, 0xb0e090d0, 0x1533a7d8,
    0x4af10498, 0xf741ecda, 0x0e7fcd50, 0x2f1791f6,
    0x8d764dd6, 0x4d43efb0, 0x54ccaa4d, 0xdfe49604,
    0xe39ed1b5, 0x1b4c6a88, 0xb8c12c1f, 0x7f466551,
    0x049d5eea, 0x5d018c35, 0x73fa8774, 0x2efb0b41,
    0x5ab3671d, 0x5292dbd2, 0x33e91056, 0x136dd647,
    0x8c9ad761, 0x7a37a10c, 0x8e59f814, 0x89eb133c,
    0xeecea927, 0x35b761c9, 0xede11ce5, 0x3c7a47b1,
    0x599cd2df, 0x3f55f273, 0x791814ce, 0xbf73c737,
    0xea53f7cd, 0x5b5ffdaa, 0x14df3d6f, 0x867844db,
    0x81caaff3, 0x3eb968c4, 0x2c382434, 0x5fc2a340,
    0x72161dc3, 0x0cbce225, 0x8b283c49, 0x41ff0d95,
    0x7139a801, 0xde080cb3, 0x9cd8b4e4, 0x906456c1,
    0x617bcb84, 0x70d532b6, 0x74486c5c, 0x42d0b857,
};
static const uint32_t D2[256] = {
    0xa75051f4, 0x65537e41, 0xa4c31a17, 0x5e963a27,
    0x6bcb3bab, 0x45f11f9d, 0x58abacfa, 0x03934be3,
    0xfa552030, 0x6df6ad76, 0x769188cc, 0x4c25f502,
    0xd7fc4fe5, 0xcbd7c52a, 0x44802635, 0xa38fb562,
    0x5a49deb1, 0x1b6725ba, 0x0e9845ea, 0xc0e15dfe,
    0x7502c32f, 0xf012814c, 0x97a38d46, 0xf9c66bd3,
    0x5fe7038f, 0x9c951592, 0x7aebbf6d, 0x59da9552,
    0x832dd4be, 0x21d35874, 0x692949e0, 0xc8448ec9,
    0x896a75c2, 0x7978f48e, 0x3e6b9958, 0x71dd27b9,
    0x4fb6bee1, 0xad17f088, 0xac66c920, 0x3ab47dce,
    0x4a1863df, 0x3182e51a, 0x33609751, 0x7f456253,
    0x77e0b164, 0xae84bb6b, 0xa01cfe81, 0x2b94f908,
    0x68587048, 0xfd198f45, 0x6c8794de, 0xf8b7527b,
    0xd323ab73, 0x02e2724b, 0x8f57e31f, 0xab2a6655,
    0x2807b2eb, 0xc2032fb5, 0x7b9a86c5, 0x08a5d337,
    0x87f23028, 0xa5b223bf, 0x6aba0203, 0x825ced16,
    0x1c2b8acf, 0xb492a779, 0xf2f0f307, 0xe2a14e69,
    0xf4cd65da, 0xbed50605, 0x621fd134, 0xfe8ac4a6,
    0x539d342e, 0x55a0a2f3, 0xe132058a, 0xeb75a4f6,
    0xec390b83, 0xefaa4060, 0x9f065e71, 0x1051bd6e,
    0x8af93e21, 0x063d96dd, 0x05aedd3e, 0xbd464de6,
    0x8db59154, 0x5d0571c4, 0xd46f0406, 0x15ff6050,
    0xfb241998, 0xe997d6bd, 0x43cc8940, 0x9e7767d9,
    0x42bdb0e8, 0x8b880789, 0x5b38e719, 0xeedb79c8,
    0x0a47a17c, 0x0fe97c42, 0x1ec9f884, 0x00000000,
    0x86830980, 0xed48322b, 0x70ac1e11, 0x724e6c5a,
    0xfffbfd0e, 0x38560f85, 0xd51e3dae, 0x3927362d,
    0xd9640a0f, 0xa621685c, 0x54d19b5b, 0x2e3a2436,
    0x67b10c0a, 0xe70f9357, 0x96d2b4ee, 0x919e1b9b,
    0xc54f80c0, 0x20a261dc, 0x4b695a77, 0x1a161c12,
    0xba0ae293, 0x2ae5c0a0, 0xe0433c22, 0x171d121b,
    0x0d0b0e09, 0xc7adf28b, 0xa8b92db6, 0xa9c8141e,
    0x198557f1, 0x074caf75, 0xddbbee99, 0x60fda37f,
    0x269ff701, 0xf5bc5c72, 0x3bc54466, 0x7e345bfb,
    0x29768b43, 0xc6dccb23, 0xfc68b6ed, 0xf163b8e4,
    0xdccad731, 0x85104263, 0x22401397, 0x112084c6,
    0x247d854a, 0x3df8d2bb, 0x3211aef9, 0xa16dc729,
    0x2f4b1d9e, 0x30f3dcb2, 0x52ec0d86, 0xe3d077c1,
    0x166c2bb3, 0xb999a970, 0x48fa1194, 0x642247e9,
    0x8cc4a8fc, 0x3f1aa0f0, 0x2cd8567d, 0x90ef2233,
    0x4ec78749, 0xd1c1d938, 0xa2fe8cca, 0x0b3698d4,
    0x81cfa6f5, 0xde28a57a, 0x8e26dab7, 0xbfa43fad,
    0x9de42c3a, 0x920d5078, 0xcc9b6a5f, 0x4662547e,
    0x13c2f68d, 0xb8e890d8, 0xf75e2e39, 0xaff582c3,
    0x80be9f5d, 0x937c69d0, 0x2da96fd5, 0x12b3cf25,
    0x993bc8ac, 0x7da71018, 0x636ee89c, 0xbb7bdb3b,
    0x7809cd26, 0x18f46e59, 0xb701ec9a, 0x9aa8834f,
    0x6e65e695, 0xe67eaaff, 0xcf0821bc, 0xe8e6ef15,
    0x9bd9bae7, 0x36ce4a6f, 0x09d4ea9f, 0x7cd629b0,
    0xb2af31a4, 0x23312a3f, 0x9430c6a5, 0x66c035a2,
    0xbc37744e, 0xcaa6fc82, 0xd0b0e090, 0xd81533a7,
    0x984af104, 0xdaf741ec, 0x500e7fcd, 0xf62f1791,
    0xd68d764d, 0xb04d43ef, 0x4d54ccaa, 0x04dfe496,
    0xb5e39ed1, 0x881b4c6a, 0x1fb8c12c, 0x517f4665,
    0xea049d5e, 0x355d018c, 0x7473fa87, 0x412efb0b,
    0x1d5ab367, 0xd25292db, 0x5633e910, 0x47136dd6,
    0x618c9ad7, 0x0c7a37a1, 0x148e59f8, 0x3c89eb13,
    0x27eecea9, 0xc935b761, 0xe5ede11c, 0xb13c7a47,
    0xdf599cd2, 0x733f55f2, 0xce791814, 0x37bf73c7,
    0xcdea53f7, 0xaa5b5ffd, 0x6f14df3d, 0xdb867844,
    0xf381caaf, 0xc43eb968, 0x342c3824, 0x405fc2a3,
    0xc372161d, 0x250cbce2, 0x498b283c, 0x9541ff0d,
    0x017139a8, 0xb3de080c, 0xe49cd8b4, 0xc1906456,
    0x84617bcb, 0xb670d532, 0x5c74486c, 0x5742d0b8,
};
static const uint32_t D3[256] = {
    0xf4a75051, 0x4165537e, 0x17a4c31a, 0x275e963a,
    0xab6bcb3b, 0x9d45f11f, 0xfa58abac, 0xe303934b,
    0x30fa5520, 0x766df6ad, 0xcc769188, 0x024c25f5,
    0xe5d7fc4f, 0x2acbd7c5, 0x35448026, 0x62a38fb5,
    0xb15a49de, 0xba1b6725, 0xea0e9845, 0xfec0e15d,
    0x2f7502c3, 0x4cf01281, 0x4697a38d, 0xd3f9c66b,
    0x8f5fe703, 0x929c9515, 0x6d7aebbf, 0x5259da95,
    0xbe832dd4, 0x7421d358, 0xe0692949, 0xc9c8448e,
    0xc2896a75, 0x8e7978f4, 0x583e6b99, 0xb971dd27,
    0xe14fb6be, 0x88ad17f0, 0x20ac66c9, 0xce3ab47d,
    0xdf4a1863, 0x1a3182e5, 0x51336097, 0x537f4562,
    0x6477e0b1, 0x6bae84bb, 0x81a01cfe, 0x082b94f9,
    0x48685870, 0x45fd198f, 0xde6c8794, 0x7bf8b752,
    0x73d323ab, 0x4b02e272, 0x1f8f57e3, 0x55ab2a66,
    0xeb2807b2, 0xb5c2032f, 0xc57b9a86, 0x3708a5d3,
    0x2887f230, 0xbfa5b223, 0x036aba02, 0x16825ced,
    0xcf1c2b8a, 0x79b492a7, 0x07f2f0f3, 0x69e2a14e,
    0xdaf4cd65, 0x05bed506, 0x34621fd1, 0xa6fe8ac4,
    0x2e539d34, 0xf355a0a2, 0x8ae13205, 0xf6eb75a4,
    0x83ec390b, 0x60efaa40, 0x719f065e, 0x6e1051bd,
    0x218af93e, 0xdd063d96, 0x3e05aedd, 0xe6bd464d,
    0x548db591, 0xc45d0571, 0x06d46f04, 0x5015ff60,
    0x98fb2419, 0xbde997d6, 0x4043cc89, 0xd99e7767,
    0xe842bdb0, 0x898b8807, 0x195b38e7, 0xc8eedb79,
    0x7c0a47a1, 0x420fe97c, 0x841ec9f8, 0x00000000,
    0x80868309, 0x2bed4832, 0x1170ac1e, 0x5a724e6c,
    0x0efffbfd, 0x8538560f, 0xaed51e3d, 0x2d392736,
    0x0fd9640a, 0x5ca62168, 0x5b54d19b, 0x362e3a24,
    0x0a67b10c, 0x57e70f93, 0xee96d2b4, 0x9b919e1b,
    0xc0c54f80, 0xdc20a261, 0x774b695a, 0x121a161c,
    0x93ba0ae2, 0xa02ae5c0, 0x22e0433c, 0x1b171d12,
    0x090d0b0e, 0x8bc7adf2, 0xb6a8b92d, 0x1ea9c814,
    0xf1198557, 0x75074caf, 0x99ddbbee, 0x7f60fda3,
    0x01269ff7, 0x72f5bc5c, 0x663bc544, 0xfb7e345b,
    0x4329768b, 0x23c6dccb, 0xedfc68b6, 0xe4f163b8,
    0x31dccad7, 0x63851042, 0x97224013, 0xc6112084,
    0x4a247d85, 0xbb3df8d2, 0xf93211ae, 0x29a16dc7,
    0x9e2f4b1d, 0xb230f3dc, 0x8652ec0d, 0xc1e3d077,
    0xb3166c2b, 0x70b999a9, 0x9448fa11, 0xe9642247,
    0xfc8cc4a8, 0xf03f1aa0, 0x7d2cd856, 0x3390ef22,
    0x494ec787, 0x38d1c1d9, 0xcaa2fe8c, 0xd40b3698,
    0xf581cfa6, 0x7ade28a5, 0xb78e26da, 0xadbfa43f,
    0x3a9de42c, 0x78920d50, 0x5fcc9b6a, 0x7e466254,
    0x8d13c2f6, 0xd8b8e890, 0x39f75e2e, 0xc3aff582,
    0x5d80be9f, 0xd0937c69, 0xd52da96f, 0x2512b3cf,
    0xac993bc8, 0x187da710, 0x9c636ee8, 0x3bbb7bdb,
    0x267809cd, 0x5918f46e, 0x9ab701ec, 0x4f9aa883,
    0x956e65e6, 0xffe67eaa, 0xbccf0821, 0x15e8e6ef,
    0xe79bd9ba, 0x6f36ce4a, 0x9f09d4ea, 0xb07cd629,
    0xa4b2af31, 0x3f23312a, 0xa59430c6, 0xa266c035,
    0x4ebc3774, 0x82caa6fc, 0x90d0b0e0, 0xa7d81533,
    0x04984af1, 0xecdaf741, 0xcd500e7f, 0x91f62f17,
    0x4dd68d76, 0xefb04d43, 0xaa4d54cc, 0x9604dfe4,
    0xd1b5e39e, 0x6a881b4c, 0x2c1fb8c1, 0x65517f46,
    0x5eea049d, 0x8c355d01, 0x877473fa, 0x0b412efb,
    0x671d5ab3, 0xdbd25292, 0x105633e9, 0xd647136d,
    0xd7618c9a, 0xa10c7a37, 0xf8148e59, 0x133c89eb,
    0xa927eece, 0x61c935b7, 0x1ce5ede1, 0x47b13c7a,
    0xd2df599c, 0xf2733f55, 0x14ce7918, 0xc737bf73,
    0xf7cdea53, 0xfdaa5b5f, 0x3d6f14df, 0x44db8678,
    0xaff381ca, 0x68c43eb9, 0x24342c38, 0xa3405fc2,
    0x1dc37216, 0xe2250cbc, 0x3c498b28, 0x0d9541ff,
    0xa8017139, 0x0cb3de08, 0xb4e49cd8, 0x56c19064,
    0xcb84617b, 0x32b670d5, 0x6c5c7448, 0xb85742d0,
};

/*
 * Set up an AESContext. `keylen' is measured in
 * bytes; it can be either 16 (128-bit), 24 (192-bit), or 32
 * (256-bit).
 */
static void aes_setup(AESContext * ctx, const unsigned char *key, int keylen)
{
    int i, j, Nk, rconst;
    size_t bufaddr;

    ctx->Nr = 6 + (keylen / 4); /* Number of rounds */

    /* Ensure the key schedule arrays are 16-byte aligned */
    bufaddr = (size_t)ctx->keysched_buf;
    ctx->keysched = ctx->keysched_buf +
        (0xF & -bufaddr) / sizeof(uint32_t);
    assert((size_t)ctx->keysched % 16 == 0);
    bufaddr = (size_t)ctx->invkeysched_buf;
    ctx->invkeysched = ctx->invkeysched_buf +
        (0xF & -bufaddr) / sizeof(uint32_t);
    assert((size_t)ctx->invkeysched % 16 == 0);

    ctx->isNI = supports_aes_ni();

    if (ctx->isNI) {
        aes_setup_ni(ctx, key, keylen);
        return;
    }

    assert(keylen == 16 || keylen == 24 || keylen == 32);

    ctx->encrypt_cbc = aes_encrypt_cbc_sw;
    ctx->decrypt_cbc = aes_decrypt_cbc_sw;
    ctx->sdctr = aes_sdctr_sw;

    Nk = keylen / 4;
    rconst = 1;
    for (i = 0; i < (ctx->Nr + 1) * NB; i++) {
	if (i < Nk)
	    ctx->keysched[i] = GET_32BIT_MSB_FIRST(key + 4 * i);
	else {
	    uint32_t temp = ctx->keysched[i - 1];
	    if (i % Nk == 0) {
		int a, b, c, d;
		a = (temp >> 16) & 0xFF;
		b = (temp >> 8) & 0xFF;
		c = (temp >> 0) & 0xFF;
		d = (temp >> 24) & 0xFF;
		temp = Sbox[a] ^ rconst;
		temp = (temp << 8) | Sbox[b];
		temp = (temp << 8) | Sbox[c];
		temp = (temp << 8) | Sbox[d];
		rconst = mulby2(rconst);
	    } else if (i % Nk == 4 && Nk > 6) {
		int a, b, c, d;
		a = (temp >> 24) & 0xFF;
		b = (temp >> 16) & 0xFF;
		c = (temp >> 8) & 0xFF;
		d = (temp >> 0) & 0xFF;
		temp = Sbox[a];
		temp = (temp << 8) | Sbox[b];
		temp = (temp << 8) | Sbox[c];
		temp = (temp << 8) | Sbox[d];
	    }
	    ctx->keysched[i] = ctx->keysched[i - Nk] ^ temp;
	}
    }

    /*
     * Now prepare the modified keys for the inverse cipher.
     */
    for (i = 0; i <= ctx->Nr; i++) {
        for (j = 0; j < NB; j++) {
	    uint32_t temp;
            temp = ctx->keysched[(ctx->Nr - i) * NB + j];
	    if (i != 0 && i != ctx->Nr) {
		/*
		 * Perform the InvMixColumn operation on i. The D
		 * tables give the result of InvMixColumn applied
		 * to Sboxinv on individual bytes, so we should
		 * compose Sbox with the D tables for this.
		 */
		int a, b, c, d;
		a = (temp >> 24) & 0xFF;
		b = (temp >> 16) & 0xFF;
		c = (temp >> 8) & 0xFF;
		d = (temp >> 0) & 0xFF;
		temp = D0[Sbox[a]];
		temp ^= D1[Sbox[b]];
		temp ^= D2[Sbox[c]];
		temp ^= D3[Sbox[d]];
	    }
            ctx->invkeysched[i * NB + j] = temp;
	}
    }
}

/*
 * Software encrypt/decrypt macros
 */
#define ADD_ROUND_KEY (block[0]^=*keysched++,   \
                       block[1]^=*keysched++,   \
                       block[2]^=*keysched++,   \
                       block[3]^=*keysched++)
#define MOVEWORD(i) ( block[i] = newstate[i] )

#define ENCWORD(i) ( newstate[i] = (E0[(block[i       ] >> 24) & 0xFF] ^ \
                                    E1[(block[(i+1)%NB] >> 16) & 0xFF] ^ \
                                    E2[(block[(i+2)%NB] >>  8) & 0xFF] ^ \
                                    E3[ block[(i+3)%NB]        & 0xFF]) )
#define ENCROUND { ENCWORD(0); ENCWORD(1); ENCWORD(2); ENCWORD(3);      \
        MOVEWORD(0); MOVEWORD(1); MOVEWORD(2); MOVEWORD(3); ADD_ROUND_KEY; }

#define ENCLASTWORD(i) ( newstate[i] =                                  \
                         (Sbox[(block[i]        >> 24) & 0xFF] << 24) |  \
                         (Sbox[(block[(i+1)%NB] >> 16) & 0xFF] << 16) |  \
                         (Sbox[(block[(i+2)%NB] >>  8) & 0xFF] <<  8) |  \
                         (Sbox[(block[(i+3)%NB]      ) & 0xFF]      ) )
#define ENCLASTROUND { ENCLASTWORD(0); ENCLASTWORD(1); ENCLASTWORD(2); ENCLASTWORD(3); \
        MOVEWORD(0); MOVEWORD(1); MOVEWORD(2); MOVEWORD(3); ADD_ROUND_KEY; }

#define DECWORD(i) ( newstate[i] =  (D0[(block[i]        >> 24) & 0xFF] ^ \
                                     D1[(block[(i+3)%NB] >> 16) & 0xFF] ^ \
                                     D2[(block[(i+2)%NB] >> 8)  & 0xFF] ^ \
                                     D3[ block[(i+1)%NB]        & 0xFF]) )
#define DECROUND { DECWORD(0); DECWORD(1); DECWORD(2); DECWORD(3);      \
        MOVEWORD(0); MOVEWORD(1); MOVEWORD(2); MOVEWORD(3); ADD_ROUND_KEY; }

#define DECLASTWORD(i) (newstate[i] =                                   \
                        (Sboxinv[(block[i]        >> 24) & 0xFF] << 24) | \
                        (Sboxinv[(block[(i+3)%NB] >> 16) & 0xFF] << 16) | \
                        (Sboxinv[(block[(i+2)%NB] >>  8) & 0xFF] <<  8) | \
                        (Sboxinv[(block[(i+1)%NB]      ) & 0xFF]      ) )
#define DECLASTROUND { DECLASTWORD(0); DECLASTWORD(1); DECLASTWORD(2); DECLASTWORD(3); \
        MOVEWORD(0); MOVEWORD(1); MOVEWORD(2); MOVEWORD(3); ADD_ROUND_KEY; }

/*
 * Software AES encrypt/decrypt core
 */
static void aes_encrypt_cbc_sw(unsigned char *blk, int len, AESContext * ctx)
{
    uint32_t block[4];
    unsigned char* finish = blk + len;
    int i;

    assert((len & 15) == 0);

    memcpy(block, ctx->iv, sizeof(block));

    while (blk < finish) {
        uint32_t *keysched = ctx->keysched;
        uint32_t newstate[4];
	for (i = 0; i < 4; i++)
            block[i] ^= GET_32BIT_MSB_FIRST(blk + 4 * i);
        ADD_ROUND_KEY;
        switch (ctx->Nr) {
          case 14:
            ENCROUND;
            ENCROUND;
          case 12:
            ENCROUND;
            ENCROUND;
          case 10:
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCLASTROUND;
            break;
          default:
            unreachable("bad AES round count");
        }
	for (i = 0; i < 4; i++)
            PUT_32BIT_MSB_FIRST(blk + 4 * i, block[i]);
	blk += 16;
    }

    memcpy(ctx->iv, block, sizeof(block));
}

static void aes_sdctr_sw(unsigned char *blk, int len, AESContext *ctx)
{
    uint32_t iv[4];
    unsigned char* finish = blk + len;
    int i;

    assert((len & 15) == 0);

    memcpy(iv, ctx->iv, sizeof(iv));

    while (blk < finish) {
        uint32_t *keysched = ctx->keysched;
        uint32_t newstate[4], block[4], tmp;
        memcpy(block, iv, sizeof(block));
        ADD_ROUND_KEY;
        switch (ctx->Nr) {
          case 14:
            ENCROUND;
            ENCROUND;
          case 12:
            ENCROUND;
            ENCROUND;
          case 10:
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCROUND;
            ENCLASTROUND;
            break;
          default:
            unreachable("bad AES round count");
        }
	for (i = 0; i < 4; i++) {
            tmp = GET_32BIT_MSB_FIRST(blk + 4 * i);
            PUT_32BIT_MSB_FIRST(blk + 4 * i, tmp ^ block[i]);
	}
        for (i = 3; i >= 0; i--)
            if ((iv[i] = (iv[i] + 1) & 0xffffffff) != 0)
                break;
	blk += 16;
    }

    memcpy(ctx->iv, iv, sizeof(iv));
}

static void aes_decrypt_cbc_sw(unsigned char *blk, int len, AESContext * ctx)
{
    uint32_t iv[4];
    unsigned char* finish = blk + len;
    int i;

    assert((len & 15) == 0);

    memcpy(iv, ctx->iv, sizeof(iv));

    while (blk < finish) {
        uint32_t *keysched = ctx->invkeysched;
        uint32_t newstate[4], ct[4], block[4];
        for (i = 0; i < 4; i++)
            block[i] = ct[i] = GET_32BIT_MSB_FIRST(blk + 4 * i);
        ADD_ROUND_KEY;
        switch (ctx->Nr) {
          case 14:
            DECROUND;
            DECROUND;
          case 12:
            DECROUND;
            DECROUND;
          case 10:
            DECROUND;
            DECROUND;
            DECROUND;
            DECROUND;
            DECROUND;
            DECROUND;
            DECROUND;
            DECROUND;
            DECROUND;
            DECLASTROUND;
            break;
          default:
            unreachable("bad AES round count");
        }
	for (i = 0; i < 4; i++) {
            PUT_32BIT_MSB_FIRST(blk + 4 * i, iv[i] ^ block[i]);
            iv[i] = ct[i];
	}
	blk += 16;
    }

    memcpy(ctx->iv, iv, sizeof(iv));
}

AESContext *aes_make_context(void)
{
    return snew(AESContext);
}

void aes_free_context(AESContext *ctx)
{
    smemclr(ctx, sizeof(*ctx));
    sfree(ctx);
}

void aes128_key(AESContext *ctx, const void *key)
{
    aes_setup(ctx, key, 16);
}

void aes192_key(AESContext *ctx, const void *key)
{
    aes_setup(ctx, key, 24);
}

void aes256_key(AESContext *ctx, const void *key)
{
    aes_setup(ctx, key, 32);
}

void aes_iv(AESContext *ctx, const void *viv)
{
    const unsigned char *iv = (const unsigned char *)viv;
    if (ctx->isNI) {
        memcpy(ctx->iv, iv, sizeof(ctx->iv));
    }
    else {
        int i;
        for (i = 0; i < 4; i++)
            ctx->iv[i] = GET_32BIT_MSB_FIRST(iv + 4 * i);
    }
}

void aes_ssh2_encrypt_blk(AESContext *ctx, void *blk, int len)
{
    aes_encrypt_cbc(blk, len, ctx);
}

void aes_ssh2_decrypt_blk(AESContext *ctx, void *blk, int len)
{
    aes_decrypt_cbc(blk, len, ctx);
}

void aes_ssh2_sdctr(AESContext *ctx, void *blk, int len)
{
    aes_sdctr(blk, len, ctx);
}

void aes256_encrypt_pubkey(const void *key, void *blk, int len)
{
    AESContext ctx;
    aes_setup(&ctx, key, 32);
    memset(ctx.iv, 0, sizeof(ctx.iv));
    aes_encrypt_cbc(blk, len, &ctx);
    smemclr(&ctx, sizeof(ctx));
}

void aes256_decrypt_pubkey(const void *key, void *blk, int len)
{
    AESContext ctx;
    aes_setup(&ctx, key, 32);
    memset(ctx.iv, 0, sizeof(ctx.iv));
    aes_decrypt_cbc(blk, len, &ctx);
    smemclr(&ctx, sizeof(ctx));
}

struct aes_ssh2_ctx {
    AESContext context;
    ssh2_cipher ciph;
};

ssh2_cipher *aes_ssh2_new(const struct ssh2_cipheralg *alg)
{
    struct aes_ssh2_ctx *ctx = snew(struct aes_ssh2_ctx);
    ctx->ciph.vt = alg;
    return &ctx->ciph;
}

static void aes_ssh2_free(ssh2_cipher *cipher)
{
    struct aes_ssh2_ctx *ctx = container_of(cipher, struct aes_ssh2_ctx, ciph);
    smemclr(ctx, sizeof(*ctx));
    sfree(ctx);
}

static void aes_ssh2_setiv(ssh2_cipher *cipher, const void *iv)
{
    struct aes_ssh2_ctx *ctx = container_of(cipher, struct aes_ssh2_ctx, ciph);
    aes_iv(&ctx->context, iv);
}

static void aes_ssh2_setkey(ssh2_cipher *cipher, const void *key)
{
    struct aes_ssh2_ctx *ctx = container_of(cipher, struct aes_ssh2_ctx, ciph);
    aes_setup(&ctx->context, key, ctx->ciph.vt->padded_keybytes);
}

static void aes_ssh2_encrypt(ssh2_cipher *cipher, void *blk, int len)
{
    struct aes_ssh2_ctx *ctx = container_of(cipher, struct aes_ssh2_ctx, ciph);
    aes_encrypt_cbc(blk, len, &ctx->context);
}

static void aes_ssh2_decrypt(ssh2_cipher *cipher, void *blk, int len)
{
    struct aes_ssh2_ctx *ctx = container_of(cipher, struct aes_ssh2_ctx, ciph);
    aes_decrypt_cbc(blk, len, &ctx->context);
}

static void aes_ssh2_sdctr_method(ssh2_cipher *cipher, void *blk, int len)
{
    struct aes_ssh2_ctx *ctx = container_of(cipher, struct aes_ssh2_ctx, ciph);
    aes_sdctr(blk, len, &ctx->context);
}

const struct ssh2_cipheralg ssh_aes128_ctr = {
    aes_ssh2_new, aes_ssh2_free, aes_ssh2_setiv, aes_ssh2_setkey,
    aes_ssh2_sdctr_method, aes_ssh2_sdctr_method, NULL, NULL,
    "aes128-ctr",
    16, 128, 16, 0, "AES-128 SDCTR",
    NULL
};

const struct ssh2_cipheralg ssh_aes192_ctr = {
    aes_ssh2_new, aes_ssh2_free, aes_ssh2_setiv, aes_ssh2_setkey,
    aes_ssh2_sdctr_method, aes_ssh2_sdctr_method, NULL, NULL,
    "aes192-ctr",
    16, 192, 24, 0, "AES-192 SDCTR",
    NULL
};

const struct ssh2_cipheralg ssh_aes256_ctr = {
    aes_ssh2_new, aes_ssh2_free, aes_ssh2_setiv, aes_ssh2_setkey,
    aes_ssh2_sdctr_method, aes_ssh2_sdctr_method, NULL, NULL,
    "aes256-ctr",
    16, 256, 32, 0, "AES-256 SDCTR",
    NULL
};

const struct ssh2_cipheralg ssh_aes128 = {
    aes_ssh2_new, aes_ssh2_free, aes_ssh2_setiv, aes_ssh2_setkey,
    aes_ssh2_encrypt, aes_ssh2_decrypt, NULL, NULL,
    "aes128-cbc",
    16, 128, 16, SSH_CIPHER_IS_CBC, "AES-128 CBC",
    NULL
};

const struct ssh2_cipheralg ssh_aes192 = {
    aes_ssh2_new, aes_ssh2_free, aes_ssh2_setiv, aes_ssh2_setkey,
    aes_ssh2_encrypt, aes_ssh2_decrypt, NULL, NULL,
    "aes192-cbc",
    16, 192, 24, SSH_CIPHER_IS_CBC, "AES-192 CBC",
    NULL
};

const struct ssh2_cipheralg ssh_aes256 = {
    aes_ssh2_new, aes_ssh2_free, aes_ssh2_setiv, aes_ssh2_setkey,
    aes_ssh2_encrypt, aes_ssh2_decrypt, NULL, NULL,
    "aes256-cbc",
    16, 256, 32, SSH_CIPHER_IS_CBC, "AES-256 CBC",
    NULL
};

/* This cipher is just ssh_aes256 under a different protocol
 * identifier; we leave it 'static' because testcrypt won't need it */
static const struct ssh2_cipheralg ssh_rijndael_lysator = {
    aes_ssh2_new, aes_ssh2_free, aes_ssh2_setiv, aes_ssh2_setkey,
    aes_ssh2_encrypt, aes_ssh2_decrypt, NULL, NULL,
    "rijndael-cbc@lysator.liu.se",
    16, 256, 32, SSH_CIPHER_IS_CBC, "AES-256 CBC",
    NULL
};

static const struct ssh2_cipheralg *const aes_list[] = {
    &ssh_aes256_ctr,
    &ssh_aes256,
    &ssh_rijndael_lysator,
    &ssh_aes192_ctr,
    &ssh_aes192,
    &ssh_aes128_ctr,
    &ssh_aes128,
};

const struct ssh2_ciphers ssh2_aes = {
    sizeof(aes_list) / sizeof(*aes_list),
    aes_list
};

/*
 * Implementation of AES for PuTTY using AES-NI
 * instuction set expansion was made by:
 * @author Pavel Kryukov <kryukov@frtk.ru>
 * @author Maxim Kuznetsov <maks.kuznetsov@gmail.com>
 * @author Svyatoslav Kuzmich <svatoslav1@gmail.com>
 *
 * For Putty AES NI project
 * http://pavelkryukov.github.io/putty-aes-ni/
 */

/*
 * Check of compiler version
 */
#ifdef _FORCE_AES_NI
#   define COMPILER_SUPPORTS_AES_NI
#elif defined(__clang__)
#   if __has_attribute(target) && __has_include(<wmmintrin.h>) && (defined(__x86_64__) || defined(__i386))
#       define COMPILER_SUPPORTS_AES_NI
#   endif
#elif defined(__GNUC__)
#    if (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 4)) && (defined(__x86_64__) || defined(__i386))
#       define COMPILER_SUPPORTS_AES_NI
#    endif
#elif defined (_MSC_VER)
#   if (defined(_M_X64) || defined(_M_IX86)) && _MSC_FULL_VER >= 150030729
#      define COMPILER_SUPPORTS_AES_NI
#   endif
#endif

#ifdef _FORCE_SOFTWARE_AES
#   undef COMPILER_SUPPORTS_AES_NI
#endif

#ifdef COMPILER_SUPPORTS_AES_NI

/*
 * Set target architecture for Clang and GCC
 */
#if !defined(__clang__) && defined(__GNUC__)
#    pragma GCC target("aes")
#    pragma GCC target("sse4.1")
#endif

#if defined(__clang__) || (defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8)))
#    define FUNC_ISA __attribute__ ((target("sse4.1,aes")))
#else
#    define FUNC_ISA
#endif

#include <wmmintrin.h>
#include <smmintrin.h>

/*
 * Determinators of CPU type
 */
#if defined(__clang__) || defined(__GNUC__)

#include <cpuid.h>
INLINE static bool supports_aes_ni()
{
    unsigned int CPUInfo[4];
    __cpuid(1, CPUInfo[0], CPUInfo[1], CPUInfo[2], CPUInfo[3]);
    return (CPUInfo[2] & (1 << 25)) && (CPUInfo[2] & (1 << 19)); /* Check AES and SSE4.1 */
}

#else /* defined(__clang__) || defined(__GNUC__) */

INLINE static bool supports_aes_ni()
{
    unsigned int CPUInfo[4];
    __cpuid(CPUInfo, 1);
    return (CPUInfo[2] & (1 << 25)) && (CPUInfo[2] & (1 << 19)); /* Check AES and SSE4.1 */
}

#endif /* defined(__clang__) || defined(__GNUC__) */

/*
 * Wrapper of SHUFPD instruction for MSVC
 */
#ifdef _MSC_VER
FUNC_ISA
INLINE static __m128i mm_shuffle_pd_i0(__m128i a, __m128i b)
{
    union {
        __m128i i;
        __m128d d;
    } au, bu, ru;
    au.i = a;
    bu.i = b;
    ru.d = _mm_shuffle_pd(au.d, bu.d, 0);
    return ru.i;
}

FUNC_ISA
INLINE static __m128i mm_shuffle_pd_i1(__m128i a, __m128i b)
{
    union {
        __m128i i;
        __m128d d;
    } au, bu, ru;
    au.i = a;
    bu.i = b;
    ru.d = _mm_shuffle_pd(au.d, bu.d, 1);
    return ru.i;
}
#else
#define mm_shuffle_pd_i0(a, b) ((__m128i)_mm_shuffle_pd((__m128d)a, (__m128d)b, 0));
#define mm_shuffle_pd_i1(a, b) ((__m128i)_mm_shuffle_pd((__m128d)a, (__m128d)b, 1));
#endif

/*
 * AES-NI key expansion assist functions
 */
FUNC_ISA
INLINE static __m128i AES_128_ASSIST (__m128i temp1, __m128i temp2)
{
    __m128i temp3;
    temp2 = _mm_shuffle_epi32 (temp2 ,0xff);
    temp3 = _mm_slli_si128 (temp1, 0x4);
    temp1 = _mm_xor_si128 (temp1, temp3);
    temp3 = _mm_slli_si128 (temp3, 0x4);
    temp1 = _mm_xor_si128 (temp1, temp3);
    temp3 = _mm_slli_si128 (temp3, 0x4);
    temp1 = _mm_xor_si128 (temp1, temp3);
    temp1 = _mm_xor_si128 (temp1, temp2);
    return temp1;
}

FUNC_ISA
INLINE static void KEY_192_ASSIST(__m128i* temp1, __m128i * temp2, __m128i * temp3)
{
    __m128i temp4;
    *temp2 = _mm_shuffle_epi32 (*temp2, 0x55);
    temp4 = _mm_slli_si128 (*temp1, 0x4);
    *temp1 = _mm_xor_si128 (*temp1, temp4);
    temp4 = _mm_slli_si128 (temp4, 0x4);
    *temp1 = _mm_xor_si128 (*temp1, temp4);
    temp4 = _mm_slli_si128 (temp4, 0x4);
    *temp1 = _mm_xor_si128 (*temp1, temp4);
    *temp1 = _mm_xor_si128 (*temp1, *temp2);
    *temp2 = _mm_shuffle_epi32(*temp1, 0xff);
    temp4 = _mm_slli_si128 (*temp3, 0x4);
    *temp3 = _mm_xor_si128 (*temp3, temp4);
    *temp3 = _mm_xor_si128 (*temp3, *temp2);
}

FUNC_ISA
INLINE static void KEY_256_ASSIST_1(__m128i* temp1, __m128i * temp2)
{
    __m128i temp4;
    *temp2 = _mm_shuffle_epi32(*temp2, 0xff);
    temp4 = _mm_slli_si128 (*temp1, 0x4);
    *temp1 = _mm_xor_si128 (*temp1, temp4);
    temp4 = _mm_slli_si128 (temp4, 0x4);
    *temp1 = _mm_xor_si128 (*temp1, temp4);
    temp4 = _mm_slli_si128 (temp4, 0x4);
    *temp1 = _mm_xor_si128 (*temp1, temp4);
    *temp1 = _mm_xor_si128 (*temp1, *temp2);
}

FUNC_ISA
INLINE static void KEY_256_ASSIST_2(__m128i* temp1, __m128i * temp3)
{
    __m128i temp2,temp4;
    temp4 = _mm_aeskeygenassist_si128 (*temp1, 0x0);
    temp2 = _mm_shuffle_epi32(temp4, 0xaa);
    temp4 = _mm_slli_si128 (*temp3, 0x4);
    *temp3 = _mm_xor_si128 (*temp3, temp4);
    temp4 = _mm_slli_si128 (temp4, 0x4);
    *temp3 = _mm_xor_si128 (*temp3, temp4);
    temp4 = _mm_slli_si128 (temp4, 0x4);
    *temp3 = _mm_xor_si128 (*temp3, temp4);
    *temp3 = _mm_xor_si128 (*temp3, temp2);
}

/*
 * AES-NI key expansion core
 */
FUNC_ISA
static void AES_128_Key_Expansion (const unsigned char *userkey, __m128i *key)
{
    __m128i temp1, temp2;
    temp1 = _mm_loadu_si128((const __m128i*)userkey);
    key[0] = temp1;
    temp2 = _mm_aeskeygenassist_si128 (temp1 ,0x1);
    temp1 = AES_128_ASSIST(temp1, temp2);
    key[1] = temp1;
    temp2 = _mm_aeskeygenassist_si128 (temp1,0x2);
    temp1 = AES_128_ASSIST(temp1, temp2);
    key[2] = temp1;
    temp2 = _mm_aeskeygenassist_si128 (temp1,0x4);
    temp1 = AES_128_ASSIST(temp1, temp2);
    key[3] = temp1;
    temp2 = _mm_aeskeygenassist_si128 (temp1,0x8);
    temp1 = AES_128_ASSIST(temp1, temp2);
    key[4] = temp1;
    temp2 = _mm_aeskeygenassist_si128 (temp1,0x10);
    temp1 = AES_128_ASSIST(temp1, temp2);
    key[5] = temp1;
    temp2 = _mm_aeskeygenassist_si128 (temp1,0x20);
    temp1 = AES_128_ASSIST(temp1, temp2);
    key[6] = temp1;
    temp2 = _mm_aeskeygenassist_si128 (temp1,0x40);
    temp1 = AES_128_ASSIST(temp1, temp2);
    key[7] = temp1;
    temp2 = _mm_aeskeygenassist_si128 (temp1,0x80);
    temp1 = AES_128_ASSIST(temp1, temp2);
    key[8] = temp1;
    temp2 = _mm_aeskeygenassist_si128 (temp1,0x1b);
    temp1 = AES_128_ASSIST(temp1, temp2);
    key[9] = temp1;
    temp2 = _mm_aeskeygenassist_si128 (temp1,0x36);
    temp1 = AES_128_ASSIST(temp1, temp2);
    key[10] = temp1;
}

FUNC_ISA
static void AES_192_Key_Expansion (const unsigned char *userkey, __m128i *key)
{
    __m128i temp1, temp2, temp3;
    temp1 = _mm_loadu_si128((const __m128i*)userkey);
    temp3 = _mm_loadu_si128((const __m128i*)(userkey+16));
    key[0]=temp1;
    key[1]=temp3;
    temp2=_mm_aeskeygenassist_si128 (temp3,0x1);
    KEY_192_ASSIST(&temp1, &temp2, &temp3);
    key[1] = mm_shuffle_pd_i0(key[1], temp1);
    key[2] = mm_shuffle_pd_i1(temp1, temp3);
    temp2=_mm_aeskeygenassist_si128 (temp3,0x2);
    KEY_192_ASSIST(&temp1, &temp2, &temp3);
    key[3]=temp1;
    key[4]=temp3;
    temp2=_mm_aeskeygenassist_si128 (temp3,0x4);
    KEY_192_ASSIST(&temp1, &temp2, &temp3);
    key[4] = mm_shuffle_pd_i0(key[4], temp1);
    key[5] = mm_shuffle_pd_i1(temp1, temp3);
    temp2=_mm_aeskeygenassist_si128 (temp3,0x8);
    KEY_192_ASSIST(&temp1, &temp2, &temp3);
    key[6]=temp1;
    key[7]=temp3;
    temp2=_mm_aeskeygenassist_si128 (temp3,0x10);
    KEY_192_ASSIST(&temp1, &temp2, &temp3);
    key[7] = mm_shuffle_pd_i0(key[7], temp1);
    key[8] = mm_shuffle_pd_i1(temp1, temp3);
    temp2=_mm_aeskeygenassist_si128 (temp3,0x20);
    KEY_192_ASSIST(&temp1, &temp2, &temp3);
    key[9]=temp1;
    key[10]=temp3;
    temp2=_mm_aeskeygenassist_si128 (temp3,0x40);
    KEY_192_ASSIST(&temp1, &temp2, &temp3);
    key[10] = mm_shuffle_pd_i0(key[10], temp1);
    key[11] = mm_shuffle_pd_i1(temp1, temp3);
    temp2=_mm_aeskeygenassist_si128 (temp3,0x80);
    KEY_192_ASSIST(&temp1, &temp2, &temp3);
    key[12]=temp1;
    key[13]=temp3;
}

FUNC_ISA
static void AES_256_Key_Expansion (const unsigned char *userkey, __m128i *key)
{
    __m128i temp1, temp2, temp3;
    temp1 = _mm_loadu_si128((const __m128i*)userkey);
    temp3 = _mm_loadu_si128((const __m128i*)(userkey+16));
    key[0] = temp1;
    key[1] = temp3;
    temp2 = _mm_aeskeygenassist_si128 (temp3,0x01);
    KEY_256_ASSIST_1(&temp1, &temp2);
    key[2]=temp1;
    KEY_256_ASSIST_2(&temp1, &temp3);
    key[3]=temp3;
    temp2 = _mm_aeskeygenassist_si128 (temp3,0x02);
    KEY_256_ASSIST_1(&temp1, &temp2);
    key[4]=temp1;
    KEY_256_ASSIST_2(&temp1, &temp3);
    key[5]=temp3;
    temp2 = _mm_aeskeygenassist_si128 (temp3,0x04);
    KEY_256_ASSIST_1(&temp1, &temp2);
    key[6]=temp1;
    KEY_256_ASSIST_2(&temp1, &temp3);
    key[7]=temp3;
    temp2 = _mm_aeskeygenassist_si128 (temp3,0x08);
    KEY_256_ASSIST_1(&temp1, &temp2);
    key[8]=temp1;
    KEY_256_ASSIST_2(&temp1, &temp3);
    key[9]=temp3;
    temp2 = _mm_aeskeygenassist_si128 (temp3,0x10);
    KEY_256_ASSIST_1(&temp1, &temp2);
    key[10]=temp1;
    KEY_256_ASSIST_2(&temp1, &temp3);
    key[11]=temp3;
    temp2 = _mm_aeskeygenassist_si128 (temp3,0x20);
    KEY_256_ASSIST_1(&temp1, &temp2);
    key[12]=temp1;
    KEY_256_ASSIST_2(&temp1, &temp3);
    key[13]=temp3;
    temp2 = _mm_aeskeygenassist_si128 (temp3,0x40);
    KEY_256_ASSIST_1(&temp1, &temp2);
    key[14]=temp1;
}

/*
 * AES-NI encrypt/decrypt core
 */
FUNC_ISA
static void aes_encrypt_cbc_ni(unsigned char *blk, int len, AESContext * ctx)
{
    __m128i enc;
    __m128i* block = (__m128i*)blk;
    const __m128i* finish = (__m128i*)(blk + len);

    assert((len & 15) == 0);

    /* Load IV */
    enc = _mm_loadu_si128((__m128i*)(ctx->iv));
    while (block < finish) {
        /* Key schedule ptr   */
        __m128i* keysched = (__m128i*)ctx->keysched;

        /* Xor data with IV */
        enc  = _mm_xor_si128(_mm_loadu_si128(block), enc);

        /* Perform rounds */
        enc  = _mm_xor_si128(enc, *keysched);
        switch (ctx->Nr) {
          case 14:
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
          case 12:
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
          case 10:
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenclast_si128(enc, *(++keysched));
            break;
          default:
            unreachable("bad AES round count");
        }

        /* Store and go to next block */
        _mm_storeu_si128(block, enc);
        ++block;
    }

    /* Update IV */
    _mm_storeu_si128((__m128i*)(ctx->iv), enc);
}

FUNC_ISA
static void aes_decrypt_cbc_ni(unsigned char *blk, int len, AESContext * ctx)
{
    __m128i* block = (__m128i*)blk;
    const __m128i* finish = (__m128i*)(blk + len);

    assert((len & 15) == 0);

    /* Load IV */
    __m128i iv = _mm_loadu_si128((__m128i*)(ctx->iv));
    while (block < finish) {
        /* Key schedule ptr   */
        __m128i* keysched = (__m128i*)ctx->invkeysched;
        __m128i last = _mm_loadu_si128(block);
        __m128i dec = _mm_xor_si128(last, *keysched);
        switch (ctx->Nr) {
          case 14:
            dec = _mm_aesdec_si128(dec, *(++keysched));
            dec = _mm_aesdec_si128(dec, *(++keysched));
          case 12:
            dec = _mm_aesdec_si128(dec, *(++keysched));
            dec = _mm_aesdec_si128(dec, *(++keysched));
          case 10:
            dec = _mm_aesdec_si128(dec, *(++keysched));
            dec = _mm_aesdec_si128(dec, *(++keysched));
            dec = _mm_aesdec_si128(dec, *(++keysched));
            dec = _mm_aesdec_si128(dec, *(++keysched));
            dec = _mm_aesdec_si128(dec, *(++keysched));
            dec = _mm_aesdec_si128(dec, *(++keysched));
            dec = _mm_aesdec_si128(dec, *(++keysched));
            dec = _mm_aesdec_si128(dec, *(++keysched));
            dec = _mm_aesdec_si128(dec, *(++keysched));
            dec = _mm_aesdeclast_si128(dec, *(++keysched));
            break;
          default:
            unreachable("bad AES round count");
        }

        /* Xor data with IV */
        dec  = _mm_xor_si128(iv, dec);

        /* Store data */
        _mm_storeu_si128(block, dec);
        iv = last;

        /* Go to next block */
        ++block;
    }

    /* Update IV */
    _mm_storeu_si128((__m128i*)(ctx->iv), iv);
}

FUNC_ISA
static void aes_sdctr_ni(unsigned char *blk, int len, AESContext *ctx)
{
    const __m128i BSWAP_EPI64 = _mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    const __m128i ONE  = _mm_setr_epi32(0,0,0,1);
    const __m128i ZERO = _mm_setzero_si128();
    __m128i iv;
    __m128i* block = (__m128i*)blk;
    const __m128i* finish = (__m128i*)(blk + len);

    assert((len & 15) == 0);

    iv = _mm_loadu_si128((__m128i*)ctx->iv);

    while (block < finish) {
        __m128i enc;
        __m128i* keysched = (__m128i*)ctx->keysched;/* Key schedule ptr   */

        /* Perform rounds */
        enc  = _mm_xor_si128(iv, *keysched); /* Note that we use IV */
        switch (ctx->Nr) {
          case 14:
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
          case 12:
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
          case 10:
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenc_si128(enc, *(++keysched));
            enc = _mm_aesenclast_si128(enc, *(++keysched));
            break;
          default:
            unreachable("bad AES round count");
        }

        /* Xor with block and store result */
        enc = _mm_xor_si128(enc, _mm_loadu_si128(block));
        _mm_storeu_si128(block, enc);

        /* Increment of IV */
        iv  = _mm_shuffle_epi8(iv, BSWAP_EPI64); /* Swap endianess     */
        iv  = _mm_add_epi64(iv, ONE);            /* Inc low part       */
        enc = _mm_cmpeq_epi64(iv, ZERO);         /* Check for carry    */
        enc = _mm_unpacklo_epi64(ZERO, enc);     /* Pack carry reg     */
        iv  = _mm_sub_epi64(iv, enc);            /* Sub carry reg      */
        iv  = _mm_shuffle_epi8(iv, BSWAP_EPI64); /* Swap enianess back */

        /* Go to next block */
        ++block;
    }

    /* Update IV */
    _mm_storeu_si128((__m128i*)ctx->iv, iv);
}

FUNC_ISA
static void aes_inv_key_10(AESContext * ctx)
{
    __m128i* keysched = (__m128i*)ctx->keysched;
    __m128i* invkeysched = (__m128i*)ctx->invkeysched;

    *(invkeysched + 10) = *(keysched + 0);
    *(invkeysched + 9) = _mm_aesimc_si128(*(keysched + 1));
    *(invkeysched + 8) = _mm_aesimc_si128(*(keysched + 2));
    *(invkeysched + 7) = _mm_aesimc_si128(*(keysched + 3));
    *(invkeysched + 6) = _mm_aesimc_si128(*(keysched + 4));
    *(invkeysched + 5) = _mm_aesimc_si128(*(keysched + 5));
    *(invkeysched + 4) = _mm_aesimc_si128(*(keysched + 6));
    *(invkeysched + 3) = _mm_aesimc_si128(*(keysched + 7));
    *(invkeysched + 2) = _mm_aesimc_si128(*(keysched + 8));
    *(invkeysched + 1) = _mm_aesimc_si128(*(keysched + 9));
    *(invkeysched + 0) = *(keysched + 10);
}

FUNC_ISA
static void aes_inv_key_12(AESContext * ctx)
{
    __m128i* keysched = (__m128i*)ctx->keysched;
    __m128i* invkeysched = (__m128i*)ctx->invkeysched;

    *(invkeysched + 12) = *(keysched + 0);
    *(invkeysched + 11) = _mm_aesimc_si128(*(keysched + 1));
    *(invkeysched + 10) = _mm_aesimc_si128(*(keysched + 2));
    *(invkeysched + 9) = _mm_aesimc_si128(*(keysched + 3));
    *(invkeysched + 8) = _mm_aesimc_si128(*(keysched + 4));
    *(invkeysched + 7) = _mm_aesimc_si128(*(keysched + 5));
    *(invkeysched + 6) = _mm_aesimc_si128(*(keysched + 6));
    *(invkeysched + 5) = _mm_aesimc_si128(*(keysched + 7));
    *(invkeysched + 4) = _mm_aesimc_si128(*(keysched + 8));
    *(invkeysched + 3) = _mm_aesimc_si128(*(keysched + 9));
    *(invkeysched + 2) = _mm_aesimc_si128(*(keysched + 10));
    *(invkeysched + 1) = _mm_aesimc_si128(*(keysched + 11));
    *(invkeysched + 0) = *(keysched + 12);
}

FUNC_ISA
static void aes_inv_key_14(AESContext * ctx)
{
    __m128i* keysched = (__m128i*)ctx->keysched;
    __m128i* invkeysched = (__m128i*)ctx->invkeysched;

    *(invkeysched + 14) = *(keysched + 0);
    *(invkeysched + 13) = _mm_aesimc_si128(*(keysched + 1));
    *(invkeysched + 12) = _mm_aesimc_si128(*(keysched + 2));
    *(invkeysched + 11) = _mm_aesimc_si128(*(keysched + 3));
    *(invkeysched + 10) = _mm_aesimc_si128(*(keysched + 4));
    *(invkeysched + 9) = _mm_aesimc_si128(*(keysched + 5));
    *(invkeysched + 8) = _mm_aesimc_si128(*(keysched + 6));
    *(invkeysched + 7) = _mm_aesimc_si128(*(keysched + 7));
    *(invkeysched + 6) = _mm_aesimc_si128(*(keysched + 8));
    *(invkeysched + 5) = _mm_aesimc_si128(*(keysched + 9));
    *(invkeysched + 4) = _mm_aesimc_si128(*(keysched + 10));
    *(invkeysched + 3) = _mm_aesimc_si128(*(keysched + 11));
    *(invkeysched + 2) = _mm_aesimc_si128(*(keysched + 12));
    *(invkeysched + 1) = _mm_aesimc_si128(*(keysched + 13));
    *(invkeysched + 0) = *(keysched + 14);
}

/*
 * Set up an AESContext. `keylen' is measured in
 * bytes; it can be either 16 (128-bit), 24 (192-bit), or 32
 * (256-bit).
 */
FUNC_ISA
static void aes_setup_ni(AESContext * ctx,
                         const unsigned char *key, int keylen)
{
    __m128i *keysched = (__m128i*)ctx->keysched;

    ctx->encrypt_cbc = aes_encrypt_cbc_ni;
    ctx->decrypt_cbc = aes_decrypt_cbc_ni;
    ctx->sdctr = aes_sdctr_ni;

    /*
     * Now do the key setup itself.
     */
    switch (keylen) {
      case 16:
        AES_128_Key_Expansion (key, keysched);
        break;
      case 24:
        AES_192_Key_Expansion (key, keysched);
        break;
      case 32:
        AES_256_Key_Expansion (key, keysched);
        break;
      default:
        unreachable("bad AES key length");
    }

    /*
     * Now prepare the modified keys for the inverse cipher.
     */
    switch (ctx->Nr) {
      case 10:
        aes_inv_key_10(ctx);
        break;
      case 12:
        aes_inv_key_12(ctx);
        break;
      case 14:
        aes_inv_key_14(ctx);
        break;
      default:
        unreachable("bad AES key length");
    }
}

#else /* COMPILER_SUPPORTS_AES_NI */

static void aes_setup_ni(AESContext * ctx, const unsigned char *key, int keylen)
{
    unreachable("aes_setup_ni called when not compiled in");
}

INLINE static bool supports_aes_ni()
{
    return false;
}

#endif /* COMPILER_SUPPORTS_AES_NI */
