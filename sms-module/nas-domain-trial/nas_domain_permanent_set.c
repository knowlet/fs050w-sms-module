/* FS050W permanent NAS service-domain set/restore.
 * Only NAS GET_SYSTEM_SELECTION_PREFERENCE (0x34) and
 * SET_SYSTEM_SELECTION_PREFERENCE (0x33) exist in this program.
 * The SET request contains exactly Change Duration 0x17=1 (permanent)
 * and Service Domain Preference 0x18=NAS_TARGET_DOMAIN (1 or 2).
 * Build separate binaries with NAS_TARGET_DOMAIN=2 for permanent CS+PS and
 * NAS_TARGET_DOMAIN=1 to restore the original PS-only preference.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef __SIZE_TYPE__ usize;

#ifndef NAS_TARGET_DOMAIN
#error NAS_TARGET_DOMAIN must be defined as 1 or 2
#endif
#if NAS_TARGET_DOMAIN != 1 && NAS_TARGET_DOMAIN != 2
#error NAS_TARGET_DOMAIN must be defined as 1 or 2
#endif

extern void *nas_get_service_object_internal_v01(int, int, int);
extern int qmi_client_init_instance(void *, unsigned, void *, void *, void *,
				    unsigned, void **);
extern int qmi_client_send_raw_msg_sync(void *, unsigned, void *, unsigned,
					void *, unsigned, unsigned *, unsigned);
extern int qmi_client_release(void *);
extern int snprintf(char *, usize, const char *, ...);
extern void *memset(void *, int, usize);
extern __PTRDIFF_TYPE__ write(int, const void *, usize);
extern void _exit(int);

enum {
	NAS_SET_SELECTION_PREFERENCE = 0x33,
	NAS_GET_SELECTION_PREFERENCE = 0x34,
	QMI_INIT_TIMEOUT_MS = 5000,
	QMI_QUERY_TIMEOUT_MS = 5000,
	QMI_SET_TIMEOUT_MS = 15000,
	QMI_OS_PARAMS_BYTES = 0x60,
	RESPONSE_CAP = 1024
};

static const u8 set_request[] = {
	0x17, 0x01, 0x00, 0x01,
	0x18, 0x04, 0x00, NAS_TARGET_DOMAIN, 0x00, 0x00, 0x00
};

static u8 response[RESPONSE_CAP];
static u32 os_params[QMI_OS_PARAMS_BYTES / sizeof(u32)];

static u16 le16(const u8 *p)
{
	return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static u32 le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) |
		((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void emit(const char *s, unsigned n)
{
	unsigned sent = 0;
	while (sent < n) {
		int rc = write(1, s + sent, n - sent);
		if (rc <= 0)
			return;
		sent += (unsigned)rc;
	}
}

static void emit_line(const char *phase, int transport, unsigned length,
			      int result_valid, u16 result, u16 error,
			      int pref_valid, u32 pref, int malformed)
{
	char line[192];
	int n = snprintf(line, sizeof(line),
			 "phase=%s target=%u duration=1 transport=%d len=%u result_valid=%d result=%u error=%u pref_valid=%d pref=%u malformed=%d\n",
			 phase, (unsigned)NAS_TARGET_DOMAIN, transport, length,
			 result_valid, (unsigned)result, (unsigned)error,
			 pref_valid, pref, malformed);
	if (n > 0 && n < (int)sizeof(line))
		emit(line, (unsigned)n);
}

static int parse_response(const u8 *bytes, unsigned length,
			  int expect_preference, u32 *preference)
{
	unsigned pos = 0;
	int result_seen = 0;
	u16 result = 0;
	u16 error = 0;
	int pref_seen = 0;
	u32 pref = 0;
	int malformed = 0;
	while (pos + 3 <= length) {
		u8 type = bytes[pos];
		u16 size = le16(bytes + pos + 1);
		pos += 3;
		if (size > length - pos) {
			malformed = 1;
			break;
		}
		if (type == 0x02) {
			if (size != 4 || result_seen)
				malformed = 1;
			else {
				result = le16(bytes + pos);
				error = le16(bytes + pos + 2);
				result_seen = 1;
			}
		} else if (expect_preference && type == 0x18) {
			if (size != 4 || pref_seen)
				malformed = 1;
			else {
				pref = le32(bytes + pos);
				pref_seen = 1;
			}
		}
		pos += size;
	}
	malformed |= pos != length;
	emit_line(expect_preference ? "before_get" : "set", 0, length,
		  result_seen, result, error, pref_seen, pref, malformed);
	if (preference)
		*preference = pref;
	return malformed || !result_seen || result != 0 || error != 0 ||
		(expect_preference && !pref_seen);
}

int main(void)
{
	void *service;
	void *client = 0;
	u8 empty = 0;
	unsigned length = 0;
	u32 previous = 0;
	int rc;
	int failed = 0;
	int release_rc;

	service = nas_get_service_object_internal_v01(1, 0x164, 6);
	if (!service) {
		emit("service_object missing\n", sizeof("service_object missing\n") - 1);
		_exit(1);
	}
	memset(os_params, 0, sizeof(os_params));
	rc = qmi_client_init_instance(service, 0xffff, 0, 0, os_params,
				      QMI_INIT_TIMEOUT_MS, &client);
	if (rc != 0 || !client) {
		emit_line("init", rc, 0, 0, 0, 0, 0, 0, 0);
		_exit(1);
	}

	/* Accept the original PS-only state or the proven temporary CS+PS trial. */
	memset(response, 0, sizeof(response));
	rc = qmi_client_send_raw_msg_sync(client, NAS_GET_SELECTION_PREFERENCE,
					&empty, 0, response, sizeof(response),
					&length, QMI_QUERY_TIMEOUT_MS);
	if (rc != 0 || length > sizeof(response)) {
		emit_line("before_get", rc, length, 0, 0, 0, 0, 0,
			  length > sizeof(response));
		failed = 1;
	} else
		failed = parse_response(response, length, 1, &previous);
	if (failed)
		goto done;
#if NAS_TARGET_DOMAIN == 2
	if (previous != 1 && previous != 2) {
		emit("permanent change refused: unexpected baseline\n",
		     sizeof("permanent change refused: unexpected baseline\n") - 1);
		failed = 1;
		goto done;
	}
#else
	if (previous == 1) {
		emit("restore skipped: already PS-only\n",
		     sizeof("restore skipped: already PS-only\n") - 1);
		goto done;
	}
#endif

	memset(response, 0, sizeof(response));
	length = 0;
	rc = qmi_client_send_raw_msg_sync(client, NAS_SET_SELECTION_PREFERENCE,
					(void *)set_request, sizeof(set_request),
					response, sizeof(response), &length,
					QMI_SET_TIMEOUT_MS);
	if (rc != 0 || length > sizeof(response)) {
		emit_line("set", rc, length, 0, 0, 0, 0, 0,
			  length > sizeof(response));
		failed = 1;
	} else
		failed = parse_response(response, length, 0, 0);
	done:
	release_rc = qmi_client_release(client);
	if (release_rc != 0) {
		emit_line("release", release_rc, 0, 0, 0, 0, 0, 0, 0);
		failed = 1;
	}
	_exit(failed);
}
