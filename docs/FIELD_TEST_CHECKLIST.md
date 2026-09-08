# Field test checklist

## Before connection

- [ ] Both validators and all host tests pass.
- [ ] Clean `wt32-eth01` PlatformIO build succeeds and fits the OTA slot.
- [ ] WT32 first-flash wiring and GPIO0 procedure are understood.
- [ ] Serial boot reports the expected version and board profile.
- [ ] Ethernet receives DHCP, or deliberate static addressing is configured.
- [ ] Management network is trusted and isolated.

## Camera path

- [ ] FS7 Wi-Fi credentials and current Basic Auth credentials are saved.
- [ ] Bridge Wi-Fi associates and camera connection test succeeds.
- [ ] Wired and Wi-Fi addresses are recorded; overlap warning is checked.
- [ ] Sony native fallback page loads through TCP 8081.
- [ ] `/linear` connects and live Sony data appears.
- [ ] Record start/stop is verified with camera/media observation.
- [ ] Rec Review and Stop + Replay timing are verified.
- [ ] Auto White/Auto Black and intended operator property controls are verified.
- [ ] Physical-camera changes produce the expected readback or are documented as unsupported.

## External control

- [ ] REST status/catalog/command error responses are recorded.
- [ ] TCP greeting, `PING`, trigger, state query, five-minute idle close, and reconnect are tested.
- [ ] Only one TCP controller owns the connection in the deployed design.
- [ ] Sequence trigger, status, conflict, timeout, and abort are exercised.
- [ ] VSM/gateway mapping uses current TCP or REST documentation, not unverified legacy files.

## OTA and recovery

- [ ] Application-only OTA succeeds from each slot.
- [ ] Interrupted upload does not schedule a reboot.
- [ ] Serial reflashing remains available.
- [ ] Reboot restores network, camera connection, mappings, and operator layout.

## Soak

- [ ] Run repeated command/reconnect cycles while monitoring heap minimum.
- [ ] Test Ethernet link loss, DHCP renewal, camera Wi-Fi loss, and camera reboot.
- [ ] Confirm no watchdog reset or unintended camera command occurs.
