# Research notes and upstream references

These references informed the implementation. They are not bundled dependencies unless explicitly stated.

## Sony PXW-FS7

Sony's PXW-FS7 documentation describes a browser-based Wi-Fi Remote reached at `http://<camera-ip>/rm.html` with Basic Authentication. The remote's shooting screen includes S&Q FPS, shutter, white balance, sensitivity/gain/exposure index, gamma, MLUT, iris, focus, zoom and several automatic functions; it also provides playback/status and assignable-button screens.

Sony also documents a critical synchronization caveat: the stock Wi-Fi Remote page may no longer match camera settings after the camera itself is operated directly, in which case Sony instructs the user to reload the browser. Consequently this project treats **true physical-camera readback as a test requirement**, not an assumption. The bridge polls discovered camera endpoints directly and records value age.

Sony documents the browser UI but does not publish the private request endpoints/response schema behind all FS7 controls and status. This project retains only behavior supported by real-camera or publicly inspectable client evidence; remaining properties stay unconfigured until captured and tested.

## Espressif / Ethernet

The current target uses the WT32-ETH01 onboard LAN8720A RMII PHY through Arduino-ESP32's unified Network stack. Ethernet is the control route and Wi-Fi is the isolated camera-side route.

Camera HTTP sockets are explicitly bound to the ESP32 Wi-Fi interface. This is important when the FS7 subnet happens to overlap a wired subnet.

## Lawo VSM

The production VSM transport is deliberately undecided in this handoff build. The facility engineering team should first identify which existing third-party interfaces are approved.

The firmware therefore keeps a transport-neutral generic point model: numbered triggers and Bool/Int/Float/Text feedback values, plus sequence triggers/status. Camera semantics stay on the bridge.

Earlier experiments used Lawo's Simple REST Client (Generic); those definition files are retained only under `vsm/legacy/` for lab/reference use. Native Ember+ and Art-Net are examples of other adapters worth evaluating **only if the facility already permits them**.

The camera/sequence architecture does not depend on that choice.

## LANC fallback research

Open-source projects retained for a future fully wired camera-side backend include:

- `sensslen/LibLanc`
- `Julusian/arduino-lanc`
- `Novgorod/LANC-USB-GUI`
- `fred-dev/arduino_lanC` and its archived Sony LANC command documentation

The Wi-Fi backend is current because it provides the FS7's native browser-control and live-property paths. LANC remains a separate future adapter possibility and is not part of this release.
