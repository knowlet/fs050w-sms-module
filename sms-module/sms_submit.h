#ifndef SMS_SUBMIT_H
#define SMS_SUBMIT_H

enum { SMS_DA_DIGITS = 12 };
#ifndef SMS_TW_MOBILE_PROFILE
#include "sms_legacy_profile.h"
#else
#define SMS_SELFTEST_DESTINATION "886900000001"
#define SMS_SELFTEST_ADDRESS_BYTES 0x88, 0x96, 0x00, 0x00, 0x00, 0x10
#endif

/* Strip the domestic trunk prefix or the international prefix. */
static int sms_normalize_mobile(const char *input, char digits[SMS_DA_DIGITS + 1])
{
	const char *subscriber;
	int i;

	while (*input == ' ')
		input++;
	if (input[0] == '0') {
		subscriber = input + 1;
	} else {
		if (*input == '+')
			input++;
		if (input[0] != '8' || input[1] != '8' || input[2] != '6')
			return 0;
		subscriber = input + 3;
	}
	if (subscriber[0] != '9')
		return 0;
	for (i = 1; i < 9; i++) {
		if (subscriber[i] < '0' || subscriber[i] > '9')
			return 0;
	}
	if (subscriber[9] != 0)
		return 0;
	digits[0] = '8';
	digits[1] = '8';
	digits[2] = '6';
	for (i = 0; i <= 9; i++)
		digits[i + 3] = subscriber[i];
	digits[SMS_DA_DIGITS] = 0;
	return 1;
}

#ifndef SMS_TW_MOBILE_PROFILE
static int sms_same_destination(const char *a, const char *b)
{
	int i;

	for (i = 0; i < SMS_DA_DIGITS; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}
#endif

/* A test build may permit one other explicitly configured mobile number. */
static int sms_allowed_destination(const char *input, char digits[SMS_DA_DIGITS + 1])
{
#ifdef SMS_TW_MOBILE_PROFILE
	return sms_normalize_mobile(input, digits);
#else
#ifdef SMS_TEST_PEER
	char peer[SMS_DA_DIGITS + 1];
#endif

	digits[0] = 0;
	if (!sms_normalize_mobile(input, digits))
		return 0;
	if (sms_same_destination(digits, sms_self_digits))
		return 1;
#ifdef SMS_TEST_PEER
	if (sms_normalize_mobile(SMS_TEST_PEER, peer) && sms_same_destination(digits, peer))
		return 1;
#endif
	digits[0] = 0;
	return 0;
#endif
}

static int pack7(const char *s, int n, unsigned char *out, int cap)
{
	unsigned acc = 0;
	int bits = 0;
	int o = 0;
	int i;

	for (i = 0; i < n; i++) {
		acc |= ((unsigned)s[i] & 0x7f) << bits;
		bits += 7;
		while (bits >= 8) {
			if (o >= cap)
				return -1;
			out[o++] = (unsigned char)(acc & 0xff);
			acc >>= 8;
			bits -= 8;
		}
	}
	if (bits) {
		if (o >= cap)
			return -1;
		out[o++] = (unsigned char)(acc & 0xff);
	}
	return o;
}

static int build_submit(const char *number, const char *text, int tlen,
			unsigned char *pdu, int cap)
{
	char digits[SMS_DA_DIGITS + 1];
	unsigned char ud[40];
	int ud_n;
	int n = 0;
	int i;

	if (tlen < 1 || tlen > 40 || !sms_normalize_mobile(number, digits))
		return -1;
	ud_n = pack7(text, tlen, ud, 40);
	if (ud_n < 0 || 8 + SMS_DA_DIGITS / 2 + ud_n > cap)
		return -1;
	pdu[n++] = 0x00;
	pdu[n++] = 0x01;
	pdu[n++] = 0x00;
	pdu[n++] = SMS_DA_DIGITS;
	pdu[n++] = 0x91;
	for (i = 0; i < SMS_DA_DIGITS; i += 2)
		pdu[n++] = (unsigned char)((digits[i] - '0') | ((digits[i + 1] - '0') << 4));
	pdu[n++] = 0x00;
	pdu[n++] = 0x00;
	pdu[n++] = (unsigned char)tlen;
	for (i = 0; i < ud_n; i++)
		pdu[n++] = ud[i];
	return n;
}

static int selftest(void)
{
	unsigned char ud[8];
	unsigned char pdu[20];
	static const unsigned char hello[] = {0xe8, 0x32, 0x9b, 0xfd, 0x06};
	static const unsigned char expect[] = {
		0x00, 0x01, 0x00, 0x0c, 0x91,
		SMS_SELFTEST_ADDRESS_BYTES,
		0x00, 0x00, 0x06, 0xe6, 0x39, 0xac, 0x06, 0xbb, 0x03
	};
	int n;
	int i;

	n = pack7("hello", 5, ud, 8);
	if (n != 5)
		return 0;
	for (i = 0; i < 5; i++) {
		if (ud[i] != hello[i])
			return 0;
	}
	n = build_submit(SMS_SELFTEST_DESTINATION, "fs050w", 6, pdu, (int)sizeof(pdu));
	if (n != (int)sizeof(expect))
		return 0;
	for (i = 0; i < n; i++) {
		if (pdu[i] != expect[i])
			return 0;
	}
	return 1;
}

#endif
