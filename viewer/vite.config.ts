import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Relative base so the built bundle works from a GitHub Pages subpath as well as from a file
// server, without a deploy-time rewrite.
export default defineConfig({
  base: "./",
  plugins: [react()],
  build: { outDir: "dist", sourcemap: true },
});
