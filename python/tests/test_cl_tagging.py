from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np
from PIL import Image

from sprintboard_tagger.cl_cli import make_parser
from sprintboard_tagger.cl_tagging import (
    MODEL_DATA_FILENAME,
    MODEL_FILENAME,
    VOCABULARY_FILENAME,
    ClTaggerV2,
    load_vocabulary,
    prepare_cl_image,
    resolve_hub_model,
    resolve_local_model,
)


def write_model_files(directory: Path) -> None:
    (directory / MODEL_FILENAME).write_bytes(b"onnx")
    (directory / MODEL_DATA_FILENAME).write_bytes(b"weights")
    (directory / VOCABULARY_FILENAME).write_text(
        json.dumps(
            {
                "idx_to_tag": {
                    "0": "safe",
                    "1": "blue_hair",
                    "2": "new_series",
                    "3": "new_character",
                    "4": "metadata",
                    "5": "best_quality",
                },
                "tag_to_idx": {
                    "safe": 0,
                    "blue_hair": 1,
                    "new_series": 2,
                    "new_character": 3,
                    "metadata": 4,
                    "best_quality": 5,
                },
                "tag_to_category": {
                    "safe": 0,
                    "blue_hair": 1,
                    "new_series": 2,
                    "new_character": 3,
                    "metadata": 4,
                    "best_quality": 5,
                },
                "categories": {
                    "rating": 0,
                    "general": 1,
                    "copyright": 2,
                    "character": 3,
                    "meta": 4,
                    "quality": 5,
                },
            }
        ),
        encoding="utf-8",
    )


class FakeNode:
    def __init__(self, name: str, shape: list[object]) -> None:
        self.name = name
        self.shape = shape


class FakeSession:
    def __init__(self) -> None:
        probabilities = np.array(
            [0.9, 0.6, 0.8, 0.7, 0.99, 0.98], dtype=np.float32
        )
        self.logits = np.log(probabilities / (1.0 - probabilities))
        self.calls = 0

    def get_inputs(self) -> list[FakeNode]:
        return [FakeNode("pixel_values", [None, 3, 384, 384])]

    def get_outputs(self) -> list[FakeNode]:
        return [FakeNode("logits", [None, 6])]

    def run(
        self,
        output_names: list[str],
        inputs: dict[str, np.ndarray],
    ) -> list[np.ndarray]:
        self.calls += 1
        if output_names != ["logits"] or set(inputs) != {"pixel_values"}:
            raise AssertionError("unexpected ONNX input or output name")
        return [np.tile(self.logits, (len(inputs["pixel_values"]), 1))]


class ClTaggingTests(unittest.TestCase):
    def test_preprocesses_rgb_and_transparency(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            rgb_path = root / "rgb.png"
            alpha_path = root / "alpha.png"
            Image.new("RGB", (2, 1), (255, 0, 127)).save(rgb_path)
            Image.new("RGBA", (2, 1), (255, 0, 0, 0)).save(alpha_path)

            rgb = prepare_cl_image(rgb_path)
            alpha = prepare_cl_image(alpha_path)

        self.assertEqual(rgb.shape, (3, 384, 384))
        self.assertEqual(rgb.dtype, np.float32)
        np.testing.assert_allclose(rgb[:, 0, 0], [1.0, -1.0, -1 / 255])
        np.testing.assert_allclose(alpha[:, 0, 0], [1.0, 1.0, 1.0])

    def test_loads_and_validates_vocabulary(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_model_files(root)
            vocabulary = load_vocabulary(root / VOCABULARY_FILENAME)

            malformed = root / "malformed.json"
            malformed.write_text(
                json.dumps(
                    {
                        "idx_to_tag": {"1": "not_zero"},
                        "tag_to_category": {"not_zero": "general"},
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "contiguous"):
                load_vocabulary(malformed)

        self.assertEqual(vocabulary.names[3], "new_character")
        self.assertEqual(vocabulary.categories[2], "copyright")

    def test_resolves_local_model_with_content_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_model_files(root)
            first = resolve_local_model(root)
            (root / MODEL_DATA_FILENAME).write_bytes(b"changed weights")
            second = resolve_local_model(root)

        self.assertNotEqual(first.analyzer_model_id, second.analyzer_model_id)
        self.assertIn("@local-sha256:", first.analyzer_model_id)

    def test_local_model_requires_all_nonempty_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / MODEL_FILENAME).write_bytes(b"onnx")
            with self.assertRaisesRegex(ValueError, MODEL_DATA_FILENAME):
                resolve_local_model(root)

            write_model_files(root)
            (root / MODEL_DATA_FILENAME).write_bytes(b"")
            with self.assertRaisesRegex(ValueError, "empty"):
                resolve_local_model(root)

    def test_hub_model_uses_resolved_revision(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            snapshot = Path(temp_dir)
            version_dir = snapshot / "v2_01a"
            version_dir.mkdir()
            write_model_files(version_dir)
            model_info = SimpleNamespace(sha="resolved-commit")

            with (
                patch(
                    "sprintboard_tagger.cl_tagging.HfApi.model_info",
                    return_value=model_info,
                ),
                patch(
                    "sprintboard_tagger.cl_tagging.snapshot_download",
                    return_value=str(snapshot),
                ) as download,
            ):
                resolved = resolve_hub_model(revision="main")

        self.assertTrue(
            resolved.analyzer_model_id.endswith("@resolved-commit")
        )
        self.assertEqual(download.call_args.kwargs["revision"], "resolved-commit")
        self.assertIn(
            "v2_01a/model.onnx.data",
            download.call_args.kwargs["allow_patterns"],
        )

    def test_analyzes_batches_and_maps_searchable_categories(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_model_files(root)
            resolved = resolve_local_model(root)
            session = FakeSession()
            paths = [Path(f"image-{index}.png") for index in range(3)]

            with (
                patch(
                    "sprintboard_tagger.cl_tagging._create_session",
                    return_value=(session, "cpu"),
                ),
                patch(
                    "sprintboard_tagger.cl_tagging.prepare_cl_image",
                    return_value=np.zeros((3, 384, 384), dtype=np.float32),
                ),
            ):
                tagger = ClTaggerV2(
                    resolved_model=resolved,
                    model_id=resolved.analyzer_model_id,
                    device="cpu",
                    batch_size=2,
                    general_threshold=0.55,
                    character_threshold=0.55,
                )
                analyses = list(tagger.analyze(paths))

        self.assertEqual(session.calls, 2)
        self.assertEqual([tag.name for tag in analyses[0].ratings], ["safe"])
        self.assertEqual(
            [tag.name for tag in analyses[0].general_tags],
            ["new_series", "blue_hair"],
        )
        self.assertEqual(
            [tag.name for tag in analyses[0].character_tags],
            ["new_character"],
        )
        all_names = {
            tag.name
            for analysis in analyses
            for group in (
                analysis.ratings,
                analysis.general_tags,
                analysis.character_tags,
            )
            for tag in group
        }
        self.assertNotIn("metadata", all_names)
        self.assertNotIn("best_quality", all_names)

    def test_validates_onnx_output_width(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_model_files(root)
            resolved = resolve_local_model(root)
            session = FakeSession()
            session.get_outputs = lambda: [FakeNode("logits", [None, 5])]

            with patch(
                "sprintboard_tagger.cl_tagging._create_session",
                return_value=(session, "cpu"),
            ):
                with self.assertRaisesRegex(ValueError, "output width"):
                    ClTaggerV2(
                        resolved_model=resolved,
                        model_id=resolved.analyzer_model_id,
                    )

    def test_provider_selection_and_fallback(self) -> None:
        calls: list[list[str]] = []

        def inference_session(
            _path: str, *, providers: list[str]
        ) -> object:
            calls.append(providers)
            if providers[0] == "CUDAExecutionProvider":
                raise RuntimeError("missing CUDA DLL")
            return SimpleNamespace(get_providers=lambda: providers)

        fake_ort = SimpleNamespace(
            get_available_providers=lambda: [
                "CUDAExecutionProvider",
                "CPUExecutionProvider",
            ],
            InferenceSession=inference_session,
        )
        with patch.dict(sys.modules, {"onnxruntime": fake_ort}):
            from sprintboard_tagger.cl_tagging import _create_session

            _session, device = _create_session(Path("model.onnx"), "auto")

        self.assertEqual(device, "cpu")
        self.assertEqual(
            calls,
            [
                ["CUDAExecutionProvider", "CPUExecutionProvider"],
                ["CPUExecutionProvider"],
            ],
        )

        cpu_only = SimpleNamespace(
            get_available_providers=lambda: ["CPUExecutionProvider"],
            InferenceSession=inference_session,
        )
        with patch.dict(sys.modules, {"onnxruntime": cpu_only}):
            with self.assertRaisesRegex(RuntimeError, "CUDA provider"):
                _create_session(Path("model.onnx"), "cuda")

    def test_cli_defaults_and_local_override(self) -> None:
        defaults = make_parser().parse_args([])
        local = make_parser().parse_args(
            ["--model-dir", "models/cl", "--device", "cpu"]
        )

        self.assertEqual(defaults.model_version, "v2_01a")
        self.assertEqual(defaults.general_threshold, 0.55)
        self.assertEqual(local.model_dir, Path("models/cl"))
        self.assertEqual(local.device, "cpu")


if __name__ == "__main__":
    unittest.main()
