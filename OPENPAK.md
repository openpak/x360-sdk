# OpenPak integration

This repository is OpenPak's fork of [xsdk](https://github.com/xenosworks/xsdk).
Upstream notices and the MIT license are preserved. OpenPak changes use `main`.

The target is native Xbox 360 homebrew on the stock 17559 dashboard, with
RGH/JTAG, BadAvatar/BadUpdate, and Wharf qualified independently. Upstream's
RGH hardware results do not establish support for the other targets.

Initial integration work:

- Build a small native diagnostic XEX using the open toolchain.
- Check filesystem, controller input, networking and return-to-dashboard on
  each target, recording firmware and entry method with the result.
- Add the imports needed by OpenPak's native client and diagnostic probes.
- Feed package generation into the homebrew store's installation pipeline.

This SDK does not implement the Xbox Live backend or the dashboard Store.
The OpenPak fork has not yet been built or tested on a console. Follow the
workspace build lock and scratch-directory rules when building.
