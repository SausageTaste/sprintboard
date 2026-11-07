# Generation metadata

[Back to README](../README.md)

Sprintboard reads ComfyUI execution graphs from PNG `prompt` text, AVIF XMP
`sprintboard:pngText_prompt` (or legacy `sprintboard:prompt`), and AVIF EXIF
`Model` values prefixed with `prompt:`. Valid Sprintboard XMP takes precedence
over EXIF. Conflicting sources are reported in image details.

## What is resolved

Extraction follows image-saving outputs back to sampling stages, then follows
their model and positive/negative conditioning inputs. Node IDs, editor positions,
and top-level JSON node order do not determine the result. Separate conditioning
contributions remain separate; Sprintboard does not reconstruct a combined string
that was never saved.

Details group sampling stages by saved output and show model filenames, positive
and negative prompt contributions, and expandable source-node information.
Unknown nodes, broken connections, cycles, and runtime-generated values produce
unresolved reasons. A known contribution can remain available when another
contribution is unresolved. Empty literal prompts are shown as empty rather than
unresolved.

Only resolved positive text and checkpoint/diffusion-model names enter search.
Negative prompts and unclassified text do not affect inclusion or exclusion
filters. AI tag search is unchanged. When multiple saving outputs are present,
Sprintboard cannot establish which produced the image: it displays each output's
results but excludes the graph's models and prompts from search.

## Supported nodes

| Purpose | Node types |
| --- | --- |
| Saved outputs | `SaveImage`, `SaveImageAdvanced` |
| Image/latent path | `VAEDecode`, `EmptyLatentImage`, `EmptyFlux2LatentImage` |
| Sampling | `KSampler`, `KSamplerAdvanced` |
| Model loading | `CheckpointLoaderSimple`, `UNETLoader` |
| Text conditioning | `CLIPTextEncode`, `ConditioningConcat`, `ConditioningCombine` |
| Literal inputs | `PrimitiveString`, `PrimitiveStringMultiline`, `PrimitiveBoolean` |
| Routing | `ComfySwitchNode`, `PreviewAny` for strings |
| Contexts | rgthree `Context`, `Context Big`, `Context Switch`, `Context Switch Big` |

Switches follow only a resolved selected branch. Context fields override inherited
fields when non-null. Context switches use the saved input order and choose the
first provably nonempty context; an earlier unknown context prevents guessing.
Text encoders, VAE files, disconnected model loaders, and prompt-generator system
instructions are not classified as the image's diffusion model or positive prompt.

## Fallback and limitations

When execution metadata is absent or malformed, Sprintboard can resolve flat
editor workflows using supported node types, named connections, named widget
values, and known widget positions. Arbitrary editor subgraphs are not expanded.
Unclassified literal text from editor text-encoding nodes remains visible but
is not indexed. A valid execution graph with unresolved nodes is never replaced
by an editor-workflow guess.

This is static metadata analysis: Sprintboard does not execute custom nodes,
contact ComfyUI, or run an LLM. The output of a runtime text generator cannot be
recovered from its inputs alone. Capturing actual runtime values in the image's
metadata is a separate future feature.

The details API adds `generationInfo` with `source`, `status`, `stages`, `issues`,
and `unclassified`. Stages contain `output`, `sampler`, `models`, `positive`, and
`negative`; each resolved value carries its `value`, source `node`, and `input`.
Existing `sdModelName` and `sdPrompt` fields remain compatibility projections;
`sdPrompt` now contains only searchable positive text. Raw metadata is unchanged.
