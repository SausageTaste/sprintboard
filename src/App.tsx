import React from "react";
import PhotoSwipeLightbox from "photoswipe/lightbox";
import "photoswipe/style.css";

export default function App() {
  const [items, setItems] = React.useState([]);
  const galleryRef = React.useRef(null);

  // load list from server
  React.useEffect(() => {
    async function load() {
      // If your React dev server is separate from Python,
      // this URL should point to Python: http://<PC>:8000/api/list?dir=images
      const res = await fetch("/api/list?dir=images");
      const data = await res.json();
      setItems(data);
    }
    load();
  }, []);

  // setup PhotoSwipe
  React.useEffect(() => {
    if (!galleryRef.current) return;

    const lightbox = new PhotoSwipeLightbox({
      gallery: "#gallery",
      children: "a",
      pswpModule: () => import("photoswipe"),
      // Optional: start zoomed to fit, enable pinch zoom etc (default is good)
    });

    lightbox.init();
    return () => lightbox.destroy();
  }, [items]);

  return (
    <div style={{ padding: 16, fontFamily: "system-ui" }}>
      <h2 style={{ margin: "0 0 12px 0" }}>React Image Viewer</h2>

      <div
        id="gallery"
        ref={galleryRef}
        style={{
          display: "grid",
          gridTemplateColumns: "repeat(auto-fill, minmax(140px, 1fr))",
          gap: 10,
        }}
      >
        {items.map((it) => (
          <a
            key={it.src}
            href={it.src}
            data-pswp-width={it.w || 1600}
            data-pswp-height={it.h || 900}
            target="_blank"
            rel="noreferrer"
            style={{
              display: "block",
              borderRadius: 10,
              overflow: "hidden",
              border: "1px solid rgba(0,0,0,0.12)",
            }}
          >
            <img
              src={it.src}
              alt={it.name}
              loading="lazy"
              style={{
                width: "100%",
                height: "100%",
                objectFit: "cover",
                display: "block",
              }}
            />
          </a>
        ))}
      </div>
    </div>
  );
}
