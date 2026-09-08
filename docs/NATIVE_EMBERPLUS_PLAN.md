# Native Ember+ provider candidate

## Status and boundary

Native Ember+ is not implemented or claimed compatible. It is a future adapter option if Lawo VSM and facility engineering approve it. The provider must call only `VsmGeneric`, Sequence Engine, Camera State, and the shared Operation Arbiter; it must contain no Sony URLs, RPC names, credentials, parsing rules, or camera transport code.

Do not implement a proprietary-looking subset. Evaluate an established, license-compatible implementation such as Lawo's Ember+ SDK components (`libs101` and `libember_slim`) and verify the chosen version/license before incorporating it.

## Required wire behavior

- real S101 framing, escaping, keepalive, and packet handling
- Glow root/directory responses with stable numeric paths and identifiers
- writable trigger parameters with exactly-once bridge dispatch semantics
- read-only Bool/Integer/Real/String feedback parameters
- value-change notifications without polling the camera independently
- clean consumer disconnect/reconnect and partial-frame recovery
- bounded memory use suitable for ESP32 and no main-loop starvation

## Candidate Glow tree

```text
SonyBridge
├── Triggers
│   ├── Trigger01 ... Trigger16
│   └── Sequence01 ... Sequence08
├── Bool
│   └── Bool01 ... Bool08
├── Integer
│   └── Int01 ... Int08
├── Float
│   └── Float01 ... Float08
├── Text
│   └── Text01 ... Text08
└── Status
    ├── BridgeOnline
    ├── CameraReachable
    ├── OperationBusy
    └── Version
```

Labels and generic slot meanings remain configurable on the bridge. Paths/identifiers must remain stable across label or mapping changes.

## Definition of done

- VSM browses and subscribes to the complete intended tree.
- Triggers execute once and return useful failure state through the adapter.
- Feedback changes generate correct notifications; stale/unknown data is explicit.
- Repeated reconnect, fragmented frame, malformed input, and command cycles are safe.
- Long-duration soak shows no heap trend, watchdog reset, or camera-control starvation.
- Interoperability is tested against the actual approved VSM/GadgetServer release.
- Public licensing and attribution review is complete.
