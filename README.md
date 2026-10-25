<div align="center">
  <img src="assets/xsdk.png" alt="xsdk" width="640">
</div>

# Xbox 360 SDK

OpenPak's Xbox 360 SDK.

| | |
| --- | --- |
| Network | Xbox 360 |
| Language | C (gnu17) and C++23 |
| Based on | [xsdk](https://github.com/xenosworks/xsdk), MIT |
| Licence | MIT |

## Packages

| Package | What |
| --- | --- |
| `include/xsdk/`, `src/` | The SDK: C17 runtime on newlib, C++23 runtime, the GPU layer (`include/xsdk/gpu.h`) |
| `imports/` | Kernel and XAM imports the SDK links by ordinal |
| `tools/` | `elf2xex.py` XEX packer, shader and import generators, toolchain build |
| `tests/` | The test title and the bring-up probes |
| `cmake/` | CMake toolchain file (`cmake/xsdk.cmake`) |

## Use

```
make
```

That builds `build/libxsdk.a` and the test title `build/xsdk_test.xex`. It needs the toolchain described in
[deploy](docs/deploy.md), which also covers linking a title and the CMake route.

## Docs

- [Protocol](docs/protocol.md)
- [Evidence](docs/evidence.md)
- [Deploy](docs/deploy.md)
- [Notes](docs/notes.md)
- [Hardware results](docs/HARDWARE.md)
- [Parity](docs/PARITY.md)
- Scope studies: [36](docs/scope/36-texel-rate.md), [37](docs/scope/37-guide.md), [38](docs/scope/38-draw-cost.md),
  [39](docs/scope/39-gpu-draw-cost.md), [40](docs/scope/40-fault-handler.md),
  [41](docs/scope/41-docs-and-samples.md), [42](docs/scope/42-target-formats.md),
  [44](docs/scope/44-tiling-and-msaa.md), [45](docs/scope/45-xost-code-quality.md), [46](docs/scope/46-loops.md),
  [47](docs/scope/47-instancing.md), [networking](docs/scope/networking.md),
  [shader compiling](docs/scope/shader-compiling.md)

## Licence

MIT. See [LICENSE](LICENSE). Third-party notices are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
