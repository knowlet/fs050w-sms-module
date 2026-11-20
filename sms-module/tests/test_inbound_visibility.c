#include <assert.h>

extern char *strstr(const char *haystack, const char *needle);

#define main sms_module_main
#include "../sms.c"
#undef main

static int fresh_calls[2][4];
static int read_calls;
static int last_read_storage = -1;
static int nv_fails;
static int delete_calls, delete_rc, delete_err;
static unsigned expected_storage, expected_index;

int frwk_module_init(int id, const void *arg)
{
	(void)id;
	(void)arg;
	return 0;
}

char *frwk_module_name(int id)
{
	(void)id;
	return "sms";
}

int frwk_ipc_service(ipc_handler handler)
{
	(void)handler;
	return 0;
}

int mal_client_init(const char *name, void *callback, int flags)
{
	(void)name;
	(void)callback;
	(void)flags;
	return 0;
}

int mal_client_send_msg_sync(unsigned id, const void *req, unsigned req_len,
			     void *rsp, unsigned rsp_len, unsigned timeout,
			     int *err)
{
	const unsigned *words = (const unsigned *)req;
	(void)req_len;
	(void)timeout;
	if (id == 0x1774) {
		memset(rsp, 0, rsp_len);
		*err = 0;
		return 0;
	}
	if (id == 0x177c) {
		unsigned *out = (unsigned *)rsp;
		unsigned storage = words[0];
		unsigned tag = words[1];
		assert(storage <= 1 && tag < 4 && rsp_len >= 0x404);
		fresh_calls[storage][tag]++;
		memset(rsp, 0, rsp_len);
		if (storage == 1 && nv_fails) {
			*err = 600;
			return 0;
		}
		if (storage == 1 && tag == 2) {
			out[0] = 1;
			out[1] = 22;
		}
		if (storage == 0 && tag == 3) {
			out[0] = 2;
			out[1] = 8;
			out[2] = 9;
		}
		*err = 0;
		return 0;
	}
	if (id == 0x1778) {
		assert(rsp_len >= 0x430);
		read_calls++;
		last_read_storage = (int)words[1];
		memset(rsp, 0, rsp_len);
#ifdef SMS_TW_MOBILE_PROFILE
		((unsigned *)rsp)[11] = 180;
		((unsigned char *)rsp)[48 + 179] = 0xa7;
#endif
		*err = 0;
		return 0;
	}
	if (id == 0x177a) {
		/* Byte-level comparison with the signed firmware ABI. In particular
		 * the zero index must not be confused with an absent index. */
		unsigned wire[7] = {expected_storage, 1, expected_index, 0, 0, 1, 1};
		unsigned i;
		assert(req_len == 28 && rsp && rsp_len == 1 && timeout >= 60000);
		for (i = 0; i < sizeof(wire); i++)
			assert(((const unsigned char *)wire)[i] == ((const unsigned char *)req)[i]);
		delete_calls++;
		*err = delete_err;
		return delete_rc;
	}
	assert(0 && "unexpected MAL message");
	return -1;
}

static void test_delete(void)
{
	static const char *invalid[] = {
		"{}", "{\"id\":0}", "{\"storage\":1}", "{\"storage\":0,\"id\":-1}",
		"{\"storage\":2,\"id\":0}", "{\"storage\":true,\"id\":0}", "{\"storage\":1,\"id\":01}",
		"{\"storage\":0,\"id\":2147483648}", "{\"storage\":0,\"id\":3.5}", "{\"storage\":0,\"id\":1e0}",
		"{\"storage\":1,\"id\":\"1\"}", "{\"storage\":1,\"id\":0,\"id\":1}", "{\"id\":0,\"id\":0}",
		"{\"id\":7,\"storage\":1,\"tag\":0}", "{\"id\":0,\"storage\":0} trailing",
		"{\"tag\":0,\"storage\":0}", "{\"id\":0,\"storage\":0", "{\"outer\":{\"storage\":0,\"id\":0}}"
	};
	const char embedded_nul[] = "{\"storage\":0,\"id\":0}\0other";
	const char sim[] = "{ \"storage\" : 0 , \"id\" : 0 }\r\n";
	const char nv[] = "{\"id\":2147483647,\"storage\":1}";
	char response[256];
	unsigned i;

	g_mal_init = 0;
	for (i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
		const char *p = invalid[i];
		int len = 0;
		while (p[len]) len++;
		assert(on_ipc(0, MSG_DELETE, p, len + 1, response, sizeof(response), 0) == 1);
		assert(strstr(response, "\"deleted\":0"));
	}
	assert(on_ipc(0, MSG_DELETE, embedded_nul, sizeof(embedded_nul), response, sizeof(response), 0) == 1);
	assert(on_ipc(0, MSG_DELETE, sim, sizeof(sim), response, 60, 0) == 1);
	g_mal_init = -1;
	assert(on_ipc(0, MSG_DELETE, sim, sizeof(sim), response, sizeof(response), 0) == 1);
	assert(delete_calls == 0);
	g_mal_init = 0;
	assert(on_ipc(0, MSG_DELETE, sim, sizeof(sim), response, sizeof(response), 0) == 0);
	assert(strstr(response, "\"msgid\":11011"));
	assert(strstr(response, "\"deleted\":1,\"storage\":0,\"id\":0,\"rc\":0,\"err\":0}"));
	expected_storage = 1;
	expected_index = 2147483647;
	assert(on_ipc(0, MSG_DELETE, nv, sizeof(nv) - 1, response, sizeof(response), 0) == 0);
	assert(strstr(response, "\"deleted\":1,\"storage\":1,\"id\":2147483647"));
	delete_err = 614;
	assert(on_ipc(0, MSG_DELETE, nv, sizeof(nv), response, sizeof(response), 0) == 1);
	assert(strstr(response, "\"deleted\":0") && strstr(response, "\"err\":614"));
	delete_err = 0;
	delete_rc = -1;
	assert(on_ipc(0, MSG_DELETE, nv, sizeof(nv), response, sizeof(response), 0) == 1);
	assert(strstr(response, "\"deleted\":0") && strstr(response, "\"rc\":-1"));
	assert(delete_calls == 4);
}

static void test_marked_delete_on_read_route(void)
{
	static const char *invalid[] = {
		"{\"fs050w_sms_delete_one\":2,\"id\":0,\"storage\":0}",
		"{\"fs050w_sms_delete_one\":true,\"id\":0,\"storage\":0}",
		"{\"fs050w_sms_delete_one\":1,\"id\":0}",
		"{\"fs050w_sms_delete_one\":1,\"id\":0,\"storage\":2}",
		"{\"fs050w_sms_delete_one\":1,\"id\":-1,\"storage\":0}",
		"{\"fs050w_sms_delete_one\":1,\"id\":2147483648,\"storage\":0}",
		"{\"fs050w_sms_delete_one\":1,\"id\":0,\"storage\":0,\"tag\":0}",
		"{\"id\":0,\"fs050w_sms_delete_one\":1,\"storage\":0}",
		"{\"outer\":{\"fs050w_sms_delete_one\":1},\"id\":0,\"storage\":0}"
	};
	const char sim[] = " { \"fs050w_sms_delete_one\" : 1 , \"id\" : 0 , \"storage\" : 0 } ";
	const char nv[] = "{\"fs050w_sms_delete_one\":1,\"id\":2147483647,\"storage\":1}";
	const char native[] = "{\"id\":0,\"storage\":0}";
	char response[0x800];
	int prior_reads = read_calls;
	unsigned i;

	delete_calls = 0;
	delete_rc = delete_err = 0;
	g_mal_init = 0;
	for (i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
		const char *p = invalid[i];
		int len = 0;
		while (p[len]) len++;
		assert(on_ipc(0, MSG_READ, p, len, response, sizeof(response), 0) == 1);
		assert(strstr(response, "\"msgid\":11011"));
		assert(strstr(response, "\"reason\":\"invalid_delete_request\""));
	}
	assert(delete_calls == 0 && read_calls == prior_reads);
	assert(on_ipc(0, MSG_DELETE, native, sizeof(native), 0, 0, 0) == 1);
	assert(delete_calls == 0);
	assert(on_ipc(0, MSG_READ, sim, sizeof(sim), response, 60, 0) == 1);
	assert(delete_calls == 0);
	expected_storage = 0;
	expected_index = 0;
	assert(on_ipc(0, MSG_READ, sim, sizeof(sim), response, sizeof(response), 0) == 0);
	assert(strstr(response, "\"msgid\":11011"));
	assert(strstr(response, "\"deleted\":1,\"storage\":0,\"id\":0,\"rc\":0,\"err\":0}"));
	expected_storage = 1;
	expected_index = 2147483647;
	assert(on_ipc(0, MSG_READ, nv, sizeof(nv) - 1, response, sizeof(response), 0) == 0);
	assert(strstr(response, "\"deleted\":1,\"storage\":1,\"id\":2147483647"));
	delete_err = 614;
	assert(on_ipc(0, MSG_READ, nv, sizeof(nv), response, sizeof(response), 0) == 1);
	assert(strstr(response, "\"deleted\":0") && strstr(response, "\"err\":614"));
	delete_err = 0;
	delete_rc = -1;
	assert(on_ipc(0, MSG_READ, nv, sizeof(nv), response, sizeof(response), 0) == 1);
	assert(strstr(response, "\"deleted\":0") && strstr(response, "\"rc\":-1"));
	assert(delete_calls == 4 && read_calls == prior_reads);
}

int main(void)
{
	char response[0x4000];
	const char empty[] = "{}";
	const char read_default[] = "{\"id\":7}";
	const char read_uim[] = "{\"id\":7,\"storage\":0}";
	const char read_invalid[] = "{\"id\":7,\"storage\":2}";
	const char *nv;
	const char *nv_tag;
	const char *uim;
	int tag;
	int prior_reads;

	assert(on_ipc(0, MSG_COUNT, empty, sizeof(empty) - 1,
		      response, sizeof(response), 0) == 0);
	assert(strstr(response, "\"delete_one\":1"));
	nv = strstr(response, "\"storage\":1,\"tags\":[");
	uim = strstr(response, "\"uim_tags\":[");
	assert(nv && uim && nv < uim);
	nv_tag = strstr(nv, "\"tag\":2,\"rc\":0,\"err\":0,\"count\":1,\"index\":[22]");
	assert(nv_tag && nv_tag < uim);
	assert(strstr(uim, "\"tag\":3,\"rc\":0,\"err\":0,\"count\":2,\"index\":[8,9]"));
	for (tag = 0; tag < 4; tag++) {
		assert(fresh_calls[0][tag] == 1);
		assert(fresh_calls[1][tag] == 1);
	}

	assert(on_ipc(0, MSG_READ, read_default, sizeof(read_default) - 1,
		      response, sizeof(response), 0) == 0);
	assert(last_read_storage == STORAGE_NV);
	assert(strstr(response, "\"storage\":1,\"mode\":1"));
	assert(on_ipc(0, MSG_READ, read_uim, sizeof(read_uim) - 1,
		      response, sizeof(response), 0) == 0);
	assert(last_read_storage == STORAGE_UIM);
	assert(strstr(response, "\"storage\":0,\"mode\":1"));
#ifdef SMS_TW_MOBILE_PROFILE
	assert(strstr(response, "a7\"}"));
#endif
	prior_reads = read_calls;
	assert(on_ipc(0, MSG_READ, read_invalid, sizeof(read_invalid) - 1,
		      response, sizeof(response), 0) == 1);
	assert(read_calls == prior_reads);
	assert(strstr(response, "\"reason\":\"invalid_storage\""));

	nv_fails = 1;
	assert(on_ipc(0, MSG_LIST, empty, sizeof(empty) - 1,
		      response, sizeof(response), 0) == 0);
	assert(strstr(response, "\"uim_tags\":["));
	test_delete();
	test_marked_delete_on_read_route();
	return 0;
}
