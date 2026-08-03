from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fastapi.testclient import TestClient

from sprintboard_tagger.api import ServiceSettings, create_app
from sprintboard_tagger.tagging import ImageTagAnalysis, ScoredTag


class FakeTagger:
    device = "cpu"
    calls: list[list[Path]] = []

    def __init__(self, **_kwargs: object) -> None:
        type(self).calls = []

    def analyze(self, paths: list[Path]):
        type(self).calls.append(list(paths))
        if any(path.name == "broken.png" for path in paths):
            raise ValueError("unreadable image")
        for path in paths:
            yield ImageTagAnalysis(
                image_path=path,
                ratings=(ScoredTag("safe", 0.9),),
                general_tags=(ScoredTag("blue_hair", 0.8),),
                character_tags=(),
            )


class ApiTests(unittest.TestCase):
    def setUp(self) -> None:
        self.settings = ServiceSettings(device="cpu", batch_size=3)
        self.client = TestClient(
            create_app(self.settings, tagger_factory=FakeTagger)
        )

    def test_info_is_stable_and_does_not_run_inference(self) -> None:
        first = self.client.get("/v1/info")
        second = self.client.get("/v1/info")

        self.assertEqual(first.status_code, 200)
        self.assertEqual(first.json(), second.json())
        self.assertEqual(first.json()["protocolVersion"], 1)
        self.assertEqual(len(first.json()["fingerprint"]), 64)
        self.assertEqual(FakeTagger.calls, [])

    def test_analyze_preserves_order_and_isolates_errors(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / "first.png"
            broken = root / "broken.png"
            last = root / "last.png"
            for path in (first, broken, last):
                path.write_bytes(b"test")

            response = self.client.post(
                "/v1/analyze",
                json={"paths": [str(first), str(broken), str(last)]},
            )

        self.assertEqual(response.status_code, 200)
        results = response.json()["results"]
        self.assertEqual([item["path"] for item in results], [str(first), str(broken), str(last)])
        self.assertEqual(results[0]["generalTags"][0]["name"], "blue_hair")
        self.assertEqual(results[1]["error"], "unreadable image")
        self.assertEqual(results[2]["ratings"][0]["name"], "safe")
        self.assertGreater(len(FakeTagger.calls), 1)

    def test_validates_paths_and_batch_limit(self) -> None:
        relative = self.client.post(
            "/v1/analyze", json={"paths": ["relative.png"]}
        )
        oversized = self.client.post(
            "/v1/analyze", json={"paths": ["/a", "/b", "/c", "/d"]}
        )

        self.assertEqual(relative.status_code, 200)
        self.assertIn("absolute", relative.json()["results"][0]["error"])
        self.assertEqual(oversized.status_code, 413)


if __name__ == "__main__":
    unittest.main()
