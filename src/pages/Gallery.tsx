import React from "react";
import { useNavigate, useParams, useLocation, useSearchParams } from "react-router-dom";
import { VirtuosoGrid } from "react-virtuoso";
import PhotoSwipeLightbox, { type PhotoSwipe } from "photoswipe/lightbox";
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
    hasMore: boolean;
    nextOffset: number | null;
    nextCursor: string | null;
    thumbnailWidth: number;
    thumbnailHeight: number;
};

const PAGE_SIZE = 100;
const LIGHTBOX_PREFETCH_THRESHOLD = 10;

type SafeAreaInsets = {
    top: number;
    right: number;
    bottom: number;
    left: number;
};

type ImageDimensions = {
    width: number;
    height: number;
};

const NO_SAFE_AREA_INSETS: SafeAreaInsets = { top: 0, right: 0, bottom: 0, left: 0 };

function usesIPhoneDocumentViewportWorkaround(): boolean {
    return /iPhone/i.test(window.navigator.userAgent)
        && window.matchMedia("(display-mode: browser)").matches;
}

// Share of the minimized-chrome inset (screen height minus 100lvh) that sits
// above the layout viewport. The status area above the viewport is constant
// (~63pt of a 120pt minimized inset on iOS 26 Safari); only the bottom bar
// grows when expanded, so the share must be applied to the minimized inset,
// never to the current innerHeight.
const IPHONE_TOP_CHROME_SHARE = 0.52;

function measureCssHeight(height: string): number {
    const probe = document.createElement("div");
    Object.assign(probe.style, {
        position: "fixed",
        top: "0",
        left: "0",
        width: "0",
        height,
        visibility: "hidden",
        pointerEvents: "none",
    });
    document.documentElement.appendChild(probe);
    const measured = probe.getBoundingClientRect().height;
    probe.remove();
    return measured;
}

function probeDeviceDimension(dimension: "width" | "height"): number {
    if (!window.matchMedia(`(min-device-${dimension}: 1px)`).matches)
        return Number.NaN;

    let low = 1;
    let high = 4000;
    while (high - low > 1) {
        const mid = Math.floor((low + high) / 2);
        if (window.matchMedia(`(min-device-${dimension}: ${mid}px)`).matches)
            low = mid;
        else
            high = mid;
    }
    return low;
}

let cachedDeviceScreenSize: { shortSide: number; longSide: number } | null | undefined;

function getDeviceScreenSizeFromMediaQueries() {
    if (cachedDeviceScreenSize === undefined) {
        const width = probeDeviceDimension("width");
        const height = probeDeviceDimension("height");
        cachedDeviceScreenSize = Number.isFinite(width) && Number.isFinite(height)
            ? { shortSide: Math.min(width, height), longSide: Math.max(width, height) }
            : null;
    }
    return cachedDeviceScreenSize;
}

function getIPhoneScreenViewport() {
    const isPortrait = window.innerHeight >= window.innerWidth;
    const screenLongSide = Math.max(window.screen.width, window.screen.height);
    const screenShortSide = Math.min(window.screen.width, window.screen.height);
    const device = getDeviceScreenSizeFromMediaQueries();

    // The layout width always reflects real screen points (CSS pixels map
    // 1:1 onto them), so it anchors everything else.
    const width = window.innerWidth;

    // Safari spoofs `window.screen` while fingerprinting protection is
    // active (always in private browsing) — e.g. it reports 414x896 on a
    // 402x874 iPhone 16 Pro. When device media queries disagree with
    // `window.screen` they carry the real dimensions; otherwise reconstruct
    // the height from the layout width and the reported aspect ratio, which
    // stays within a few points of truth even when spoofed because recent
    // iPhones share nearly the same aspect ratio.
    const aspectRatio = screenLongSide / screenShortSide;
    const height = device
        && (device.longSide !== screenLongSide || device.shortSide !== screenShortSide)
        ? (isPortrait ? device.longSide : device.shortSide)
        : (isPortrait ? width * aspectRatio : width / aspectRatio);

    // Anchor the top inset to the large viewport (chrome minimized) so the
    // overlay keeps the same screen position while Safari's bottom bar
    // expands and collapses.
    const largeViewportHeight = Math.max(window.innerHeight, measureCssHeight("100lvh"));
    const topInset = Math.max(0, (height - largeViewportHeight) * IPHONE_TOP_CHROME_SHARE);

    return { width, height, topInset };
}

function positionEdgeToEdgeOverlay(element: HTMLElement): void {
    if (!usesIPhoneDocumentViewportWorkaround()
        || !element.classList.contains("pswp--edge-to-edge")) {
        return;
    }

    const viewport = getIPhoneScreenViewport();

    element.classList.add("pswp--document-viewport");
    element.style.setProperty("--viewer-control-top-inset", `${viewport.topInset}px`);
    element.style.top = `${window.scrollY - viewport.topInset}px`;
    element.style.left = `${window.scrollX}px`;
    element.style.width = `${viewport.width}px`;
    element.style.height = `${viewport.height}px`;
}

function formatDebugNumber(value: number): string {
    return Number.isFinite(value) ? value.toFixed(1) : "?";
}

function formatDebugRect(element: Element | null | undefined): string {
    if (!element)
        return "missing";

    const rect = element.getBoundingClientRect();
    return `${formatDebugNumber(rect.x)},${formatDebugNumber(rect.y)} ${formatDebugNumber(rect.width)}x${formatDebugNumber(rect.height)}`;
}

function startViewerGeometryDiagnostics(pswp: PhotoSwipe): () => void {
    const root = pswp.element;
    if (!root)
        return () => { };

    const debugPanel = document.createElement("pre");
    debugPanel.setAttribute("aria-hidden", "true");
    Object.assign(debugPanel.style, {
        position: "fixed",
        top: "96px",
        left: "4px",
        zIndex: "2147483647",
        maxWidth: "calc(100vw - 8px)",
        maxHeight: "52vh",
        margin: "0",
        padding: "6px",
        overflow: "hidden",
        border: "1px solid rgba(255,255,255,0.8)",
        borderRadius: "4px",
        background: "rgba(0,0,0,0.78)",
        color: "white",
        font: "9px/1.25 ui-monospace, SFMono-Regular, Menlo, monospace",
        whiteSpace: "pre",
        pointerEvents: "none",
        userSelect: "none",
    });
    document.body.appendChild(debugPanel);

    type SavedOutline = { outline: string; outlineOffset: string };
    const outlinedElements = new Map<HTMLElement, SavedOutline>();
    let currentImageElement: HTMLElement | null = null;

    const setOutline = (element: HTMLElement | null | undefined, color: string) => {
        if (!element)
            return;
        if (!outlinedElements.has(element)) {
            outlinedElements.set(element, {
                outline: element.style.outline,
                outlineOffset: element.style.outlineOffset,
            });
        }
        element.style.outline = `2px solid ${color}`;
        element.style.outlineOffset = "-2px";
    };

    const restoreOutline = (element: HTMLElement | null) => {
        if (!element)
            return;
        const saved = outlinedElements.get(element);
        if (!saved)
            return;
        element.style.outline = saved.outline;
        element.style.outlineOffset = saved.outlineOffset;
        outlinedElements.delete(element);
    };

    setOutline(root, "#ff2bd6");
    setOutline(pswp.scrollWrap, "#00e5ff");

    // Ruler ticks labelled in layout-viewport coordinates. A device
    // screenshot then reveals exactly where the layout viewport sits on the
    // physical screen and whether CSS pixels map 1:1 onto screen points.
    const ruler = document.createElement("div");
    ruler.setAttribute("aria-hidden", "true");
    Object.assign(ruler.style, {
        position: "absolute",
        inset: "0",
        zIndex: "2147483646",
        pointerEvents: "none",
        userSelect: "none",
        font: "8px/1 ui-monospace, SFMono-Regular, Menlo, monospace",
    });
    root.appendChild(ruler);

    let rulerOrigin = { x: NaN, y: NaN };

    const addRulerElement = (left: number, top: number, style: Partial<CSSStyleDeclaration>, text?: string) => {
        const el = document.createElement(text === undefined ? "div" : "span");
        if (text !== undefined)
            el.textContent = text;
        Object.assign(el.style, {
            position: "absolute",
            left: `${left}px`,
            top: `${top}px`,
            background: "#00ff6a",
            ...style,
        });
        ruler.appendChild(el);
    };

    const rebuildRuler = (rootRect: DOMRect) => {
        rulerOrigin = { x: rootRect.x, y: rootRect.y };
        ruler.replaceChildren();

        const labelStyle: Partial<CSSStyleDeclaration> = {
            padding: "1px 2px",
            background: "rgba(0,0,0,0.7)",
            color: "#00ff6a",
        };

        for (let viewportY = -300; viewportY <= 1500; viewportY += 50) {
            const localY = viewportY - rootRect.y;
            if (localY < 0 || localY > rootRect.height)
                continue;
            addRulerElement(0, localY, { width: "40px", height: "1px" });
            addRulerElement(44, localY - 4, labelStyle, `y${viewportY}`);
        }

        const xRulerLocalY = 620 - rootRect.y;
        for (let viewportX = -100; viewportX <= 600; viewportX += 50) {
            const localX = viewportX - rootRect.x;
            if (localX < 0 || localX > rootRect.width)
                continue;
            addRulerElement(localX, xRulerLocalY, { width: "1px", height: "24px" });
            addRulerElement(localX + 2, xRulerLocalY + 26, labelStyle, `x${viewportX}`);
        }
    };

    let animationFrame = 0;
    let lastUpdate = 0;
    let stopped = false;

    const update = (timestamp: number) => {
        if (stopped)
            return;

        const slide = pswp.currSlide;
        const imageElement = slide?.content.element instanceof HTMLElement
            ? slide.content.element
            : null;
        if (imageElement !== currentImageElement) {
            restoreOutline(currentImageElement);
            currentImageElement = imageElement;
            setOutline(currentImageElement, "#ffe600");
        }

        if (timestamp - lastUpdate >= 150) {
            lastUpdate = timestamp;
            const visualViewport = window.visualViewport;
            const rootStyle = window.getComputedStyle(root);
            const bounds = slide?.bounds;

            const rootRect = root.getBoundingClientRect();
            if (rootRect.x !== rulerOrigin.x || rootRect.y !== rulerOrigin.y)
                rebuildRuler(rootRect);

            const screenAny = window.screen as unknown as Record<string, number | undefined>;

            debugPanel.textContent = [
                "VIEWER GEOMETRY (magenta=root cyan=wrap yellow=image)",
                `screen ${screen.width}x${screen.height} avail ${screen.availWidth}x${screen.availHeight} dpr ${window.devicePixelRatio}`,
                (() => {
                    const device = getDeviceScreenSizeFromMediaQueries();
                    return device
                        ? `mq device ${device.shortSide}x${device.longSide}`
                        : "mq device missing";
                })(),
                `outer ${window.outerWidth}x${window.outerHeight}`,
                `screenXY ${formatDebugNumber(window.screenX)},${formatDebugNumber(window.screenY)} availTL ${formatDebugNumber(screenAny.availLeft ?? NaN)},${formatDebugNumber(screenAny.availTop ?? NaN)}`,
                `inner ${window.innerWidth}x${window.innerHeight} client ${document.documentElement.clientWidth}x${document.documentElement.clientHeight}`,
                `lvh ${formatDebugNumber(measureCssHeight("100lvh"))} svh ${formatDebugNumber(measureCssHeight("100svh"))}`,
                `scroll ${formatDebugNumber(window.scrollX)},${formatDebugNumber(window.scrollY)}`,
                visualViewport
                    ? `visual ${formatDebugNumber(visualViewport.width)}x${formatDebugNumber(visualViewport.height)} off ${formatDebugNumber(visualViewport.offsetLeft)},${formatDebugNumber(visualViewport.offsetTop)}`
                    : "visual missing",
                visualViewport
                    ? `vpage ${formatDebugNumber(visualViewport.pageLeft)},${formatDebugNumber(visualViewport.pageTop)} scale ${formatDebugNumber(visualViewport.scale)}`
                    : "",
                `html ${formatDebugRect(document.documentElement)}`,
                `body ${formatDebugRect(document.body)}`,
                `root ${formatDebugRect(root)}`,
                `root css ${rootStyle.position} top ${rootStyle.top} left ${rootStyle.left}`,
                `wrap ${formatDebugRect(pswp.scrollWrap)}`,
                `topbar ${formatDebugRect(pswp.topBar)}`,
                `pswp viewport ${formatDebugNumber(pswp.viewportSize.x)}x${formatDebugNumber(pswp.viewportSize.y)} off ${formatDebugNumber(pswp.offset.x)},${formatDebugNumber(pswp.offset.y)}`,
                `slide ${slide ? `${slide.width}x${slide.height}` : "missing"} panArea ${slide ? `${formatDebugNumber(slide.panAreaSize.x)}x${formatDebugNumber(slide.panAreaSize.y)}` : "missing"}`,
                `image ${formatDebugRect(imageElement)}`,
                imageElement instanceof HTMLImageElement
                    ? `natural ${imageElement.naturalWidth}x${imageElement.naturalHeight}`
                    : "natural missing",
                slide
                    ? `zoom ${formatDebugNumber(slide.currZoomLevel)} init ${formatDebugNumber(slide.zoomLevels.initial)} fit ${formatDebugNumber(slide.zoomLevels.fit)} fill ${formatDebugNumber(slide.zoomLevels.fill)}`
                    : "zoom missing",
                slide
                    ? `pan ${formatDebugNumber(slide.pan.x)},${formatDebugNumber(slide.pan.y)}`
                    : "pan missing",
                bounds
                    ? `boundX ${formatDebugNumber(bounds.min.x)}..${formatDebugNumber(bounds.max.x)} center ${formatDebugNumber(bounds.center.x)}`
                    : "boundX missing",
                bounds
                    ? `boundY ${formatDebugNumber(bounds.min.y)}..${formatDebugNumber(bounds.max.y)} center ${formatDebugNumber(bounds.center.y)}`
                    : "boundY missing",
            ].filter(Boolean).join("\n");
        }

        animationFrame = window.requestAnimationFrame(update);
    };

    animationFrame = window.requestAnimationFrame(update);

    return () => {
        stopped = true;
        window.cancelAnimationFrame(animationFrame);
        restoreOutline(currentImageElement);
        for (const [element, saved] of outlinedElements) {
            element.style.outline = saved.outline;
            element.style.outlineOffset = saved.outlineOffset;
        }
        outlinedElements.clear();
        ruler.remove();
        debugPanel.remove();
    };
}


const folderNameCollator = new Intl.Collator(undefined, { numeric: true });

function sortFoldersNaturallyDescending(folders: FolderInfo[]): FolderInfo[] {
    return [...folders].sort((a, b) => {
        const naturalOrder = folderNameCollator.compare(b.name, a.name);
        if (naturalOrder !== 0)
            return naturalOrder;

        if (a.name === b.name)
            return 0;
        return a.name < b.name ? 1 : -1;
    });
}

let lockedPageScroll = { x: 0, y: 0 };

function preventViewerPageScroll(event: TouchEvent) {
    if (document.documentElement.classList.contains("pswp-open-noselect"))
        event.preventDefault();
}

function restoreViewerPageScroll() {
    if (!document.documentElement.classList.contains("pswp-open-noselect"))
        return;

    if (window.scrollX !== lockedPageScroll.x || window.scrollY !== lockedPageScroll.y)
        window.scrollTo(lockedPageScroll.x, lockedPageScroll.y);
}


function lockSelection() {
    lockedPageScroll = { x: window.scrollX, y: window.scrollY };
    document.documentElement.classList.add("pswp-open-noselect");
    document.body.classList.add("pswp-open-noselect");
    document.addEventListener("touchmove", preventViewerPageScroll, { passive: false });
    window.addEventListener("scroll", restoreViewerPageScroll);
    (document.activeElement as HTMLElement | null)?.blur?.();
    window.getSelection?.()?.removeAllRanges?.();
}

function unlockSelection() {
    document.removeEventListener("touchmove", preventViewerPageScroll);
    window.removeEventListener("scroll", restoreViewerPageScroll);
    document.documentElement.classList.remove("pswp-open-noselect");
    document.body.classList.remove("pswp-open-noselect");
    window.scrollTo(lockedPageScroll.x, lockedPageScroll.y);
    window.getSelection?.()?.removeAllRanges?.();
}

async function fetchImageList(
    dir: string,
    query: string,
    cursor: string | null,
    recursive: boolean,
    signal?: AbortSignal,
): Promise<ImageListResponse> {
    const url = new URL("/api/images/list", window.location.origin);
    url.searchParams.set("dir", dir);
    url.searchParams.set("query", query);
    if (cursor)
        url.searchParams.set("cursor", cursor);
    url.searchParams.set("limit", String(PAGE_SIZE));
    url.searchParams.set("recursive", recursive ? "1" : "0");

    const res = await fetch(url.toString(), { signal });
    if (!res.ok)
        throw new Error(`HTTP ${res.status}`);

    const data = (await res.json()) as ImageListResponse;
    return {
        ...data,
        folders: sortFoldersNaturallyDescending(data.folders ?? []),
    };
}


export default function Gallery() {
    // Query result of image list
    const [imgItems, setImgItems] = React.useState<ImageFileInfo[]>([]);
    const [folders, setFolders] = React.useState<FolderInfo[]>([]);
    const [totalImgCount, setTotalImgCount] = React.useState<number>(0);
    const [thumbnailWidth, setThumbnailWidth] = React.useState<number>(3);
    const [thumbnailHeight, setThumbnailHeight] = React.useState<number>(4);

    const [menuOpen, setMenuOpen] = React.useState(false);
    const [settings, setSettings] = React.useState<ViewerSettings>(() => loadSettings());
    const settingsRef = React.useRef(settings);
    const [lightboxReady, setLightboxReady] = React.useState(false);
    const [searchBoxText, setSearchBoxText] = React.useState(settings.searchText || "");

    const virtuosoRef = React.useRef<any>(null);
    const lightboxRef = React.useRef<PhotoSwipeLightbox | null>(null);
    const lastIndexRef = React.useRef<number>(0);
    const loadingRef = React.useRef(false);
    const imgItemsRef = React.useRef(imgItems);
    const pathnameRef = React.useRef("");
    const nextCursorRef = React.useRef<string | null | undefined>(undefined);
    const requestGenerationRef = React.useRef(0);
    const activeRequestRef = React.useRef<AbortController | null>(null);
    const loadMoreRef = React.useRef<() => Promise<void>>(async () => { });
    const safeAreaProbeRef = React.useRef<HTMLDivElement | null>(null);
    const safeAreaInsetsRef = React.useRef<SafeAreaInsets>(NO_SAFE_AREA_INSETS);
    const naturalDimensionsRef = React.useRef(new Map<string, ImageDimensions>());

    const { "*": path } = useParams();   // catch-all route
    const navigate = useNavigate();
    const location = useLocation();
    const [searchParams] = useSearchParams();
    const curDir = path ?? ""; // "" = root

    React.useLayoutEffect(() => {
        const updateSafeAreaInsets = () => {
            const probe = safeAreaProbeRef.current;
            if (!probe)
                return;

            const style = window.getComputedStyle(probe);
            safeAreaInsetsRef.current = {
                top: Number.parseFloat(style.paddingTop) || 0,
                right: Number.parseFloat(style.paddingRight) || 0,
                bottom: Number.parseFloat(style.paddingBottom) || 0,
                left: Number.parseFloat(style.paddingLeft) || 0,
            };

            const pswp = lightboxRef.current?.pswp;
            if (pswp?.element)
                positionEdgeToEdgeOverlay(pswp.element);
            pswp?.updateSize(true);
        };

        updateSafeAreaInsets();
        window.addEventListener("resize", updateSafeAreaInsets);
        window.visualViewport?.addEventListener("resize", updateSafeAreaInsets);

        return () => {
            window.removeEventListener("resize", updateSafeAreaInsets);
            window.visualViewport?.removeEventListener("resize", updateSafeAreaInsets);
        };
    }, []);

    function openAt(index: number) {
        lastIndexRef.current = index;

        const it = imgItems[index];
        if (!it)
            return;

        setSrcInUrl(it.src, false);
        lockSelection();
        lightboxRef.current?.loadAndOpen(index);
    }

    function openDetailsPage(i: number) {
        const it = imgItemsRef.current[i];
        if (!it)
            return;

        const params = new URLSearchParams();
        params.set("src", it.src);
        params.set("dir", curDir);
        params.set("index", String(i));

        const url = `/imagedetails?${params.toString()}`;
        window.open(url, "_blank", "noopener,noreferrer");
    }

    function downloadImage(src: string) {
        const a = document.createElement("a");
        a.href = src;
        a.download = ""; // lets browser pick filename if same-origin
        a.rel = "noopener";
        document.body.appendChild(a);
        a.click();
        a.remove();
    }

    async function deleteCurrentImage(pswp: PhotoSwipe) {
        const i = pswp.currIndex;
        const it = imgItemsRef.current[i];
        if (!it)
            return;

        if (!window.confirm(`Are you sure you want to delete "${it.name}"? This action cannot be undone.`))
            return;

        const url = new URL("/api/images/delete", window.location.origin);
        url.searchParams.set("path", it.src);
        const res = await fetch(url.toString(), { method: "DELETE" });
        if (!res.ok) {
            alert(`Delete failed: HTTP ${res.status}`);
            return;
        }

        activeRequestRef.current?.abort();
        activeRequestRef.current = null;
        loadingRef.current = false;
        setImgItems(prev => prev.filter(x => x.src !== it.src));
        imgItemsRef.current = imgItemsRef.current.filter(x => x.src !== it.src);
        setTotalImgCount(prev => Math.max(0, prev - 1));
        pswp.close();
    }

    const loadMore = React.useCallback(async () => {
        if (loadingRef.current)
            return;
        const cursor = nextCursorRef.current;
        if (cursor === null || cursor === undefined)
            return;
        if (totalImgCount > 0 && imgItemsRef.current.length >= totalImgCount)
            return;

        const generation = requestGenerationRef.current;
        const controller = new AbortController();
        activeRequestRef.current = controller;
        loadingRef.current = true;
        try {
            const data = await fetchImageList(
                curDir,
                settings.searchText,
                cursor,
                settings.filesRecursive,
                controller.signal,
            );
            if (generation !== requestGenerationRef.current)
                return;

            nextCursorRef.current = data.nextCursor;
            setTotalImgCount(data.totalImageCount);

            setImgItems(prev => {
                const seen = new Set(prev.map(x => x.src));
                const merged = [...prev];
                for (const it of data.imageFiles ?? []) {
                    if (!seen.has(it.src)) {
                        seen.add(it.src);
                        merged.push(it);
                    }
                }
                imgItemsRef.current = merged;
                return merged;
            });
        } catch (err) {
            if (!(err instanceof DOMException && err.name === "AbortError"))
                console.error("Failed to load more images:", err);
        } finally {
            if (activeRequestRef.current === controller) {
                activeRequestRef.current = null;
                loadingRef.current = false;
            }
        }
    }, [curDir, settings.filesRecursive, settings.searchText, totalImgCount]);

    React.useEffect(() => {
        loadMoreRef.current = loadMore;
    }, [loadMore]);

    const setSrcInUrl = React.useCallback((src: string | null, replace: boolean) => {
        const p = new URLSearchParams(searchParams);
        if (src) p.set("src", src);
        else p.delete("src");

        const s = p.toString();

        navigate(
            { pathname: pathnameRef.current, search: s ? `?${s}` : "" },
            { replace }
        );
    }, [navigate, searchParams]);

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
    }, [imgItems]);

    React.useEffect(() => {
        pathnameRef.current = location.pathname;
    }, [location.pathname]);

    React.useEffect(() => {
        const generation = requestGenerationRef.current + 1;
        requestGenerationRef.current = generation;
        activeRequestRef.current?.abort();

        const controller = new AbortController();
        activeRequestRef.current = controller;
        nextCursorRef.current = undefined;
        setFolders([]);
        setImgItems([]);
        setTotalImgCount(0);
        imgItemsRef.current = [];
        loadingRef.current = true;

        (async () => {
            try {
                let data = await fetchImageList(
                    curDir,
                    settings.searchText,
                    null,
                    settings.filesRecursive,
                    controller.signal,
                );
                if (generation !== requestGenerationRef.current)
                    return;

                const targetSrc = new URLSearchParams(window.location.search).get("src");
                const merged: ImageFileInfo[] = [];
                const seen = new Set<string>();

                const appendPage = (page: ImageListResponse) => {
                    for (const it of page.imageFiles ?? []) {
                        if (!seen.has(it.src)) {
                            seen.add(it.src);
                            merged.push(it);
                        }
                    }
                    nextCursorRef.current = page.nextCursor;
                    imgItemsRef.current = [...merged];
                    setImgItems([...merged]);
                    setTotalImgCount(page.totalImageCount);
                };

                setFolders(data.folders || []);
                setThumbnailWidth(data.thumbnailWidth || 512);
                setThumbnailHeight(data.thumbnailHeight || 512);
                appendPage(data);

                while (
                    targetSrc &&
                    !seen.has(targetSrc) &&
                    data.nextCursor !== null
                ) {
                    data = await fetchImageList(
                        curDir,
                        settings.searchText,
                        data.nextCursor,
                        settings.filesRecursive,
                        controller.signal,
                    );
                    if (generation !== requestGenerationRef.current)
                        return;
                    appendPage(data);
                }
            } catch (err) {
                if (!(err instanceof DOMException && err.name === "AbortError"))
                    console.error("Failed to load images:", err);
            } finally {
                if (activeRequestRef.current === controller) {
                    activeRequestRef.current = null;
                    loadingRef.current = false;
                }
            }
        })();

        return () => controller.abort();
    }, [curDir, settings.filesRecursive, settings.searchText]);

    React.useEffect(() => {
        if (!lightboxRef.current) {
            let stopGeometryDiagnostics: (() => void) | null = null;
            const lb = new PhotoSwipeLightbox({
                pswpModule: () => import("photoswipe"),
                loop: false,
                maxZoomLevel: 4,
                paddingFn: () => settings.edgeToEdge
                    ? NO_SAFE_AREA_INSETS
                    : safeAreaInsetsRef.current,
                getViewportSizeFn: (_options, pswp) => {
                    const element = (pswp as PhotoSwipe).element;
                    if (settings.edgeToEdge && element) {
                        return { x: element.clientWidth, y: element.clientHeight };
                    }

                    return { x: document.documentElement.clientWidth, y: window.innerHeight };
                },
                bgOpacity: 1,
            });

            lb.on("firstUpdate", () => {
                const element = lb.pswp?.element;
                if (element)
                    positionEdgeToEdgeOverlay(element);
            });

            lb.on("afterInit", () => {
                const pswp = lb.pswp;
                if (!pswp?.element || !settingsRef.current.viewerDiagnostics)
                    return;

                stopGeometryDiagnostics?.();
                stopGeometryDiagnostics = startViewerGeometryDiagnostics(pswp);
            });

            lb.on("destroy", () => {
                stopGeometryDiagnostics?.();
                stopGeometryDiagnostics = null;
            });

            lb.on("loadComplete", ({ content, slide, isError }) => {
                const image = content.element;
                if (isError || !slide || !(image instanceof HTMLImageElement))
                    return;

                const width = image.naturalWidth;
                const height = image.naturalHeight;
                if (!width || !height)
                    return;

                const src = String(content.data.src ?? "");
                if (src)
                    naturalDimensionsRef.current.set(src, { width, height });

                if (slide.width === width && slide.height === height)
                    return;

                Object.assign(content.data, { w: width, h: height, width, height });
                Object.assign(slide.data, { w: width, h: height, width, height });
                content.width = width;
                content.height = height;
                slide.width = width;
                slide.height = height;

                // PhotoSwipe lays out a slide from the dimensions supplied before
                // the image loads. Recalculate once the browser has decoded the
                // image so zoom, pan bounds, and centering use its real boundary.
                slide.resize();
            });

            lb.on("change", () => {
                const pswp = lb.pswp;
                if (!pswp)
                    return;

                const onKeyDown = async (e: KeyboardEvent) => {
                    if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement) {
                        return;
                    }

                    if (e.key === "Delete") {
                        e.preventDefault();
                        deleteCurrentImage(pswp);
                    }
                    else if (e.key === "d" || e.key === "D") {
                        e.preventDefault();
                        const i = pswp.currIndex;
                        const it = imgItemsRef.current[i];
                        if (it)
                            downloadImage(it.src);
                    }
                    else if (e.key === "i" || e.key === "I") {
                        e.preventDefault();
                        openDetailsPage(pswp.currIndex);
                    }
                };

                pswp.on("afterInit", () => {
                    window.addEventListener("keydown", onKeyDown);
                });

                pswp.on("destroy", () => {
                    window.removeEventListener("keydown", onKeyDown);
                });

                pswp.element?.classList.remove("pswp-menu-open");

                const i = pswp.currIndex;
                lastIndexRef.current = i;

                if (
                    i >= imgItemsRef.current.length - LIGHTBOX_PREFETCH_THRESHOLD
                ) {
                    void loadMoreRef.current();
                }

                const it = imgItemsRef.current[i];
                if (!it)
                    return;

                setSrcInUrl(it.src, true);
            });

            lb.on("close", () => {
                unlockSelection();

                if (location.pathname.startsWith("/images")) {
                    setSrcInUrl(null, true);
                }

                virtuosoRef.current?.scrollToIndex({
                    index: lastIndexRef.current,
                    align: "center",   // center it nicely
                    behavior: "smooth"
                });
            });

            lb.on("uiRegister", () => {
                const pswp = lb.pswp;
                if (!pswp)
                    return;
                if (!pswp.ui)
                    return;

                pswp.ui.registerElement({
                    name: "tap-prev",
                    order: 5,
                    isButton: false,
                    appendTo: "root",
                    html: "",
                    onInit: (el) => {
                        el.classList.add("pswp-tapzone");

                        el.style.position = "absolute";
                        el.style.left = "env(safe-area-inset-left, 0px)";
                        el.style.top = "80%";
                        el.style.bottom = "env(safe-area-inset-bottom, 0px)";
                        el.style.width = "calc((100% - env(safe-area-inset-left, 0px) - env(safe-area-inset-right, 0px)) / 2)";
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
                        el.style.right = "env(safe-area-inset-right, 0px)";
                        el.style.top = "80%";
                        el.style.bottom = "env(safe-area-inset-bottom, 0px)";
                        el.style.width = "calc((100% - env(safe-area-inset-left, 0px) - env(safe-area-inset-right, 0px)) / 2)";
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
                    name: "menu-btn",
                    order: 7,              // choose position
                    isButton: true,
                    appendTo: "bar",
                    html: "☰",             // start with text icon
                    onClick: () => {
                        // toggle menu
                        const root = pswp.element;               // PhotoSwipe root
                        root?.classList.toggle("pswp-menu-open");
                    },
                });

                pswp.ui.registerElement({
                    name: "menu-panel",
                    order: 9,
                    isButton: false,
                    appendTo: "root",
                    html: `
    <div class="pswp-menu">
      <button class="pswp-menu-item" data-action="details">Details...</button>
      <button class="pswp-menu-item" data-action="download">Download</button>
      <button class="pswp-menu-item" data-action="newtab">Open in new tab</button>
      <button class="pswp-menu-item" data-action="copy">Copy URL</button>
      <button class="pswp-menu-item" data-action="delete">Delete</button>
    </div>
    <div class="pswp-menu-backdrop"></div>
  `,
                    onInit: (el) => {
                        const root = pswp.element!;

                        const backdrop = el.querySelector(".pswp-menu-backdrop") as HTMLDivElement;
                        backdrop.addEventListener("click", () => root.classList.remove("pswp-menu-open"));

                        el.addEventListener("click", async (e) => {
                            const t = e.target as HTMLElement;
                            const btn = t.closest(".pswp-menu-item") as HTMLElement | null;
                            if (!btn) return;

                            const action = btn.getAttribute("data-action");
                            const i = pswp.currIndex;
                            const it = imgItemsRef.current[i];
                            if (!it)
                                return;

                            if (action === "newtab") {
                                window.open(it.src, "_blank", "noopener,noreferrer");
                            } else if (action === "download") {
                                downloadImage(it.src);
                            } else if (action === "copy") {
                                try { await navigator.clipboard.writeText(it.src); } catch { }
                            } else if (action === "details") {
                                openDetailsPage(pswp.currIndex);
                            }
                            else if (action === "delete") {
                                deleteCurrentImage(pswp);
                            }

                            root.classList.remove("pswp-menu-open");
                        });
                    },
                });
            });

            lb.init();
            lightboxRef.current = lb;
            setLightboxReady(true);
        }

        const lb = lightboxRef.current;
        const lbOptions = lb.options;
        const dataSource = imgItems.map((it) => {
            const naturalDimensions = naturalDimensionsRef.current.get(it.src);
            return {
                src: it.src,
                w: naturalDimensions?.width ?? it.w ?? 512,
                h: naturalDimensions?.height ?? it.h ?? 512,
                msrc: it.thumb ?? it.src,
            };
        });

        lbOptions.initialZoomLevel = settings.fillScreen
            ? (zoomLevel) => {
                if (!zoomLevel.elementSize || !zoomLevel.panAreaSize)
                    return zoomLevel.fill;

                return Math.max(
                    zoomLevel.panAreaSize.x / zoomLevel.elementSize.x,
                    zoomLevel.panAreaSize.y / zoomLevel.elementSize.y,
                );
            }
            : "fit";
        lbOptions.secondaryZoomLevel = settings.fillScreen ? "fit" : "fill";
        lbOptions.mainClass = settings.edgeToEdge ? "pswp--edge-to-edge" : "";
        lbOptions.paddingFn = () => settings.edgeToEdge
            ? NO_SAFE_AREA_INSETS
            : safeAreaInsetsRef.current;
        lbOptions.getViewportSizeFn = (_options, pswpBase) => {
            const element = (pswpBase as PhotoSwipe).element;
            if (settings.edgeToEdge && element) {
                return { x: element.clientWidth, y: element.clientHeight };
            }

            return { x: document.documentElement.clientWidth, y: window.innerHeight };
        };
        lbOptions.dataSource = dataSource;

        const pswp = lb.pswp;
        if (pswp) {
            const previousCount = pswp.getNumItems();
            pswp.options.dataSource = dataSource;
            pswp.updateSize(true);

            const nextIndex = pswp.currIndex + 1;
            if (dataSource.length > previousCount && nextIndex < dataSource.length)
                pswp.refreshSlideContent(nextIndex);
        }

        return () => {
            // don't destroy on every imgItems change; destroy only on unmount
        };
    }, [imgItems, settings.fillScreen, settings.edgeToEdge]);

    React.useEffect(() => {
        if (!lightboxReady)
            return;

        const lb = lightboxRef.current;
        if (!lb)
            return;

        const src = searchParams.get("src");

        // no src => close
        if (!src) {
            if (lb.pswp)
                lb.pswp.close();
            return;
        }

        // wait until list is loaded enough to find it
        const idx = imgItemsRef.current.findIndex(x => x.src === src);
        if (idx < 0)
            return;

        // already open at correct slide
        if (lb.pswp && lb.pswp.currIndex === idx)
            return;

        lockSelection();
        lb.loadAndOpen(idx);
    }, [lightboxReady, imgItems.length, searchParams, location.key]);

    // destroy on unmount
    React.useEffect(() => {
        return () => {
            activeRequestRef.current?.abort();
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

        // iPhone Safari re-letterboxes an edge-to-edge page around its chrome
        // when the document stops being scrollable, and can leave it that way
        // after the drawer closes. Pin the scroll position and swallow
        // background touch scrolling instead of toggling body overflow there.
        const lockedScroll = { x: window.scrollX, y: window.scrollY };

        const preventBackgroundTouchScroll = (event: TouchEvent) => {
            if (event.target instanceof Element && event.target.closest(".drawer"))
                return;
            event.preventDefault();
        };

        const restoreScroll = () => {
            if (window.scrollX !== lockedScroll.x || window.scrollY !== lockedScroll.y)
                window.scrollTo(lockedScroll.x, lockedScroll.y);
        };

        const lockBodyOverflow = !usesIPhoneDocumentViewportWorkaround();
        const prevOverflow = document.body.style.overflow;
        if (lockBodyOverflow)
            document.body.style.overflow = "hidden";
        document.addEventListener("touchmove", preventBackgroundTouchScroll, { passive: false });
        window.addEventListener("scroll", restoreScroll);

        return () => {
            document.removeEventListener("touchmove", preventBackgroundTouchScroll);
            window.removeEventListener("scroll", restoreScroll);
            if (lockBodyOverflow)
                document.body.style.overflow = prevOverflow;
        };
    }, [menuOpen]);

    React.useEffect(() => {
        settingsRef.current = settings;
        saveSettings(settings);
    }, [settings]);

    return (
        <div className="gallery-page">
            <div ref={safeAreaProbeRef} className="safe-area-probe" aria-hidden="true" />
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

                <h2 style={{ margin: 0 }}>Sprintboard Images</h2>
            </div>

            <div style={{ height: 12 }} />

            <GalleryDrawer
                open={menuOpen}
                curDir={curDir}
                settings={settings}
                onChangeSettings={(updater) => setSettings(updater)}
                onClose={() => setMenuOpen(false)}
                onRefreshNow={async () => {
                    const response = await fetch("/api/images/index/refresh", {
                        method: "POST",
                    });
                    if (!response.ok) {
                        alert(`Refresh failed: HTTP ${response.status}`);
                        return;
                    }
                    navigate(0);
                }}
            />

            <Breadcrumbs dir={curDir} onNavigate={p => navigate(`/images/${p}`)} />

            <div style={{ height: 12 }} />

            <form
                onSubmit={(e) => {
                    e.preventDefault();
                    setSettings(prev => ({ ...prev, searchText: searchBoxText }));
                }}
            >
                <input
                    type="search"
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
                        fontSize: 16,
                    }}
                />
            </form>

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
