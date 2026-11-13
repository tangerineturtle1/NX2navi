# Optional final-module optimization

This experimental helper runs SPIRV-Tools 1.4.363.0 performance passes on a final Vulkan module, after the runtime's binding, capability and reconvergence fixups. It preserves descriptor bindings and specialization constants, validates the input and output, and falls back to the original module on failure. It does not enable relaxed floating-point semantics. Each pipeline worker has its own optimizer instance; compilation remains parallel.

Enable `spirv_opt = 1` in `[performance]` in `nx2navi.ini`, or set `NX2NAVI_SPIRV_OPT=1`. Default is off. Place `nx2navi_spirv_opt.dll` beside the game executable. A missing helper preserves the original path. Modules smaller than 4,000 words retain the original path because the additional optimizer work is unlikely to pay off.

The runtime caches results in `spirv-opt-363-perf-v1` beside the executable. Records include the complete original module and a checksum of the optimized module. Hash collisions and damaged records are rejected; writes publish by rename. Existing shader translations and the Vulkan driver cache are retained. Updating the optimizer version or pass configuration requires a new cache directory version. The helper uses a caller-supplied allocator, so allocation ownership never crosses a C runtime boundary.

For Windows, obtain the unmodified MSYS2 UCRT64 SPIRV-Tools package and build:

```text
python tools/spirv_opt/build.py --toolchain <UCRT64-prefix> --spirv-tools <SPIRV-Tools-UCRT64-prefix> --out <game-directory>
```

The tested package is `mingw-w64-ucrt-x86_64-spirv-tools-3~1.4.363.0-1-any.pkg.tar.zst`, SHA256 `d31d67fe918aeee21d1e930fd96360856a60146d23094e4ae8baf071a4049aaa`. Its libraries are Apache-2.0; retain `spirv-tools-LICENSE.txt` when distributing the helper. The Windows build command links the package's static optimizer libraries. On Linux, build it with `python tools/spirv_opt/build.py --spirv-tools /usr --out <game-directory>`, using the prefix where the SPIRV-Tools headers and libraries are installed. A kit carrying the helper in `bin/` copies it beside the game when compiling. Linux helper loading and synthetic module validation have been tested; gameplay equivalence and performance on every driver remain unverified.

This improves the code presented to the host shader compiler. It does not execute tensor operations on matrix hardware or replace the game's renderer. Successful validation and pipeline creation do not establish visual equivalence; gameplay comparison remains required.

Run `python tests/spirv_opt_check.py <path-to-helper>` to check optimization and rejection of an invalid module, and `python tests/spirv_opt_cache_check.py` to check cache hits, corruption and fallback. Neither test needs game files or a GPU.
