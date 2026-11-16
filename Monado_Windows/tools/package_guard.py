"""Compatibility entry point for the strict reviewed-source publication guard.

Usage: package_guard.py <clean-reviewed-export> [<clean-reviewed-export> ...]
The old --git scan is refused: it checked mutable working files rather than the
reviewed publication tree. Export reviewed Windows sources first, then scan that
complete tree. No player input or private build directory should be supplied.
"""
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True


def scan(root):
    """Apply the current whole-tree manifest, path and content policy."""
    helper = Path(__file__).resolve().parents[2] / 'tools/publication_guard.py'
    try:
        spec = importlib.util.spec_from_file_location('_windows_publication_guard', helper)
        if spec is None or spec.loader is None:
            return ['current publication guard is unavailable; use a complete reviewed source kit']
        guard = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(guard)
        return guard.verify_tree(root)
    except (OSError, ValueError, ImportError) as error:
        return ['current publication guard could not check the complete tree: ' + str(error)]


def scan_tracked(repo):
    """Do not retain the old partial Git/worktree publication approval path."""
    return ['--git is no longer publication approval; create a clean reviewed source '
            'export with tools/export_windows_sources.py and check that complete export']


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        print(__doc__)
        return 1
    bad = scan_tracked(argv[1] if len(argv) > 1 else '.') if argv[0] == '--git' else []
    if argv[0] != '--git':
        for root in argv:
            bad += scan(root)
    for reason in bad:
        print('REJECT:', reason)
    if not bad:
        print('PASS: exact reviewed source publication guard; provenance review still required')
    return bool(bad)


if __name__ == '__main__':
    sys.exit(main())
