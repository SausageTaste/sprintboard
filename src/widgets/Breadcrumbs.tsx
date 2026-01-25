import React from "react";


function joinParts(parts: string[]) {
    return parts.filter(Boolean).join("/");
}


export default function Breadcrumbs({
    dir,
    onNavigate,
}: {
    dir: string;
    onNavigate: (dir: string) => void;
}) {
    const parts = dir ? dir.split("/").filter(Boolean) : [];
    // crumbs: ["", "2026", "2026/01", ...]
    const crumbs = ["", ...parts.map((_, i) => joinParts(parts.slice(0, i + 1)))];

    return (
        <div style={{ display: "flex", gap: 8, flexWrap: "wrap", alignItems: "center" }}>
            {crumbs.map((p, i) => {
                const label = i === 0 ? "root" : parts[i - 1];
                const isLast = i === crumbs.length - 1;

                return (
                    <React.Fragment key={p || "root"}>
                        <button
                            onClick={() => onNavigate(p)}
                            disabled={isLast}
                            style={{
                                padding: "6px 10px",
                                borderRadius: 10,
                                border: "1px solid rgba(255,255,255,0.12)",
                                background: isLast ? "rgba(255,255,255,0.10)" : "transparent",
                                color: "inherit",
                                cursor: isLast ? "default" : "pointer",
                            }}
                        >
                            {label}
                        </button>
                        {!isLast && <span style={{ opacity: 0.6 }}>/</span>}
                    </React.Fragment>
                );
            })}
        </div>
    );
}
