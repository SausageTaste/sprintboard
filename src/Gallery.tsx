import React from "react";
import { useNavigate, useParams } from "react-router-dom";
import { VirtuosoGrid } from "react-virtuoso";
import PhotoSwipeLightbox from "photoswipe/lightbox";
import "photoswipe/style.css";

import Breadcrumbs from "./widgets/Breadcrumbs";
import FolderCard from "./widgets/FolderCard";
import GalleryDrawer from "./widgets/GalleryDrawer";
import { loadSettings, saveSettings, type ViewerSettings } from "./func/ViewerSetings";


type Item = {
    src: string;
    name: string;
    thumb?: string;
    w?: number;
    h?: number;
};

type Folder = { name: string; path: string };


function lockSelection() {
    document.documentElement.classList.add("pswp-open-noselect");
    document.body.classList.add("pswp-open-noselect");
    (document.activeElement as HTMLElement | null)?.blur?.();
    window.getSelection?.()?.removeAllRanges?.();
}

function unlockSelection() {
    document.documentElement.classList.remove("pswp-open-noselect");
    document.body.classList.remove("pswp-open-noselect");
    window.getSelection?.()?.removeAllRanges?.();
}

export default function Gallery() {
    const [items, setItems] = React.useState<Item[]>([]);
    const [folders, setFolders] = React.useState<Folder[]>([]);
    const [total, setTotal] = React.useState<number | null>(null);
    const [thumbnailWidth, setThumbnailWidth] = React.useState<number>(3);
    const [thumbnailHeight, setThumbnailHeight] = React.useState<number>(4);
    const [query, setQuery] = React.useState("");
    const [menuOpen, setMenuOpen] = React.useState(false);
    const [settings, setSettings] = React.useState<ViewerSettings>(() => loadSettings());

    const virtuosoRef = React.useRef<any>(null);
    const lastIndexRef = React.useRef<number>(0);
    const lightboxRef = React.useRef<PhotoSwipeLightbox | null>(null);
    const loadingRef = React.useRef(false);

    const { "*": path } = useParams();   // catch-all route
    const navigate = useNavigate();
    const curDir = path ?? ""; // "" = root

    const loadMore = React.useCallback(async () => {
        if (loadingRef.current) return;
        if (total !== null && items.length >= total) return;

        loadingRef.current = true;
        try {
            const offset = items.length;
            console.log("Cur dir", curDir);
            const res = await fetch(`/api/images/list?dir=${encodeURIComponent(curDir)}&offset=${offset}&`);
            const data = await res.json();

            setFolders(Array.isArray(data.folders) ? data.folders : []);
            setThumbnailWidth(data.thumbnail_width || 512);
            setThumbnailHeight(data.thumbnail_height || 512);

            const incoming: Item[] = data.files ?? [];
            setTotal(incoming.length);

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

    /*
    const refreshNewFiles = React.useCallback(async () => {
        if (loadingRef.current) return;

        try {
            // Ask backend for newest first
            const res = await fetch(
                `/api/images/list?dir=${encodeURIComponent(curDir)}&offset=0&limit=50`
            );

            const data = await res.json();

            const incoming: Item[] = data.files ?? [];

            if (!incoming.length) return;

            setItems(prev => {
                const seen = new Set(prev.map(x => x.src));
                const merged = [...prev];

                let added = false;

                for (const it of incoming) {
                    if (!seen.has(it.src)) {
                        merged.unshift(it); // newest on top
                        added = true;
                    }
                }

                return added ? merged : prev;
            });

        } catch (e) {
            console.warn("refresh failed", e);
        }
    }, [curDir]);
    */

    React.useEffect(() => {
        setItems([]);
        setTotal(null);
        loadingRef.current = false;

        (async () => {
            if (loadingRef.current) return;
            loadingRef.current = true;
            try {
                const res = await fetch(`/api/images/list?dir=${encodeURIComponent(curDir)}&offset=0&`);
                const data = await res.json();

                setFolders(Array.isArray(data.folders) ? data.folders : []);
                setThumbnailWidth(data.thumbnail_width || 512);
                setThumbnailHeight(data.thumbnail_height || 512);

                const incoming: Item[] = data.files ?? [];
                setItems(incoming);

                // IMPORTANT: total should come from backend, not incoming.length
                setTotal(data.total ?? null);
            } finally {
                loadingRef.current = false;
            }
        })();
    }, [curDir]);

    React.useEffect(() => {
        if (!lightboxRef.current) {
            const lb = new PhotoSwipeLightbox({
                pswpModule: () => import("photoswipe"),
                loop: false,
                maxZoomLevel: 4,
                padding: { top: 0, bottom: 0, left: 0, right: 0 },
                bgOpacity: 1,
            });

            // Track current slide
            lb.on("change", () => {
                const pswp = lb.pswp;
                if (!pswp)
                    return;

                const i = pswp.currIndex;
                lastIndexRef.current = i;

                // ✅ scroll grid to current image
                /*
                virtuosoRef.current?.scrollToIndex({
                  index: i,
                  align: "center",
                  behavior: "smooth",
                });
                */
            });

            // When closing → scroll back
            lb.on("close", () => {
                unlockSelection();

                // Scroll thumbnail into view
                const i = lastIndexRef.current;
                virtuosoRef.current?.scrollToIndex({
                    index: i,
                    align: "center",   // center it nicely
                    behavior: "smooth"
                });
            });

            lb.on("uiRegister", () => {
                const pswp = lb.pswp;
                if (!pswp || !pswp.ui) return;

                pswp.ui.registerElement({
                    name: "tap-prev",
                    order: 5,
                    isButton: false,
                    appendTo: "root",
                    html: "",
                    onInit: (el) => {
                        el.classList.add("pswp-tapzone");

                        el.style.position = "absolute";
                        el.style.left = "0";
                        el.style.top = "80%";
                        el.style.bottom = "0";
                        el.style.width = "50%";          // left tap zone
                        el.style.zIndex = "9999";
                        el.style.touchAction = "manipulation";
                        el.style.pointerEvents = "auto";

                        function flash() {
                            el.classList.add("active");
                            requestAnimationFrame(() => {
                                setTimeout(() => el.classList.remove("active"), 120);
                            });
                        }

                        el.addEventListener("pointerdown", (_) => {
                            flash();
                        });

                        el.addEventListener("click", (e) => {
                            e.preventDefault();
                            e.stopPropagation();
                            pswp.prev();
                        });
                    },
                });

                pswp.ui.registerElement({
                    name: "tap-next",
                    order: 6,
                    isButton: false,
                    appendTo: "root",
                    html: "",
                    onInit: (el) => {
                        el.classList.add("pswp-tapzone");

                        el.style.position = "absolute";
                        el.style.right = "0";
                        el.style.top = "80%";
                        el.style.bottom = "0";
                        el.style.width = "50%";
                        el.style.zIndex = "9999";
                        el.style.touchAction = "manipulation";
                        el.style.pointerEvents = "auto";

                        function flash() {
                            el.classList.add("active");
                            requestAnimationFrame(() => {
                                setTimeout(() => el.classList.remove("active"), 120);
                            });
                        }

                        el.addEventListener("pointerdown", (_) => {
                            flash();
                        });

                        el.addEventListener("click", (e) => {
                            e.preventDefault();
                            e.stopPropagation();
                            pswp.next();
                        });
                    },
                });

                pswp.ui.registerElement({
                    name: "info-btn",
                    order: 8,
                    isButton: true,
                    appendTo: "bar",
                    html: "ⓘ",
                    onClick: async () => {
                        const i = pswp.currIndex;
                        const src = items[i]?.src;
                        console.log("Image info:", src);
                    },
                });
            });

            lb.init();
            lightboxRef.current = lb;
        }

        const lb = lightboxRef.current;
        const lbOptions = lb.options;

        lbOptions.initialZoomLevel = settings.fillScreen ? "fill" : "fit";
        lbOptions.secondaryZoomLevel = settings.fillScreen ? "fit" : "fill";

        // ✅ dataSource drives the gallery length, NOT the DOM
        lbOptions.dataSource = items.map((it) => ({
            src: it.src,
            w: it.w ?? 1600,
            h: it.h ?? 900,
            msrc: it.thumb ?? it.src, // thumb used in animation (optional)
        }));

        return () => {
            // don't destroy on every items change; destroy only on unmount
        };
    }, [items, settings.fillScreen]);

    // destroy on unmount
    React.useEffect(() => {
        return () => {
            lightboxRef.current?.destroy();
            lightboxRef.current = null;
        };
    }, []);

    /*
    React.useEffect(() => {
        const interval = setInterval(() => {
            refreshNewFiles();
        }, 5000); // every 5s (tune later)

        return () => clearInterval(interval);
    }, [curDir, items.length]);
    */

    React.useEffect(() => {
        setMenuOpen(false);

        const lb = lightboxRef.current;
        const pswp = lb?.pswp;

        // If lightbox is open, close it
        if (pswp) {
            pswp.close();
        }
    }, [curDir]);

    React.useEffect(() => {
        if (!menuOpen) return;

        const prev = document.body.style.overflow;
        document.body.style.overflow = "hidden";
        return () => {
            document.body.style.overflow = prev;
        };
    }, [menuOpen]);

    React.useEffect(() => {
        saveSettings(settings);
    }, [settings]);

    function openAt(index: number) {
        lastIndexRef.current = index;

        // loadAndOpen(index, dataSource) is supported in PhotoSwipe 5
        const ds = items.map((it) => ({
            src: it.src,
            w: it.w ?? 512,
            h: it.h ?? 512,
            msrc: it.thumb ?? it.src,
        }));

        lockSelection();
        lightboxRef.current?.loadAndOpen(index, ds);
    }

    return (
        <div style={{ padding: 16, fontFamily: "system-ui" }}>
            <div className="headerRow" style={{ display: "flex", alignItems: "center", gap: 12 }}>
                <button
                    className="menuBtn"
                    onClick={() => setMenuOpen(true)}
                    aria-label="Open menu"
                    style={{
                        width: 44,
                        height: 44,
                        borderRadius: 12,
                        border: "1px solid rgba(255,255,255,0.12)",
                        background: "rgba(255,255,255,0.06)",
                        color: "inherit",
                        fontSize: 22,
                        lineHeight: 1,
                        cursor: "pointer",
                    }}
                >
                    ☰
                </button>

                <h2 style={{ margin: 0 }}>Virtuoso Infinite</h2>
            </div>

            <div style={{ height: 12 }} />

            <GalleryDrawer
                open={menuOpen}
                curDir={curDir}
                settings={settings}
                onChangeSettings={(updater) => setSettings(updater)}
                onClose={() => setMenuOpen(false)}
                onRefreshNow={() => {
                    // hook this up to whatever you want later
                    // e.g. refreshNewFiles();
                }}
            />

            <Breadcrumbs dir={curDir} onNavigate={p => navigate(`/images/${p}`)} />

            <div style={{ height: 12 }} />

            <input
                type="text"
                placeholder="Search..."
                value={query}
                onChange={(e) => setQuery(e.target.value)}
                style={{
                    width: "100%",
                    padding: "10px 12px",
                    borderRadius: 10,
                    border: "1px solid rgba(255,255,255,0.15)",
                    background: "rgba(255,255,255,0.05)",
                    color: "inherit",
                }}
            />

            <div style={{ height: 12 }} />

            <div
                style={{
                    display: "grid",
                    gridTemplateColumns: `repeat(auto-fill, minmax(${settings.thumbnailSize}px, 1fr))`,
                    gap: 12,
                }}
            >
                {folders.map((d) => (
                    <FolderCard key={d.path} name={d.name} onClick={() => navigate(`/images/${d.path}`)} />
                ))}
            </div>

            <div style={{ height: 12 }} />

            <VirtuosoGrid
                ref={virtuosoRef}
                useWindowScroll
                key={curDir}
                style={{ height: "100%" }}
                data={items}
                endReached={loadMore}
                overscan={600}
                listClassName="grid"
                itemContent={(index, it) => (
                    <div
                        onClick={(e) => {
                            e.preventDefault();
                            openAt(index); // ✅ open by index
                        }}
                        style={{
                            borderRadius: 10,
                            overflow: "hidden",
                            border: "1px solid rgba(255,255,255,0.10)",
                        }}
                    >
                        <a
                            href={it.src}
                            data-pswp
                            data-pswp-width={it.w ?? 512}
                            data-pswp-height={it.h ?? 512}
                            style={{ display: "block", borderRadius: 10, overflow: "hidden" }}
                        >
                            <img
                                src={it.thumb || it.src}
                                alt={it.name}
                                loading="lazy"
                                decoding="async"
                                style={{
                                    width: "100%",
                                    aspectRatio: `${thumbnailWidth} / ${thumbnailHeight}`,
                                    objectFit: "cover",
                                    display: "block",
                                }}
                            />
                        </a>
                    </div>
                )}
            />

            <style>{`
        .grid {
          display: grid;
          grid-template-columns: repeat(auto-fill, minmax(${settings.thumbnailSize}px, 1fr));
          gap: 10px;
          padding: 4px;
        }
      `}</style>
        </div >
    );
}
