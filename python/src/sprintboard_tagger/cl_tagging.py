from __future__ import annotations

import hashlib
import json
from collections.abc import Iterator, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from huggingface_hub import HfApi, snapshot_download
from PIL import Image, ImageOps

from .tagging import ImageTagAnalysis, ScoredTag


REPO_ID = "cella110n/cl_tagger_v2"
MODEL_VERSION = "v2_01a"
GENERAL_THRESHOLD = 0.55
CHARACTER_THRESHOLD = 0.55
IMAGE_SIZE = 384

MODEL_FILENAME = "model.onnx"
MODEL_DATA_FILENAME = "model.onnx.data"
VOCABULARY_FILENAME = "model_vocabulary.json"
REQUIRED_FILENAMES = (
    MODEL_FILENAME,
    MODEL_DATA_FILENAME,
    VOCABULARY_FILENAME,
)

KNOWN_CATEGORIES = {
    "artist",
    "character",
    "copyright",
    "general",
    "meta",
    "quality",
    "rating",
}
STANDARD_CATEGORY_IDS = {
    0: "general",
    1: "artist",
    3: "copyright",
    4: "character",
    5: "meta",
    9: "rating",
}


@dataclass(frozen=True, slots=True)
class ResolvedClModel:
    directory: Path
    analyzer_model_id: str

    @property
    def model_path(self) -> Path:
        return self.directory / MODEL_FILENAME

    @property
    def vocabulary_path(self) -> Path:
        return self.directory / VOCABULARY_FILENAME


@dataclass(frozen=True, slots=True)
class ClVocabulary:
    names: tuple[str, ...]
    categories: tuple[str, ...]


def _validate_model_directory(directory: Path) -> Path:
    resolved = directory.expanduser().resolve()
    missing = [
        filename
        for filename in REQUIRED_FILENAMES
        if not (resolved / filename).is_file()
    ]
    if missing:
        raise ValueError(
            f"CL Tagger model directory is missing: {', '.join(missing)}"
        )
    empty = [
        filename
        for filename in REQUIRED_FILENAMES
        if (resolved / filename).stat().st_size == 0
    ]
    if empty:
        raise ValueError(
            f"CL Tagger model files are empty: {', '.join(empty)}"
        )
    return resolved


def _hash_model_files(directory: Path) -> str:
    digest = hashlib.sha256()
    for filename in REQUIRED_FILENAMES:
        digest.update(filename.encode("utf-8"))
        digest.update(b"\0")
        with open(directory / filename, "rb") as model_file:
            while chunk := model_file.read(1024 * 1024):
                digest.update(chunk)
    return digest.hexdigest()


def resolve_local_model(
    directory: Path,
    *,
    model_version: str = MODEL_VERSION,
) -> ResolvedClModel:
    resolved = _validate_model_directory(directory)
    content_digest = _hash_model_files(resolved)
    return ResolvedClModel(
        directory=resolved,
        analyzer_model_id=(
            f"{REPO_ID}:{model_version}@local-sha256:{content_digest}"
        ),
    )


def resolve_hub_model(
    *,
    model_version: str = MODEL_VERSION,
    revision: str = "main",
) -> ResolvedClModel:
    try:
        info = HfApi().model_info(REPO_ID, revision=revision)
        if not info.sha:
            raise RuntimeError("Hugging Face did not return a model revision")
        resolved_revision = info.sha
        snapshot = Path(
            snapshot_download(
                repo_id=REPO_ID,
                revision=resolved_revision,
                allow_patterns=[
                    f"{model_version}/{filename}"
                    for filename in REQUIRED_FILENAMES
                ],
            )
        )
    except Exception as exc:
        raise RuntimeError(
            "Unable to download gated CL Tagger v2 files. Accept the model "
            "license on Hugging Face and run `hf auth login`, or use "
            "--model-dir."
        ) from exc

    directory = _validate_model_directory(snapshot / model_version)
    return ResolvedClModel(
        directory=directory,
        analyzer_model_id=(
            f"{REPO_ID}:{model_version}@{resolved_revision}"
        ),
    )


def _indexed_names(value: object) -> tuple[str, ...]:
    if isinstance(value, list):
        names = value
    elif isinstance(value, Mapping):
        try:
            indexed = {int(index): name for index, name in value.items()}
        except (TypeError, ValueError) as exc:
            raise ValueError("vocabulary indices must be integers") from exc
        if sorted(indexed) != list(range(len(indexed))):
            raise ValueError("vocabulary indices must be contiguous from zero")
        names = [indexed[index] for index in range(len(indexed))]
    else:
        raise ValueError("idx_to_tag must be an array or object")

    if not names or any(not isinstance(name, str) or not name for name in names):
        raise ValueError("vocabulary tag names must be non-empty strings")
    if len(set(names)) != len(names):
        raise ValueError("vocabulary contains duplicate tag names")
    return tuple(names)


def _category_lookup(value: object) -> dict[int, str]:
    if isinstance(value, list):
        return {
            index: str(name).strip().lower()
            for index, name in enumerate(value)
        }
    if not isinstance(value, Mapping):
        return {}

    output: dict[int, str] = {}
    for key, raw_value in value.items():
        try:
            output[int(key)] = str(raw_value).strip().lower()
            continue
        except (TypeError, ValueError):
            pass
        try:
            output[int(raw_value)] = str(key).strip().lower()
        except (TypeError, ValueError):
            continue
    return output


def _normalize_category(
    raw_category: object,
    category_names: Mapping[int, str],
) -> str:
    normalized = (
        raw_category.strip().lower()
        if isinstance(raw_category, str)
        else raw_category
    )
    if isinstance(normalized, str) and not normalized.isdigit():
        category = normalized
    else:
        try:
            category_id = int(normalized)  # type: ignore[arg-type]
        except (TypeError, ValueError) as exc:
            raise ValueError(f"invalid tag category: {raw_category!r}") from exc
        category = category_names.get(
            category_id,
            STANDARD_CATEGORY_IDS.get(category_id, ""),
        )

    category = category.removesuffix("_tags").removesuffix("s")
    if category not in KNOWN_CATEGORIES:
        raise ValueError(f"unsupported tag category: {raw_category!r}")
    return category


def load_vocabulary(path: Path) -> ClVocabulary:
    try:
        with open(path, encoding="utf-8") as vocabulary_file:
            payload = json.load(vocabulary_file)
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"unable to read CL Tagger vocabulary: {exc}") from exc
    if not isinstance(payload, Mapping):
        raise ValueError("CL Tagger vocabulary must be a JSON object")

    names = _indexed_names(payload.get("idx_to_tag"))
    tag_to_index = payload.get("tag_to_idx")
    if tag_to_index is not None:
        try:
            valid_indices = isinstance(tag_to_index, Mapping) and all(
                int(tag_to_index.get(name)) == index
                for index, name in enumerate(names)
            )
        except (TypeError, ValueError):
            valid_indices = False
        if not valid_indices:
            raise ValueError("tag_to_idx does not match idx_to_tag")

    tag_categories = payload.get("tag_to_category")
    if not isinstance(tag_categories, Mapping):
        raise ValueError("tag_to_category must be a JSON object")
    category_names = _category_lookup(payload.get("categories"))
    categories: list[str] = []
    for index, name in enumerate(names):
        raw_category = tag_categories.get(name)
        if raw_category is None:
            raw_category = tag_categories.get(str(index))
        if raw_category is None:
            raise ValueError(f"missing category for tag: {name}")
        categories.append(_normalize_category(raw_category, category_names))

    return ClVocabulary(names=names, categories=tuple(categories))


def prepare_cl_image(image_path: Path) -> np.ndarray:
    with Image.open(image_path) as opened_image:
        image = ImageOps.exif_transpose(opened_image)
        if image.mode == "RGBA" or "transparency" in image.info:
            foreground = image.convert("RGBA")
            background = Image.new("RGBA", foreground.size, "white")
            background.alpha_composite(foreground)
            image = background.convert("RGB")
        else:
            image = image.convert("RGB")
        image = image.resize(
            (IMAGE_SIZE, IMAGE_SIZE),
            Image.Resampling.BICUBIC,
        )
        pixels = np.asarray(image, dtype=np.float32)

    return ((pixels / 255.0 - 0.5) / 0.5).transpose(2, 0, 1)


def _create_session(model_path: Path, device: str) -> tuple[Any, str]:
    try:
        import onnxruntime as ort
    except ImportError as exc:
        raise RuntimeError(
            "ONNX Runtime is not installed; run with `uv run --group cl`"
        ) from exc

    requested = device.lower()
    if requested not in {"auto", "cpu", "cuda"}:
        raise ValueError("device must be auto, cpu, or cuda")
    available = set(ort.get_available_providers())
    if "CPUExecutionProvider" not in available:
        raise RuntimeError("ONNX Runtime CPU provider is unavailable")
    if requested == "cuda" and "CUDAExecutionProvider" not in available:
        raise RuntimeError("ONNX Runtime CUDA provider is unavailable")

    use_cuda = requested != "cpu" and "CUDAExecutionProvider" in available
    providers = (
        ["CUDAExecutionProvider", "CPUExecutionProvider"]
        if use_cuda
        else ["CPUExecutionProvider"]
    )
    try:
        session = ort.InferenceSession(str(model_path), providers=providers)
        if use_cuda and "CUDAExecutionProvider" not in session.get_providers():
            raise RuntimeError("CUDA provider was not activated")
        return session, "cuda" if use_cuda else "cpu"
    except Exception as exc:
        if requested != "auto" or not use_cuda:
            raise RuntimeError(
                f"unable to initialize ONNX Runtime {requested} provider"
            ) from exc
        print(
            "CLTagger: CUDA initialization failed; falling back to CPU: "
            f"{exc}"
        )
        try:
            return (
                ort.InferenceSession(
                    str(model_path), providers=["CPUExecutionProvider"]
                ),
                "cpu",
            )
        except Exception as cpu_exc:
            raise RuntimeError(
                "unable to initialize ONNX Runtime CPU provider"
            ) from cpu_exc


def _select_tags(
    probabilities: np.ndarray,
    indexed_names: Sequence[tuple[int, str]],
    threshold: float | None,
) -> tuple[ScoredTag, ...]:
    selected = (
        ScoredTag(name=name, confidence=float(probabilities[index]))
        for index, name in indexed_names
        if threshold is None or probabilities[index] >= threshold
    )
    return tuple(
        sorted(selected, key=lambda tag: tag.confidence, reverse=True)
    )


class ClTaggerV2:
    def __init__(
        self,
        *,
        resolved_model: ResolvedClModel,
        model_id: str,
        device: str = "auto",
        batch_size: int = 4,
        general_threshold: float = GENERAL_THRESHOLD,
        character_threshold: float = CHARACTER_THRESHOLD,
    ) -> None:
        if model_id != resolved_model.analyzer_model_id:
            raise ValueError("resolved CL Tagger model identity changed")
        if batch_size < 1:
            raise ValueError("batch_size must be at least 1")
        for name, threshold in (
            ("general", general_threshold),
            ("character", character_threshold),
        ):
            if not 0 <= threshold <= 1:
                raise ValueError(f"{name} threshold must be between 0 and 1")

        self.model_id = model_id
        self.batch_size = batch_size
        self.general_threshold = general_threshold
        self.character_threshold = character_threshold
        self.vocabulary = load_vocabulary(resolved_model.vocabulary_path)
        self.rating_tags = tuple(
            (index, name)
            for index, (name, category) in enumerate(
                zip(
                    self.vocabulary.names,
                    self.vocabulary.categories,
                    strict=True,
                )
            )
            if category == "rating"
        )
        self.general_tags = tuple(
            (index, name)
            for index, (name, category) in enumerate(
                zip(
                    self.vocabulary.names,
                    self.vocabulary.categories,
                    strict=True,
                )
            )
            if category in {"general", "copyright"}
        )
        self.character_tags = tuple(
            (index, name)
            for index, (name, category) in enumerate(
                zip(
                    self.vocabulary.names,
                    self.vocabulary.categories,
                    strict=True,
                )
            )
            if category == "character"
        )
        self.session, self.device = _create_session(
            resolved_model.model_path, device
        )

        inputs = self.session.get_inputs()
        outputs = self.session.get_outputs()
        if len(inputs) != 1 or len(outputs) != 1:
            raise ValueError("CL Tagger ONNX model must have one input and output")
        self.input_name = inputs[0].name
        self.output_name = outputs[0].name
        input_shape = inputs[0].shape
        expected_input = (3, IMAGE_SIZE, IMAGE_SIZE)
        if len(input_shape) != 4 or any(
            isinstance(actual, int) and actual != expected
            for actual, expected in zip(
                input_shape[-3:], expected_input, strict=True
            )
        ):
            raise ValueError(
                "CL Tagger input must have shape [batch, 3, 384, 384]"
            )
        output_shape = outputs[0].shape
        if (
            not output_shape
            or not isinstance(output_shape[-1], int)
            or output_shape[-1] != len(self.vocabulary.names)
        ):
            raise ValueError(
                "CL Tagger output width does not match its vocabulary"
            )

    def analyze(
        self,
        image_paths: Sequence[Path],
    ) -> Iterator[ImageTagAnalysis]:
        if not image_paths:
            raise ValueError("No images were provided for tag analysis")

        for start in range(0, len(image_paths), self.batch_size):
            batch_paths = image_paths[start : start + self.batch_size]
            batch = np.stack(
                [prepare_cl_image(path) for path in batch_paths]
            ).astype(np.float32, copy=False)
            logits = np.asarray(
                self.session.run(
                    [self.output_name], {self.input_name: batch}
                )[0]
            )
            expected_shape = (len(batch_paths), len(self.vocabulary.names))
            if logits.shape != expected_shape:
                raise ValueError(
                    f"CL Tagger returned shape {logits.shape}, expected "
                    f"{expected_shape}"
                )
            if not np.isfinite(logits).all():
                raise ValueError("CL Tagger returned non-finite logits")
            probabilities = 1.0 / (
                1.0 + np.exp(-np.clip(logits, -80.0, 80.0))
            )

            for image_path, scores in zip(
                batch_paths, probabilities, strict=True
            ):
                yield ImageTagAnalysis(
                    image_path=image_path,
                    ratings=_select_tags(
                        scores,
                        self.rating_tags,
                        None,
                    ),
                    general_tags=_select_tags(
                        scores,
                        self.general_tags,
                        self.general_threshold,
                    ),
                    character_tags=_select_tags(
                        scores,
                        self.character_tags,
                        self.character_threshold,
                    ),
                )
