# Deploy: x360-sdk

## Building

The toolchain is GCC 15, binutils and newlib for `powerpc-xsdk-elf`, built in WSL Debian by
`tools/toolchain/build.sh` (its specs and patches say what differs from a stock build) and installed at `~/xsdk-gcc`.
Then, in the repository:

```
make
```

That makes `build/libxsdk.a` and the test title, `build/xsdk_test.xex`, as well as the bring-up probes. A title links
its own objects and `libxsdk.a` with `src/xsdk.ld`, which starts it at `xsdk_start` in the library, then packs the ELF
with `tools/elf2xex.py`; vulkan4xenon's Makefile does it that way. CMake projects get the same through
`-DCMAKE_TOOLCHAIN_FILE=cmake/xsdk.cmake` and `xsdk_add_xex(<target>)`; `make cmake_check` builds a title that way.
