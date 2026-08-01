#!/usr/bin/env bash
# Builds the library for the browser, into the viewer's public/ so a static build carries it.
#
# The flags matter and are chosen rather than copied:
#
#   -O3 -flto            the run is a few hundred thousand operations; a debug build is visibly slower on a
#                        phone, and there is no debugger on the far side to keep symbols for.
#   -fno-exceptions      the library throws nothing. Leaving them on costs size for a feature nothing uses.
#   -sMODULARIZE         the page loads this alongside a React app; a module that installed itself on the
#                        global object would be a name collision waiting for the next dependency.
#   -sALLOW_MEMORY_GROWTH the recovery buffer is bounded but the trace of a 1,200-message run is not tiny,
#                        and a fixed heap would mean choosing the maximum for every visitor's phone.
#   -sFILESYSTEM=1       the writers write to a FILE*, which is how the browser and the terminal share one
#                        formatter instead of having two. See wasm/api.cpp.
#   NDEBUG               the library's own assertions are precondition checks on internal invariants, and an
#                        abort in WebAssembly kills the module for the life of the page. The boundary in
#                        api.cpp clamps its inputs instead, which is what those preconditions ask for.
#   -sSTACK_SIZE=4MB     trace::recorder<4096> is 640 KB of flat array and the client is another 53 KB, both
#                        stack locals by design: "an object this size is a member, not a stack local" is a
#                        rule the library states and this respects. WebAssembly defaults to a 64 KB stack,
#                        which native never does, so the first run scribbled off the end of memory. Native
#                        gives 8 MB; 4 MB is the same decision made explicitly.
#   -sINITIAL_MEMORY     enough for the largest run so the heap does not grow four times on first click.
#   ENVIRONMENT=...,node the browser is the target and node is not, but a module that cannot be loaded
#                        outside a browser cannot be checked against the native build, and that check is
#                        the only evidence the page runs this library rather than a port of it. A few
#                        kilobytes of loader is a cheap price for a verifiable artifact.
# The output is committed so that Pages can deploy without an Emscripten toolchain. It is not
# byte-reproducible across emsdk versions or hosts, and CI does not pretend otherwise: it checks the
# committed module's *behaviour* against the native build rather than diffing the binary.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${here}/viewer/public/wasm"

if ! command -v emcc >/dev/null 2>&1; then
  for candidate in "${EMSDK:-}" "${HOME}/emsdk" /opt/homebrew/opt/emscripten/libexec; do
    if [[ -n "${candidate}" && -f "${candidate}/emsdk_env.sh" ]]; then
      # shellcheck disable=SC1091
      source "${candidate}/emsdk_env.sh" >/dev/null 2>&1
      break
    fi
  done
fi

if ! command -v emcc >/dev/null 2>&1; then
  echo "build-wasm: emcc not found. Install the Emscripten SDK, or source its emsdk_env.sh." >&2
  exit 1
fi

mkdir -p "${out}"

emcc "${here}/wasm/api.cpp" \
  -I "${here}/include" \
  -I "${here}/tools" \
  -std=c++20 \
  -O3 -flto \
  -DNDEBUG \
  -fno-exceptions \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sEXPORT_NAME=createDfr \
  -sALLOW_MEMORY_GROWTH=1 \
  -sSTACK_SIZE=4MB \
  -sINITIAL_MEMORY=32MB \
  -sFILESYSTEM=1 \
  -sEXPORTED_FUNCTIONS='["_dfr_run_trace","_dfr_run_session","_dfr_run_glimpse","_malloc","_free"]' \
  -sEXPORTED_RUNTIME_METHODS='["ccall","cwrap","UTF8ToString"]' \
  -sENVIRONMENT=web,worker,node \
  -o "${out}/dfr.js"

echo "build-wasm: wrote ${out}/dfr.js and ${out}/dfr.wasm"
ls -la "${out}"
