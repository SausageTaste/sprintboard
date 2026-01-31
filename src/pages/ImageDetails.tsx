import React from "react";
import { useSearchParams } from "react-router-dom";


interface ImageDetailsResponse {
    sdModelName: string;
    sdPrompt: Array<string>;
    width: number;
    height: number;
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
        </div >
    );
}
