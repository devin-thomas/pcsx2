# PCSX2 Laboratory Foundation

This branch establishes the non-gameplay-changing foundation for the AC+
research laboratory. It is based exactly on upstream tag `v2.7.492`, commit
`66c0771d12b16d6f5277eba1d1d9483fe86d7776`.

The foundation commit changes documentation only. It does not modify guest
logic, timing, RNG, physics, character data, memory, input, or rendering.

## Reproduce the x86_64 macOS build

Use Xcode 26.2 and the dependency/build steps pinned in
`.github/workflows/macos_build.yml`. The effective configure options are:

```text
-DCMAKE_PREFIX_PATH="$HOME/deps"
-DCMAKE_BUILD_TYPE=Release
-DCMAKE_OSX_ARCHITECTURES=x86_64
-DDISABLE_ADVANCE_SIMD=ON
-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
-DUSE_LINKED_FFMPEG=ON
-DCMAKE_C_COMPILER_LAUNCHER=ccache
-DCMAKE_CXX_COMPILER_LAUNCHER=ccache
-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
```

The workflow installs `ccache` and `nasm`, builds the pinned macOS dependency
set under `~/deps`, downloads the official PCSX2 patches bundle, builds PCSX2,
and runs `unittests`. Fork builds are unsigned; locally trust the exact hashed
artifact only after the project-side provenance and isolated legal-game smoke
checks pass.
