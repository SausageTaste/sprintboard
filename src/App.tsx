import React from "react";
import { VirtuosoGrid } from "react-virtuoso";
import PhotoSwipeLightbox from "photoswipe/lightbox";
import "photoswipe/style.css";

type Item = {
  src: string;
  name: string;
  thumb?: string;
  w?: number;
  h?: number;
};

const PAGE = 200;


function joinParts(parts: string[]) {
  return parts.filter(Boolean).join("/");
}


function Breadcrumbs({
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


function FolderCard({
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


export default function App() {
  const [curDir, setCurDir] = React.useState<string>("images");
  const [items, setItems] = React.useState<Item[]>([]);
  const [total, setTotal] = React.useState<number | null>(null);

  const loadingRef = React.useRef(false);

  const loadMore = React.useCallback(async () => {
    if (loadingRef.current) return;
    if (total !== null && items.length >= total) return;

    loadingRef.current = true;
    try {
      const offset = items.length;
      const res = await fetch(`/api/list?dir=${encodeURIComponent(curDir)}&offset=${offset}&limit=${PAGE}`);
      const data = await res.json();

      console.log("loaded data:", data);

      const incoming: Item[] = Array.isArray(data)
        ? data
        : Array.isArray(data.items)
          ? data.items
          : [];

      const incomingTotal =
        !Array.isArray(data) && typeof data.total === "number" ? data.total : null;

      if (incomingTotal !== null) setTotal(incomingTotal);

      // dedupe by src so it still works even if backend ignores offset/limit for now
      setItems(prev => {
        const seen = new Set(prev.map(x => x.src));
        const merged = [...prev];
        for (const it of incoming) {
          if (!seen.has(it.src)) {
            seen.add(it.src);
            merged.push(it);
          }
        }
        return merged;
      });
    } finally {
      loadingRef.current = false;
    }
  }, [items.length, total, curDir]);

  React.useEffect(() => {
    setItems([]);
    setTotal(null);
    loadingRef.current = false;

    queueMicrotask(() => loadMore());
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [curDir]);

  React.useEffect(() => {
    const lightbox = new PhotoSwipeLightbox({
      gallery: document.body,
      children: "a[data-pswp]",
      pswpModule: () => import("photoswipe"),
    });
    lightbox.init();
    return () => lightbox.destroy();
  }, []);

  return (
    <div style={{ padding: 16, fontFamily: "system-ui" }}>
      <h2 style={{ margin: "0 0 12px 0" }}>Virtuoso Infinite</h2>

      <Breadcrumbs dir={curDir} onNavigate={setCurDir} />

      <div
        style={{
          display: "grid",
          gridTemplateColumns: "repeat(auto-fill, minmax(220px, 1fr))",
          gap: 12,
        }}
      >
        <FolderCard key={"shit"} name={"ass"} onClick={() => setCurDir("2026-01-25")} />
        <FolderCard key={"shit2"} name={"ass2"} onClick={() => setCurDir("2026-01-23")} />
      </div>

      <VirtuosoGrid
        useWindowScroll
        key={curDir}
        style={{ height: "100%" }}
        data={items}
        endReached={loadMore}
        overscan={600}
        listClassName="grid"
        itemContent={(_, it) => (
          <div
            style={{
              borderRadius: 10,
              overflow: "hidden",
              border: "1px solid rgba(255,255,255,0.10)",
            }}
          >
            <a
              href={it.src}
              data-pswp
              data-pswp-width={it.w ?? 1600}
              data-pswp-height={it.h ?? 900}
              style={{ display: "block", borderRadius: 10, overflow: "hidden" }}
            >
              <img
                src={it.thumb || it.src}
                alt={it.name}
                loading="lazy"
                decoding="async"
                style={{
                  width: "100%",
                  aspectRatio: "3 / 4",
                  objectFit: "cover",
                  display: "block",
                }}
              />
            </a>
          </div>
        )}
        components={{
          Footer: () => (
            <div style={{ padding: 12, opacity: 0.7 }}>
              {total === null ? "" : items.length >= total ? "End." : "Loading more…"}
            </div>
          ),
        }}
      />

      <style>{`
        .grid {
          display: grid;
          grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
          gap: 10px;
          padding: 4px;
        }
      `}</style>
    </div >
  );
}
