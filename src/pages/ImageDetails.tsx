import React from "react";
import { useLocation, useSearchParams } from "react-router-dom";


export default function ImageDetails() {
    const [sp] = useSearchParams();

    const imgSrc = sp.get("src") ?? "";
    const dir = sp.get("dir") ?? "";
    const index = Number(sp.get("index") ?? "0") || 0;

    const loc = useLocation();
    React.useEffect(() => {
        console.log("URL:", loc.pathname + loc.search);
    }, [loc]);

    return (
        <div style={{ padding: 16, fontFamily: "system-ui" }}>
            <h2>Image details</h2>
            <text>Image source: {imgSrc}</text>
            <div style={{ height: 12 }} />
            <text>Directory: {dir}</text>
            <div style={{ height: 12 }} />
            <text>Index: {index}</text>
        </div >
    );
}
