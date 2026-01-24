import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],
  server: {
    host: true,        // = 0.0.0.0
    proxy: {
      "/api": "http://localhost:8000",
      "/images": "http://localhost:8000",
      "/2026-01-25": "http://localhost:8000",
      "/2026-01-23": "http://localhost:8000",
    },
  },
})
