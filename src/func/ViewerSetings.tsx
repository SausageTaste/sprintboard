
type ViewerSettings = {
    thumbnailSize: number;
    fillScreen: boolean;
};

const DEFAULT_SETTINGS: ViewerSettings = {
    thumbnailSize: 150,
    fillScreen: false,
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
