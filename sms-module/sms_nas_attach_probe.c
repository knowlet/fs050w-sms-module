/* Read-only NAS attachment and SMS-domain probe for FS050W V2.1.0.
 * Sends GET_SERVING_SYSTEM (0x24), GET_SYSTEM_SELECTION_PREFERENCE (0x34),
 * and GET_SYSTEM_INFO (0x4d). Wire field layouts follow libqmi's NAS schema;
 * the FS050W vendor IDL previously confirmed LTE SMS TLV 0x38. There are no
 * SET requests, registration actions, or SMS send operations in this file.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef __SIZE_TYPE__ usize;

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
	NAS_GET_SERVING_SYSTEM = 0x24,
	NAS_GET_SELECTION_PREFERENCE = 0x34,
	NAS_GET_SYSTEM_INFO = 0x4d,
	QMI_INIT_TIMEOUT_MS = 5000,
	QMI_QUERY_TIMEOUT_MS = 5000,
	QMI_OS_PARAMS_BYTES = 0x60,
	RESPONSE_CAP = 4096
};

struct result {
	int seen;
	u16 value;
	u16 error;
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

static void emit_header(unsigned id, int rc, unsigned received,
			const struct result *result, int malformed)
{
	char buf[160];
	int n = snprintf(buf, sizeof(buf),
			 "nas id=0x%02x transport=%d len=%u result_valid=%d result=%u error=%u malformed=%d\n",
			 id, rc, received, result->seen,
			 (unsigned)result->value, (unsigned)result->error, malformed);
	if (n > 0 && n < (int)sizeof(buf))
		emit(buf, (unsigned)n);
}

static int emit_serving(const u8 *p, unsigned len)
{
	char buf[160];
	unsigned count;
	int n;
	if (len < 5) {
		emit("serving malformed\n", sizeof("serving malformed\n") - 1);
		return 1;
	}
	count = p[4];
	if (count > len - 5) {
		emit("serving radio_array_malformed\n",
		     sizeof("serving radio_array_malformed\n") - 1);
		return 1;
	}
	n = snprintf(buf, sizeof(buf),
		     "serving registration=%u cs_attach=%u ps_attach=%u network=%u radio_count=%u\n",
		     (unsigned)p[0], (unsigned)p[1], (unsigned)p[2],
		     (unsigned)p[3], count);
	if (n > 0 && n < (int)sizeof(buf))
		emit(buf, (unsigned)n);
	return 0;
}

static int emit_preference(const u8 *p, unsigned len)
{
	char buf[96];
	int n;
	if (len != 4) {
		emit("service_domain_preference malformed\n",
		     sizeof("service_domain_preference malformed\n") - 1);
		return 1;
	}
	n = snprintf(buf, sizeof(buf), "service_domain_preference value=%u\n",
		     le32(p));
	if (n > 0 && n < (int)sizeof(buf))
		emit(buf, (unsigned)n);
	return 0;
}

static int emit_pref_hex(const char *name, const u8 *p, unsigned len,
			 unsigned expected)
{
	static const char hex[] = "0123456789abcdef";
	char buf[128];
	unsigned i;
	int n = snprintf(buf, sizeof(buf), "%s len=%u hex=", name, len);
	unsigned used;
	if (n <= 0 || n >= (int)sizeof(buf))
		return 1;
	used = (unsigned)n;
	if (used + len * 2 + 2 > sizeof(buf))
		return 1;
	for (i = 0; i < len; ++i) {
		buf[used++] = hex[p[i] >> 4];
		buf[used++] = hex[p[i] & 15];
	}
	buf[used++] = '\n';
	emit(buf, used);
	return len != expected;
}

static int emit_lte_info_v2(const u8 *p, unsigned len)
{
	char buf[180];
	int n;
	/* Fixed sequence of booleans, enums, LAC/CID and network identifiers. */
	if (len != 29) {
		n = snprintf(buf, sizeof(buf), "lte_info_v2 unexpected_len=%u\n", len);
		if (n > 0 && n < (int)sizeof(buf))
			emit(buf, (unsigned)n);
		return 1;
	}
	n = snprintf(buf, sizeof(buf),
		     "lte_info_v2 domain_valid=%u domain=%u capability_valid=%u capability=%u roaming_valid=%u roaming=%u forbidden_valid=%u forbidden=%u\n",
		     (unsigned)p[0], (unsigned)p[1], (unsigned)p[2],
		     (unsigned)p[3], (unsigned)p[4], (unsigned)p[5],
		     (unsigned)p[6], (unsigned)p[7]);
	if (n > 0 && n < (int)sizeof(buf))
		emit(buf, (unsigned)n);
	n = snprintf(buf, sizeof(buf),
		     "lte_info_v2 reject_valid=%u reject_domain=%u reject_cause=%u network_id_valid=%u mcc_hex=%02x%02x%02x mnc_hex=%02x%02x%02x\n",
		     (unsigned)p[16], (unsigned)p[17], (unsigned)p[18],
		     (unsigned)p[19], (unsigned)p[20], (unsigned)p[21],
		     (unsigned)p[22], (unsigned)p[23], (unsigned)p[24],
		     (unsigned)p[25]);
	if (n > 0 && n < (int)sizeof(buf))
		emit(buf, (unsigned)n);
	return 0;
}

static int emit_u32_field(const char *name, const u8 *p, unsigned len)
{
	char buf[96];
	int n;
	if (len != 4) {
		n = snprintf(buf, sizeof(buf), "%s unexpected_len=%u\n", name, len);
	} else {
		n = snprintf(buf, sizeof(buf), "%s value=%u\n", name, le32(p));
	}
	if (n > 0 && n < (int)sizeof(buf))
		emit(buf, (unsigned)n);
	return len != 4;
}

static int query(void *client, unsigned id)
{
	unsigned received = 0;
	unsigned pos = 0;
	struct result result = {0, 0, 0};
	int malformed = 0;
	int rc;
	int serving_seen = 0;
	int pref_seen = 0;
	int rat_seen = 0;
	int band_seen = 0;
	int network_seen = 0;
	int lte_info_seen = 0;
	int reg_seen = 0;
	int sms_seen = 0;
	u8 empty = 0;
	memset(response, 0, sizeof(response));
	rc = qmi_client_send_raw_msg_sync(client, id, &empty, 0, response,
					   sizeof(response), &received,
					   QMI_QUERY_TIMEOUT_MS);
	if (rc != 0 || received > sizeof(response)) {
		emit_header(id, rc, received, &result, 1);
		return 1;
	}
	while (pos + 3 <= received) {
		u8 tlv_id = response[pos];
		u16 len = le16(response + pos + 1);
		const u8 *p;
		pos += 3;
		if (len > received - pos) {
			malformed = 1;
			break;
		}
		p = response + pos;
		if (tlv_id == 0x02) {
			if (len != 4 || result.seen)
				malformed = 1;
			else {
				result.value = le16(p);
				result.error = le16(p + 2);
				result.seen = 1;
			}
		} else if (id == NAS_GET_SERVING_SYSTEM && tlv_id == 0x01) {
			if (serving_seen++) malformed = 1;
			else malformed |= emit_serving(p, len);
		} else if (id == NAS_GET_SELECTION_PREFERENCE && tlv_id == 0x18) {
			if (pref_seen++) malformed = 1;
			else malformed |= emit_preference(p, len);
		} else if (id == NAS_GET_SELECTION_PREFERENCE && tlv_id == 0x11) {
			if (rat_seen++) malformed = 1;
			else malformed |= emit_pref_hex("rat_mode_preference", p, len, 2);
		} else if (id == NAS_GET_SELECTION_PREFERENCE && tlv_id == 0x15) {
			if (band_seen++) malformed = 1;
			else malformed |= emit_pref_hex("lte_band_preference", p, len, 8);
		} else if (id == NAS_GET_SELECTION_PREFERENCE && tlv_id == 0x16) {
			if (network_seen++) malformed = 1;
			else malformed |= emit_pref_hex("network_selection_preference", p, len, 1);
		} else if (id == NAS_GET_SYSTEM_INFO && tlv_id == 0x19) {
			if (lte_info_seen++) malformed = 1;
			else malformed |= emit_lte_info_v2(p, len);
		} else if (id == NAS_GET_SYSTEM_INFO && tlv_id == 0x31) {
			if (reg_seen++) malformed = 1;
			else malformed |= emit_u32_field("lte_registration_domain", p, len);
		} else if (id == NAS_GET_SYSTEM_INFO && tlv_id == 0x38) {
			if (sms_seen++) malformed = 1;
			else malformed |= emit_u32_field("lte_sms_domain", p, len);
		}
		pos += len;
	}
	if (pos != received) malformed = 1;
	emit_header(id, rc, received, &result, malformed);
	if (id == NAS_GET_SERVING_SYSTEM && !serving_seen)
		emit("serving absent\n", sizeof("serving absent\n") - 1);
	if (id == NAS_GET_SELECTION_PREFERENCE && !pref_seen)
		emit("service_domain_preference absent\n",
		     sizeof("service_domain_preference absent\n") - 1);
	if (id == NAS_GET_SYSTEM_INFO) {
		if (!lte_info_seen)
			emit("lte_info_v2 absent\n", sizeof("lte_info_v2 absent\n") - 1);
		if (!reg_seen)
			emit("lte_registration_domain absent\n",
			     sizeof("lte_registration_domain absent\n") - 1);
		if (!sms_seen)
			emit("lte_sms_domain absent\n",
			     sizeof("lte_sms_domain absent\n") - 1);
	}
	return malformed || !result.seen || result.value != 0 || result.error != 0;
}

int main(void)
{
	void *service;
	void *client = 0;
	int rc;
	int failed = 0;
	service = nas_get_service_object_internal_v01(1, 0x164, 6);
	if (!service) {
		emit("service_object missing\n", sizeof("service_object missing\n") - 1);
		_exit(1);
	}
	memset(os_params, 0, sizeof(os_params));
	rc = qmi_client_init_instance(service, 0xffff, 0, 0, os_params,
				      QMI_INIT_TIMEOUT_MS, &client);
	if (rc || !client) {
		struct result result = {0, 0, 0};
		emit_header(0, rc, 0, &result, 0);
		_exit(1);
	}
	failed |= query(client, NAS_GET_SERVING_SYSTEM);
	failed |= query(client, NAS_GET_SELECTION_PREFERENCE);
	failed |= query(client, NAS_GET_SYSTEM_INFO);
	rc = qmi_client_release(client);
	if (rc) failed = 1;
	_exit(failed);
}
