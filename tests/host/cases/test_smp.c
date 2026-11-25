/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_smp.c
 * @brief fck_drv_smp: SMP's requests built and framed, and its answers read.
 *
 * The wire vectors are worked out by hand from Zephyr's mcumgr serial
 * transport (len+2, packet, CRC16-XMODEM, base64), and the answers are
 * written the way zcbor writes them in MCUboot's serial recovery and in the
 * firmware's mcumgr: indefinite-length maps, and the outermost one sometimes
 * without its end. What matters most is what must never pass: a frame whose
 * CRC does not match, an answer to another request, an image whose hash is
 * read from the wrong place.
 */
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "fck_bsp.h"
#include "fck_drv_smp.h"

static int s_packets;

/* Every byte of @p b, as the UART would hand them over. The last packet
 * read goes to @p out; s_packets counts them. */
static size_t feed(fck_drv_smp_receive_t *rx, const uint8_t *b, size_t n, uint8_t *out, size_t cap)
{
    size_t last = 0U;
    s_packets   = 0;
    for (size_t k = 0U; k < n; k++) {
        size_t const got = fck_drv_smp_receive_byte(rx, b[k], out, cap);
        if (got != 0U) {
            last = got;
            s_packets++;
        }
    }
    return last;
}

static size_t feed_str(fck_drv_smp_receive_t *rx, const char *s, uint8_t *out, size_t cap)
{
    return feed(rx, (const uint8_t *)s, strlen(s), out, cap);
}

/* ---- Requests --------------------------------------------------------------- */

TEST_CASE("each builder writes mcumgr's header and CBOR, byte for byte", "[smp]")
{
    fck_drv_smp_request_t r;

    static const uint8_t echo[] = { 0x02, 0x00, 0x00, 0x06, 0x00, 0x00, 0x07, 0x00,
                                    0xA1, 0x61, 'd',  0x62, 'h',  'i' };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_smp_request_echo(&r, 7, "hi"));
    TEST_ASSERT_EQUAL_size_t(sizeof(echo), r.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(echo, r.pkt, sizeof(echo));

    static const uint8_t state[] = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x08, 0x00, 0xA0 };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_smp_request_state(&r, 8));
    TEST_ASSERT_EQUAL_size_t(sizeof(state), r.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(state, r.pkt, sizeof(state));

    static const uint8_t reset[] = { 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x09, 0x05, 0xA0 };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_smp_request_reset(&r, 9));
    TEST_ASSERT_EQUAL_size_t(sizeof(reset), r.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(reset, r.pkt, sizeof(reset));

    static const uint8_t confirm[] = { 0x02, 0x00, 0x00, 0x0A, 0x00, 0x01, 0x0A, 0x00,
                                       0xA1, 0x67, 'c',  'o',  'n',  'f',  'i',  'r',
                                       'm',  0xF5 };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_smp_request_confirm(&r, 10));
    TEST_ASSERT_EQUAL_size_t(sizeof(confirm), r.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(confirm, r.pkt, sizeof(confirm));
}

TEST_CASE("the test request names the image by its hash and does not confirm it", "[smp]")
{
    uint8_t hash[64];
    for (size_t k = 0U; k < sizeof(hash); k++) {
        hash[k] = (uint8_t)(0xA0U + k);
    }
    fck_drv_smp_request_t r;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_smp_request_test(&r, 11, hash, sizeof(hash)));
    static const uint8_t head[] = { 0x02, 0x00, 0x00, 0x51, 0x00, 0x01, 0x0B, 0x00, 0xA2,
                                    0x64, 'h',  'a',  's',  'h',  0x58, 0x40 };
    static const uint8_t tail[] = { 0x67, 'c', 'o', 'n', 'f', 'i', 'r', 'm', 0xF4 };
    TEST_ASSERT_EQUAL_size_t(sizeof(head) + sizeof(hash) + sizeof(tail), r.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, r.pkt, sizeof(head));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(hash, r.pkt + sizeof(head), sizeof(hash));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tail, r.pkt + sizeof(head) + sizeof(hash), sizeof(tail));

    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_smp_request_test(&r, 11, hash, 0));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_smp_request_test(&r, 11, hash, 65));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_smp_request_test(&r, 11, NULL, 64));
}

TEST_CASE("the upload goes in 128-byte pieces, and only the first says the length", "[smp]")
{
    uint8_t img[300];
    for (size_t k = 0U; k < sizeof(img); k++) {
        img[k] = (uint8_t)k;
    }
    fck_drv_smp_request_t r;
    size_t        took = 0U;

    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_smp_request_upload(&r, 1, img, sizeof(img), 0, &took));
    TEST_ASSERT_EQUAL_size_t(128, took);
    static const uint8_t first[] = { 0xA4, 0x65, 'i', 'm', 'a', 'g', 'e', 0x00, 0x63, 'l', 'e',
                                     'n',  0x19, 0x01, 0x2C, 0x63, 'o', 'f', 'f', 0x00, 0x64,
                                     'd',  'a',  't',  'a',  0x58, 0x80 };
    TEST_ASSERT_EQUAL_HEX8(0x02, r.pkt[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, r.pkt[5]);
    TEST_ASSERT_EQUAL_HEX8(0x01, r.pkt[7]);
    TEST_ASSERT_EQUAL_size_t(8U + sizeof(first) + 128U, r.len);
    TEST_ASSERT_EQUAL(sizeof(first) + 128U, ((size_t)r.pkt[2] << 8) | r.pkt[3]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(first, r.pkt + 8, sizeof(first));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(img, r.pkt + 8 + sizeof(first), 128);

    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_smp_request_upload(&r, 2, img, sizeof(img), 128, &took));
    TEST_ASSERT_EQUAL_size_t(128, took);
    static const uint8_t next[] = { 0xA2, 0x63, 'o', 'f', 'f', 0x18, 0x80, 0x64,
                                    'd',  'a',  't', 'a', 0x58, 0x80 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(next, r.pkt + 8, sizeof(next));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(img + 128, r.pkt + 8 + sizeof(next), 128);

    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_smp_request_upload(&r, 3, img, sizeof(img), 256, &took));
    TEST_ASSERT_EQUAL_size_t(44, took);
    static const uint8_t last[] = { 0xA2, 0x63, 'o', 'f', 'f', 0x19, 0x01, 0x00,
                                    0x64, 'd',  'a', 't', 'a', 0x58, 0x2C };
    TEST_ASSERT_EQUAL_size_t(8U + sizeof(last) + 44U, r.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(last, r.pkt + 8, sizeof(last));

    took = 99U;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_smp_request_upload(&r, 4, img, sizeof(img), 300, &took));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_smp_request_upload(&r, 4, img, 0, 0, &took));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_smp_request_upload(&r, 4, NULL, 10, 0, &took));
    TEST_ASSERT_EQUAL_size_t(99, took);
}

TEST_CASE("an echo longer than 64 bytes is refused", "[smp]")
{
    char s[66];
    memset(s, 'x', sizeof(s) - 1U);
    s[65] = '\0';
    fck_drv_smp_request_t r;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_smp_request_echo(&r, 0, s));
    s[64] = '\0';
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_smp_request_echo(&r, 0, s));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_smp_request_echo(&r, 0, NULL));
}

/* ---- The wire --------------------------------------------------------------- */

TEST_CASE("a frame is len+2, the packet and its CRC16-XMODEM, in base64", "[smp]")
{
    /* CRC16-XMODEM of "123456789" is 0x31C3: the check value of the CRC. */
    fck_drv_smp_request_t r = { .len = 9 };
    memcpy(r.pkt, "123456789", 9);
    uint8_t      w[FCK_DRV_SMP_WIRE_MAX];
    size_t const n = fck_drv_smp_frame(&r, w, sizeof(w));
    static const char want[] = "\x06\x09"
                               "AAsxMjM0NTY3ODkxww=="
                               "\n";
    TEST_ASSERT_EQUAL_size_t(sizeof(want) - 1U, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, w, n);

    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_smp_request_echo(&r, 0, "hi"));
    static const char echo[] = "\x06\x09"
                               "ABACAAAGAAAAAKFhZGJoaQkX"
                               "\n";
    TEST_ASSERT_EQUAL_size_t(sizeof(echo) - 1U, fck_drv_smp_frame(&r, w, sizeof(w)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(echo, w, sizeof(echo) - 1U);

    TEST_ASSERT_EQUAL_size_t(0, fck_drv_smp_frame(&r, w, sizeof(echo) - 2U));
}

TEST_CASE("a long packet is cut into lines of at most 127 bytes, read back whole", "[smp]")
{
    uint8_t img[300];
    for (size_t k = 0U; k < sizeof(img); k++) {
        img[k] = (uint8_t)(k * 7U);
    }
    fck_drv_smp_request_t r;
    size_t        took;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_smp_request_upload(&r, 5, img, sizeof(img), 0, &took));

    uint8_t      w[FCK_DRV_SMP_WIRE_MAX];
    size_t const n = fck_drv_smp_frame(&r, w, sizeof(w));
    TEST_ASSERT_NOT_EQUAL(0, n);
    int    lines = 0;
    size_t start = 0U;
    for (size_t k = 0U; k < n; k++) {
        if (w[k] == '\n') {
            TEST_ASSERT_LESS_OR_EQUAL_size_t(FCK_DRV_SMP_LINE_MAX, k + 1U - start);
            TEST_ASSERT_EQUAL_HEX8(lines == 0 ? 0x06 : 0x04, w[start]);
            TEST_ASSERT_EQUAL_HEX8(lines == 0 ? 0x09 : 0x14, w[start + 1U]);
            TEST_ASSERT_EQUAL(0, (k - start - 2U) % 4U);
            lines++;
            start = k + 1U;
        }
    }
    TEST_ASSERT_EQUAL_size_t(n, start);
    TEST_ASSERT_GREATER_THAN(1, lines);

    fck_drv_smp_receive_t rx;
    fck_drv_smp_receive_init(&rx);
    uint8_t pkt[FCK_DRV_SMP_PACKET_MAX];
    TEST_ASSERT_EQUAL_size_t(r.len, feed(&rx, w, n, pkt, sizeof(pkt)));
    TEST_ASSERT_EQUAL(1, s_packets);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(r.pkt, pkt, r.len);
    TEST_ASSERT_EQUAL_UINT32(0, rx.dropped);
}

TEST_CASE("the firmware's text around a frame is skipped, and CRLF is read as LF", "[smp]")
{
    fck_drv_smp_receive_t rx;
    fck_drv_smp_receive_init(&rx);
    uint8_t pkt[FCK_DRV_SMP_PACKET_MAX];
    static const uint8_t want[] = { 0x02, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
                                    0xA1, 0x61, 'd',  0x62, 'h',  'i' };

    TEST_ASSERT_EQUAL_size_t(0, feed_str(&rx, "#NRFINFO fw=nrf_fck_agent\r\nuart:~$ ", pkt,
                                         sizeof(pkt)));
    TEST_ASSERT_EQUAL_size_t(sizeof(want), feed_str(&rx, "\x06\x09"
                                                         "ABACAAAGAAAAAKFhZGJoaQkX\r\n",
                                                    pkt, sizeof(pkt)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, pkt, sizeof(want));
    TEST_ASSERT_EQUAL_size_t(0, feed_str(&rx, "uart:~$ \n", pkt, sizeof(pkt)));
    TEST_ASSERT_EQUAL_UINT32(0, rx.dropped);

    /* Text longer than any line, then a frame on the same line. */
    char noise[300];
    memset(noise, 'z', sizeof(noise) - 1U);
    noise[299] = '\0';
    feed_str(&rx, noise, pkt, sizeof(pkt));
    TEST_ASSERT_EQUAL_size_t(sizeof(want), feed_str(&rx, "\x06\x09"
                                                         "ABACAAAGAAAAAKFhZGJoaQkX\n",
                                                    pkt, sizeof(pkt)));
    TEST_ASSERT_EQUAL_UINT32(0, rx.dropped);
}

TEST_CASE("a frame with a wrong CRC, bad base64 or no first line is dropped", "[smp]")
{
    fck_drv_smp_receive_t rx;
    fck_drv_smp_receive_init(&rx);
    uint8_t pkt[FCK_DRV_SMP_PACKET_MAX];

    /* The echo's last CRC byte 0x17 made 0x16: "kX" -> "kW". */
    TEST_ASSERT_EQUAL_size_t(0, feed_str(&rx, "\x06\x09"
                                              "ABACAAAGAAAAAKFhZGJoaQkW\n",
                                         pkt, sizeof(pkt)));
    TEST_ASSERT_EQUAL_UINT32(1, rx.dropped);

    TEST_ASSERT_EQUAL_size_t(0, feed_str(&rx, "\x06\x09"
                                              "ABACAAAG*AAAAKFhZGJoaQkX\n",
                                         pkt, sizeof(pkt)));
    TEST_ASSERT_EQUAL_UINT32(2, rx.dropped);

    TEST_ASSERT_EQUAL_size_t(0, feed_str(&rx, "\x06\x09"
                                              "ABACAAAGAAAAAKFhZGJoaQk\n",
                                         pkt, sizeof(pkt)));
    TEST_ASSERT_EQUAL_UINT32(3, rx.dropped);

    TEST_ASSERT_EQUAL_size_t(0, feed_str(&rx, "\x04\x14"
                                              "ABACAAAGAAAAAKFhZGJoaQkX\n",
                                         pkt, sizeof(pkt)));
    TEST_ASSERT_EQUAL_UINT32(4, rx.dropped);

    /* A good frame after all that still reads. */
    TEST_ASSERT_EQUAL_size_t(14, feed_str(&rx, "\x06\x09"
                                               "ABACAAAGAAAAAKFhZGJoaQkX\n",
                                          pkt, sizeof(pkt)));

    /* And one that does not fit where it is asked to go is dropped too. */
    TEST_ASSERT_EQUAL_size_t(0, feed_str(&rx, "\x06\x09"
                                              "ABACAAAGAAAAAKFhZGJoaQkX\n",
                                         pkt, 13));
    TEST_ASSERT_EQUAL_UINT32(5, rx.dropped);
}

/* ---- Answers ---------------------------------------------------------------- */

/* An answer packet: the header, with its length, in front of @p cbor. */
static size_t answer(uint8_t *pkt, uint8_t op, uint16_t grp, uint8_t seq, uint8_t id,
                     const uint8_t *cbor, size_t n)
{
    pkt[0] = op;
    pkt[1] = 0U;
    pkt[2] = (uint8_t)(n >> 8);
    pkt[3] = (uint8_t)n;
    pkt[4] = (uint8_t)(grp >> 8);
    pkt[5] = (uint8_t)grp;
    pkt[6] = seq;
    pkt[7] = id;
    memcpy(pkt + 8, cbor, n);
    return 8U + n;
}

TEST_CASE("an echo answer is read with its map ended or not, and matched to its request",
          "[smp]")
{
    uint8_t       pkt[FCK_DRV_SMP_PACKET_MAX];
    fck_drv_smp_response_t a;
    fck_drv_smp_request_t q;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_smp_request_echo(&q, 4, "hi"));

    static const uint8_t ended[] = { 0xBF, 0x61, 'r', 0x62, 'h', 'i', 0xFF };
    TEST_ASSERT_TRUE(fck_drv_smp_parse(pkt, answer(pkt, 3, 0, 4, 0, ended, sizeof(ended)), &a));
    TEST_ASSERT_EQUAL_STRING("hi", a.echo);
    TEST_ASSERT_FALSE(a.has_rc);
    TEST_ASSERT_TRUE(fck_drv_smp_answers(&a, &q));

    /* MCUboot's serial recovery leaves the outermost map open. */
    static const uint8_t open[] = { 0xBF, 0x61, 'r', 0x62, 'h', 'i' };
    TEST_ASSERT_TRUE(fck_drv_smp_parse(pkt, answer(pkt, 3, 0, 4, 0, open, sizeof(open)), &a));
    TEST_ASSERT_EQUAL_STRING("hi", a.echo);

    static const uint8_t definite[] = { 0xA1, 0x61, 'r', 0x62, 'h', 'i' };
    TEST_ASSERT_TRUE(fck_drv_smp_parse(pkt, answer(pkt, 3, 0, 4, 0, definite, sizeof(definite)), &a));
    TEST_ASSERT_EQUAL_STRING("hi", a.echo);

    TEST_ASSERT_TRUE(fck_drv_smp_parse(pkt, answer(pkt, 3, 0, 5, 0, ended, sizeof(ended)), &a));
    TEST_ASSERT_FALSE(fck_drv_smp_answers(&a, &q));   /* another seq */
    TEST_ASSERT_TRUE(fck_drv_smp_parse(pkt, answer(pkt, 2, 0, 4, 0, ended, sizeof(ended)), &a));
    TEST_ASSERT_FALSE(fck_drv_smp_answers(&a, &q));   /* the request itself, echoed */
    TEST_ASSERT_TRUE(fck_drv_smp_parse(pkt, answer(pkt, 3, 1, 4, 0, ended, sizeof(ended)), &a));
    TEST_ASSERT_FALSE(fck_drv_smp_answers(&a, &q));   /* another group */
}

TEST_CASE("an upload answer gives rc and the next offset; err.rc counts as rc", "[smp]")
{
    uint8_t       pkt[FCK_DRV_SMP_PACKET_MAX];
    fck_drv_smp_response_t a;

    static const uint8_t up[] = { 0xBF, 0x62, 'r', 'c', 0x00, 0x63, 'o', 'f', 'f',
                                  0x19, 0x01, 0x80, 0xFF };
    TEST_ASSERT_TRUE(fck_drv_smp_parse(pkt, answer(pkt, 3, 1, 1, 1, up, sizeof(up)), &a));
    TEST_ASSERT_TRUE(a.has_rc);
    TEST_ASSERT_EQUAL_INT32(0, a.rc);
    TEST_ASSERT_TRUE(a.has_off);
    TEST_ASSERT_EQUAL_UINT32(384, a.off);

    static const uint8_t err[] = { 0xBF, 0x63, 'e', 'r', 'r', 0xBF, 0x65, 'g', 'r', 'o', 'u',
                                   'p',  0x01, 0x62, 'r', 'c', 0x05, 0xFF, 0xFF };
    TEST_ASSERT_TRUE(fck_drv_smp_parse(pkt, answer(pkt, 3, 1, 1, 1, err, sizeof(err)), &a));
    TEST_ASSERT_TRUE(a.has_rc);
    TEST_ASSERT_EQUAL_INT32(5, a.rc);
    TEST_ASSERT_FALSE(a.has_off);

    /* An "rc" or "off" inside another key's map is not the answer's. */
    static const uint8_t deep[] = { 0xBF, 0x61, 'x', 0xA2, 0x62, 'r', 'c', 0x03, 0x63,
                                    'o',  'f',  'f', 0x10, 0xFF };
    TEST_ASSERT_TRUE(fck_drv_smp_parse(pkt, answer(pkt, 3, 1, 1, 1, deep, sizeof(deep)), &a));
    TEST_ASSERT_FALSE(a.has_rc);
    TEST_ASSERT_FALSE(a.has_off);

    static const uint8_t neg[] = { 0xA1, 0x62, 'r', 'c', 0x20 };
    TEST_ASSERT_TRUE(fck_drv_smp_parse(pkt, answer(pkt, 3, 1, 1, 1, neg, sizeof(neg)), &a));
    TEST_ASSERT_EQUAL_INT32(-1, a.rc);
}

TEST_CASE("a state answer gives each slot, and skips the keys it does not name", "[smp]")
{
    uint8_t cbor[256];
    size_t  n = 0U;
#define PUT(...)                                                                                  \
    do {                                                                                          \
        static const uint8_t b_[] = { __VA_ARGS__ };                                              \
        memcpy(cbor + n, b_, sizeof(b_));                                                         \
        n += sizeof(b_);                                                                          \
    } while (0)
    PUT(0xBF, 0x66, 'i', 'm', 'a', 'g', 'e', 's', 0x9F);
    PUT(0xBF, 0x64, 's', 'l', 'o', 't', 0x00, 0x67, 'v', 'e', 'r', 's', 'i', 'o', 'n', 0x65, '1',
        '.', '0', '.', '0', 0x64, 'h', 'a', 's', 'h', 0x58, 0x40);
    for (int k = 0; k < 64; k++) {
        cbor[n++] = (uint8_t)k;
    }
    PUT(0x68, 'b', 'o', 'o', 't', 'a', 'b', 'l', 'e', 0xF5, 0x67, 'p', 'e', 'n', 'd', 'i', 'n',
        'g', 0xF4, 0x69, 'c', 'o', 'n', 'f', 'i', 'r', 'm', 'e', 'd', 0xF5, 0x66, 'a', 'c', 't',
        'i', 'v', 'e', 0xF5, 0x69, 'p', 'e', 'r', 'm', 'a', 'n', 'e', 'n', 't', 0xF4, 0xFF);
    PUT(0xBF, 0x64, 's', 'l', 'o', 't', 0x01, 0x67, 'v', 'e', 'r', 's', 'i', 'o', 'n', 0x65, '2',
        '.', '0', '.', '0', 0x67, 'p', 'e', 'n', 'd', 'i', 'n', 'g', 0xF5, 0xFF);
    PUT(0xFF, 0x6B, 's', 'p', 'l', 'i', 't', 'S', 't', 'a', 't', 'u', 's', 0x00, 0xFF);
#undef PUT

    uint8_t       pkt[FCK_DRV_SMP_PACKET_MAX];
    fck_drv_smp_response_t a;
    TEST_ASSERT_TRUE(fck_drv_smp_parse(pkt, answer(pkt, 1, 1, 2, 0, cbor, n), &a));
    TEST_ASSERT_EQUAL_UINT8(2, a.images);

    TEST_ASSERT_EQUAL_INT8(0, a.image[0].slot);
    TEST_ASSERT_EQUAL_STRING("1.0.0", a.image[0].version);
    TEST_ASSERT_EQUAL_UINT8(64, a.image[0].hash_len);
    TEST_ASSERT_EQUAL_HEX8(0x00, a.image[0].hash[0]);
    TEST_ASSERT_EQUAL_HEX8(0x3F, a.image[0].hash[63]);
    TEST_ASSERT_TRUE(a.image[0].active);
    TEST_ASSERT_TRUE(a.image[0].confirmed);
    TEST_ASSERT_FALSE(a.image[0].pending);
    TEST_ASSERT_TRUE(a.image[0].bootable);

    TEST_ASSERT_EQUAL_INT8(1, a.image[1].slot);
    TEST_ASSERT_EQUAL_STRING("2.0.0", a.image[1].version);
    TEST_ASSERT_EQUAL_UINT8(0, a.image[1].hash_len);
    TEST_ASSERT_TRUE(a.image[1].pending);
    TEST_ASSERT_FALSE(a.image[1].active);

    TEST_ASSERT_EQUAL_PTR(&a.image[0], fck_drv_smp_slot(&a, 0));
    TEST_ASSERT_EQUAL_PTR(&a.image[1], fck_drv_smp_slot(&a, 1));
    TEST_ASSERT_NULL(fck_drv_smp_slot(&a, 2));
    TEST_ASSERT_NULL(fck_drv_smp_slot(&a, -1));
    TEST_ASSERT_NULL(fck_drv_smp_slot(NULL, 0));
}

TEST_CASE("an answer that does not parse leaves the caller's answer as it was", "[smp]")
{
    uint8_t       pkt[FCK_DRV_SMP_PACKET_MAX];
    fck_drv_smp_response_t a;
    memset(&a, 0, sizeof(a));
    a.rc = 77;

    static const uint8_t inner_open[] = { 0xBF, 0x63, 'e', 'r', 'r', 0xBF, 0x62, 'r', 'c', 0x05 };
    TEST_ASSERT_FALSE(
        fck_drv_smp_parse(pkt, answer(pkt, 3, 1, 1, 1, inner_open, sizeof(inner_open)), &a));
    static const uint8_t short_str[] = { 0xBF, 0x61, 'r', 0x65, 'h', 'i', 0xFF };
    TEST_ASSERT_FALSE(
        fck_drv_smp_parse(pkt, answer(pkt, 3, 0, 1, 0, short_str, sizeof(short_str)), &a));
    static const uint8_t not_map[] = { 0x82, 0x01, 0x02 };
    TEST_ASSERT_FALSE(fck_drv_smp_parse(pkt, answer(pkt, 3, 0, 1, 0, not_map, sizeof(not_map)), &a));
    static const uint8_t lost_value[] = { 0xBF, 0x62, 'r', 'c', 0xFF };
    TEST_ASSERT_FALSE(
        fck_drv_smp_parse(pkt, answer(pkt, 3, 0, 1, 0, lost_value, sizeof(lost_value)), &a));
    static const uint8_t reserved[] = { 0xBF, 0x62, 'r', 'c', 0x1C, 0xFF };
    TEST_ASSERT_FALSE(
        fck_drv_smp_parse(pkt, answer(pkt, 3, 0, 1, 0, reserved, sizeof(reserved)), &a));

    /* A header whose length is not the CBOR's. */
    static const uint8_t ok[] = { 0xA1, 0x62, 'r', 'c', 0x00 };
    size_t const         n    = answer(pkt, 3, 0, 1, 0, ok, sizeof(ok));
    TEST_ASSERT_FALSE(fck_drv_smp_parse(pkt, n - 1U, &a));
    TEST_ASSERT_FALSE(fck_drv_smp_parse(pkt, 7, &a));

    /* Arrays inside arrays, deeper than anything SMP sends. */
    uint8_t deep[32] = { 0xA1, 0x61, 'x' };
    memset(deep + 3, 0x81, 20);
    deep[23] = 0x00;
    TEST_ASSERT_FALSE(fck_drv_smp_parse(pkt, answer(pkt, 3, 0, 1, 0, deep, 24), &a));

    TEST_ASSERT_EQUAL_INT32(77, a.rc);
    TEST_ASSERT_TRUE(fck_drv_smp_parse(pkt, answer(pkt, 3, 0, 1, 0, ok, sizeof(ok)), &a));
    TEST_ASSERT_EQUAL_INT32(0, a.rc);
}

/* ---- The image -------------------------------------------------------------- */

/* A small MCUboot image: a 32-byte header, 16 bytes of code, a protected
 * TLV area, and the unprotected one with a key hash, the image hash and a
 * signature, as imgtool writes them. */
static size_t image(uint8_t *img, uint16_t hash_type, size_t hash_len)
{
    size_t n = 0U;
    memset(img, 0, 32);
    img[0]  = 0x3D;
    img[1]  = 0xB8;
    img[2]  = 0xF3;
    img[3]  = 0x96;
    img[8]  = 32;   /* hdr_size */
    img[12] = 16;   /* img_size */
    n       = 32U;
    memset(img + n, 0xC0, 16);
    n += 16U;

    img[n++] = 0x08;   /* protected: sec counter */
    img[n++] = 0x69;
    img[n++] = 12;
    img[n++] = 0;
    img[n++] = 0x50;
    img[n++] = 0;
    img[n++] = 4;
    img[n++] = 0;
    memset(img + n, 0x11, 4);
    n += 4U;

    size_t const tot = 4U + (4U + 32U) + (4U + hash_len) + (4U + 64U);
    img[n++]         = 0x07;
    img[n++]         = 0x69;
    img[n++]         = (uint8_t)tot;
    img[n++]         = (uint8_t)(tot >> 8);
    img[n++]         = 0x01;   /* KEYHASH */
    img[n++]         = 0;
    img[n++]         = 32;
    img[n++]         = 0;
    memset(img + n, 0x22, 32);
    n += 32U;
    img[n++] = (uint8_t)hash_type;
    img[n++] = (uint8_t)(hash_type >> 8);
    img[n++] = (uint8_t)hash_len;
    img[n++] = 0;
    for (size_t k = 0U; k < hash_len; k++) {
        img[n++] = (uint8_t)(0x80U + k);
    }
    img[n++] = 0x24;   /* ED25519 */
    img[n++] = 0;
    img[n++] = 64;
    img[n++] = 0;
    memset(img + n, 0x33, 64);
    n += 64U;
    return n;
}

TEST_CASE("the image hash is the SHA TLV of the unprotected area", "[smp]")
{
    uint8_t        img[512];
    const uint8_t *h = NULL;
    size_t         hl = 0U;

    size_t n = image(img, 0x12, 64);
    TEST_ASSERT_TRUE(fck_drv_smp_image_hash(img, n, &h, &hl));
    TEST_ASSERT_EQUAL_size_t(64, hl);
    TEST_ASSERT_EQUAL_HEX8(0x80, h[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBF, h[63]);

    n = image(img, 0x10, 32);
    TEST_ASSERT_TRUE(fck_drv_smp_image_hash(img, n, &h, &hl));
    TEST_ASSERT_EQUAL_size_t(32, hl);
    TEST_ASSERT_EQUAL_HEX8(0x80, h[0]);

    /* No hash: a TLV of another type where the hash was. */
    n = image(img, 0x20, 32);
    TEST_ASSERT_FALSE(fck_drv_smp_image_hash(img, n, &h, &hl));

    /* Not an image, or one cut short inside its TLVs. */
    n = image(img, 0x12, 64);
    TEST_ASSERT_FALSE(fck_drv_smp_image_hash(img, n - 100U, &h, &hl));
    img[0] = 0x3E;
    TEST_ASSERT_FALSE(fck_drv_smp_image_hash(img, n, &h, &hl));
    TEST_ASSERT_FALSE(fck_drv_smp_image_hash(img, 16, &h, &hl));
}

/* ---- Whether an image can start in a slot ----------------------------------- */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The image above with a vector table for its 16 bytes of code, which start
 * 32 bytes into it: at 0xE020 once in a slot at 0xE000. */
static size_t vectors(uint8_t *img, uint32_t sp, uint32_t reset, uint32_t nmi, uint32_t fault)
{
    size_t const n = image(img, 0x12, 64);
    put32(img + 32, sp);
    put32(img + 36, reset);
    put32(img + 40, nmi);
    put32(img + 44, fault);
    return n;
}

#define SLOT     0xE000U
#define RAM      0x20000000U
#define RAM_SIZE 0x40000U

static fck_drv_smp_image_check_t check(const uint8_t *img, size_t n, uint32_t slot)
{
    return fck_drv_smp_image_check(img, n, slot, 0xAB000U, RAM, RAM_SIZE);
}

TEST_CASE("an image starts where it was built for, and nowhere else", "[smp]")
{
    uint8_t img[512];
    size_t  n = vectors(img, 0x20040000U, 0xE021U, 0xE025U, 0xE029U);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_FITS, check(img, n, SLOT));
    /* Its 16 bytes of code at 0x10000 (a first module's one-slot MCUboot) or at 0xC000:
     * its handlers fall outside them. A real image's code is longer than
     * those 8 KB, and passes; fck_drv_smp_image_holds() is what tells. */
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_ADDRESS, check(img, n, 0x10000U));
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_ADDRESS, check(img, n, 0xC000U));

    /* NMI and hard fault may be 0; the reset handler may not, and each must
     * be a Thumb address inside the code. */
    n = vectors(img, 0x20040000U, 0xE021U, 0U, 0U);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_FITS, check(img, n, SLOT));
    n = vectors(img, 0x20040000U, 0U, 0xE025U, 0xE029U);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_ADDRESS, check(img, n, SLOT));
    n = vectors(img, 0x20040000U, 0xE020U, 0xE025U, 0xE029U);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_ADDRESS, check(img, n, SLOT));
    n = vectors(img, 0x20040000U, 0xE02FU, 0xE025U, 0xE031U);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_ADDRESS, check(img, n, SLOT));
    n = vectors(img, 0x20040000U, 0xE02FU, 0xE01FU, 0U);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_ADDRESS, check(img, n, SLOT));
}

TEST_CASE("the first stack pointer is above the RAM's start, up to its end, on a word", "[smp]")
{
    uint8_t img[512];
    size_t  n = vectors(img, 0x20000004U, 0xE021U, 0U, 0U);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_FITS, check(img, n, SLOT));
    uint32_t const bad[] = { 0x20040004U, 0x20000000U, 0x2003FFFEU, 0U, 0x00010000U };
    for (size_t k = 0U; k < sizeof(bad) / sizeof(bad[0]); k++) {
        n = vectors(img, bad[k], 0xE021U, 0U, 0U);
        TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_SMP_IMAGE_STACK, check(img, n, SLOT), "stack pointer");
    }
}

TEST_CASE("what is not an image, or says sizes it cannot have, is told apart", "[smp]")
{
    uint8_t img[512];
    size_t  n = vectors(img, 0x20040000U, 0xE021U, 0U, 0U);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_NOT_MCUBOOT, check(img, 31, SLOT));
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_NOT_MCUBOOT, check(NULL, n, SLOT));
    img[0] = 0x3E;
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_NOT_MCUBOOT, check(img, n, SLOT));

    n      = vectors(img, 0x20040000U, 0xE021U, 0U, 0U);
    img[8] = 16;                  /* a header shorter than MCUboot's own */
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_SIZES, check(img, n, SLOT));
    n       = vectors(img, 0x20040000U, 0xE021U, 0U, 0U);
    img[12] = 12;                 /* code too short for a vector table */
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_SIZES, check(img, n, SLOT));
    n       = vectors(img, 0x20040000U, 0xE021U, 0U, 0U);
    img[13] = 0x10;               /* more code than bytes given */
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_SIZES, check(img, n, SLOT));
    n = vectors(img, 0x20040000U, 0xE021U, 0U, 0U);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_SIZES, check(img, 40, SLOT));   /* cut in its code */

    n = vectors(img, 0x20040000U, 0xE021U, 0U, 0U);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_LARGER,
                      fck_drv_smp_image_check(img, n, SLOT, (uint32_t)n - 1U, RAM, RAM_SIZE));
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_FITS,
                      fck_drv_smp_image_check(img, n, SLOT, (uint32_t)n, RAM, RAM_SIZE));
}

TEST_CASE("an image larger than the slot is larger, and one of its size fits", "[smp]")
{
    /* The module's one slot takes an image as large as itself; the first modules' kept MCUboot's
     * trailer and a free sector, and was the shorter (fck_bsp.h). */
    TEST_ASSERT_EQUAL_UINT32(FCK_BSP_NRF_SLOT0_SIZE, FCK_BSP_NRF_IMAGE_MAX);
    TEST_ASSERT_LESS_THAN_UINT32(FCK_BSP_NRF_FIRST_SLOT0_SIZE, FCK_BSP_NRF_FIRST_IMAGE_MAX);
    TEST_ASSERT_LESS_THAN_UINT32(FCK_BSP_NRF_IMAGE_MAX, FCK_BSP_NRF_FIRST_IMAGE_MAX);
    TEST_ASSERT_EQUAL_HEX32(0x0000E800, FCK_BSP_NRF_IMAGE_START);
    size_t const n   = (size_t)FCK_BSP_NRF_IMAGE_MAX + 1U;
    uint8_t     *img = calloc(n, 1U);
    TEST_ASSERT_NOT_NULL(img);
    (void)vectors(img, 0x20040000U, 0xE021U, 0U, 0U);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_LARGER,
                      fck_drv_smp_image_check(img, n, SLOT, FCK_BSP_NRF_IMAGE_MAX, RAM, RAM_SIZE));
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_FITS,
                      fck_drv_smp_image_check(img, n - 1U, SLOT, FCK_BSP_NRF_IMAGE_MAX, RAM, RAM_SIZE));
    free(img);
}

TEST_CASE("an image holds a word only in its code, at a multiple of 4", "[smp]")
{
    uint8_t img[512];
    size_t  n = vectors(img, 0x20040000U, 0xE021U, 0U, 0U);
    put32(img + 40, 0x0000E020U);
    TEST_ASSERT_TRUE(fck_drv_smp_image_holds(img, n, 0x0000E020U));
    TEST_ASSERT_FALSE(fck_drv_smp_image_holds(img, n, 0x00010020U));

    /* The same bytes two places on: not a word the code can load. */
    n = vectors(img, 0x20040000U, 0xE021U, 0U, 0U);
    put32(img + 42, 0x0000E020U);
    TEST_ASSERT_FALSE(fck_drv_smp_image_holds(img, n, 0x0000E020U));

    /* The TLVs after the code are not looked at: the hash's first bytes. */
    n = vectors(img, 0x20040000U, 0xE021U, 0U, 0U);
    TEST_ASSERT_FALSE(fck_drv_smp_image_holds(img, n, 0x83828180U));
    img[0] = 0x3E;
    TEST_ASSERT_FALSE(fck_drv_smp_image_holds(img, n, 0x20040000U));
}

/* ---- The text between the packets ------------------------------------------- */

/* @p n bytes of @p s as text: what the last line they ended was. */
static fck_drv_smp_line_t text(fck_drv_smp_text_t *t, const char *s, size_t n, char *out, size_t cap)
{
    fck_drv_smp_line_t last = FCK_DRV_SMP_LINE_NONE;
    for (size_t k = 0U; k < n; k++) {
        fck_drv_smp_line_t const l = fck_drv_smp_text_byte(t, (uint8_t)s[k], out, cap);
        if (s[k] == '\n') {
            last = l;
        }
    }
    return last;
}

#define TEXT(t, lit, out) text((t), (lit), sizeof(lit) - 1U, (out), sizeof(out))

TEST_CASE("a line reads without a prompt, escapes, CR or a reset's stray bytes", "[smp]")
{
    fck_drv_smp_text_t t;
    char               out[48];
    fck_drv_smp_text_init(&t);

    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_TEXT, TEXT(&t, "#NRFINFO fw=nrf_fck_agent ver=2.0.1\r\n", out));
    TEST_ASSERT_EQUAL_STRING("#NRFINFO fw=nrf_fck_agent ver=2.0.1", out);

    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_TEXT,
                      TEXT(&t, "\x1b[1;32muart:~$ \x1b[m#NRFINFO fw=x\r\n", out));
    TEST_ASSERT_EQUAL_STRING("#NRFINFO fw=x", out);

    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_TEXT, TEXT(&t, "\x00\xff" "ready kode-nrf-test 1 \r\n", out));
    TEST_ASSERT_EQUAL_STRING("ready kode-nrf-test 1", out);

    /* Longer than the report keeps: cut. */
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_TEXT,
                      TEXT(&t, "0123456789012345678901234567890123456789012345678901234567890123456789\n",
                           out));
    TEST_ASSERT_EQUAL_size_t(47, strlen(out));
    TEST_ASSERT_EQUAL_STRING_LEN("0123456789", out, 10);
}

TEST_CASE("frames, Zephyr's banners and MCUboot's lines are told from an image's text", "[smp]")
{
    fck_drv_smp_text_t t;
    char               out[48] = "kept";
    fck_drv_smp_text_init(&t);

    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_NONE, TEXT(&t, "\x06\x09" "AAoAAAEAAQD/\n", out));
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_NONE, TEXT(&t, "\x04\x14" "v2Zpb\n", out));
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_NONE, TEXT(&t, "uart:~$ \x06\x09" "AAoA\n", out));
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_NONE, TEXT(&t, "uart:~$ \r\n", out));
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_NONE, TEXT(&t, "   \r\n", out));
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_NONE, TEXT(&t, "*** Using Zephyr OS v4.3.99 ***\r\n", out));
    TEST_ASSERT_EQUAL_STRING("kept", out);

    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_BANNER,
                      TEXT(&t, "*** Booting nRF Connect SDK v3.3.0-1ac0f28 ***\r\n", out));
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_BOOTLOADER, TEXT(&t, "*** Booting MCUboot v2.2.0 ***\r\n", out));
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_BOOTLOADER,
                      TEXT(&t, "[00:00:00.004,000] <inf> mcuboot: Starting bootloader\r\n", out));
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_BOOTLOADER, TEXT(&t, "I: MCUBoot image\n", out));
    TEST_ASSERT_EQUAL_STRING("kept", out);

    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LINE_NONE, fck_drv_smp_text_byte(NULL, '\n', out, sizeof(out)));
}

/* ---- Which MCUboot boots which image, and where an image says it was linked ---------- */

/* An image as imgtool writes one for the module's one-slot MCUboot: @p code
 * bytes of code after a 32-byte header, and an unprotected TLV area with the
 * hash alone, or with @p sig after it. */
static size_t plain(uint8_t *img, size_t code, uint16_t hash_type, size_t hash_len, uint16_t sig)
{
    memset(img, 0, 32U + code);
    img[0]  = 0x3D;
    img[1]  = 0xB8;
    img[2]  = 0xF3;
    img[3]  = 0x96;
    img[8]  = 32;
    put32(img + 12, (uint32_t)code);
    size_t       n   = 32U + code;
    size_t const tot = 4U + (4U + hash_len) + ((sig != 0U) ? (4U + 64U) : 0U);
    img[n++]         = 0x07;
    img[n++]         = 0x69;
    img[n++]         = (uint8_t)tot;
    img[n++]         = (uint8_t)(tot >> 8);
    img[n++]         = (uint8_t)hash_type;
    img[n++]         = 0;
    img[n++]         = (uint8_t)hash_len;
    img[n++]         = 0;
    memset(img + n, 0x44, hash_len);
    n += hash_len;
    if (sig != 0U) {
        img[n++] = (uint8_t)sig;
        img[n++] = 0;
        img[n++] = 64;
        img[n++] = 0;
        memset(img + n, 0x33, 64);
        n += 64U;
    }
    return n;
}

TEST_CASE("the one-slot MCUboot boots an image with a SHA-256 and no signature, and a first module's only a signed one", "[smp]")
{
    uint8_t img[512];
    /* What a build of fck-nrf 0.1.0 gives: SHA-256, nothing else. */
    size_t n = plain(img, 16U, 0x10, 32U, 0U);
    TEST_ASSERT_TRUE(fck_drv_smp_image_suits(img, n, FCK_DRV_SMP_LOADER_HASH));
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_smp_image_suits(img, n, FCK_DRV_SMP_LOADER_SIGNED),
                              "uploaded to a first module it would never boot");
    /* What NCS signs by default, and the first modules carry: ED25519 over SHA-512. */
    n = image(img, 0x12, 64);
    TEST_ASSERT_TRUE(fck_drv_smp_image_suits(img, n, FCK_DRV_SMP_LOADER_SIGNED));
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_smp_image_suits(img, n, FCK_DRV_SMP_LOADER_HASH),
                              "uploaded to the one slot it would leave the nRF in recovery");

    /* A SHA-256 with a signature beside it, a SHA-512 with none, a SHA-384, another signature. */
    n = plain(img, 16U, 0x10, 32U, 0x24);
    TEST_ASSERT_FALSE(fck_drv_smp_image_suits(img, n, FCK_DRV_SMP_LOADER_HASH));
    TEST_ASSERT_FALSE(fck_drv_smp_image_suits(img, n, FCK_DRV_SMP_LOADER_SIGNED));
    n = plain(img, 16U, 0x12, 64U, 0U);
    TEST_ASSERT_FALSE(fck_drv_smp_image_suits(img, n, FCK_DRV_SMP_LOADER_HASH));
    TEST_ASSERT_FALSE(fck_drv_smp_image_suits(img, n, FCK_DRV_SMP_LOADER_SIGNED));
    n = plain(img, 16U, 0x11, 48U, 0U);
    TEST_ASSERT_FALSE(fck_drv_smp_image_suits(img, n, FCK_DRV_SMP_LOADER_HASH));
    n = plain(img, 16U, 0x12, 64U, 0x22);
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_smp_image_suits(img, n, FCK_DRV_SMP_LOADER_SIGNED), "ECDSA is not its key");

    /* No hash at all, not an image, nothing, and a loader that is neither. */
    n = plain(img, 16U, 0x30, 32U, 0U);
    TEST_ASSERT_FALSE(fck_drv_smp_image_suits(img, n, FCK_DRV_SMP_LOADER_HASH));
    n = plain(img, 16U, 0x10, 32U, 0U);
    TEST_ASSERT_FALSE(fck_drv_smp_image_suits(img, n, (fck_drv_smp_loader_t)7));
    img[0] ^= 0xFFU;
    TEST_ASSERT_FALSE(fck_drv_smp_image_suits(img, n, FCK_DRV_SMP_LOADER_HASH));
    TEST_ASSERT_FALSE(fck_drv_smp_image_suits(NULL, n, FCK_DRV_SMP_LOADER_HASH));
    TEST_ASSERT_FALSE(fck_drv_smp_image_suits(img, 0U, FCK_DRV_SMP_LOADER_HASH));
}

TEST_CASE("an image says where it was linked in its FCKMAP tag, wherever the tag lies in its code", "[smp]")
{
    static const uint8_t MAGIC[8] = { 'F', 'C', 'K', 'M', 'A', 'P', 0x00, 0x01 };
    uint8_t  img[512];
    uint32_t at = 0xDEADBEEFU;
    size_t   n  = plain(img, 128U, 0x10, 32U, 0U);
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_smp_image_map(img, n, &at), "no tag: any firmware but fck-nrf");
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, at);

    /* fck-nrf 0.1.0's: 0x0000E800, at an odd place in its .rodata. */
    memcpy(img + 32 + 41, MAGIC, sizeof(MAGIC));
    put32(img + 32 + 41 + 8, 0x0000E800U);
    TEST_ASSERT_TRUE(fck_drv_smp_image_map(img, n, &at));
    TEST_ASSERT_EQUAL_HEX32(FCK_BSP_NRF_IMAGE_START, at);
    /* One built for a slot at 0x10000 says so. */
    put32(img + 32 + 41 + 8, 0x00010800U);
    TEST_ASSERT_TRUE(fck_drv_smp_image_map(img, n, &at));
    TEST_ASSERT_EQUAL_HEX32(0x00010800, at);

    /* The magic to the letter: another version byte is another tag. */
    img[32 + 41 + 7] = 0x02;
    TEST_ASSERT_FALSE(fck_drv_smp_image_map(img, n, &at));
    img[32 + 41 + 7] = 0x01;

    /* At the code's very end it is read; cut by the code's end, or lying in the header or the
     * TLVs, it is not. */
    n = plain(img, 128U, 0x10, 32U, 0U);
    memcpy(img + 32 + 128 - 12, MAGIC, sizeof(MAGIC));
    put32(img + 32 + 128 - 4, 0x0000E800U);
    TEST_ASSERT_TRUE(fck_drv_smp_image_map(img, n, &at));
    n = plain(img, 128U, 0x10, 32U, 0U);
    memcpy(img + 32 + 128 - 8, MAGIC, sizeof(MAGIC));
    TEST_ASSERT_FALSE(fck_drv_smp_image_map(img, n, &at));
    n = plain(img, 128U, 0x10, 32U, 0U);
    memcpy(img + 16, MAGIC, sizeof(MAGIC));
    TEST_ASSERT_FALSE(fck_drv_smp_image_map(img, n, &at));

    TEST_ASSERT_FALSE(fck_drv_smp_image_map(img, n, NULL));
    TEST_ASSERT_FALSE(fck_drv_smp_image_map(NULL, n, &at));
    TEST_ASSERT_FALSE(fck_drv_smp_image_map(img, 20U, &at));
}
