import React from "react";
import { useSearchParams } from "react-router-dom";


interface ComfyuiInfo {
    workflowSrc: string;
}

interface PngInfo {
    text_chunks: Record<string, string>;
};

interface AvifInfo {
    xmp: string;
};

interface ImageDetailsResponse {
    sdModelName: string;
    sdPrompt: Array<string>;
    width: number;
    height: number;

    comfyuiInfo?: ComfyuiInfo;
    pngInfo?: PngInfo;
    avifInfo?: AvifInfo;
};


async function fetchImageDetails(path: string): Promise<ImageDetailsResponse> {
    const url = new URL("/api/images/details", window.location.origin);
    url.searchParams.set("path", path);

    const res = await fetch(url.toString());
    if (!res.ok)
        throw new Error(`HTTP ${res.status}`);
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
    }, []);

    return (
        <div style={{ padding: 16, fontFamily: "system-ui" }}>
            <h2>Image details</h2>
            <a href={imgSrc} target="_blank" rel="noopener noreferrer">{imgSrc}</a>
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
