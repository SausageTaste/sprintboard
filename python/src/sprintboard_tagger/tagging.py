from __future__ import annotations

import csv
from collections.abc import Callable, Iterator, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import timm
import torch
from huggingface_hub import hf_hub_download
from PIL import Image, ImageOps
from timm.data import create_transform, resolve_data_config


MODEL_ID = "SmilingWolf/wd-eva02-large-tagger-v3"
GENERAL_THRESHOLD = 0.35
CHARACTER_THRESHOLD = 0.75

RATING_CATEGORY = 9
GENERAL_CATEGORY = 0
CHARACTER_CATEGORY = 4


@dataclass(frozen=True, slots=True)
class ScoredTag:
    name: str
    confidence: float


@dataclass(frozen=True, slots=True)
class ImageTagAnalysis:
    image_path: Path
    ratings: tuple[ScoredTag, ...]
    general_tags: tuple[ScoredTag, ...]
    character_tags: tuple[ScoredTag, ...]


def choose_device(device: str | torch.device = "auto") -> torch.device:
    if str(device) != "auto":
        return torch.device(device)
    if torch.cuda.is_available():
        return torch.device("cuda")
    if torch.backends.mps.is_available():
        return torch.device("mps")
    return torch.device("cpu")


def load_labels(model_id: str = MODEL_ID) -> tuple[list[str], list[int]]:
    csv_path = hf_hub_download(
        repo_id=model_id,
        filename="selected_tags.csv",
    )

    names: list[str] = []
    categories: list[int] = []
    with open(csv_path, encoding="utf-8", newline="") as csv_file:
        for row in csv.DictReader(csv_file):
            names.append(row["name"])
            categories.append(int(row["category"]))
    return names, categories


def prepare_image(
    image_path: Path,
    transform: Callable[[Image.Image], torch.Tensor],
) -> torch.Tensor:
    with Image.open(image_path) as opened_image:
        image = ImageOps.exif_transpose(opened_image)

        if image.mode not in {"RGB", "RGBA"}:
            image = (
                image.convert("RGBA")
                if "transparency" in image.info
                else image.convert("RGB")
            )

        if image.mode == "RGBA":
            background = Image.new("RGBA", image.size, (255, 255, 255, 255))
            background.alpha_composite(image)
            image = background.convert("RGB")

        width, height = image.size
        square_size = max(width, height)
        square = Image.new("RGB", (square_size, square_size), (255, 255, 255))
        square.paste(
            image,
            ((square_size - width) // 2, (square_size - height) // 2),
        )

    image_tensor = transform(square)
    return image_tensor[[2, 1, 0]]


def select_tags(
    probabilities: torch.Tensor,
    names: Sequence[str],
    categories: Sequence[int],
    category: int,
    threshold: float | None = None,
) -> tuple[ScoredTag, ...]:
    selected = (
        ScoredTag(name=name, confidence=score.item())
        for name, score, tag_category in zip(
            names,
            probabilities,
            categories,
            strict=True,
        )
        if tag_category == category
        and (threshold is None or bool(score >= threshold))
    )
    return tuple(
        sorted(selected, key=lambda tag: tag.confidence, reverse=True)
    )


class WdEva02Tagger:
    """Reusable, lazily loaded WD EVA02-Large Tagger v3 service."""

    def __init__(
        self,
        *,
        model_id: str = MODEL_ID,
        device: str | torch.device = "auto",
        batch_size: int = 4,
        general_threshold: float = GENERAL_THRESHOLD,
        character_threshold: float = CHARACTER_THRESHOLD,
    ) -> None:
        if batch_size < 1:
            raise ValueError("batch_size must be at least 1")

        self.model_id = model_id
        self.device = choose_device(device)
        self.batch_size = batch_size
        self.general_threshold = general_threshold
        self.character_threshold = character_threshold

        self._model: Any | None = None
        self._transform: Callable[[Image.Image], torch.Tensor] | None = None
        self._names: list[str] | None = None
        self._categories: list[int] | None = None

    def _ensure_loaded(self) -> None:
        if self._model is not None:
            return

        model = timm.create_model(
            f"hf_hub:{self.model_id}",
            pretrained=True,
        ).eval()
        names, categories = load_labels(self.model_id)
        transform = create_transform(
            **resolve_data_config(model.pretrained_cfg, model=model)
        )

        self._model = model.to(self.device)
        self._transform = transform
        self._names = names
        self._categories = categories

    def analyze(
        self,
        image_paths: Sequence[Path],
    ) -> Iterator[ImageTagAnalysis]:
        if not image_paths:
            raise ValueError("No images were provided for tag analysis")

        self._ensure_loaded()
        assert self._model is not None
        assert self._transform is not None
        assert self._names is not None
        assert self._categories is not None

        for start in range(0, len(image_paths), self.batch_size):
            batch_paths = image_paths[start : start + self.batch_size]
            batch = torch.stack(
                [prepare_image(path, self._transform) for path in batch_paths]
            ).to(self.device)

            with torch.inference_mode():
                batch_probabilities = torch.sigmoid(self._model(batch)).cpu()

            for image_path, probabilities in zip(
                batch_paths,
                batch_probabilities,
                strict=True,
            ):
                yield ImageTagAnalysis(
                    image_path=image_path,
                    ratings=select_tags(
                        probabilities,
                        self._names,
                        self._categories,
                        RATING_CATEGORY,
                    ),
                    general_tags=select_tags(
                        probabilities,
                        self._names,
                        self._categories,
                        GENERAL_CATEGORY,
                        self.general_threshold,
                    ),
                    character_tags=select_tags(
                        probabilities,
                        self._names,
                        self._categories,
                        CHARACTER_CATEGORY,
                        self.character_threshold,
                    ),
                )
