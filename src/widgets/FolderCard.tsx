import React from "react";


export default function FolderCard({
    name,
    onClick,
}: {
    name: string;
    onClick: () => void;
}) {
    return (
        <button
            onClick={onClick}
            style={{
                textAlign: "left",
                borderRadius: 16,
                border: "1px solid rgba(255,255,255,0.10)",
                background: "rgba(255,255,255,0.04)",
                padding: 14,
                cursor: "pointer",
                display: "flex",
                gap: 12,
                alignItems: "center",
                minHeight: 64,
            }}
        >
            {/* simple folder icon */}
            <div
                style={{
                    width: 40,
                    height: 32,
                    borderRadius: 10,
                    background: "rgba(255,255,255,0.10)",
                    position: "relative",
                    flex: "0 0 auto",
                }}
            >
                <div
                    style={{
                        position: "absolute",
                        left: 6,
                        top: -6,
                        width: 18,
                        height: 10,
                        borderRadius: 6,
                        background: "rgba(255,255,255,0.14)",
                    }}
                />
            </div>

            <div style={{ minWidth: 0 }}>
                <div style={{ fontWeight: 700, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>
                    {name}
                </div>
                <div style={{ opacity: 0.65, fontSize: 12 }}>Folder</div>
            </div>
        </button>
    );
}
