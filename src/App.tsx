import React from "react";
import { NavLink, Route, Routes, useLocation } from "react-router-dom";

import { loadSettings, saveSettings } from "./func/ViewerSetings";
import Dashboard from "./pages/Dashboard";
import Gallery from "./pages/Gallery";
import ImageDetails from "./pages/ImageDetails";
import Settings from "./pages/Settings";


const LAST_IMAGE_LOCATION_KEY = "sprintboard-last-image-location";

function readLastImageLocation(): string {
  try {
    const stored = sessionStorage.getItem(LAST_IMAGE_LOCATION_KEY);
    return stored?.startsWith("/images") ? stored : "/images";
  } catch {
    return "/images";
  }
}

function HomeIcon() {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true">
      <path d="m3 11 9-8 9 8" />
      <path d="M5 10v10h14V10M9 20v-6h6v6" />
    </svg>
  );
}

function ImageIcon() {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true">
      <rect x="3" y="4" width="18" height="16" rx="2" />
      <circle cx="8.5" cy="9" r="1.5" />
      <path d="m4 17 5-5 3.5 3.5 2-2L20 19" />
    </svg>
  );
}

function SettingsIcon() {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true">
      <circle cx="12" cy="12" r="3" />
      <path d="M19.4 15a1.7 1.7 0 0 0 .3 1.9l.1.1-2.8 2.8-.1-.1a1.7 1.7 0 0 0-1.9-.3 1.7 1.7 0 0 0-1 1.6v.2h-4V21a1.7 1.7 0 0 0-1-1.6 1.7 1.7 0 0 0-1.9.3l-.1.1L4.2 17l.1-.1a1.7 1.7 0 0 0 .3-1.9A1.7 1.7 0 0 0 3 14H2.8v-4H3a1.7 1.7 0 0 0 1.6-1 1.7 1.7 0 0 0-.3-1.9L4.2 7 7 4.2l.1.1a1.7 1.7 0 0 0 1.9.3A1.7 1.7 0 0 0 10 3v-.2h4V3a1.7 1.7 0 0 0 1 1.6 1.7 1.7 0 0 0 1.9-.3l.1-.1L19.8 7l-.1.1a1.7 1.7 0 0 0-.3 1.9 1.7 1.7 0 0 0 1.6 1h.2v4H21a1.7 1.7 0 0 0-1.6 1Z" />
    </svg>
  );
}

function AppShell() {
  const location = useLocation();
  const [settings, setSettings] = React.useState(loadSettings);
  const isImageView = location.pathname.startsWith("/images");
  const currentLocation = `${location.pathname}${location.search}${location.hash}`;
  const imageTabLocation = isImageView ? currentLocation : readLastImageLocation();

  React.useEffect(() => {
    if (!isImageView)
      return;

    try {
      sessionStorage.setItem(LAST_IMAGE_LOCATION_KEY, currentLocation);
    } catch {
      // Navigation still works when session storage is unavailable.
    }
  }, [currentLocation, isImageView]);

  React.useEffect(() => {
    saveSettings(settings);
  }, [settings]);

  return (
    <div className="app-shell">
      <aside className="app-rail">
        <nav className="app-rail-nav" aria-label="Application views">
          <NavLink
            to="/"
            end
            className={({ isActive }) => `app-rail-link${isActive ? " active" : ""}`}
            aria-label="Front View"
            title="Front View"
          >
            <HomeIcon />
          </NavLink>
          <NavLink
            to={imageTabLocation}
            className={() => `app-rail-link${isImageView ? " active" : ""}`}
            aria-label="Image View"
            title="Image View"
          >
            <ImageIcon />
          </NavLink>
          <NavLink
            to="/settings"
            className={({ isActive }) => `app-rail-link app-rail-settings${isActive ? " active" : ""}`}
            aria-label="Settings"
            title="Settings"
          >
            <SettingsIcon />
          </NavLink>
        </nav>
      </aside>

      <main className="app-shell-content">
        <Routes>
          <Route path="/" element={<Dashboard />} />
          <Route
            path="/images/*"
            element={<Gallery settings={settings} onChangeSettings={setSettings} />}
          />
          <Route
            path="/settings"
            element={<Settings settings={settings} onChangeSettings={setSettings} />}
          />
          <Route path="*" element={<div className="app-page">Page not found.</div>} />
        </Routes>
      </main>
    </div>
  );
}

export default function App() {
  return (
    <Routes>
      <Route path="/imagedetails" element={<ImageDetails />} />
      <Route path="/*" element={<AppShell />} />
    </Routes>
  );
}
