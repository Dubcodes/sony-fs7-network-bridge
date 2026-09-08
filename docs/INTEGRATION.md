# External-control integration

All integrations must use the bridge's semantic commands, sequences, and state. They must not embed Sony HTTP paths, `/linear` RPC names, credentials, or camera parsing. This keeps REST, TCP, VSM, and future RCP adapters behaviorally consistent.

## ASCII TCP — recommended simple controller path

Connect from the wired control network to `<bridge-ip>:5000`. The server sends:

```text
FS7-BRIDGE 0.2.12-integration-cleanup READY\r\n
```

Send one ASCII/UTF-8 command per line. LF and CRLF are accepted. Every non-empty line receives exactly one LF/CRLF-terminated response. Commands and identifiers are case-insensitive where the protocol already normalizes them; state and command IDs should be sent exactly as listed by `HELP` and `/api/v1/catalog`.

```text
PING                         -> PONG
STATUS                       -> one-line JSON status
STATE                        -> one-line JSON camera-state document
GET recording               -> VALUE recording true
GET recording               -> STALE recording true 4100
GET recording               -> UNKNOWN recording
CMD record_start             -> OK record_start <message>
REC_START                    -> same semantic command
REC_STOP
STOP_REPLAY
STOP_REPLAY_ABORT
REC_REVIEW
AWB
SEQ 1
SEQ_STATUS
SEQ_ABORT 1
HELP
```

Errors are deterministic text beginning with `ERR`; command errors include semantic ID and status code, for example `ERR record_start 409 <message>`. Automation should parse the leading token and status code, not human message wording.

The firmware serves one TCP client at a time. A quiet connection is closed after five minutes. Send `PING` more often than five minutes (60 seconds is a conservative integration setting), and reconnect on EOF, timeout, Ethernet interruption, or bridge reboot. A new client is accepted after the active client disconnects or times out. Do not depend on TCP keepalive alone because application input refreshes the idle timer.

### Lawo VSM concept

The lowest-complexity current VSM path is a generic outbound TCP connection from VSM/GadgetServer to the bridge. Configure newline-terminated request/response transactions, use `PING` for health, map buttons to `CMD <id>` or `SEQ <n>`, and poll `GET <state-id>`, `STATUS`, or `SEQ_STATUS` only as often as operations require. Treat `STALE` and `UNKNOWN` as unavailable feedback, never false/zero.

Exact VSM setup depends on the installed VSM/GadgetServer version and facility policy. The bridge-side contract above is current; files in `vsm/legacy/` are historical experiments and are not asserted to match it.

## REST API

HTTP-capable controllers use `/api/v1`. Examples:

```text
GET  http://<bridge-ip>/api/v1/status
GET  http://<bridge-ip>/api/v1/catalog
GET  http://<bridge-ip>/api/v1/camera-state
POST http://<bridge-ip>/api/v1/command/record_start
POST http://<bridge-ip>/api/v1/sequences/1/trigger
GET  http://<bridge-ip>/api/v1/sequences/status
POST http://<bridge-ip>/api/v1/vsm/trigger/1
GET  http://<bridge-ip>/api/v1/vsm/feedback
```

No body is required for command/trigger POSTs. Preserve HTTP status: 409 means the operation conflicts with the current arbiter owner, 404 means an unknown ID/slot, and other 4xx responses indicate invalid or unavailable configuration. See `API.md` for the complete surface.

## Generic trigger/value model

The `/vsm` page maps 16 trigger slots to semantic commands or `sequence:<n>`, plus eight each of Bool, Int, Float, and Text feedback slots. REST exposes these through `/api/v1/vsm/*`; native adapters should call the same `VsmGeneric` boundary. Invalid or stale typed data is `null`.

## Future native Ember+

Native Ember+ would offer the strongest direct Lawo integration, but no partial protocol is shipped. A future provider must implement proper S101 framing and keepalive, a stable Glow tree, writable triggers, read-only feedback, value-change notifications, reconnect safety, and interoperability/soak tests. It must call only the generic semantic layer. See `NATIVE_EMBERPLUS_PLAN.md`.

## CyanView and other RCP systems

The bridge does not impersonate an FX6 or guess a CyanView Sony API. Future CyanView compatibility can safely happen through one of three explicit paths:

1. CyanView supplies a driver/profile for this documented REST or TCP API.
2. A separate gateway translates a CyanView-supported protocol to the bridge API.
3. The bridge implements a known camera protocol only after that protocol is documented or captured and interoperability-tested.

Any such adapter remains outside the FS7 Sony backend and uses the shared semantic layer. The same rule applies to other RCP or broadcast-control systems.

## Discovery

DHCP plus serial-reported address is the current discovery method; static addressing is available for controlled networks. mDNS is not enabled in this release because it has not been coexistence-tested on the current dual-interface stack. Integrators should configure a DHCP reservation or documented static address for deterministic control.
