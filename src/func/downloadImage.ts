function triggerDownload(href: string) {
    const link = document.createElement("a");
    link.href = href;
    link.download = "";
    link.rel = "noopener";
    document.body.appendChild(link);
    link.click();
    link.remove();
}

export function downloadImage(src: string) {
    if (!src)
        return;

    triggerDownload(src);
}

export function downloadSourceImage(src: string) {
    if (!src)
        return;

    const url = new URL("/api/images/download", window.location.origin);
    url.searchParams.set("path", src);
    triggerDownload(url.toString());
}
