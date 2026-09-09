# FS7 protocol qualification — 0.2.13

## Thumbnail readiness conclusion

The FS7 thumbnail grid loads variably and a fixed 4.5-second delay is not a readiness signal. Live captures showed that an early `Button.SendKeys [["Set"]]` may be silently ignored: the WebSocket transport upgrades, but no matching MessagePack RPC response arrives. Once the current thumbnail is selectable, the same Set request receives an immediate matching success response.

The firmware therefore uses:

1. `Button.SendKeys [["Thumbnail"]]`.
2. A configurable conservative minimum before the first Set attempt (default 4500 ms).
3. Bounded Set attempts. Only a missing RPC response is treated as “not ready yet”; transport failures and explicit RPC errors fail immediately.
4. Immediate cessation of Set attempts after the first matched success response.
5. A configurable minimum selection-to-Play interval (default 2500 ms).
6. `Button.SendKeys [["Play"]]`.
7. Active `Property.GetValue [{"P.Clip.Mediabox.Status":null}]` checks until the value is `Playing`.
8. A 25-second default overall timeout.

`P.Menu.pmw-f5x.Event.EventID = 65540` (`EVENT_THUMBNAIL_UPDATE` in Sony JavaScript) is not used as a completion condition. Source inspection shows it triggers thumbnail-related refresh handling, but neither source nor live notification timing proves that it means all thumbnail images are loaded.

## Discovered clip and thumbnail surface

The complete captured Sony JavaScript tree contains `P.Clip.Mediabox.TotalClips` and `P.Clip.Mediabox.ClipPosition`, and Savona exposes `Clip.GetList` and `Clip.GetThumbnailUrls`. The native remote did not provide live evidence that these values represent image-load completion, so they are not used as readiness gates. Clip count is never inferred from elapsed load time or request count.

## Exact wire forms

- Recorder: `Clip.Recorder.Start []`, `Clip.Recorder.Stop []`
- Button keys: `Button.SendKeys [["KeyName"]]`
- Properties: `Property.SetValue [{"Property.Name":value}]`
- AWB/ABB: `Process.Execute.AutomaticAdjustment [["Camera.WhiteBalance"]]` / `[["Camera.BlackBalance"]]`
- Playback verification: `Property.GetValue [{"P.Clip.Mediabox.Status":null}]`

Raw captures and downloaded Sony assets remain local and Git-ignored. This document records conclusions without redistributing proprietary material.
