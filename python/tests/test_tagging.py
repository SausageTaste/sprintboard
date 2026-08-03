from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import torch
from PIL import Image

from sprintboard_tagger.tagging import (
    WdEva02Tagger,
    choose_device,
    prepare_image,
    select_tags,
)


class TaggingTests(unittest.TestCase):
    def test_choose_device_prefers_cuda_then_mps_then_cpu(self) -> None:
        with (
            patch(
                "sprintboard_tagger.tagging.torch.cuda.is_available",
                return_value=True,
            ),
            patch(
                "sprintboard_tagger.tagging.torch.backends.mps.is_available"
            ) as mps,
        ):
            self.assertEqual(choose_device(), torch.device("cuda"))
            mps.assert_not_called()

        with (
            patch(
                "sprintboard_tagger.tagging.torch.cuda.is_available",
                return_value=False,
            ),
            patch(
                "sprintboard_tagger.tagging.torch.backends.mps.is_available",
                return_value=True,
            ),
        ):
            self.assertEqual(choose_device(), torch.device("mps"))

    def test_prepare_image_pads_and_converts_to_bgr(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            image_path = Path(temp_dir) / "sample.png"
            Image.new("RGB", (2, 1), (255, 0, 0)).save(image_path)

            def transform(square: Image.Image) -> torch.Tensor:
                pixels = torch.frombuffer(
                    bytearray(square.tobytes()), dtype=torch.uint8
                ).reshape(2, 2, 3)
                return pixels.permute(2, 0, 1)

            tensor = prepare_image(image_path, transform)

        self.assertEqual(tensor.shape, (3, 2, 2))
        self.assertEqual(tensor[:, 0, 0].tolist(), [0, 0, 255])
        self.assertEqual(tensor[:, 1, 0].tolist(), [255, 255, 255])

    def test_select_tags_filters_and_sorts(self) -> None:
        selected = select_tags(
            torch.tensor([0.35, 0.8, 0.9, 0.34]),
            ["boundary", "higher", "other", "below"],
            [0, 0, 4, 0],
            category=0,
            threshold=0.35,
        )
        self.assertEqual([tag.name for tag in selected], ["higher", "boundary"])

    def test_analyze_batches_and_loads_model_once(self) -> None:
        class FakeModel:
            pretrained_cfg: dict[str, object] = {}

            def __init__(self) -> None:
                self.call_count = 0

            def eval(self):
                return self

            def to(self, _device: object):
                return self

            def __call__(self, batch: torch.Tensor) -> torch.Tensor:
                self.call_count += 1
                probabilities = torch.tensor([0.2, 0.8, 0.34, 0.9])
                return torch.logit(probabilities).repeat(len(batch), 1)

        fake_model = FakeModel()
        paths = [Path(f"image-{index}.png") for index in range(3)]
        with (
            patch(
                "sprintboard_tagger.tagging.timm.create_model",
                return_value=fake_model,
            ) as create_model,
            patch(
                "sprintboard_tagger.tagging.load_labels",
                return_value=(
                    ["rating", "general", "below", "character"],
                    [9, 0, 0, 4],
                ),
            ),
            patch(
                "sprintboard_tagger.tagging.resolve_data_config",
                return_value={},
            ),
            patch(
                "sprintboard_tagger.tagging.create_transform",
                return_value=lambda _image: torch.zeros((3, 1, 1)),
            ),
            patch(
                "sprintboard_tagger.tagging.prepare_image",
                return_value=torch.zeros((3, 1, 1)),
            ),
        ):
            tagger = WdEva02Tagger(device="cpu", batch_size=2)
            first = list(tagger.analyze(paths))
            second = list(tagger.analyze(paths[:1]))

        self.assertEqual(len(first), 3)
        self.assertEqual(len(second), 1)
        self.assertEqual(fake_model.call_count, 3)
        create_model.assert_called_once()


if __name__ == "__main__":
    unittest.main()
