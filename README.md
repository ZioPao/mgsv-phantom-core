# mgsv-phantom-core

# PhantomCore

The generic base for add-ons that change how **Metal Gear Solid V: The Phantom Pain** renders, under ReShade. PhantomCore
itself is not a graphics mod: it has no effects of its own. Phantom FX (graphics) and the body blood add-on both build on
it.

## Where it comes from

''s
constant buffers, shaders and lights by name, the constant capture, the frame order and the settings file format.

When a second add-on needed the same things (the [body blood](https://github.com/ZioPao/mgsv-phantom-blood) add-on),
those files were separated from Phantom FX into this repository, so neither add-on has to carry the other. The 7 commits
of its history here are the ones that moved the code out (`src/core/` in Phantom FX); nothing earlier was kept.

Phantom FX itself still has its own copy of these files until it is changed to use this repository. What moved out is
only code that was not specific to any one effect.

## What it is

A static library, `phantom_fx_core`: the plumbing an add-on needs before it can draw anything, and nothing it chooses to
draw. It makes no ReShade calls, has no effects, draws no overlay and installs no hooks of its own; each add-on keeps its
own events, effects, settings and pages. There is no shared DLL and no version handshake between add-ons: each one links
the library and carries its own copy.

## Using it

Mount it as a git submodule at `src/core` (the directory must be named `core`) and link `phantom_fx_core`. The steps,
what it publishes and how the frame order works are in [docs/addon-core.md](docs/addon-core.md).

## Building and testing alone

Windows and MSVC, C++20: `cmake -S . -B build`, then `ctest --test-dir build`.

## Game version

The offsets and shader names are for one build of the game; `device_check` says when it is not that build.

## Consumers

- **Phantom FX** ([mgsv-phantom-fx](https://github.com/teemupynnonen/mgsv-phantom-fx)): the graphics add-on. Not yet
  re-pointed; it carries its own copy of these files.
- **Body blood** ([mgsv-phantom-blood](https://github.com/ZioPao/mgsv-phantom-blood)): the blood add-on, `standalone`
  branch, pinned to `v1.0`.
-
