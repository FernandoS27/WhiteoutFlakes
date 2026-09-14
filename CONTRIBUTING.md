# Contributing to WhiteoutFlakes

**All contributions are welcome.** Bug reports, format-adapter fixes, a new
backend path, a missing shader feature, translations, documentation, a whole
new game profile — if it makes the renderer read or draw Blizzard asset data
better, it is in scope. You do not need to be an expert in every game's format
to help; most of this project was built one format quirk at a time.

Start with [`README.md`](README.md) for the feature matrix and build options,
and [`AGENTS.md`](AGENTS.md) for the short version of how the codebase is
organized and the traps that have actually bitten.

## Before you start

- **Open an issue first for anything non-trivial.** A short description of what
  you intend to change saves you from writing code that collides with work
  already in flight, or that belongs in a different layer.
- **Check the design docs.** Most subsystems have a `*_DESIGN.md` or `*_RE.md`
  at the repo root (`SC2_PARTICLE_DESIGN.md`, `WEM_DESIGN.md`,
  `GLTF_DESIGN.md`, …). They record *why* something is shaped the way it is,
  and frequently the reverse-engineering evidence behind it. If your change
  contradicts one, say so in the PR — the doc may simply be out of date, but
  that is worth knowing.

## Pull request size

**PRs are reviewed manually, by a human, line by line.** That is the single
biggest constraint on how this project accepts changes.

Keep contributions to an adequate, reviewable size. A focused PR that does one
thing lands quickly; a 10,000- or 20,000-line PR does not, and will be sent
back unreviewed.

**If your change is going to be large — say, more than a few hundred lines of
real diff — talk to the maintainer first**, in an issue or a draft PR, before
writing it. Large work is not unwelcome: a new game profile or a new graphics
backend is genuinely big. But it needs to be agreed on and split into stages
that can be reviewed one at a time. An unannounced megapatch is the one thing
guaranteed to stall.

Practical guidance:

- One logical change per PR. Don't bundle a refactor with a bug fix.
- Mechanical churn (formatting, renames) goes in its own commit or its own PR,
  never mixed into a substantive change — it makes the real diff unreadable.
- If you find yourself touching more than one game profile at once, that is
  usually a sign the change belongs at the shared interface instead.

## AI-assisted contributions

**Using an AI coding assistant is welcome and expected.** Parts of this project
were written that way. There is no stigma here and no disclosure ritual.

There is one requirement, and it is not negotiable:

> **You must understand the code you submit.** If a reviewer asks why a
> particular line is there, "the model wrote it" is not an answer. You are the
> author of the patch regardless of what typed it.

Concretely, that means:

- **Understand the underlying code**, not just the diff. If your change touches
  `RenderService`, you should be able to explain what the pass order is and why
  your change sits where it does. If it touches a format adapter, you should
  know what the on-disk bytes mean.
- **Use proper design patterns and follow the local architecture.** Generated
  code tends to invent its own structure — a new manager class, a new global, a
  parallel code path for one game — where the codebase already has an
  established mechanism. Fit the change into what is there. The conventions in
  [`AGENTS.md`](AGENTS.md) apply to AI-written code exactly as they apply to
  hand-written code.
- **Verify the claims.** AI tools are confident about file formats, field
  meanings, and shader semantics they have actually guessed. This project is
  built on reverse-engineered formats where a plausible-looking wrong constant
  renders fine on one model and breaks fifty others. Check against a real asset
  and say in the PR how you checked.
- **Green tests are not proof.** A passing suite means nothing broke that was
  already covered. If you changed rendering behaviour, look at the render.

PRs that are obviously unreviewed model output — dead code, invented APIs,
comments narrating the obvious, scattershot edits across unrelated files — will
be closed. That is a judgement about the patch, not about the tool.

Note also that [`LICENSE-AI.md`](LICENSE-AI.md) treats AI-generated code derived
from this software as a derivative work under the BSD-3-Clause terms.

## Code conventions

The full list is in [`AGENTS.md`](AGENTS.md). The ones that come up most in
review:

- **The renderer stays host-agnostic.** UI and host policy — file dialogs,
  camera presets, sequence dropdowns, focus handling — belong in
  `tools/basic_viewer/` or `tools/max_plugin/`, never in `RenderService` or
  `SceneManager`. The engine exposes state; hosts decide what to do with it.
- **A game is a new profile, not a branch through an existing one.** Resist
  `if (game == …)` inside shared code.
- **Scope dependencies to the target that needs them.** A new third-party
  library for the viewer is linked into the viewer alone, not pushed up into
  `WhiteoutFlakesLib`.
- **Comments proportional to the code.** Explain *why*, not *what*. No
  multi-paragraph essay above a one-line change.
- **Match the surrounding code.** Mirror the local file's naming, idiom, and
  comment density instead of importing a different house style.
- **Sources are listed explicitly in `CMakeLists.txt`** — there is no globbing.
  A new `.cpp` that isn't added to its target's source list silently never
  compiles.
- **New user-facing strings need all 11 localization catalogs.** Add the key to
  every `resources/lang/<code>.ini` (UTF-8, no BOM) to keep key parity. And
  never pass a translated string as a printf format:
  `ImGui::TextUnformatted(i18n::tr(k))`, not `ImGui::Text(i18n::tr(k))`.

Run `format.bat` (Windows) or clang-format with the repo's `.clang-format`
over `src/`, `include/`, and `tools/` before committing. `.clang-tidy` is in
the repo too.

## Building and testing

Windows / MSVC, the primary toolchain:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target WhiteoutFlakesStandalone
```

The viewer lands at `build/standalone/WhiteoutFlakes.exe`. Clang, MinGW, GCC,
AppleClang and Emscripten are all supported — see the toolchain table in the
README.

Tests are Catch2, headless, and need no GPU, window, or game archive:

```
cmake -S . -B build -DWDX_BUILD_TESTS=ON
cmake --build build --config Release --target wdx_tests --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

`cmake --build build --target check` does the last two in one step, and
`ctest -R <name>` filters by test case.

**Add tests for what you change** where the change is testable headlessly —
format parsing, animation math, skinning, particle and ribbon simulation,
storage and path rules all are. Rendering changes that can only be seen on
screen should come with a description of what you looked at, and a screenshot
where it helps.

Reconfigure CMake after adding or removing source files.

## Submitting

1. Fork and branch off `master`.
2. Keep commits coherent — a reviewer should be able to read them in order.
   Commit messages in this repo are prefixed by area, e.g.
   `Rendering/Sc2: fix particle emitter pre-roll`.
3. Make sure the build is clean and the test suite passes.
4. Open the PR with: what it changes, why, how you verified it, and — for
   format or shader work — what asset you tested against.
5. Expect review comments. They are about the code, and they are how things get
   merged.

## Reporting bugs

A useful report names the **game**, the **asset** (file path inside CASC/MPQ,
or the model name), the **graphics backend** and **OS**, and what you expected
to see versus what was drawn. A screenshot is worth a lot for anything visual.
For a crash, include the log console output (Debug menu) or a stack trace.

## License

By contributing you agree that your contributions are licensed under the
project's BSD 3-Clause License — see [`LICENSE`](LICENSE) and
[`LICENSE-AI.md`](LICENSE-AI.md).

Do not contribute Blizzard game assets, extracted data, or code copied from
proprietary sources. WhiteoutFlakes reads only assets the user already owns,
and it stays that way.
