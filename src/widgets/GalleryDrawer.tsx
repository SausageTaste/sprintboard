import type { ViewerSettings } from "../func/ViewerSetings";


type Props = {
    open: boolean;
    curDir: string;
    settings: ViewerSettings;
    onChangeSettings: (updater: (prev: ViewerSettings) => ViewerSettings) => void;
    onClose: () => void;

    // optional actions
    onRefreshNow?: () => void;
};


export default function GalleryDrawer({
    open,
    curDir,
    settings,
    onChangeSettings,
    onClose,
    onRefreshNow,
}: Props) {
    return (
        <>
            {/* Backdrop */}
            <div
                className={`drawer-backdrop ${open ? "open" : ""}`}
                onClick={onClose}
            />

            {/* Drawer */}
            <aside className={`drawer ${open ? "open" : ""}`} role="dialog" aria-modal="true">
                <div className="drawer-header">
                    <div style={{ fontWeight: 800 }}>Menu</div>
                    <button className="drawer-close" onClick={onClose} aria-label="Close menu">
                        ✕
                    </button>
                </div>

                <div className="drawer-body">
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
                            <span>Thumbnail size</span>
                            <input
                                type="number"
                                value={settings.thumbnailSize}
                                onChange={(e) =>
                                    onChangeSettings((s) => ({
                                        ...s,
                                        thumbnailSize: Number(e.target.value),
                                    }))
                                }
                                style={{ width: 60, height: 30, fontSize: 16 }}
                            />
                        </label>

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
                                    onChangeSettings((s) => ({
                                        ...s,
                                        fillScreen: e.target.checked,
                                    }))
                                }
                                style={{ width: 20, height: 20 }}
                            />
                        </label>

                        <label
                            style={{
                                display: "flex",
                                alignItems: "center",
                                justifyContent: "space-between",
                                padding: "10px 0",
                            }}
                        >
                            <span>Recursive images</span>
                            <input
                                type="checkbox"
                                checked={settings.filesRecursive}
                                onChange={(e) =>
                                    onChangeSettings((s) => ({
                                        ...s,
                                        filesRecursive: e.target.checked,
                                    }))
                                }
                                style={{ width: 20, height: 20 }}
                            />
                        </label>
                    </div>

                    <div className="drawer-section">
                        <div className="drawer-label">Actions</div>
                        <button
                            className="drawer-btn"
                            onClick={() => {
                                onRefreshNow?.();
                                onClose();
                            }}
                        >
                            Refresh now
                        </button>
                    </div>
                </div>
            </aside>
        </>
    );
}
