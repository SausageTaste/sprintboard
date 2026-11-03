# Local image tagging

[Back to README](../README.md)

Tagging is optional. It adds searchable general and character tags to your images
using a separate local service.

## WD EVA02

Sprintboard includes a self-contained WD EVA02 tagging service in `python/`.
Install [uv](https://docs.astral.sh/uv/), then start the service from the
repository root:

```sh
uv run --project python sprintboard-tagger \
  --host 127.0.0.1 \
  --port 8790
```

The first analysis downloads and loads
`SmilingWolf/wd-eva02-large-tagger-v3`. The service selects CUDA, then Apple
MPS, then CPU unless `--device` is supplied. Other options include
`--model-id`, `--batch-size`, `--general-threshold`, and
`--character-threshold`.

## CL Tagger v2

CL Tagger v2 is available through a separate server command and dependency
group, leaving the WD/PixAI service unchanged. Before its first use, accept the
[CL Tagger v2 license](https://huggingface.co/cella110n/cl_tagger_v2) and log
in with the Hugging Face CLI:

```sh
hf auth login
uv run --project python --group cl sprintboard-cl-tagger \
  --host 127.0.0.1 \
  --port 8790
```

The command defaults to the provisional `v2_01a` model, a batch size of `4`,
and `0.55` general and character thresholds. It downloads approximately 2.2 GB
of gated ONNX weights into the normal Hugging Face cache. The resolved Hub
commit is part of the analyzer fingerprint, so an upstream replacement of the
provisional model triggers retagging instead of silently reusing old results.
The Windows dependency uses ONNX Runtime's CUDA package and prefers CUDA when
available; macOS and other platforms use CPU ONNX Runtime. `--device cpu`
forces CPU, while an explicit `--device cuda` fails if CUDA is unavailable.

For offline use, pass `--model-dir PATH`. The directory must directly contain
the following files downloaded from the selected model-version directory:

```text
model.onnx
model.onnx.data
model_vocabulary.json
```

Local model contents are hashed into the analyzer fingerprint. Other CL server
options include `--model-version`, `--revision`, `--batch-size`,
`--general-threshold`, and `--character-threshold`. Copyright/series
predictions are returned with general tags so they participate in Sprintboard
search; meta and quality predictions are omitted.

Run either `sprintboard-tagger` or `sprintboard-cl-tagger` on port 8790, not
both. Switching between them changes the analyzer fingerprint and causes the
collection to be retagged.

Set `tagger_enabled` to `true` in `server_configs.json` after starting the
service. A generated `image.png.sprintboard.avif` shares the absolute
normalized logical key of `image.png`; if that source exists, it is used for
inference. When tagging is enabled, a new proxy waits for successful current
analysis. Existing proxies remain usable while replacement analysis is
pending, and a stale proxy never hides a changed source image.

## Storage and availability

Content scans queue new or changed images for tagging. Completed images stay
idle until a scan detects a change. The periodic `tagger_poll_interval_seconds`
check only asks the service for its analyzer fingerprint; a model or threshold
change requeues known images once. Queued batches run independently of that
polling interval, and failed images retry with increasing delays.

Tags live in each image collection's SQLite database. Include its `.sprintboard`
folder in backups. See [storage and backups](storage-and-indexing.md#tag-database)
for details and migration behavior.

If the tagging service is unavailable, indexing and HTTP requests continue.
With tagging enabled, only missing or stale proxy generation waits for the
service; already-current proxies remain available.

## Service API

The versioned HTTP interface consists of `GET /v1/info` and
`POST /v1/analyze`. The latter accepts JSON shaped as
`{"paths":["/absolute/image/path.png"]}`. The service returns results in the
same order, with `ratings`, `generalTags`, and `characterTags`, or an `error`
for an individual path. Both endpoints report protocol version `1` and an
analyzer fingerprint derived from the model and thresholds.

The service accepts local filesystem paths from clients. It binds only to
localhost by default. Binding it to a network-visible address allows remote
clients to request reads of local image paths and should be done only on a
trusted network with appropriate external access controls.
