# Responsiveness and continuous zoom qualification

Live qualification was performed on 2026-09-09 against the Sony FS7 connected through bridge `192.168.0.65`. Raw browser/CDP captures are intentionally retained only in the ignored `captures/fs7-native/` development directory.

## Sony Native reference

Sony's served `zoom_slider.js` uses a 100 ms timer while the control is held and writes `Camera.Zoom.Velocity`; release writes velocity `0`. A lossless three-second native hold recorded:

| Direction | Movement writes | Mean interval | Range | Release stops |
| --- | ---: | ---: | ---: | ---: |
| Tele (`5`) | 30 | 100.03 ms | 98.59–101.61 ms | 1 |
| Wide (`-4`) | 30 | 100.06 ms | 98.34–101.29 ms | 1 |

The native client did not send a new write before the previous RPC response.

## Operator result

The operator implementation uses the same 100 ms target and serializes writes through the REST/RPC acknowledgement. It stops on keyup, pointer release/cancel, lost capture, mouse leave, blur, page hide, disconnect, and direction changes.

With no Sony Native proxy page open, a clean live three-second hold recorded:

| Direction | Movement writes | Mean interval | Range | Mean HTTP acknowledgement | Release stops |
| --- | ---: | ---: | ---: | ---: | ---: |
| Tele (`H`) | 30 | 100.35 ms | 98.76–102.19 ms | 49.93 ms | 1 |
| Wide (`J`) | 30 | 100.25 ms | 96.94–101.44 ms | 48.98 ms | 1 |

No requests overlapped. A prior stress run with three stale Sony Native `/linear` proxy sessions open produced approximately 500 ms command stalls; closing those pages restored the qualified cadence. Connection counts are exposed in `/api/v1/status` so this condition is observable.

## One-shot responsiveness

Replacing per-command WebSocket setup with one persistent subscribed backend `/linear` session materially reduced live bridge command time. Representative direct REST client/bridge-Sony measurements changed as follows:

| Command | Before | After |
| --- | ---: | ---: |
| Record start | 156.8 / 36 ms | 95.1 / 6 ms |
| Record stop | 83.0 / 46 ms | 42.9 / 5 ms |
| Menu | 60.0 / 22 ms | 71.9 / 32 ms |
| Cursor up | 61.7 / 23 ms | 42.6 / 6 ms |
| Assign 1 | 281.6 / 241 ms | 44.6 / 8 ms |
| Play | 746.2 / 708 ms | 51.2 / 15 ms |
| Zoom movement | 99.3 / 62 ms | 47.4 / 11 ms |
| Zoom stop | 60.7 / 23 ms | 42.7 / 6 ms |

Each pair is client wall time / bridge-reported Sony response time. Camera state and network conditions can vary, so these values characterize this live run rather than establish hard real-time guarantees.

A final clean `/operator` browser pass on the installed image, with no Sony Native proxy session, measured event-to-HTTP-response totals of 75.3 ms Record, 67.3 ms Record Stop, 105.7 ms Menu, 88.1/89.2/84.9/80.5 ms Up/Down/Left/Right, 93.4 ms Assign 1, 71.2 ms Stop Playback, 68.0 ms Play, and 85.8 ms Pause. All returned HTTP 200; immediate local feedback occurs before these requests complete.

## Remaining physical observation

Wire cadence, acknowledgement serialization, and release-stop behavior are qualified. Final judgement of perceived lens smoothness and suitability at the operator position remains a physical camera/operator observation.

Record start and the complete Stop + Replay path were repeated on the final OTA image. Record returned an acknowledged Sony RPC, Stop + Replay reached `REVIEW_DISPATCHED`, and Stop Playback then returned the camera from review.

An auxiliary `--no-precache` native-page asset-burst probe made the bridge temporarily unresponsive and it recovered by rebooting. Normal sequential-precache Sony Native captures succeeded; the unsupported burst probe remains a proxy stress/failure-recovery observation, not part of the accepted command/zoom timing path.
