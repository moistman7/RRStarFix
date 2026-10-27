# RR Star Fix

Restores night-sky stars that DLSS Ray Reconstruction removes in **The Witcher 3 (DX12)**.

RR Star Fix is a ReShade add-on. It detects stars in the game's input to Ray Reconstruction, smooths them over time, and adds them back to the reconstructed image. It activates automatically after checking that the game safely restores its rendering state. No on/off hotkey is needed.

## Requirements

- The Witcher 3 running in DirectX 12 mode on Windows 64-bit.
- DLSS Ray Reconstruction enabled and working in the game.
- [ReShade with full add-on support](https://reshade.me/#download), installed for `bin/x64_dx12/witcher3.exe`. The standard ReShade installer does not provide full add-on support. If ReShade is already installed, keep your existing presets and other add-ons.

ReShade, game files, and NVIDIA DLSS/Streamline runtime DLLs are not included.

## Installation

1. Close the game.
2. Download `RRStarFix-v0.9.1.zip` from the [release page](https://github.com/moistman7/RRStarFix/releases/tag/v0.9.1).
3. Extract the ZIP into the game's installation folder, merging its `bin` folder. The add-on must end up at `The Witcher 3/bin/x64_dx12/RRStarFix.addon64`, beside `witcher3.exe`.
4. Launch the DX12 game with Ray Reconstruction enabled. Load a scene with a visible night sky. Stars should fade in over roughly a second.

Manual installation is recommended. Do not put the add-on in the game's `mods` folder. No Script Merger step is required.

On first launch, `RRStarFix.ini` is created beside the add-on. Existing INI files can be kept: old `StarComposite=0` and `ToggleKey` entries are ignored in v0.9.1. The star fix always activates after its safety check. F8 is unused.

## Smoothing

Press **F7** while the game has focus to cycle **medium → strong → light**. Your choice is saved automatically.

| Mode | Effect |
| --- | --- |
| Light | Faster response, more twinkle |
| Medium (default) | Balanced smoothing and response |
| Strong | Less twinkle, slower fade-in and response |

You can also edit the INI with the game closed:

```ini
[RRStarFix]
Smoothing=medium
SmoothingKey=F7
Diagnostics=0
SnapshotKey=F9
TraceKey=F10
```

Hotkeys support F1–F24, letters, digits, PageUp, PageDown, Home, End, Insert, Delete, Pause, ScrollLock, NumPad0–NumPad9, hex virtual-key codes, and `None` to disable a hotkey. Diagnostics should remain off for normal play.

## Compatibility and testing

The tested setup uses an RTX 5090 at 3440×1440 output with 1995×835 internal rendering, alongside RenoDX HDR and other graphics mods. That is one tested setup, not a guarantee for every combination.

v0.9.1's log confirmed automatic activation after three safe RR frames, with no RRStarFix errors or automatic disable messages during the approximately 28-minute session inspected before release. Longer play testing is ongoing. All six automated tests passed, including D3D12 debug-layer and GPU-based validation checks. Other GPUs/drivers, DLSS modes/resolutions, HDR configurations, frame-generation combinations, RR switching, alt-tab, and resizing still need broader testing.

Earlier measurements on this setup put the composite at about **0.04 ms GPU time per frame**. It uses two history buffers, about **79 MB at 3440×1440**. These are measurements from one setup; performance can vary.

Known limitations:

- Stars are accumulated over time, so they fade in rather than appearing instantly.
- Some twinkle remains; panning can soften stars.
- A few stars already preserved by RR may become brighter.
- Very faint stars can remain missing. Clouds affect the result only when they already dim the stars in RR's input.
- The fix only runs after successful RR evaluation with supported resources and rendering state. It does not add stars when RR is off.

## Troubleshooting

Check `ReShade.log` beside `witcher3.exe` for:

```text
[RRStarFix] v0.9.1 ready. Star composite always on (after its safety check)
[RRStarFix] Streamline hooks installed (4/4).
[RRStarFix] State-restore check installed
[RRStarFix] Star composite ON (state check passed: ..., 0 unsafe).
```

The first line continues with your smoothing settings. ReShade may load, unload, and reload add-ons once during startup; that is expected.

If the log reports `auto-disabled`, the add-on detected the game using rendering bindings without restoring them after RR and disabled its composite for that session. Report the message and your configuration. If the add-on is absent entirely, check the installation path and that ReShade has full add-on support.

For bug reports, include the game version, GPU and driver version, output resolution, DLSS/RR/frame-generation settings, relevant graphics mods, and the RRStarFix lines from ReShade.log. Review logs before posting them publicly.

To uninstall, close the game and remove `bin/x64_dx12/RRStarFix.addon64`. You can also remove its `RRStarFix.ini`. Keep other mods' files and ReShade itself if you still use them.

## Source and license

[Source code](https://github.com/moistman7/RRStarFix) and [build instructions](DEVELOPMENT.md).

RR Star Fix is MIT licensed. Bundled dependencies retain their own notices: MinHook (Tsuda Kageyu), ReShade (Patrick Mours), and NVIDIA Streamline headers. See `LICENSE` and `licenses/`.
