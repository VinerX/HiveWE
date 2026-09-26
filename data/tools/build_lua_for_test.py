from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Assemble and syntax-check a map's split Lua before HiveWE launches Warcraft III."
    )
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    project_root = args.project_root.resolve()
    builder = project_root / "build_map_lua.py"
    split_dir = project_root / "map.w3x" / "_lua" / "monolith_split"
    output = args.output.resolve()

    if not builder.is_file():
        print(f"Lua build script not found: {builder}", file=sys.stderr)
        return 2
    if not (split_dir / "manifest.json").is_file():
        print(f"Lua source manifest not found: {split_dir / 'manifest.json'}", file=sys.stderr)
        return 2

    sys.path.insert(0, str(project_root))
    try:
        from build_map_lua import rebuild_from_manifest
        from luaparser import ast
    except ImportError as error:
        print(f"Lua build dependencies are unavailable: {error}", file=sys.stderr)
        return 2

    try:
        script = rebuild_from_manifest(split_dir)
        ast.parse(script)
    except Exception as error:
        print(f"Lua build or syntax check failed: {error}", file=sys.stderr)
        return 1

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".hivewe-tmp")
    try:
        temporary.write_text(script, encoding="utf-8", newline="\n")
        os.replace(temporary, output)
    finally:
        if temporary.exists():
            temporary.unlink()

    print(f"Lua rebuilt and syntax-checked: {output}")
    print(f"Lines: {script.count(chr(10)) + 1}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
