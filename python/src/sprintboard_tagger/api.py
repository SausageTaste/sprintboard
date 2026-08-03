from __future__ import annotations

import hashlib
import json
import threading
from collections.abc import Callable, Iterator, Sequence
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Protocol

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

from .tagging import (
    CHARACTER_THRESHOLD,
    GENERAL_THRESHOLD,
    MODEL_ID,
    ImageTagAnalysis,
    WdEva02Tagger,
)


PROTOCOL_VERSION = 1


@dataclass(frozen=True, slots=True)
class ServiceSettings:
    model_id: str = MODEL_ID
    device: str = "auto"
    batch_size: int = 4
    general_threshold: float = GENERAL_THRESHOLD
    character_threshold: float = CHARACTER_THRESHOLD

    def fingerprint(self) -> str:
        payload = {
            "protocolVersion": PROTOCOL_VERSION,
            "modelId": self.model_id,
            "generalThreshold": self.general_threshold,
            "characterThreshold": self.character_threshold,
        }
        serialized = json.dumps(
            payload,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        )
        return hashlib.sha256(serialized.encode("utf-8")).hexdigest()


class AnalyzeRequest(BaseModel):
    paths: list[str] = Field(min_length=1)


class ImageTagger(Protocol):
    device: object

    def analyze(
        self,
        image_paths: Sequence[Path],
    ) -> Iterator[ImageTagAnalysis]: ...


TaggerFactory = Callable[..., ImageTagger]


def _tag_json(tag: object) -> dict[str, object]:
    return asdict(tag)  # type: ignore[arg-type]


def _analysis_json(analysis: ImageTagAnalysis) -> dict[str, object]:
    return {
        "path": str(analysis.image_path),
        "ratings": [_tag_json(tag) for tag in analysis.ratings],
        "generalTags": [_tag_json(tag) for tag in analysis.general_tags],
        "characterTags": [_tag_json(tag) for tag in analysis.character_tags],
    }


class AnalysisService:
    def __init__(
        self,
        settings: ServiceSettings,
        tagger_factory: TaggerFactory,
    ) -> None:
        self.settings = settings
        self.fingerprint = settings.fingerprint()
        self.tagger = tagger_factory(
            model_id=settings.model_id,
            device=settings.device,
            batch_size=settings.batch_size,
            general_threshold=settings.general_threshold,
            character_threshold=settings.character_threshold,
        )
        self.lock = threading.Lock()

    def _analyze_isolated(
        self,
        paths: Sequence[Path],
    ) -> list[dict[str, object]]:
        if not paths:
            return []
        try:
            analyses = list(self.tagger.analyze(paths))
            by_path = {analysis.image_path: analysis for analysis in analyses}
            return [_analysis_json(by_path[path]) for path in paths]
        except Exception as exc:
            if len(paths) == 1:
                return [{"path": str(paths[0]), "error": str(exc)}]
            middle = len(paths) // 2
            return self._analyze_isolated(
                paths[:middle]
            ) + self._analyze_isolated(paths[middle:])

    def analyze(self, path_strings: Sequence[str]) -> list[dict[str, object]]:
        validated: list[Path] = []
        indexed_results: dict[int, dict[str, object]] = {}
        valid_indices: list[int] = []

        for index, path_string in enumerate(path_strings):
            path = Path(path_string)
            if not path.is_absolute():
                indexed_results[index] = {
                    "path": path_string,
                    "error": "path must be absolute",
                }
            elif not path.is_file():
                indexed_results[index] = {
                    "path": path_string,
                    "error": "path must reference a regular file",
                }
            else:
                validated.append(path)
                valid_indices.append(index)

        if validated:
            with self.lock:
                analyzed = self._analyze_isolated(validated)
            for index, result in zip(valid_indices, analyzed, strict=True):
                # Paths are opaque correlation values in the protocol. Path
                # stringification changes separators on Windows, so return the
                # request spelling verbatim for the C++ client to match.
                result["path"] = path_strings[index]
                indexed_results[index] = result

        return [indexed_results[index] for index in range(len(path_strings))]


def create_app(
    settings: ServiceSettings | None = None,
    *,
    tagger_factory: TaggerFactory = WdEva02Tagger,
) -> FastAPI:
    resolved = settings or ServiceSettings()
    if resolved.batch_size < 1:
        raise ValueError("batch_size must be at least 1")

    service = AnalysisService(resolved, tagger_factory)
    app = FastAPI(title="Sprintboard Tagger", version="1")

    @app.get("/v1/info")
    def info() -> dict[str, object]:
        return {
            "protocolVersion": PROTOCOL_VERSION,
            "fingerprint": service.fingerprint,
            "modelId": resolved.model_id,
            "device": str(service.tagger.device),
            "generalThreshold": resolved.general_threshold,
            "characterThreshold": resolved.character_threshold,
        }

    @app.post("/v1/analyze")
    def analyze(request: AnalyzeRequest) -> dict[str, object]:
        if len(request.paths) > resolved.batch_size:
            raise HTTPException(
                status_code=413,
                detail=f"batch contains more than {resolved.batch_size} paths",
            )
        return {
            "protocolVersion": PROTOCOL_VERSION,
            "fingerprint": service.fingerprint,
            "results": service.analyze(request.paths),
        }

    return app
