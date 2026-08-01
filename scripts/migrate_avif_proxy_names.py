#!/usr/bin/env python3

"""Rename legacy AVIF files to Sprintboard's managed proxy convention.

Stop Sprintboard (or disable AVIF generation) before applying a migration.
This script intentionally treats every non-Sprintboard AVIF as a legacy proxy,
including files that may originally have been native AVIF sources.
"""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
from typing import Callable, Iterable, Sequence


# Edit these values, then double-click the script or run it with Python.
WORKING_DIRECTORIES: list[str] = [
    # r"/path/to/images",
]
DRY_RUN = True
PAUSE_AT_END = True


SPRINTBOARD_PROXY_SUFFIX = ".sprintboard.avif"
OutputFunction = Callable[[str], None]


class AmbiguousPngError(ValueError):
    """Raised when more than one PNG could be the legacy AVIF's source."""


@dataclass
class MigrationStats:
    planned: int = 0
    renamed: int = 0
    skipped: int = 0
    conflicts: int = 0
    errors: int = 0


def is_avif_path(path: Path) -> bool:
    return path.suffix.lower() == ".avif"


def is_sprintboard_proxy_path(path: Path) -> bool:
    name = path.name.lower()
    return len(name) > len(SPRINTBOARD_PROXY_SUFFIX) and name.endswith(
        SPRINTBOARD_PROXY_SUFFIX
    )


def select_target_path(source: Path, sibling_names: Iterable[str]) -> Path:
    """Return the new proxy path, preserving an unambiguous PNG source name."""

    png_names = sorted(
        {
            name
            for name in sibling_names
            if Path(name).suffix.lower() == ".png"
            and Path(name).stem == source.stem
        },
        key=lambda name: (name.casefold(), name),
    )
    if len(png_names) > 1:
        joined = ", ".join(png_names)
        raise AmbiguousPngError(f"multiple same-stem PNG files: {joined}")

    if png_names:
        source_name = png_names[0]
    else:
        source_name = source.with_suffix(".png").name

    if not source_name or source_name in {".", ".."}:
        raise ValueError("could not infer a valid source filename")
    return source.parent / f"{source_name}{SPRINTBOARD_PROXY_SUFFIX}"


def _path_exists(path: Path) -> bool:
    """Return true for every directory entry, including dangling symlinks."""

    return os.path.lexists(path)


def _rename_without_overwrite(source: Path, target: Path) -> None:
    """Rename source without ever replacing a pre-existing target entry."""

    if _path_exists(target):
        raise FileExistsError(f"target already exists: {target}")

    if os.name == "nt":
        # Windows os.rename() fails rather than replacing an existing target.
        os.rename(source, target)
        return

    # POSIX rename() replaces its target. A same-directory hard-link followed
    # by unlink provides no-overwrite behavior while retaining the file inode,
    # contents, and timestamps. If the filesystem cannot create hard links,
    # the source is left untouched and the caller reports the error.
    os.link(source, target, follow_symlinks=False)
    try:
        os.unlink(source)
    except OSError as unlink_error:
        try:
            os.unlink(target)
        except OSError as rollback_error:
            raise RuntimeError(
                "source removal failed and the newly created target could not "
                f"be rolled back: {rollback_error}"
            ) from unlink_error
        raise


def _path_key(path: Path) -> str:
    return os.path.normcase(os.path.abspath(path))


def migrate_directories(
    working_directories: Sequence[str | os.PathLike[str]],
    *,
    dry_run: bool,
    output: OutputFunction = print,
) -> MigrationStats:
    stats = MigrationStats()
    if not working_directories:
        output("[ERROR] WORKING_DIRECTORIES is empty; edit the script first.")
        stats.errors += 1
        return stats

    configured_roots: list[Path] = []
    for directory in working_directories:
        raw_directory = os.fspath(directory)
        if not raw_directory.strip():
            output("[ERROR] A working directory is empty.")
            stats.errors += 1
            continue
        configured_roots.append(Path(raw_directory).expanduser())

    seen_sources: set[str] = set()
    roots = sorted(
        configured_roots,
        key=lambda path: (str(path).casefold(), str(path)),
    )

    for root in roots:
        try:
            if root.is_symlink():
                raise ValueError("working directory must not be a symlink")
            if not root.is_dir():
                raise ValueError("working directory does not exist")
        except OSError as error:
            output(f"[ERROR] {root}: {error}")
            stats.errors += 1
            continue
        except ValueError as error:
            output(f"[ERROR] {root}: {error}")
            stats.errors += 1
            continue

        walk_errors: list[OSError] = []
        for dirpath, dirnames, filenames in os.walk(
            root, topdown=True, onerror=walk_errors.append, followlinks=False
        ):
            dirnames.sort(key=lambda name: (name.casefold(), name))
            filenames.sort(key=lambda name: (name.casefold(), name))
            directory = Path(dirpath)

            regular_names: list[str] = []
            avif_paths: list[Path] = []
            for filename in filenames:
                path = directory / filename
                try:
                    if path.is_symlink():
                        if is_avif_path(path):
                            output(f"[SKIPPED] symbolic link: {path}")
                            stats.skipped += 1
                        continue
                    if not path.is_file():
                        continue
                except OSError as error:
                    output(f"[ERROR] {path}: {error}")
                    stats.errors += 1
                    continue

                regular_names.append(filename)
                if is_avif_path(path):
                    avif_paths.append(path)

            for source in avif_paths:
                source_key = _path_key(source)
                if source_key in seen_sources:
                    continue
                seen_sources.add(source_key)

                if is_sprintboard_proxy_path(source):
                    output(f"[SKIPPED] already migrated: {source}")
                    stats.skipped += 1
                    continue

                try:
                    target = select_target_path(source, regular_names)
                except (AmbiguousPngError, ValueError) as error:
                    output(f"[CONFLICT] {source}: {error}")
                    stats.conflicts += 1
                    continue

                if _path_exists(target):
                    output(f"[CONFLICT] target already exists: {target}")
                    stats.conflicts += 1
                    continue

                stats.planned += 1
                if dry_run:
                    output(f"[DRY RUN] {source} -> {target}")
                    continue

                try:
                    _rename_without_overwrite(source, target)
                except OSError as error:
                    output(f"[ERROR] {source} -> {target}: {error}")
                    stats.errors += 1
                    continue
                except RuntimeError as error:
                    output(f"[ERROR] {source} -> {target}: {error}")
                    stats.errors += 1
                    continue

                output(f"[RENAMED] {source} -> {target}")
                stats.renamed += 1

        for error in walk_errors:
            filename = error.filename or root
            output(f"[ERROR] {filename}: {error}")
            stats.errors += 1

    output("")
    output("Migration summary")
    output(f"  Planned:  {stats.planned}")
    output(f"  Renamed:  {stats.renamed}")
    output(f"  Skipped:  {stats.skipped}")
    output(f"  Conflicts: {stats.conflicts}")
    output(f"  Errors:    {stats.errors}")
    return stats


def main() -> int:
    mode = "DRY RUN" if DRY_RUN else "APPLY"
    print(f"Sprintboard legacy AVIF migration ({mode})")
    if not DRY_RUN:
        print("WARNING: Stop Sprintboard before applying this migration.")
        print("Every legacy AVIF will be reclassified as a managed proxy.")
    print("")

    stats = migrate_directories(WORKING_DIRECTORIES, dry_run=DRY_RUN)
    return 1 if stats.conflicts or stats.errors else 0


if __name__ == "__main__":
    exit_code = main()
    if PAUSE_AT_END:
        try:
            input("\nPress Enter to close...")
        except EOFError:
            pass
    raise SystemExit(exit_code)
