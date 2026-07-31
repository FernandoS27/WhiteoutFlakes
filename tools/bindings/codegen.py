# SPDX-License-Identifier: BSD-3-Clause
"""Binding codegen for the WhiteoutFlakes renderer service library.

    python -m tools.bindings.codegen flakes --backend rust
    python -m tools.bindings.codegen flakes            # the whole Rust chain

The parser, IR and emitters are WhiteoutLib's, imported from the submodule
at `externals/WhiteoutLib/tools/codegen/` — see BINDINGS.md §3 for why we
reuse rather than fork. What lives here is the flakes-specific part: the
module configs under `modules/`, and the output layout below.

We drive the emitters directly instead of shelling out to upstream's CLI
for two reasons: its `_load_module_config` resolves configs only under
`tools.codegen.modules`, and its Rust output path is hardcoded to
WhiteoutLib's own crate. Both are one-line assumptions upstream, not
extension points.

Rust is the only backend wired up today. The C ABI is not a deliverable of
its own — it is the layer the Rust crate calls through, so `--backend all`
regenerates it alongside.
"""

from __future__ import annotations

import argparse
import importlib
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WHITEOUT_LIB = REPO_ROOT / 'externals' / 'WhiteoutLib'

if not (WHITEOUT_LIB / 'tools' / 'codegen' / 'codegen.py').is_file():
    raise SystemExit(
        f'WhiteoutLib codegen not found under {WHITEOUT_LIB}.\n'
        'Run: git submodule update --init externals/WhiteoutLib')

# Ahead of the repo root: both trees have a `tools` package, and the
# emitters import each other as `tools.codegen.*`.
sys.path.insert(0, str(WHITEOUT_LIB))

# Rust crate root. The generated per-module file lands in its `src/`
# alongside the hand-written support code.
RUST_CRATE = 'bindings/rust/whiteoutflakes'

# Runtime types (whiteout_Bytes / whiteout_CString) plus the shared math
# accessors. Module-independent, emitted once.
C_COMMON_HEADER = 'bindings/c/whiteout_c_common.h'
C_COMMON_SOURCE = 'bindings/c/whiteout_c_common.cpp'

BACKENDS = ('c-common-header', 'c-common', 'c-header', 'c-source', 'rust')


def _load_config(name: str):
    return importlib.import_module(f'tools.bindings.modules.{name}').CONFIG


def _write(rel: str, text: str) -> None:
    path = REPO_ROOT / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    # Explicit open() rather than write_text(newline=...): the latter is
    # 3.10+, and codegen should keep running on older interpreters.
    with open(path, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(text)
    print(f'  {rel} ({len(text)} bytes, {len(text.splitlines())} lines)')


def _run(backend: str, config, module) -> None:
    """Emit one backend. `module` is None for the module-independent ones."""
    from tools.codegen import emit_c

    if backend == 'c-common-header':
        _write(C_COMMON_HEADER, emit_c.emit_common_header())
    elif backend == 'c-common':
        _write(C_COMMON_SOURCE, emit_c.emit_common())
    elif backend == 'c-header':
        _write(config.c_header_output_path, emit_c.emit_header(module))
    elif backend == 'c-source':
        _write(config.c_source_output_path, emit_c.emit_source(module))
    elif backend == 'rust':
        from tools.codegen import emit_rust
        _write(f'{RUST_CRATE}/src/{config.name}.rs',
               emit_rust.emit_module(module))


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('module', nargs='?', default='flakes',
                   help='Module under tools/bindings/modules/ (default: flakes)')
    p.add_argument('--backend', choices=BACKENDS + ('all',), default='all',
                   help='"all" runs the full Rust chain (default)')
    args = p.parse_args(argv)

    backends = BACKENDS if args.backend == 'all' else (args.backend,)

    # A libclang parse of the umbrella TU costs ~40s, so do it once and
    # feed every module-dependent emitter from the same IR.
    config = _load_config(args.module)
    module = None
    if any(b not in ('c-common', 'c-common-header') for b in backends):
        print(f'== parse {config.name} ==')
        from tools.codegen.parser import parse_module
        module = parse_module(config, REPO_ROOT)

    for backend in backends:
        print(f'== {backend} ==')
        _run(backend, config, module)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
