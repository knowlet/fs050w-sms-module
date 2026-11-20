/* SMS IPC module 11.
 * Count and list call MAL 0x177c for NV and UIM, keeping the original NV
 * tags field and adding uim_tags for the OEM's UIM receive routes.
 * The 12-byte request is storage, tag, mode. mal_service 0x72b54
 * forces both valid flags and returns count at word 0, then packed
 * index words. Read is MAL 0x1778: index, storage 0 or 1, mode.
 * A read request may select storage 0 or 1; the default remains NV (1).
 * Send is MAL 0x1779:
 * word 0 is the raw length (1..0x134), bytes at offset 8 are the
 * SMSC octet plus the TPDU. The server forces format 6.
 * Send's server path also runs the route check at 0x6ff40.
 * Before send, SMS init checks the modem routes with the configured NV
 * storage and store-and-notify action. Delete only accepts an explicit
 * storage and index and can never issue a tag-wide / storage-wide operation.
 */

#include "sms_submit.h"

typedef int (*ipc_handler)(int sender, int msg_id, const void *req, int req_len,
			   void *rsp, int rsp_cap, int hdr_0c);
typedef void (*sighandler_t)(int);

extern int frwk_module_init(int id, const void *arg);
extern char *frwk_module_name(int id);
extern int frwk_ipc_service(ipc_handler handler);
extern int mal_client_init(const char *name, void *cb, int flags);
extern int mal_client_send_msg_sync(unsigned id, const void *req, unsigned req_len,
				    void *rsp, unsigned rsp_len, unsigned timeout,
				    int *err);
extern int libmal_sms_fresh_message(const void *req, void *rsp);
extern int libmal_sms_read_message(const void *req, void *rsp);
extern int libmal_sms_send_msg(const void *arg);
extern int snprintf(char *dst, unsigned cap, const char *fmt, ...);
extern void *memset(void *dst, int value, unsigned len);
extern sighandler_t signal(int sig, sighandler_t handler);
extern int write(int fd, const void *buf, unsigned len);

enum {
	SMS_MODULE_ID = 11,
	MSG_SEND = 11001,
	MSG_COUNT = 11003,
	MSG_LIST = 11008,
	MSG_READ = 11009,
	MSG_DELETE = 11011,
	STORAGE_UIM = 0,
	STORAGE_NV = 1,
	MODE_GW = 1,
	INDEX_CAP = 64
};

static unsigned fresh_rsp[0x101];
static unsigned read_rsp[0x10c];
static unsigned send_buf[0x102];
static int pack_ok;
static int g_mal_init = -1;

static int mal_rx(int id, void *payload, int len, void *extra)
{
	(void)id;
	(void)payload;
	(void)len;
	(void)extra;
	return 0;
}

struct out {
	char *p;
	unsigned cap;
	unsigned n;
};

static void bout(struct out *o, const char *s)
{
	while (*s && o->n + 1 < o->cap)
		o->p[o->n++] = *s++;
	if (o->n < o->cap)
		o->p[o->n] = 0;
}

static void bout_u(struct out *o, unsigned v)
{
	char tmp[16];
	char rev[16];
	unsigned i = 0;
	unsigned j = 0;

	if (v == 0) {
		bout(o, "0");
		return;
	}
	while (v && i < sizeof(tmp)) {
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i > 0)
		rev[j++] = tmp[--i];
	rev[j] = 0;
	bout(o, rev);
}

static void bout_d(struct out *o, int v)
{
	if (v < 0) {
		bout(o, "-");
		bout_u(o, 0u - (unsigned)v);
		return;
	}
	bout_u(o, (unsigned)v);
}

static void log_mal_result(const char *operation, int rc, int err)
{
	char line[80];
	int length = snprintf(line, sizeof(line), "sms %s rc=%d err=%d\n",
			      operation, rc, err);

	if (length > 0 && length < (int)sizeof(line))
		write(2, line, (unsigned)length);
}

static const char *find_key(const char *s, int n, const char *key)
{
	int klen = 0;
	int i;
	int j;

	while (key[klen])
		klen++;
	if (s == 0 || n < klen)
		return 0;
	for (i = 0; i + klen <= n; i++) {
		for (j = 0; j < klen; j++) {
			if (s[i + j] != key[j])
				break;
		}
		if (j == klen)
			return s + i + klen;
	}
	return 0;
}

static const char *skip_colon(const char *p, const char *end)
{
	if (p == 0)
		return 0;
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	if (p >= end || *p != ':')
		return 0;
	p++;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	return p;
}

static int json_int(const char *s, int n, const char *key, int *out)
{
	const char *end = s ? s + n : s;
	const char *p = skip_colon(find_key(s, n, key), end);
	int sign = 1;
	int v = 0;
	int any = 0;

	if (p == 0)
		return 0;
	if (*p == '-') {
		sign = -1;
		p++;
	}
	while (p < end && *p >= '0' && *p <= '9') {
		v = v * 10 + (*p - '0');
		p++;
		any = 1;
	}
	if (!any)
		return 0;
	*out = sign < 0 ? -v : v;
	return 1;
}

static int json_str(const char *s, int n, const char *key, char *dst, int cap)
{
	const char *end = s ? s + n : s;
	const char *p = skip_colon(find_key(s, n, key), end);
	int i = 0;

	if (p == 0 || p >= end || *p != '"' || cap < 2)
		return 0;
	p++;
	while (p < end && *p != '"' && i + 1 < cap) {
		if (*p == '\\')
			return 0;
		dst[i++] = *p++;
	}
	if (p >= end || *p != '"')
		return 0;
	dst[i] = 0;
	return 1;
}

static int gsm_text(const char *s, int *len)
{
	int n = 0;

	while (s[n]) {
		char c = s[n];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == ' '))
			return 0;
		n++;
		if (n > 40)
			return 0;
	}
	*len = n;
	return n > 0;
}

static void begin(struct out *o, void *rsp, int rsp_cap, int msg_id)
{
	o->p = (char *)rsp;
	o->cap = rsp_cap > 0 ? (unsigned)rsp_cap : 0;
	o->n = 0;
	if (o->cap == 0)
		return;
	o->p[0] = 0;
	bout(o, "{\"module\":\"sms\",\"msgid\":");
	bout_d(o, msg_id);
	bout(o, ",\"mal_init\":");
	bout_d(o, g_mal_init);
}

static void query_sca(struct out *o)
{
	unsigned char buf[0x1c];
	int err = 0x258;
	int rc;
	unsigned i;
	static const char hexd[] = "0123456789abcdef";

	memset(buf, 0, sizeof(buf));
	rc = mal_client_send_msg_sync(0x1774, 0, 0, buf, 0x1c, 0xbb8, &err);
	bout(o, ",\"sca_rc\":");
	bout_d(o, rc);
	bout(o, ",\"sca_err\":");
	bout_d(o, err);
	bout(o, ",\"sca_hex\":\"");
	for (i = 0; i < sizeof(buf) && o->n + 3 < o->cap; i++) {
		o->p[o->n++] = hexd[buf[i] >> 4];
		o->p[o->n++] = hexd[buf[i] & 0xf];
	}
	if (o->n < o->cap)
		o->p[o->n] = 0;
	bout(o, "\",\"sca_txt\":\"");
	for (i = 4; i < sizeof(buf) && buf[i] >= 0x20 && buf[i] < 0x7f; i++) {
		if (buf[i] == '"' || buf[i] == '\\')
			break;
		if (o->n + 2 >= o->cap)
			break;
		o->p[o->n++] = (char)buf[i];
	}
	if (o->n < o->cap)
		o->p[o->n] = 0;
	bout(o, "\"");
}

static int query_tags(struct out *o, int storage, const char *field)
{
	int tag;
	int any = 0;

	if (storage == STORAGE_NV) {
		bout(o, ",\"storage\":");
		bout_d(o, storage);
	}
	bout(o, ",\"");
	bout(o, field);
	bout(o, "\":[");
	for (tag = 0; tag < 4; tag++) {
		unsigned req[3];
		unsigned count = 0;
		unsigned show;
		unsigned i;
		int rc;
		int err = 0x258;

		req[0] = (unsigned)storage;
		req[1] = (unsigned)tag;
		req[2] = MODE_GW;
		memset(fresh_rsp, 0, sizeof(fresh_rsp));
		rc = mal_client_send_msg_sync(0x177c, req, 0xc, fresh_rsp, 0x404,
					      0xbb8, &err);
		if (rc == 0 && err == 0)
			any = 1;
		if (tag)
			bout(o, ",");
		bout(o, "{\"tag\":");
		bout_d(o, tag);
		bout(o, ",\"rc\":");
		bout_d(o, rc);
		bout(o, ",\"err\":");
		bout_d(o, err);
		if (rc == 0 && err == 0) {
			count = fresh_rsp[0];
			if (count > 256)
				count = 256;
		}
		bout(o, ",\"count\":");
		bout_u(o, count);
		bout(o, ",\"index\":[");
		show = count > INDEX_CAP ? INDEX_CAP : count;
		for (i = 0; i < show; i++) {
			if (i)
				bout(o, ",");
			bout_u(o, fresh_rsp[1 + i]);
		}
		if (count > show)
			bout(o, "],\"truncated\":1}");
		else
			bout(o, "]}");
	}
	bout(o, "]");
	return any;
}

static int on_read(struct out *o, const char *req, int req_len)
{
	unsigned mal[3];
	int id = 0;
	int storage = STORAGE_NV;
	int rc;
	int err = 0x258;
	unsigned i;
	unsigned n;
	static const char hexd[] = "0123456789abcdef";

	if (!json_int(req, req_len, "\"id\"", &id) || id < 0) {
		bout(o, ",\"accepted\":0,\"reason\":\"no_id\"}");
		return 1;
	}
	if (find_key(req, req_len, "\"storage\"") &&
	    (!json_int(req, req_len, "\"storage\"", &storage) ||
	     (storage != STORAGE_UIM && storage != STORAGE_NV))) {
		bout(o, ",\"accepted\":0,\"reason\":\"invalid_storage\"}");
		return 1;
	}
	mal[0] = (unsigned)id;
	mal[1] = (unsigned)storage;
	mal[2] = MODE_GW;
	memset(read_rsp, 0, sizeof(read_rsp));
	rc = mal_client_send_msg_sync(0x1778, mal, 0xc, read_rsp, 0x430, 0xbb8, &err);
	bout(o, ",\"id\":");
	bout_d(o, id);
	bout(o, ",\"storage\":");
	bout_d(o, storage);
	bout(o, ",\"mode\":1,\"rc\":");
	bout_d(o, rc);
	bout(o, ",\"err\":");
	bout_d(o, err);
	bout(o, ",\"hex\":\"");
	n = 192;
#ifdef SMS_TW_MOBILE_PROFILE
	/* RAW_READ stores its PDU length in word 11, then the PDU at byte 48. */
	if (rc == 0 && err == 0 && read_rsp[11] <= sizeof(read_rsp) - 48 &&
	    48 + read_rsp[11] > n)
		n = 48 + read_rsp[11];
#endif
	if (n > sizeof(read_rsp))
		n = sizeof(read_rsp);
	for (i = 0; i < n && o->n + 3 < o->cap; i++) {
		unsigned char b = ((unsigned char *)read_rsp)[i];

		o->p[o->n++] = hexd[b >> 4];
		o->p[o->n++] = hexd[b & 0xf];
	}
	if (o->n < o->cap)
		o->p[o->n] = 0;
	bout(o, "\"}");
	return (rc == 0 && err == 0) ? 0 : 1;
}

/* The webserver supplies the JSON object including a possible final NUL.
 * Its native delete route has no response buffer, so the UI sends a marked
 * delete through the read route, which can return an explicit result. Both
 * forms require exact literal keys and integers; neither may default a
 * storage or index or ignore the remainder of the object. */
struct input {
	const char *p;
	const char *end;
};

static void skip_space(struct input *in)
{
	while (in->p < in->end && (*in->p == ' ' || *in->p == '\t' ||
	       *in->p == '\r' || *in->p == '\n'))
		in->p++;
}

static int consume(struct input *in, const char *token)
{
	const char *p;

	skip_space(in);
	p = in->p;
	while (*token) {
		if (p == in->end || *p++ != *token++)
			return 0;
	}
	in->p = p;
	return 1;
}

static int delete_integer(struct input *in, unsigned *value)
{
	unsigned n = 0;
	int leading_zero;

	skip_space(in);
	if (in->p == in->end || *in->p < '0' || *in->p > '9')
		return 0;
	leading_zero = *in->p == '0';
	do {
		unsigned digit = (unsigned)(*in->p++ - '0');
		if (n > (0x7fffffffu - digit) / 10u)
			return 0;
		n = n * 10 + digit;
	} while (!leading_zero && in->p < in->end && *in->p >= '0' && *in->p <= '9');
	*value = n;
	return 1;
}

static int delete_target(const char *req, int len, unsigned *storage,
			 unsigned *index, int marked)
{
	struct input in;
	int first_id;
	unsigned flag;

	if (!req || len <= 0 || len > 256)
		return 0;
	if (req[len - 1] == 0)
		len--;
	in.p = req;
	in.end = req + len;
	if (!consume(&in, "{"))
		return 0;
	if (marked && (!consume(&in, "\"fs050w_sms_delete_one\"") ||
	    !consume(&in, ":") || !delete_integer(&in, &flag) || flag != 1 ||
	    !consume(&in, ",")))
		return 0;
	first_id = consume(&in, "\"id\"");
	if (!first_id && !consume(&in, "\"storage\""))
		return 0;
	if (!consume(&in, ":") || !delete_integer(&in, first_id ? index : storage) ||
	    !consume(&in, ",") || !consume(&in, first_id ? "\"storage\"" : "\"id\"") ||
	    !consume(&in, ":") || !delete_integer(&in, first_id ? storage : index) ||
	    !consume(&in, "}"))
		return 0;
	skip_space(&in);
	return in.p == in.end && *storage <= STORAGE_NV;
}

static int on_delete(struct out *o, const char *req, int req_len, int marked)
{
	unsigned index, storage;
	unsigned char one = 0;
	int err = 0x258;
	int rc;

	if (o->cap < 160 || g_mal_init != 0 ||
	    !delete_target(req, req_len, &storage, &index, marked)) {
		bout(o, ",\"deleted\":0,\"reason\":\"invalid_delete_request\"}");
		return 1;
	}

	/* mal_service 0x726ac passes these aligned fields to WMS DELETE:
	 * storage, index_valid, index, tag_valid, tag, mode_valid, mode. */
	unsigned wire[7] = {storage, 1, index, 0, 0, 1, MODE_GW};
	_Static_assert(sizeof wire == 0x1c, "unexpected MAL delete request size");
	rc = mal_client_send_msg_sync(0x177a, wire, sizeof(wire), &one, 1, 65000, &err);
	bout(o, ",\"deleted\":");
	bout_u(o, rc == 0 && err == 0);
	bout(o, ",\"storage\":");
	bout_u(o, storage);
	bout(o, ",\"id\":");
	bout_u(o, index);
	bout(o, ",\"rc\":");
	bout_d(o, rc);
	bout(o, ",\"err\":");
	bout_d(o, err);
	bout(o, "}");
	return rc == 0 && err == 0 ? 0 : 1;
}

static int on_send(struct out *o, const char *req, int req_len)
{
	char number[24];
	char digits[SMS_DA_DIGITS + 1];
	char text[48];
	unsigned route_req[3] = {STORAGE_NV, 1, 0};
	int tlen = 0;
	int pdulen;
	int rc;
	int route_rc;
	int route_err = 0x258;
	unsigned char *raw;

	if (!pack_ok) {
		bout(o, ",\"accepted\":0,\"reason\":\"packer_selftest_failed\"}");
		return 1;
	}
	if (!json_str(req, req_len, "\"Number\"", number, (int)sizeof(number)) ||
	    !sms_allowed_destination(number, digits)) {
#if defined(SMS_TW_MOBILE_PROFILE) || defined(SMS_TEST_PEER)
		bout(o, ",\"accepted\":0,\"reason\":\"number_not_allowed\"}");
#else
		bout(o, ",\"accepted\":0,\"reason\":\"number_not_own_msisdn\"}");
#endif
		return 1;
	}
	if (!json_str(req, req_len, "\"Content\"", text, (int)sizeof(text)) ||
	    !gsm_text(text, &tlen)) {
		bout(o, ",\"accepted\":0,\"reason\":\"content_not_gsm7\"}");
		return 1;
	}
	memset(send_buf, 0, sizeof(send_buf));
	raw = (unsigned char *)send_buf + 8;
	pdulen = build_submit(digits, text, tlen, raw, 0x134);
	if (pdulen < 1 || pdulen > 0x134) {
		bout(o, ",\"accepted\":0,\"reason\":\"pdu_len\"}");
		return 1;
	}
	send_buf[0] = (unsigned)pdulen;
	send_buf[1] = 0;
	/* GET/SET routes may retry for about 369 seconds. */
	route_rc = mal_client_send_msg_sync(0x177b, route_req, sizeof(route_req),
					  0, 0, 420000, &route_err);
	log_mal_result("routes", route_rc, route_err);
	if (route_rc != 0 || route_err != 0) {
		bout(o, ",\"accepted\":0,\"reason\":\"route_check_failed\",\"route_rc\":");
		bout_d(o, route_rc);
		bout(o, ",\"route_err\":");
		bout_d(o, route_err);
		bout(o, "}");
		return 1;
	}
	{
		unsigned char one = 0;
		int err = 0x258;

		rc = mal_client_send_msg_sync(0x1779, send_buf, 0x408, &one, 1,
					      0x1b7740, &err);
		log_mal_result("send", rc, err);
		bout(o, ",\"accepted\":1,\"to\":\"");
		bout(o, digits);
		bout(o, "\",\"pdu_len\":");
		bout_d(o, pdulen);
		bout(o, ",\"rc\":");
		bout_d(o, rc);
		bout(o, ",\"err\":");
		bout_d(o, err);
		bout(o, ",\"rsp\":");
		bout_u(o, one);
		bout(o, "}");
		return (rc == 0 && err == 0) ? 0 : 1;
	}
}

static int on_ipc(int sender, int msg_id, const void *req, int req_len,
		  void *rsp, int rsp_cap, int hdr_0c)
{
	struct out o;
	const char *body = (const char *)req;

	(void)sender;
	(void)hdr_0c;
	if (rsp == 0 || rsp_cap < 8)
		return 1;
	if (body == 0)
		req_len = 0;
	if (msg_id == MSG_READ &&
	    find_key(body, req_len, "\"fs050w_sms_delete_one\"")) {
		/* This is the only destructive use of MSG_READ. A malformed marker
		 * must fail closed instead of falling through to an ordinary read. */
		begin(&o, rsp, rsp_cap, MSG_DELETE);
		return on_delete(&o, body, req_len, 1);
	}
	begin(&o, rsp, rsp_cap, msg_id);
	if (msg_id == MSG_COUNT || msg_id == MSG_LIST) {
		int nv_ok;
		int uim_ok;

		bout(&o, ",\"delete_one\":1");
		query_sca(&o);
		nv_ok = query_tags(&o, STORAGE_NV, "tags");
		uim_ok = query_tags(&o, STORAGE_UIM, "uim_tags");
		bout(&o, "}");
		return (nv_ok || uim_ok) ? 0 : 1;
	}
	if (msg_id == MSG_DELETE)
		return on_delete(&o, body, req_len, 0);
	if (msg_id == MSG_READ)
		return on_read(&o, body, req_len);
	if (msg_id == MSG_SEND)
		return on_send(&o, body, req_len);
	bout(&o, ",\"accepted\":0,\"reason\":\"not_implemented\"}");
	return 1;
}

int main(void)
{
	int rc;
	static const char ready[] = "sms-mal ready\n";

	pack_ok = selftest();
	rc = frwk_module_init(SMS_MODULE_ID, 0);
	if (rc != 0)
		return rc;
	signal(1, (sighandler_t)1);
	g_mal_init = mal_client_init(frwk_module_name(-1), (void *)mal_rx, 0);
	write(1, ready, sizeof(ready) - 1);
	return frwk_ipc_service(on_ipc);
}
