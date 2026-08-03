import React from "react";
import { useSearchParams } from "react-router-dom";
import { downloadImage, downloadSourceImage } from "../func/downloadImage";


interface ComfyuiInfo {
    workflowSrc: string;
}

interface PngInfo {
    text_chunks: Record<string, string>;
};

interface AvifInfo {
    xmp: string;
};

interface ScoredTag {
    name: string;
    confidence: number;
}

interface TagAnalysis {
    analyzerFingerprint: string;
    modelId: string;
    generalThreshold: number;
    characterThreshold: number;
    analyzedAt: number;
    ratings: ScoredTag[];
    generalTags: ScoredTag[];
    characterTags: ScoredTag[];
}

interface ImageDetailsResponse {
    sdModelName: string;
    sdPrompt: Array<string>;
    width: number;
    height: number;

    comfyuiInfo?: ComfyuiInfo;
    pngInfo?: PngInfo;
    avifInfo?: AvifInfo;
    tagAnalysis?: TagAnalysis;
};

function TagGroup({ title, tags }: { title: string; tags: ScoredTag[] }) {
    return (
        <>
            <h3>{title}</h3>
            {tags.length === 0 ? (
                <p>None</p>
            ) : (
                <div
                    style={{
                        display: "flex",
                        flexWrap: "wrap",
                        gap: 8,
                        marginBottom: 16,
                    }}
                >
                    {tags.map((tag) => (
                        <span
                            key={`${title}:${tag.name}`}
                            title={`Confidence: ${tag.confidence.toFixed(6)}`}
                            style={{
                                padding: "5px 9px",
                                borderRadius: 999,
                                background: "rgba(255,255,255,0.08)",
                                border: "1px solid rgba(255,255,255,0.12)",
                                fontSize: 13,
                            }}
                        >
                            {tag.name} · {(tag.confidence * 100).toFixed(1)}%
                        </span>
                    ))}
                </div>
            )}
        </>
    );
}


async function fetchImageDetails(path: string): Promise<ImageDetailsResponse> {
    const url = new URL("/api/images/details", window.location.origin);
    url.searchParams.set("path", path);

    const res = await fetch(url.toString());
    if (!res.ok) {
        const details = await res.text();
        throw new Error(
            `HTTP ${res.status}${details ? `: ${details}` : ""}`
        );
    }
    return (await res.json()) as ImageDetailsResponse;
}

export default function ImageDetails() {
    const [imageDetails, setImageDetails] = React.useState<ImageDetailsResponse | null>(null);

    const [sp] = useSearchParams();

    const imgSrc = sp.get("src") ?? "";
    // const dir = sp.get("dir") ?? "";
    // const index = Number(sp.get("index") ?? "0") || 0;

    React.useEffect(() => {
        fetchImageDetails(imgSrc).then((data) => {
            console.log("Fetched image details:", data);
            setImageDetails(data);
        }).catch((err) => {
            console.error("Error fetching image details:", err);
        });

        return () => {
        };
    }, [imgSrc]);

    return (
        <div className="app-page">
            <h2>Image details</h2>
            <a href={imgSrc} target="_blank" rel="noopener noreferrer">
                <img
                    src={imgSrc}
                    alt="Detailed"
                    style={{ maxWidth: "500px", width: "100%", borderRadius: 10, marginBottom: 16 }}
                />
            </a>
            <div style={{ marginBottom: 16 }}>
                <button
                    type="button"
                    onClick={() => downloadImage(imgSrc)}
                    disabled={!imgSrc}
                >
                    Download image
                </button>
                <button
                    type="button"
                    onClick={() => downloadSourceImage(imgSrc)}
                    disabled={!imgSrc}
                    style={{ marginLeft: 8 }}
                >
                    Download source image
                </button>
            </div>
            <p>Dimensions: {imageDetails?.width} x {imageDetails?.height}</p>

            <h2>Stable Diffusion</h2>

            <h3>Model Name</h3>
            <p>{imageDetails?.sdModelName}</p>

            <h3>SD Prompt</h3>
            {imageDetails?.sdPrompt.map((s, i) => (
                <p key={i}>{s}</p>
            ))}

            {imageDetails?.comfyuiInfo && (
                <>
                    <h2>ComfyUI Info</h2>
                    <h3>Workflow</h3>
                    <pre
                        style={{
                            maxHeight: 220,
                            overflow: "auto",
                            whiteSpace: "pre-wrap",
                            padding: "10px 12px",
                            borderRadius: 8,
                            background: "rgba(255,255,255,0.05)",
                            border: "1px solid rgba(255,255,255,0.1)",
                            fontSize: 12,
                            lineHeight: 1.5,
                        }}
                    >
                        {imageDetails.comfyuiInfo.workflowSrc}
                    </pre>
                </>
            )}

            {imageDetails?.tagAnalysis && (
                <>
                    <h2>Analyzed Tags</h2>
                    <p>
                        Model: {imageDetails.tagAnalysis.modelId}
                    </p>
                    <TagGroup
                        title="Ratings"
                        tags={imageDetails.tagAnalysis.ratings}
                    />
                    <TagGroup
                        title="Characters"
                        tags={imageDetails.tagAnalysis.characterTags}
                    />
                    <TagGroup
                        title="General Tags"
                        tags={imageDetails.tagAnalysis.generalTags}
                    />
                </>
            )}

            {imageDetails?.pngInfo && (
                <>
                    <h2>PNG Info</h2>
                    <h3>Text Chunks</h3>
                    {Object.entries(imageDetails.pngInfo.text_chunks).map(([key, value]) => (
                        <div key={key} style={{ marginBottom: 16 }}>
                            <div style={{ fontWeight: 600, marginBottom: 6 }}>
                                {key}
                            </div>

                            <pre
                                style={{
                                    maxHeight: 220,
                                    overflow: "auto",
                                    whiteSpace: "pre-wrap",
                                    padding: "10px 12px",
                                    borderRadius: 8,
                                    background: "rgba(255,255,255,0.05)",
                                    border: "1px solid rgba(255,255,255,0.1)",
                                    fontSize: 12,
                                    lineHeight: 1.5,
                                }}
                            >
                                {value}
                            </pre>
                        </div>
                    ))}
                </>
            )}

            {imageDetails?.avifInfo && (
                <>
                    <h2>AVIF Info</h2>
                    <h3>XMP Data</h3>
                    <div style={{ marginBottom: 16 }}>
                        <div style={{ fontWeight: 600, marginBottom: 6 }}>
                            XMP
                        </div>

                        <pre
                            style={{
                                maxHeight: 220,
                                overflow: "auto",
                                whiteSpace: "pre-wrap",
                                padding: "10px 12px",
                                borderRadius: 8,
                                background: "rgba(255,255,255,0.05)",
                                border: "1px solid rgba(255,255,255,0.1)",
                                fontSize: 12,
                                lineHeight: 1.5,
                            }}
                        >
                            {imageDetails.avifInfo.xmp}
                        </pre>
                    </div>
                </>
            )}
        </div >
    );
}
