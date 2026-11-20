/* Host-only fake QMI service: verifies baseline guard, exact wire request,
 * response error handling, and restore behavior without touching a modem.
 */
#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* macOS SDK secure wrappers are macros that cannot be redeclared by the
 * freestanding firmware source. Link the already-declared libc functions. */
#undef snprintf
#undef memset

static jmp_buf exit_point;
static int exit_status;
static unsigned current_preference;
static int set_calls;
static int fail_get;
static int fail_set_result;

static void mock_exit(int status) __attribute__((noreturn));
#define main firmware_main
#define _exit mock_exit
#include "nas_domain_permanent_set.c"
#undef main
#undef _exit

void *nas_get_service_object_internal_v01(int major, int minor, int tool)
{
	assert(major == 1 && minor == 0x164 && tool == 6);
	return (void *)1;
}

int qmi_client_init_instance(void *service, unsigned instance, void *callback,
			     void *context, void *params, unsigned timeout,
			     void **client)
{
	assert(service == (void *)1 && instance == 0xffff);
	assert(callback == NULL && context == NULL && params != NULL);
	assert(timeout == 5000);
	*client = (void *)2;
	return 0;
}

int qmi_client_send_raw_msg_sync(void *client, unsigned message_id,
				 void *request, unsigned request_len,
				 void *response_buf, unsigned response_cap,
				 unsigned *received, unsigned timeout)
{
	static const unsigned char success[] = {0x02, 0x04, 0, 0, 0, 0, 0};
	unsigned char *response_bytes = response_buf;
	assert(client == (void *)2);
	if (message_id == 0x34) {
		assert(request_len == 0 && timeout == 5000);
		if (fail_get)
			return -3;
		assert(response_cap >= sizeof(success) + 7);
		memcpy(response_bytes, success, sizeof(success));
		response_bytes[7] = 0x18;
		response_bytes[8] = 4;
		response_bytes[9] = 0;
		response_bytes[10] = (unsigned char)current_preference;
		response_bytes[11] = 0;
		response_bytes[12] = 0;
		response_bytes[13] = 0;
		*received = 14;
		return 0;
	}
	assert(message_id == 0x33 && timeout == 15000);
	assert(request_len == 11 && response_cap >= sizeof(success));
	{
		const unsigned char expected[] = {
			0x17, 0x01, 0, 1,
			0x18, 0x04, 0, NAS_TARGET_DOMAIN, 0, 0, 0
		};
		assert(memcmp(request, expected, sizeof(expected)) == 0);
	}
	++set_calls;
	memcpy(response_bytes, success, sizeof(success));
	if (fail_set_result)
		response_bytes[3] = 1;
	*received = sizeof(success);
	return 0;
}

int qmi_client_release(void *client)
{
	assert(client == (void *)2);
	return 0;
}

static void mock_exit(int status)
{
	exit_status = status;
	longjmp(exit_point, 1);
}

static void run_case(unsigned original, int get_fails, int set_fails,
		     int expected_sets, int expected_exit)
{
	current_preference = original;
	set_calls = 0;
	fail_get = get_fails;
	fail_set_result = set_fails;
	exit_status = -999;
	if (setjmp(exit_point) == 0) {
		firmware_main();
		assert(0 && "firmware_main returned without _exit");
	}
	assert(set_calls == expected_sets);
	assert(exit_status == expected_exit);
}

int main(void)
{
#if NAS_TARGET_DOMAIN == 2
	run_case(1, 0, 0, 1, 0);
	run_case(2, 0, 0, 1, 0);
	run_case(3, 0, 0, 0, 1);
	run_case(1, 1, 0, 0, 1);
	run_case(1, 0, 1, 1, 1);
#else
	run_case(2, 0, 0, 1, 0);
	run_case(1, 0, 0, 0, 0);
	run_case(2, 1, 0, 0, 1);
	run_case(2, 0, 1, 1, 1);
#endif
	puts("host QMI fixture passed");
	return 0;
}
