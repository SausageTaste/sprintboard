import React from "react";
import { VirtuosoGrid } from "react-virtuoso";

type Item = {
  src: string;
  name: string;
  thumb?: string;
  w?: number;
  h?: number;
};

export default function App() {
  const [items, setItems] = React.useState<Item[]>([]);

  React.useEffect(() => {
    (async () => {
      const res = await fetch("/api/list?dir=images");
      const data = await res.json();

      // support both API shapes
      const incoming: Item[] = Array.isArray(data)
        ? data
        : Array.isArray(data.items)
        ? data.items
        : [];

      console.log("items loaded:", incoming.length, incoming[0]);
      setItems(incoming);
    })();
  }, []);

  return (
    <div style={{ padding: 16, fontFamily: "system-ui" }}>
      <h2 style={{ margin: "0 0 12px 0" }}>Virtuoso Test</h2>

      {/* IMPORTANT: Virtuoso needs a height */}
      <div style={{ height: "calc(100vh - 80px)" }}>
        <VirtuosoGrid
          style={{ height: "100%" }}
          data={items}
          listClassName="grid"
          itemContent={(_, it) => (
            <div
              style={{
                borderRadius: 10,
                overflow: "hidden",
                border: "1px solid rgba(255,255,255,0.10)",
              }}
            >
              <img
                src={it.thumb || it.src}
                alt={it.name}
                loading="lazy"
                decoding="async"
                style={{
                  width: "100%",
                  aspectRatio: "1 / 1",
                  objectFit: "cover",
                  display: "block",
                }}
              />
            </div>
          )}
        />
      </div>

      <style>{`
        .grid {
          display: grid;
          grid-template-columns: repeat(auto-fill, minmax(140px, 1fr));
          gap: 10px;
          padding: 4px;
        }
      `}</style>
    </div>
  );
}
