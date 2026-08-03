import React from "react";

import type { ViewerSettings } from "../func/ViewerSetings";


type SettingsProps = {
    settings: ViewerSettings;
    onChangeSettings: React.Dispatch<React.SetStateAction<ViewerSettings>>;
};

export default function Settings({ settings, onChangeSettings }: SettingsProps) {
    const [refreshing, setRefreshing] = React.useState(false);
    const [refreshStatus, setRefreshStatus] = React.useState("");

    const updateSetting = <Key extends keyof ViewerSettings>(
        key: Key,
        value: ViewerSettings[Key],
    ) => {
        onChangeSettings((current) => ({ ...current, [key]: value }));
    };

    const refreshImageIndex = async () => {
        setRefreshing(true);
        setRefreshStatus("");
        try {
            const response = await fetch("/api/images/index/refresh", { method: "POST" });
            if (!response.ok)
                throw new Error(`HTTP ${response.status}`);
            setRefreshStatus("Image index refreshed.");
        } catch (error) {
            const details = error instanceof Error ? error.message : "Unknown error";
            setRefreshStatus(`Refresh failed: ${details}`);
        } finally {
            setRefreshing(false);
        }
    };

    return (
        <div className="app-page settings-page">
            <header className="settings-header">
                <h2>Settings</h2>
                <p>Configure the image grid and full-screen viewer.</p>
            </header>

            <section className="settings-panel" aria-labelledby="viewer-settings-heading">
                <h3 id="viewer-settings-heading">Viewer</h3>

                <label className="settings-row">
                    <span>Thumbnail size</span>
                    <input
                        type="number"
                        min="50"
                        value={settings.thumbnailSize}
                        onChange={(event) => updateSetting("thumbnailSize", Number(event.target.value))}
                    />
                </label>

                <label className="settings-row">
                    <span>Edge-to-edge viewer</span>
                    <input
                        type="checkbox"
                        checked={settings.edgeToEdge}
                        onChange={(event) => updateSetting("edgeToEdge", event.target.checked)}
                    />
                </label>

                <label className="settings-row">
                    <span>Fill screen</span>
                    <input
                        type="checkbox"
                        checked={settings.fillScreen}
                        onChange={(event) => updateSetting("fillScreen", event.target.checked)}
                    />
                </label>

                <label className="settings-row">
                    <span>Viewer debug overlay</span>
                    <input
                        type="checkbox"
                        checked={settings.viewerDiagnostics}
                        onChange={(event) => updateSetting("viewerDiagnostics", event.target.checked)}
                    />
                </label>

                <label className="settings-row">
                    <span>Recursive images</span>
                    <input
                        type="checkbox"
                        checked={settings.filesRecursive}
                        onChange={(event) => updateSetting("filesRecursive", event.target.checked)}
                    />
                </label>

                <label className="settings-row">
                    <span>AVIF only mode</span>
                    <input
                        type="checkbox"
                        checked={settings.avifOnly}
                        onChange={(event) => updateSetting("avifOnly", event.target.checked)}
                    />
                </label>
            </section>

            <section className="settings-panel" aria-labelledby="settings-actions-heading">
                <h3 id="settings-actions-heading">Actions</h3>
                <button
                    className="settings-action"
                    type="button"
                    disabled={refreshing}
                    onClick={refreshImageIndex}
                >
                    {refreshing ? "Refreshing…" : "Refresh now"}
                </button>
                <div className="settings-status" role="status" aria-live="polite">
                    {refreshStatus}
                </div>
            </section>
        </div>
    );
}
