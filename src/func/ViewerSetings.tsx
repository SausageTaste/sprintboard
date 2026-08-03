
type ImageSortOrder = "date-desc" | "date-asc" | "name-asc" | "name-desc";
type ImageViewMode = "grid" | "list";

type ViewerSettings = {
    searchText: string;
    thumbnailSize: number;
    imageSortOrder: ImageSortOrder;
    imageViewMode: ImageViewMode;
    railCollapsed: boolean;
    fillScreen: boolean;
    edgeToEdge: boolean;
    filesRecursive: boolean;
    viewerDiagnostics: boolean;
    avifOnly: boolean;
};

const DEFAULT_SETTINGS: ViewerSettings = {
    searchText: "",
    thumbnailSize: 150,
    imageSortOrder: "date-desc",
    imageViewMode: "grid",
    railCollapsed: false,
    fillScreen: false,
    edgeToEdge: true,
    filesRecursive: false,
    viewerDiagnostics: false,
    avifOnly: false,
};


function loadSettings(): ViewerSettings {
    try {
        const raw = localStorage.getItem("gallery-settings");
        if (!raw) return DEFAULT_SETTINGS;
        return { ...DEFAULT_SETTINGS, ...JSON.parse(raw) };
    } catch {
        return DEFAULT_SETTINGS;
    }
}


function saveSettings(settings: ViewerSettings) {
    localStorage.setItem(
        "gallery-settings",
        JSON.stringify(settings)
    );
}


export {
    loadSettings,
    saveSettings,
};

export type {
    ImageSortOrder,
    ImageViewMode,
    ViewerSettings,
};
