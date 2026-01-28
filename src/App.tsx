import { Routes, Route, Navigate } from "react-router-dom";

import Dashboard from "./pages/Dashboard";
import Gallery from "./pages/Gallery";
import ImageDetails from "./pages/ImageDetails";


export default function App() {
  return (
    <Routes>
      <Route path="/" element={<Navigate to="/dashboard" replace />} />
      <Route path="/dashboard/*" element={<Dashboard />} />
      <Route path="/images/*" element={<Gallery />} />
      <Route path="/imagedetails" element={<ImageDetails />} />
      <Route path="*" element={<div style={{ padding: 16 }}>Not found. Try <a href="/images">/images</a></div>} />
    </Routes>
  );
}
