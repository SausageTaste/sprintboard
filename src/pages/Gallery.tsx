import React from "react";
import { useNavigate, useParams } from "react-router-dom";
import { VirtuosoGrid } from "react-virtuoso";
import PhotoSwipeLightbox from "photoswipe/lightbox";
import "photoswipe/style.css";

import Breadcrumbs from "../widgets/Breadcrumbs";
import FolderCard from "../widgets/FolderCard";
import GalleryDrawer from "../widgets/GalleryDrawer";
import { loadSettings, saveSettings, type ViewerSettings } from "../func/ViewerSetings";


type ImageFileInfo = {
    src: string;
    name: string;
    thumb?: string;
    w?: number;
    h?: number;
};

type FolderInfo = {
    name: string;
    path: string;
};

interface ImageListResponse {
    folders: FolderInfo[];
    imageFiles: ImageFileInfo[];
    totalImageCount: number;
    thumbnailWidth: number;
    thumbnailHeight: number;
};


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
    // Query result of image list
    const [imgItems, setImgItems] = React.useState<ImageFileInfo[]>([]);
    const [folders, setFolders] = React.useState<FolderInfo[]>([]);
    const [totalImgCount, setTotalImgCount] = React.useState<number | null>(null);
    const [thumbnailWidth, setThumbnailWidth] = React.useState<number>(3);
    const [thumbnailHeight, setThumbnailHeight] = React.useState<number>(4);

    const [searchBoxText, setSearchBoxText] = React.useState("");
    const [menuOpen, setMenuOpen] = React.useState(false);
    const [settings, setSettings] = React.useState<ViewerSettings>(() => loadSettings());

    const virtuosoRef = React.useRef<any>(null);
    const lightboxRef = React.useRef<PhotoSwipeLightbox | null>(null);
    const lastIndexRef = React.useRef<number>(0);
    const loadingRef = React.useRef(false);
    const imgItemsRef = React.useRef(imgItems);

    const { "*": path } = useParams();   // catch-all route
    const navigate = useNavigate();
    const curDir = path ?? ""; // "" = root

    async function fetchImageList(dir: string, offset: number): Promise<ImageListResponse> {
        const res = await fetch(`/api/images/list?dir=${encodeURIComponent(dir)}&offset=${offset}&`);
        if (!res.ok)
            throw new Error(`HTTP ${res.status}`);
        return (await res.json()) as ImageListResponse;
    }

    function openAt(index: number) {
        lastIndexRef.current = index;

        const it = imgItems[index];
        if (it) {
            const url = new URL(window.location.href);
            url.searchParams.set("src", it.src);
            window.history.pushState({ pswp: true }, "", url);
        }

        const ds = imgItems.map((it) => ({
            src: it.src,
            w: it.w ?? 512,
            h: it.h ?? 512,
            msrc: it.thumb ?? it.src,
        }));

        lockSelection();
        lightboxRef.current?.loadAndOpen(index, ds);
    }

    const loadMore = React.useCallback(async () => {
        return;

        if (loadingRef.current) return;
        if (totalImgCount !== null && imgItems.length >= totalImgCount) return;

        loadingRef.current = true;
        try {
            const offset = imgItems.length;
            const data = await fetchImageList(curDir, offset);

            setFolders(data.folders);
            setThumbnailWidth(data.thumbnail_width || 512);
            setThumbnailHeight(data.thumbnail_height || 512);

            const incoming: ImageFileInfo[] = data.files ?? [];
            setTotalImgCount(incoming.length);

            // dedupe by src so it still works even if backend ignores offset/limit for now
            setImgItems(prev => {
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
    }, [imgItems.length, totalImgCount, curDir]);

    /*
    const refreshNewFiles = React.useCallback(async () => {
        if (loadingRef.current) return;

        try {
            // Ask backend for newest first
            const res = await fetch(
                `/api/images/list?dir=${encodeURIComponent(curDir)}&offset=0&limit=50`
            );

            const data = await res.json();

            const incoming: ImageFileInfo[] = data.files ?? [];

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
        imgItemsRef.current = imgItems;

        const src = new URL(window.location.href).searchParams.get("src");
        if (!src)
            return;
        const idx = imgItems.findIndex(x => x.src === src);
        if (idx >= 0)
            openAt(idx);
    }, [imgItems]);

    React.useEffect(() => {
        setImgItems([]);
        setTotalImgCount(null);
        loadingRef.current = false;
        imgItemsRef.current = imgItems;

        (async () => {
            if (loadingRef.current)
                return;
            loadingRef.current = true;

            try {
                const data = await fetchImageList(curDir, 0);
                console.log("Fetched image list:", data);

                setFolders(Array.isArray(data.folders) ? data.folders : []);
                setImgItems(data.imageFiles)
                setTotalImgCount(data.totalImageCount);
                setThumbnailWidth(data.thumbnailWidth || 512);
                setThumbnailHeight(data.thumbnailHeight || 512);
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

                const it = imgItemsRef.current[i];
                if (!it)
                    return;

                const url = new URL(window.location.href);
                url.searchParams.set("src", it.src);
                window.history.replaceState({ pswp: true }, "", url);

                // ✅ scroll grid to current image
                /*
                virtuosoRef.current?.scrollToIndex({
                  index: i,
                  align: "center",
                  behavior: "smooth",
                });
                */
            });

            lb.on("close", () => {
                unlockSelection();

                if (window.location.pathname.startsWith("/images")) {
                    const url = new URL(window.location.href);
                    url.searchParams.delete("src");
                    window.history.replaceState({}, "", url);
                }

                virtuosoRef.current?.scrollToIndex({
                    index: lastIndexRef.current,
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
                        const it = imgItemsRef.current[i];
                        if (!it)
                            return;

                        pswp.close();

                        const params = new URLSearchParams();
                        params.set("src", it.src);
                        params.set("dir", curDir);
                        params.set("index", String(i));
                        navigate(`/imagedetails?${params.toString()}`);
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
        lbOptions.dataSource = imgItems.map((it) => ({
            src: it.src,
            w: it.w ?? 1600,
            h: it.h ?? 900,
            msrc: it.thumb ?? it.src, // thumb used in animation (optional)
        }));

        return () => {
            // don't destroy on every imgItems change; destroy only on unmount
        };
    }, [imgItems, settings.fillScreen]);

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
    }, [curDir, imgItems.length]);
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

    React.useEffect(() => {
        function onPopState() {
            const lb = lightboxRef.current;
            const pswp = lb?.pswp;
            if (pswp) {
                pswp.close();
            }
        }

        window.addEventListener("popstate", onPopState);
        return () => window.removeEventListener("popstate", onPopState);
    }, []);

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
                onRefreshNow={() => { navigate(0); }}
            />

            <Breadcrumbs dir={curDir} onNavigate={p => navigate(`/images/${p}`)} />

            <div style={{ height: 12 }} />

            <input
                type="text"
                placeholder="Search..."
                value={searchBoxText}
                onChange={(e) => setSearchBoxText(e.target.value)}
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
                data={imgItems}
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
                                decoding="auto"
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
