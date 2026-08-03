from __future__ import annotations

import argparse

import uvicorn

from .api import ServiceSettings, create_app
from .tagging import CHARACTER_THRESHOLD, GENERAL_THRESHOLD, MODEL_ID


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run Sprintboard's local WD EVA02 tagging service."
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=8790, type=int)
    parser.add_argument("--model-id", default=MODEL_ID)
    parser.add_argument("--device", default="auto")
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
    settings = ServiceSettings(
        model_id=args.model_id,
        device=args.device,
        batch_size=args.batch_size,
        general_threshold=args.general_threshold,
        character_threshold=args.character_threshold,
    )
    uvicorn.run(create_app(settings), host=args.host, port=args.port)


if __name__ == "__main__":
    main()
