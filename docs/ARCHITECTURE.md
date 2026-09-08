# Architecture

Firmware `0.2.12-integration-cleanup` runs on a WT32-ETH01 (ESP32, LAN8720A, 4 MB flash).

```text
          wired control network
 browser / REST / TCP / adapter
                 |
        +--------v---------+
        | Web/TCP surfaces |
        | Generic IO model |
        +--------+---------+
                 |
        +--------v---------+
        | Command Engine   |
        | Sequence Engine  |
        | Operation Arbiter|
        | Camera State     |
        +--------+---------+
                 |
    source-bound Wi-Fi HTTP/WebSocket
                 |
        +--------v---------+
        | Sony PXW-FS7     |
        +------------------+
```

## Network separation

Ethernet is the general management/control route and defaults to DHCP. Wi-Fi STA joins the FS7 access point. Every camera-side Sony socket is explicitly bound to the Wi-Fi address so an overlapping wired subnet does not silently attract camera traffic. Distinct subnets are still preferred. The bridge does not route or NAT between interfaces.

## Semantic boundary

REST, ASCII TCP, the generic VSM model, and future adapters do not call Sony transport directly. They invoke stable command IDs or sequence slots through the shared engines. The arbiter serializes manual control, Stop + Replay, sequences, and firmware update ownership. Conflicts fail with 409-style results, and every terminal path releases ownership.

Sony-specific HTTP, Basic Auth, `/linear` WebSocket framing, MessagePack RPC, and response parsing remain behind `SonyRemote`, `SonyLinear`, and `SonyProxy`. Only captured/evidence-backed behavior belongs there.

## Camera state and replay

The shared store holds 23 semantic values with validity, update time, and freshness. Operator UI, TCP `GET`, sequence waits, REST, and generic feedback all read this one store. Stale data is diagnostic, not current truth.

Stop + Replay dispatches stop, then prefers a fresh post-stop transition in this order: `ready=true`, `writing=false`, `recording=false`. If no usable telemetry exists, it uses the bounded configured delay. Overall timeout and abort paths fail closed.

## Sequences

Eight non-blocking slots support semantic command steps, timed waits, and waits for fresh state. Sequences never contain Sony URLs or payloads. They use the same command engine and arbiter as manual interfaces.

## External adapters

The generic model provides 16 writable triggers and typed Bool/Int/Float/Text feedback slots. Generic TCP on port 5000 is the practical first VSM path. Native Ember+ and other RCP adapters must remain outside the Sony backend and call this semantic layer only. See `INTEGRATION.md`.

## Runtime constraints

The web server and TCP server are cooperative and single-threaded. Sony HTTP/telemetry operations are synchronous and bounded; telemetry processes at most one eligible source per loop and backs off after failures. The native Sony proxy independently listens on TCP 8081 and tunnels `/linear` WebSockets without interpreting them.
