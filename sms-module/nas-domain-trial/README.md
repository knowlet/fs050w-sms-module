# FS050W NAS service-domain temporary trial (offline prepared)

This directory contains **two** small ARM binaries. They are deliberately not
installed, uploaded, or run by the build/test scripts. The main investigation
must decide separately whether a live trial is acceptable.

The router currently reports NAS `GET_SYSTEM_SELECTION_PREFERENCE` service
domain `1` (PS only), `GET_SERVING_SYSTEM` CS detached / PS attached, IMS
registration off, and LTE SMS domain `0` (no SMS support). A change to CS+PS is
a targeted *hypothesis test*: it may cause a combined EPS/IMSI attach and make
SMS over NAS available, but the carrier may still reject or omit that service.

## Exact wire behavior

Both programs first issue read-only NAS `GET_SYSTEM_SELECTION_PREFERENCE`
`0x34` and print its result and service-domain value. The trial refuses to
write unless the value is exactly the observed baseline `1`. The restore
skips the write if it is already `1`.

The trial binary sends one NAS `SET_SYSTEM_SELECTION_PREFERENCE` `0x33`
request containing **only** these two TLVs:

| TLV | Value | Meaning |
| --- | --- | --- |
| `0x17` | `00` | Change duration: until next power cycle |
| `0x18` | `02 00 00 00` | Service domain: CS+PS |

The restore binary sends the same `0x17=00` and `0x18=01 00 00 00` (PS only).
Neither binary calls any other setter or touches APN, `dialup_dataswitch`,
CFUN, physical buttons, SMS routing, or MCFG. Each parses the synchronous QMI
result/error and exits nonzero if the transport or modem rejects the request.
A successful response **does not** prove that network attach or SMS service
changed; separate read-only NAS queries must verify those outcomes.

Source definitions:
[libqmi NAS message schema](https://chromium.googlesource.com/chromiumos/third_party/libqmi/+/refs/tags/1.34.0/data/qmi-service-nas.json)
and [libqmi NAS enums](https://chromium.googlesource.com/chromiumos/third_party/libqmi/+/master/src/libqmi-glib/qmi-enums-nas.h).
`ChangeDuration=0` is the *power-cycle* duration in this QMI enum; it must not
be confused with similarly named preference-duration enums.

## Build and offline checks

From the repository root:

```sh
sh sms-module/nas-domain-trial/build.sh
sh sms-module/nas-domain-trial/test_host.sh
shellcheck sms-module/nas-domain-trial/build.sh sms-module/nas-domain-trial/test_host.sh
```

Default outputs are `/private/tmp/fs050w-nas-csps-trial` and
`/private/tmp/fs050w-nas-ps-restore`; set `OUT_DIR` to an existing output
directory if needed. `test_host.sh` uses a fake QMI service and checks:
baseline refusal, exact request bytes, response error handling, GET failure,
and idempotent restore. `verify_wire.py` independently decodes both TLVs.
None of these checks sends a command to a real modem.

The current build produced two 4,524-byte ARM PIEs:

| Binary | SHA-256 |
| --- | --- |
| `fs050w-nas-csps-trial` | `33fd6b9f6dc7e420598debff114c6f895fbc52604cce8e0bfb51f113ed6aeadd` |
| `fs050w-nas-ps-restore` | `5c2eb8d8717ae9bee3be51f0092f99fa2b9984b9b88ed5323d0480c80d0081ce` |

## Live-use gate and rollback

Before considering a live run, snapshot the original `0x24`, `0x34`, `0x4d`
NAS values, RAT/band/network selection TLVs, data switch `on`, APN profile
`18`, SIM ready, physical button `enable`, and local access. The command may
re-register the modem and temporarily interrupt cellular data even though it
does not turn off the data switch. If a live trial is authorized, upload and
hash-check both binaries and keep the restore binary available before running
the trial; then poll the same NAS GETs and the router safety state. If SMS
domain stays `0`, PS data fails, or an unexpected preference changes, run the
restore binary while QMI remains available. If QMI is unavailable, a physical
power cycle is the QMI-defined expiration path, but this fallback has **not**
been tested on the FS050W and is not a guaranteed recovery mechanism.

No outgoing SMS is included here. A further send requires separate
authorization from the main investigation; an incoming test requires a new
message from the user's phone.

## Separate permanent variants (offline preparation only)

`nas_domain_permanent_set.c` is a **separate** implementation. Its build
script does not modify the temporary binaries above. It checks GET first;
the permanent CS+PS program accepts only the original `1` or the successful
temporary trial state `2`, while the permanent restore skips if already `1`.
It has the same result parsing and one SET message, but sends `0x17=01` (permanent)
instead of `0x17=00` (until power cycle):

| Binary | Request TLV bytes | SHA-256 |
| --- | --- | --- |
| `fs050w-nas-csps-permanent` | `17 01 00 01 18 04 00 02 00 00 00` | `7dbe4a78b362e5def6d9554961e5750fd6675f1408e3c50aea80b98da1eb76c5` |
| `fs050w-nas-ps-permanent-restore` | `17 01 00 01 18 04 00 01 00 00 00` | `d75c19562cf0b08505c059a18765deb5ad6875074efe492a17a7631d7dc9cd03` |

Build and test **without** device access:

```sh
sh sms-module/nas-domain-trial/build_permanent.sh
sh sms-module/nas-domain-trial/test_permanent_host.sh
shellcheck sms-module/nas-domain-trial/build_permanent.sh sms-module/nas-domain-trial/test_permanent_host.sh
```

The permanent variants were prepared for a **distinct future decision that
requires explicit authorization** after a temporary live trial demonstrates
that CS+PS is accepted, data remains connected, and SMS domain or delivery
actually improves. Power cycling will not undo a permanent setting. Reversal
requires the separate permanent restore binary while QMI remains reachable,
followed by a fresh GET and a reboot check. If QMI becomes unavailable after
the permanent write, software rollback is not guaranteed. Neither permanent
binary has been uploaded or executed on the FS050W.
