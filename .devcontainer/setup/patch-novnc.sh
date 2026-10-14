#!/usr/bin/env bash
set -Eeuo pipefail

UI_JS="${UI_JS:-/usr/share/novnc/app/ui.js}"

if [ ! -f "$UI_JS" ]; then
    echo "Warning: $UI_JS not found, skipping patch."
    exit 0
fi

echo "Patching noVNC for direct seamless clipboard synchronization..."

SUDO=""
if command -v sudo >/dev/null 2>&1 && [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
fi

$SUDO node -e '
const fs = require("fs");
const file = process.argv[1];
let code = fs.readFileSync(file, "utf8");

// 1. Direct Copy (Remote X11 -> Host OS)
if (!code.includes("/* PATCHED_SEAMLESS_CLIPBOARD_RECEIVE */")) {
  code = code.replace(
    /clipboardReceive\(e\)\s*\{/,
    `clipboardReceive(e) { /* PATCHED_SEAMLESS_CLIPBOARD_RECEIVE */
        if (navigator.clipboard && navigator.clipboard.writeText && e.detail && e.detail.text) {
            navigator.clipboard.writeText(e.detail.text).catch(() => {});
        }`
  );
}

// 2. Direct Paste & Tab Focus Sync (Host OS -> Remote X11)
if (!code.includes("/* PATCHED_SEAMLESS_CLIPBOARD_SEND */")) {
  code = code.replace(
    /addClipboardHandlers\(\)\s*\{/,
    `addClipboardHandlers() { /* PATCHED_SEAMLESS_CLIPBOARD_SEND */
        window.addEventListener("focus", () => {
            if (navigator.clipboard && navigator.clipboard.readText) {
                navigator.clipboard.readText().then((text) => {
                    if (text && UI.rfb) {
                        UI.rfb.clipboardPasteFrom(text);
                    }
                }).catch(() => {});
            }
        });

        window.addEventListener("paste", (e) => {
            if (e.target && (e.target.tagName === "TEXTAREA" || e.target.tagName === "INPUT")) return;
            const text = (e.clipboardData || window.clipboardData)?.getData("text");
            if (text && UI.rfb) {
                UI.rfb.clipboardPasteFrom(text);
            }
        });`
  );
}

fs.writeFileSync(file, code);
console.log("Successfully patched " + file);
' "$UI_JS"
