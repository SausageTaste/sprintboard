import { Routes, Route, Navigate } from "react-router-dom";
import Gallery from "./Gallery";

export default function App() {
  return (
    <Routes>
      <Route path="/" element={<Navigate to="/images" replace />} />
      <Route path="/images/*" element={<Gallery />} />
      <Route path="*" element={<div style={{ padding: 16 }}>Not found. Try <a href="/images">/images</a></div>} />
    </Routes>
  );
}
