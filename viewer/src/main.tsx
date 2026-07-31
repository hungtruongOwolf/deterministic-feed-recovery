import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";
import "./ui/theme.css";
import "./ui/layout.css";

const root = document.getElementById("root");
if (root === null) {
  throw new Error("index.html is missing #root");
}
createRoot(root).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
