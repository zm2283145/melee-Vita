# SPDX-License-Identifier: GPL-3.0-or-later
"""Apply the 3DS Slippi port's determinism edits to this tree's game sources.

The 3DS port (melee-3ds, slippi-beta-v0.1.0) keeps its changes to the decomp
as (old text, new text, count) tuples in tools/slippi_edits/*.py and applies
them at build time.  This tree keeps the decomp sources checked in, so the
edits are applied once, here, and committed.

    python3 tools/slippi/apply_3ds_edits.py <melee-3ds checkout>/native-3ds [--dry-run]

Paths in the 3DS files are relative to the decomp: melee/..., sysdolphin/...
and MSL/... live under src/ here.  extern/dolphin/src/dolphin/mtx/*.c is the
Dolphin SDK's C matrix code, which aurora's extern/aurora/lib/dolphin/mtx
replaces on this port.  The GX files only draw, so they are skipped.

Every edit prints one line: applied, already applied, or NOT FOUND.  An edit
that isn't found has to be ported by hand, because this tree's decomp
(src/UPSTREAM_COMMIT) is newer than the 3DS one and has PC-port changes.
"""
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Which edit tables to apply, from tools/slippi_edits/ in the 3DS checkout.
# Same order as the 3DS build (tools/slippi_mods.py: modules by name, and
# determinism.py puts HAND before PORTED); some PORTED edits change text
# right next to a HAND edit.
TABLES = [
    ('decomp_fixes.py', 'FIXES'),
    ('determinism.py', 'HAND'),
    ('determinism_ported.py', 'PORTED'),
    # The call sites of Slippi's gameplay codes (src/melee/slippi/slippi_rules.c).
    ('online_rules.py', 'FIXES'),
]

# Names that differ between the 3DS tree and this one, fixed in the new text.
RENAMES = [
    ('#include <slippi_rules.h>\n', '#include <melee/slippi/slippi_rules.h>\n'),
    ('*seed_ptr', '*HSD_RandSeedPtr'),
]

# Edits this tree already has, or doesn't want, with the reason.
SKIP = {
    'MSL/trigf.c': 'done by hand in src/MSL/trigf.c (#ifdef TARGET_PC)',
    'extern/dolphin/src/dolphin/gx/GXDraw.c': 'rendering only',
    'extern/dolphin/src/dolphin/gx/GXLight.c': 'rendering only',
}


def load_table(directory, filename, name):
    spec = importlib.util.spec_from_file_location(filename[:-3], directory / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return getattr(module, name)


def local_path(edit_path):
    if edit_path.startswith('extern/dolphin/src/dolphin/mtx/'):
        return ROOT / 'extern/aurora/lib/dolphin/mtx' / Path(edit_path).name
    return ROOT / 'src' / edit_path


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    native = Path(sys.argv[1])
    dry_run = '--dry-run' in sys.argv
    edits_dir = native / 'tools' / 'slippi_edits'
    # determinism.py imports its siblings by name.
    sys.path.insert(0, str(native / 'tools'))
    sys.path.insert(0, str(edits_dir))

    applied = 0
    already = 0
    missing = 0
    for filename, name in TABLES:
        table = load_table(edits_dir, filename, name)
        for edit_path, edits in table.items():
            if edit_path in SKIP:
                print(f'skip     {edit_path}: {SKIP[edit_path]}')
                continue
            path = local_path(edit_path)
            if not path.exists():
                print(f'NO FILE  {edit_path} ({path.relative_to(ROOT)})')
                missing += len(edits)
                continue
            text = path.read_text(encoding='utf-8')
            for old, new, count in edits:
                for theirs, ours in RENAMES:
                    new = new.replace(theirs, ours)
                first_line = old.strip().splitlines()[0][:70]
                # Check "already there" first: an edit that only adds lines
                # next to its anchor still finds the anchor after applying.
                if new in text:
                    already += 1
                    print(f'already  {edit_path}: {first_line}')
                elif text.count(old) == count:
                    text = text.replace(old, new)
                    applied += 1
                    print(f'applied  {edit_path}: {first_line}')
                else:
                    missing += 1
                    found = text.count(old)
                    print(f'NOT FOUND {edit_path} (found {found}, want {count}): {first_line}')
            if not dry_run:
                path.write_text(text, encoding='utf-8')

    print(f'\n{applied} applied, {already} already there, {missing} need porting by hand')
    return 0


if __name__ == '__main__':
    sys.exit(main())
