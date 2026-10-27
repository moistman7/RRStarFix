# Changelog

## 0.9.1 — 2026-10-26

First public release.

- Restore night-sky stars removed by DLSS Ray Reconstruction using a temporally smoothed, motion-reprojected star layer.
- Enable automatically after the rendering-state safety check; no F8 toggle.
- Ignore legacy StarComposite and ToggleKey settings so old disabled configurations cannot prevent activation.
- Keep F7 smoothing selection and automatic saving.
- Preserve automatic safety disable if the game fails to restore rendering bindings.
- Developer diagnostics off by default.

Validation: six automated tests passed with GPU-based validation enabled and no D3D12 debug-layer errors. Automatic activation confirmed in-game on RTX 5090; broader compatibility testing and longer play testing are ongoing.
