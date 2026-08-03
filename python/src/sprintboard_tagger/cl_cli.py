from __future__ import annotations

import argparse
from pathlib import Path

import uvicorn

from .api import ServiceSettings, create_app
from .cl_tagging import (
    CHARACTER_THRESHOLD,
    GENERAL_THRESHOLD,
    MODEL_VERSION,
    ClTaggerV2,
    resolve_hub_model,
    resolve_local_model,
)


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run Sprintboard's local CL Tagger v2 service."
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=8790, type=int)
    parser.add_argument("--model-version", default=MODEL_VERSION)
    parser.add_argument("--revision", default="main")
    parser.add_argument("--model-dir", type=Path)
    parser.add_argument(
        "--device", choices=("auto", "cpu", "cuda"), default="auto"
    )
    parser.add_argument("--batch-size", default=4, type=int)
    parser.add_argument(
        "--general-threshold", default=GENERAL_THRESHOLD, type=float
    )
    parser.add_argument(
        "--character-threshold", default=CHARACTER_THRESHOLD, type=float
    )
    return parser


def main() -> None:
    args = make_parser().parse_args()
    if args.model_dir is None:
        resolved_model = resolve_hub_model(
            model_version=args.model_version,
            revision=args.revision,
        )
    else:
        resolved_model = resolve_local_model(
            args.model_dir,
            model_version=args.model_version,
        )

    settings = ServiceSettings(
        model_id=resolved_model.analyzer_model_id,
        device=args.device,
        batch_size=args.batch_size,
        general_threshold=args.general_threshold,
        character_threshold=args.character_threshold,
    )

    def create_cl_tagger(
        *,
        model_id: str,
        device: str,
        batch_size: int,
        general_threshold: float,
        character_threshold: float,
    ) -> ClTaggerV2:
        return ClTaggerV2(
            resolved_model=resolved_model,
            model_id=model_id,
            device=device,
            batch_size=batch_size,
            general_threshold=general_threshold,
            character_threshold=character_threshold,
        )

    uvicorn.run(
        create_app(settings, tagger_factory=create_cl_tagger),
        host=args.host,
        port=args.port,
    )


if __name__ == "__main__":
    main()
