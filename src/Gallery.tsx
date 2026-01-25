import React from "react";
import { useNavigate, useParams } from "react-router-dom";
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

type Folder = { name: string; path: string };

const PAGE = 200;

function joinParts(parts: string[]) {
    return parts.filter(Boolean).join("/");
}

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

export default function Gallery() {
    const [items, setItems] = React.useState<Item[]>([]);
    const [folders, setFolders] = React.useState<Folder[]>([]);
    const [total, setTotal] = React.useState<number | null>(null);
    const [thumbnailWidth, setThumbnailWidth] = React.useState<number>(3);
    const [thumbnailHeight, setThumbnailHeight] = React.useState<number>(4);
    const [query, setQuery] = React.useState("");
    const [menuOpen, setMenuOpen] = React.useState(false);

    const [settings, setSettings] = React.useState({
        fillScreen: false,
    });

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
            const res = await fetch(`/api/images/list?dir=${encodeURIComponent(curDir)}&offset=${offset}&limit=${PAGE}`);
            const data = await res.json();

            setFolders(Array.isArray(data.folders) ? data.folders : []);
            setThumbnailWidth(data.thumbnail_width || 3);
            setThumbnailHeight(data.thumbnail_height || 4);

            const incoming: Item[] = data.files;
            const incomingTotal = data.files.Length;

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

    React.useEffect(() => {
        setItems([]);
        setTotal(null);
        loadingRef.current = false;

        queueMicrotask(() => loadMore());
        // eslint-disable-next-line react-hooks/exhaustive-deps
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
                if (!pswp) return;

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

                        el.addEventListener("pointerdown", (e) => {
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

                        el.addEventListener("pointerdown", (e) => {
                            flash();
                        });

                        el.addEventListener("click", (e) => {
                            e.preventDefault();
                            e.stopPropagation();
                            pswp.next();
                        });
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

    React.useEffect(() => {
        const interval = setInterval(() => {
            refreshNewFiles();
        }, 5000); // every 5s (tune later)

        return () => clearInterval(interval);
    }, [curDir, items.length]);

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

    function openAt(index: number) {
        lastIndexRef.current = index;

        // loadAndOpen(index, dataSource) is supported in PhotoSwipe 5
        const ds = items.map((it) => ({
            src: it.src,
            w: it.w ?? 1600,
            h: it.h ?? 900,
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

            {/* Backdrop */}
            <div
                className={`drawer-backdrop ${menuOpen ? "open" : ""}`}
                onClick={() => setMenuOpen(false)}
            />

            {/* Drawer */}
            <aside className={`drawer ${menuOpen ? "open" : ""}`} role="dialog" aria-modal="true">
                <div className="drawer-header">
                    <div style={{ fontWeight: 800 }}>Menu</div>
                    <button className="drawer-close" onClick={() => setMenuOpen(false)} aria-label="Close menu">
                        ✕
                    </button>
                </div>

                <div className="drawer-body">
                    {/* Put your UI here */}
                    <div className="drawer-section">
                        <div className="drawer-label">Current folder</div>
                        <div className="drawer-value">{curDir || "root"}</div>
                    </div>

                    <div className="drawer-section">
                        <div className="drawer-label">Flags</div>

                        <label
                            style={{
                                display: "flex",
                                alignItems: "center",
                                justifyContent: "space-between",
                                padding: "10px 0",
                            }}
                        >
                            <span>Fill screen</span>

                            <input
                                type="checkbox"
                                checked={settings.fillScreen}
                                onChange={(e) =>
                                    setSettings(s => ({
                                        ...s,
                                        fillScreen: e.target.checked
                                    }))
                                }
                                style={{
                                    width: 20,
                                    height: 20,
                                }}
                            />
                        </label>

                    </div>

                    <div className="drawer-section">
                        <div className="drawer-label">Sort</div>
                        <button
                            className="drawer-btn"
                            onClick={() => {
                                // example action
                                // setSort("newestFirst")
                            }}
                        >
                            Newest first
                        </button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                        <button className="drawer-btn">Oldest first</button>
                    </div>

                    <div className="drawer-section">
                        <div className="drawer-label">Actions</div>
                        <button
                            className="drawer-btn"
                            onClick={() => {
                                // example: refresh now
                                // refreshNewFiles();
                                setMenuOpen(false);
                            }}
                        >
                            Refresh now
                        </button>
                    </div>
                </div>
            </aside>

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
                    gridTemplateColumns: "repeat(auto-fill, minmax(240px, 1fr))",
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
          grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
          gap: 10px;
          padding: 4px;
        }
      `}</style>
        </div >
    );
}
