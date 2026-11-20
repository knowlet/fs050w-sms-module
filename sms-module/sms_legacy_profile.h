/* Local test profile only. Build with SMS_TW_MOBILE_PROFILE=1 for normal use. */
static const char sms_self_digits[] = "886900000099";
#define SMS_SELFTEST_DESTINATION sms_self_digits
#define SMS_SELFTEST_ADDRESS_BYTES 0x88, 0x96, 0x00, 0x00, 0x00, 0x99
