interface GenerationValue {
    value: string;
    node: string;
    input: string;
}

interface GenerationStage {
    output: string;
    sampler: string;
    models: GenerationValue[];
    positive: GenerationValue[];
    negative: GenerationValue[];
}

export interface GenerationInfo {
    source: string;
    status: "resolved" | "partial" | "unresolved" | "ambiguous";
    stages: GenerationStage[];
    issues: { node: string; input: string; reason: string }[];
    unclassified: GenerationValue[];
}

function Values({ title, entries, prompt = false }: { title: string; entries: GenerationValue[]; prompt?: boolean }) {
    return (
        <>
            <h4>{title}</h4>
            {entries.length === 0 ? <p>Not resolved</p> : entries.map((entry, index) => (
                <p key={index} className={prompt ? "generation-value prompt-box" : "generation-value"}>
                    {entry.value || "(empty)"}
                </p>
            ))}
        </>
    );
}

export default function GenerationMetadata({ info, legacyModel, legacyPrompts }: {
    info?: GenerationInfo;
    legacyModel: string;
    legacyPrompts: string[];
}) {
    if (!info) {
        return (
            <>
                <h2>Stable Diffusion</h2>
                <h3>Model Name</h3>
                <p>{legacyModel}</p>
                <h3>SD Prompt</h3>
                {legacyPrompts.map((text, index) => <p key={index} className="generation-value prompt-box">{text}</p>)}
            </>
        );
    }

    const outputs = [...new Set(info.stages.map(stage => stage.output))];
    const statusText = {
        resolved: "Resolved from saved metadata.",
        partial: "Some values could not be resolved from saved metadata. Search uses only resolved models and positive prompt contributions.",
        unresolved: "Generation values could not be resolved from saved metadata.",
        ambiguous: "This metadata contains multiple saved outputs. Their association with this image is unknown, so these values are excluded from search.",
    }[info.status];

    return (
        <section aria-label="Generation metadata">
            <h2>Generation</h2>
            <p>{statusText}</p>
            {outputs.map((output, outputIndex) => (
                <section key={output}>
                    {outputs.length > 1 && <h3>Saved output {outputIndex + 1}</h3>}
                    {info.stages.filter(stage => stage.output === output).map((stage, index, stages) => (
                        <section key={stage.sampler}>
                            {stages.length > 1 && <h3>Sampling stage {index + 1}</h3>}
                            <Values title="Models" entries={stage.models} />
                            <Values title="Positive Prompt" entries={stage.positive} prompt />
                            <Values title="Negative Prompt" entries={stage.negative} prompt />
                            <details>
                                <summary>Source nodes</summary>
                                <p>Output: {stage.output}; sampler: {stage.sampler}</p>
                                <ul>
                                    {[
                                        ...stage.models.map(entry => ({ ...entry, role: "Model" })),
                                        ...stage.positive.map(entry => ({ ...entry, role: "Positive" })),
                                        ...stage.negative.map(entry => ({ ...entry, role: "Negative" })),
                                    ].map((entry, i) => (
                                        <li key={i}>{entry.role}: node {entry.node}, input {entry.input}</li>
                                    ))}
                                </ul>
                            </details>
                        </section>
                    ))}
                </section>
            ))}
            {info.unclassified.length > 0 && (
                <>
                    <Values title="Unclassified Text" entries={info.unclassified} />
                    <p>This text is displayed for reference and is not searchable.</p>
                </>
            )}
            <details>
                <summary>Extraction details</summary>
                <p>Metadata source: {info.source}</p>
                {info.issues.length > 0 && (
                    <ul>
                        {info.issues.map((issue, index) => (
                            <li key={index}>
                                {issue.reason}
                                {issue.node && ` — node ${issue.node}`}
                                {issue.input && ` (${issue.input})`}
                            </li>
                        ))}
                    </ul>
                )}
            </details>
        </section>
    );
}
