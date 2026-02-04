
type ViewerSettings = {
    searchText: string;
    thumbnailSize: number;
    fillScreen: boolean;
    filesRecursive: boolean;
};

const DEFAULT_SETTINGS: ViewerSettings = {
    searchText: "",
    thumbnailSize: 150,
    fillScreen: false,
    filesRecursive: false,
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
    ViewerSettings,
};
