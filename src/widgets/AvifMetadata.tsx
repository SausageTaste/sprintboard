export interface AvifInfo {
    xmp: string;
    exif?: Record<string, string>;
}

function exifDisplay(name: string, value: string) {
    const prefix = name === "Make" ? "workflow:" : name === "Model" ? "prompt:" : undefined;
    if (!prefix || !value.startsWith(prefix))
        return { title: `EXIF ${name}`, text: value };

    const title = name === "Make" ? "Workflow (EXIF Make)" : "Execution Graph (EXIF Model)";
    try {
        return { title, text: JSON.stringify(JSON.parse(value.slice(prefix.length)), null, 2) };
    } catch {
        return { title, text: value };
    }
}

function MetadataText({ title, text }: { title: string; text: string }) {
    return (
        <div style={{ marginBottom: 16 }}>
            <h3>{title}</h3>
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
                {text}
            </pre>
        </div>
    );
}

export default function AvifMetadata({ metadata }: { metadata: AvifInfo }) {
    const fields = Object.entries(metadata.exif ?? {});
    if (!metadata.xmp && fields.length === 0)
        return null;

    return (
        <>
            <h2>AVIF Info</h2>
            {fields.map(([name, value]) => (
                <MetadataText key={name} {...exifDisplay(name, value)} />
            ))}
            {metadata.xmp && <MetadataText title="XMP Data" text={metadata.xmp} />}
        </>
    );
}
