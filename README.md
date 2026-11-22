# mgsv-phantom-core

The part of Phantom FX that more than one add-on uses, for **Metal Gear Solid V: The Phantom Pain** under ReShade: the
game's constant buffers and shaders by name, shader bytecode tools, the frame order, the settings file format, and the
Direct3D helpers. A static library with no ReShade calls, no overlay and no hooks of its own.

Consumers mount it as a git submodule at `src/core` and link `phantom_fx_core`. See [docs/addon-core.md](docs/addon-core.md).

- **Consumers:** [mgsv-phantom-fx](https://github.com/ZioPao/mgsv-phantom-fx) (PhantomFX) and
  [mgsv-phantom-blood](https://github.com/ZioPao/mgsv-phantom-blood) (body blood).
- **Build alone:** `cmake -S . -B build` then `ctest --test-dir build` (Windows, MSVC, C++20).
- **Game version:** the offsets and shader names are for one build of the game; the core's `device_check` says when it is
  not that build.