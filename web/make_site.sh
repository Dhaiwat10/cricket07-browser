#!/bin/sh
# Assembles a folder you can put on any static host: the page and the browser build, no game files (each
# player picks their own game folder, which stays in their browser).
#   web/make_site.sh [out]     (default: site/; run build_web.sh first)
# The host must send COOP/COEP headers on every file or threads are disabled; the _headers file written here
# does that on Cloudflare Pages and Netlify. Elsewhere, set these yourself:
#   Cross-Origin-Opener-Policy: same-origin
#   Cross-Origin-Embedder-Policy: require-corp
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-site}
[ -f "$R/build_web/www/recomp.wasm" ] || { echo "run ./build_web.sh first" >&2; exit 1; }
mkdir -p "$OUT/build"
for f in index.html loader.js netplay.js audio-worklet.js inter-latin.woff2 inter-latin-ext.woff2 inter-LICENSE.txt; do
    cp "$R/web/$f" "$OUT/$f"
done
cp "$R/build_web/www/recomp.js" "$R/build_web/www/recomp.wasm" "$OUT/build/"
cat > "$OUT/_headers" <<'H'
/*
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp
  Cross-Origin-Resource-Policy: same-origin
  X-Robots-Tag: noindex
H
echo "$OUT/: page + build ($(du -sh "$OUT" | cut -f1)); upload it to a static host with the headers above"
