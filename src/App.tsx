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

export default function App() {
  const [items, setItems] = React.useState<Item[]>([]);
  const [total, setTotal] = React.useState<number | null>(null);

  const loadingRef = React.useRef(false);

  const loadMore = React.useCallback(async () => {
    if (loadingRef.current) return;
    if (total !== null && items.length >= total) return;

    loadingRef.current = true;
    try {
      const offset = items.length;
      const res = await fetch(`/api/list?dir=images&offset=${offset}&limit=${PAGE}`);
      const data = await res.json();

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
  }, [items.length, total]);

  React.useEffect(() => {
    loadMore();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

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

      <VirtuosoGrid
        useWindowScroll
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
    </div>
  );
}
