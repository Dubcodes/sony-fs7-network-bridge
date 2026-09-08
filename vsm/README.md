# Lawo VSM integration

The current practical bridge interface is generic ASCII TCP on port 5000. Configure VSM/GadgetServer to open an outbound TCP connection to the bridge, send one newline-terminated command per transaction, use `PING`/`PONG` for health, and reconnect after EOF or bridge reboot. Trigger examples include `CMD record_start`, `STOP_REPLAY`, and `SEQ 1`; feedback can use `GET <state-id>`, `STATUS`, or `SEQ_STATUS`.

Where an approved HTTP-capable workflow exists, the versioned REST interface under `/api/v1` provides JSON status, catalog, commands, sequences, and generic trigger/feedback slots.

See `docs/INTEGRATION.md` for the full current contract and `docs/NATIVE_EMBERPLUS_PLAN.md` for the future native-provider boundary.

## Legacy material

`legacy/` contains early Simple REST experiments. They have not been proven against the current API or a production VSM installation. They are retained only as clearly historical reference and must not be described or deployed as current production definitions without a fresh endpoint-by-endpoint review and facility approval.
