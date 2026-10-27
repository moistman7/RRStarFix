# Development

Build on Windows x64 with Visual Studio 2022 Build Tools (C++ and a Windows SDK), CMake 3.24 or newer, and the DirectX Shader Compiler (`dxc.exe`). The release was built using CMake 4.4. Dependency headers and MinHook sources are included.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DRRSTARFIX_DXC="C:/path/to/dxc.exe"
cmake --build build --config Release
$env:RRSTARFIX_GBV = '1'
ctest --test-dir build -C Release --output-on-failure
```

The add-on is `build/Release/RRStarFix.addon64`. Close the game before installing. Windows Graphics Tools supplies the D3D12 debug layer; tests report when it is unavailable. GPU tests use WARP. No GPU validation error was reported in the six-test v0.9.1 release run.

`src/star_composite.cpp` checks command-list state restoration and records the composite. `src/star_composite.hlsl` detects sky stars and reprojects their accumulated history. `src/sl_probe.cpp` hooks Streamline while forwarding the original calls unchanged. `src/config.*` handles smoothing and diagnostics settings. `src/addon.cpp` provides ReShade entry points and hotkeys. Diagnostic readback is in `src/gpu_snapshot.*`.

The fix waits for three safe RR frames and never activates after an unsafe binding use in the session. A later unsafe use automatically disables it. v0.9.1 removes user toggling without changing this safety mechanism or the rendering shader.

Optional developer diagnostics (`Diagnostics=1`) enable F9 pixel snapshots and F10 draw tracing under `%LOCALAPPDATA%/RRStarFix`. They are disabled by default. Snapshots can be large and cause capture hitches.

Tools: `RRStarFixAnalyze` analyzes snapshots, `RRStarFixCompositeBench` compares the shader against the bundled v0.7 reference on snapshot data, and `RRStarFixHookBench` measures hook overhead. Snapshot datasets and machine-specific investigation logs are not part of the source release.
