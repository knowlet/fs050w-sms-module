#include <assert.h>
#include <string.h>

#include "../sms_submit.h"

int main(void)
{
	char digits[SMS_DA_DIGITS + 1];
	unsigned char pdu[40];
	static const char *forms[] = {"0900000002", "886900000002", "+886900000002"};
	unsigned i;

	assert(selftest());
	for (i = 0; i < sizeof(forms) / sizeof(forms[0]); ++i) {
		assert(sms_allowed_destination(forms[i], digits));
		assert(strcmp(digits, "886900000002") == 0);
		assert(build_submit(digits, "A", 1, pdu, sizeof(pdu)) == 15);
		assert(pdu[5] == 0x88 && pdu[10] == 0x20 && pdu[14] == 0x41);
	}
	assert(!sms_allowed_destination("090000000", digits));
	assert(!sms_allowed_destination("09bad00000", digits));
	assert(!sms_allowed_destination("+12025550101", digits));
	return 0;
}
