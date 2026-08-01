import os
from pathlib import Path
import tempfile
import unittest

from scripts.migrate_avif_proxy_names import (
    AmbiguousPngError,
    migrate_directories,
    select_target_path,
)


class MigrateAvifProxyNamesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        self.output: list[str] = []

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def write_file(self, relative_path: str, contents: bytes = b"avif") -> Path:
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def migrate(self, *, dry_run: bool):
        return migrate_directories(
            [self.root], dry_run=dry_run, output=self.output.append
        )

    def test_dry_run_recurses_without_changing_files(self) -> None:
        source = self.write_file("nested/Émilie.final.AVIF")
        migrated = self.write_file(
            "nested/already.png.SPRINTBOARD.AVIF", b"existing"
        )

        stats = self.migrate(dry_run=True)

        self.assertEqual(stats.planned, 1)
        self.assertEqual(stats.renamed, 0)
        self.assertEqual(stats.skipped, 1)
        self.assertTrue(source.exists())
        self.assertTrue(migrated.exists())
        self.assertFalse(
            (self.root / "nested/Émilie.final.png.sprintboard.avif").exists()
        )

    def test_apply_preserves_existing_png_name_and_file_metadata(self) -> None:
        source = self.write_file("photo.avif", b"proxy contents")
        png = self.write_file("photo.PNG", b"source contents")
        timestamp_ns = 1_700_000_000_123_456_789
        os.utime(source, ns=(timestamp_ns, timestamp_ns))

        stats = self.migrate(dry_run=False)

        target = self.root / "photo.PNG.sprintboard.avif"
        self.assertEqual(stats.planned, 1)
        self.assertEqual(stats.renamed, 1)
        self.assertFalse(source.exists())
        self.assertEqual(target.read_bytes(), b"proxy contents")
        self.assertEqual(target.stat().st_mtime_ns, timestamp_ns)
        self.assertEqual(png.read_bytes(), b"source contents")

    def test_apply_infers_lowercase_png_name_for_orphan(self) -> None:
        source = self.write_file("multi.part.AVIF", b"orphan")

        stats = self.migrate(dry_run=False)

        target = self.root / "multi.part.png.sprintboard.avif"
        self.assertEqual(stats.renamed, 1)
        self.assertFalse(source.exists())
        self.assertEqual(target.read_bytes(), b"orphan")

    def test_select_target_rejects_ambiguous_png_names(self) -> None:
        source = self.root / "photo.avif"

        with self.assertRaises(AmbiguousPngError):
            select_target_path(source, ["photo.png", "photo.PNG"])

    def test_target_collision_never_overwrites_either_file(self) -> None:
        source = self.write_file("photo.avif", b"legacy")
        target = self.write_file(
            "photo.png.sprintboard.avif", b"already migrated"
        )

        stats = self.migrate(dry_run=False)

        self.assertEqual(stats.planned, 0)
        self.assertEqual(stats.renamed, 0)
        self.assertEqual(stats.conflicts, 1)
        self.assertEqual(source.read_bytes(), b"legacy")
        self.assertEqual(target.read_bytes(), b"already migrated")

    def test_empty_and_missing_working_directories_are_errors(self) -> None:
        empty_stats = migrate_directories(
            [], dry_run=True, output=self.output.append
        )
        missing_stats = migrate_directories(
            [self.root / "missing"],
            dry_run=True,
            output=self.output.append,
        )
        blank_stats = migrate_directories(
            [""], dry_run=True, output=self.output.append
        )

        self.assertEqual(empty_stats.errors, 1)
        self.assertEqual(missing_stats.errors, 1)
        self.assertEqual(blank_stats.errors, 1)

    def test_does_not_follow_directory_symlinks(self) -> None:
        outside = self.root.parent / f"{self.root.name}-outside"
        outside.mkdir()
        try:
            source = outside / "outside.avif"
            source.write_bytes(b"outside")
            try:
                (self.root / "linked").symlink_to(outside, target_is_directory=True)
            except OSError as error:
                self.skipTest(f"directory symlinks unavailable: {error}")

            stats = self.migrate(dry_run=False)

            self.assertEqual(stats.planned, 0)
            self.assertTrue(source.exists())
            self.assertFalse(
                (outside / "outside.png.sprintboard.avif").exists()
            )
        finally:
            for path in outside.iterdir():
                path.unlink()
            outside.rmdir()


if __name__ == "__main__":
    unittest.main()
