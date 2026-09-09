# Working-directory cleanup candidates

Reviewed for the 0.2.13 qualification pass. No source or evidence was deleted automatically.

Safe generated/local cleanup (already Git-ignored):

- `.pio/` — PlatformIO build cache; regenerate with `python -m platformio run`.
- `.pytest_cache/` and `tests/__pycache__/` — Python test caches.
- `test-artifacts/` — validator output.
- `captures/` — local raw camera captures and downloaded Sony assets. Keep until the protocol qualification is signed off, then archive outside the public repository or remove locally.

Retain:

- `vsm/legacy/` — clearly marked historical integration references; still linked from the VSM documentation/UI.
- `WT32_BUILD_AND_FLASH.ps1` — current Windows build/serial-flash helper.
- all tools under `tools/` — each still has a distinct capture, scan, analysis, mock, validation, or build-support role.

Before any future deletion, verify the candidate is ignored/untracked and that its evidence has been archived if required. Do not commit proprietary Sony assets.
