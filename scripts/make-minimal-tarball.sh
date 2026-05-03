#!/bin/bash
#
# make-minimal-tarball.sh — produce a "minimal" source tarball for downstream
# distributors (GentooPlayer, AudioLinux, etc.) that manage system-level
# tuning through their own framework.
#
# The minimal tarball is identical to the standard one EXCEPT that
# webui/profiles/diretta_renderer.json is replaced with the content of
# webui/profiles/diretta_renderer_minimal.json (Family-1 / app-only
# settings), and the latter file is removed (no longer needed since the
# minimal profile is now the only one).
#
# Usage:
#   scripts/make-minimal-tarball.sh [VERSION]
#
# If VERSION is omitted, the script uses `git describe --tags --abbrev=0`
# (latest tag) or "HEAD" as fallback. Run from the repository root.
#
# Optional environment variables:
#   STRIP_SUFFIX=0   keep the "(Minimal)" suffix in product_name
#                    (default: 1, i.e. suffix stripped for a clean user UI)
#   OUTPUT_DIR=...   directory where the tarball is written (default: cwd)
#

set -euo pipefail

VERSION="${1:-$(git describe --tags --abbrev=0 2>/dev/null || echo HEAD)}"
PROJECT="DirettaRendererUPnP"
PREFIX="${PROJECT}-${VERSION}-minimal"
OUTPUT_DIR="${OUTPUT_DIR:-.}"
OUTPUT="${OUTPUT_DIR%/}/${PREFIX}.tar.gz"
STRIP_SUFFIX="${STRIP_SUFFIX:-1}"

# Sanity: must be run from the repo root (where webui/profiles/ lives)
if [ ! -f webui/profiles/diretta_renderer.json ] \
   || [ ! -f webui/profiles/diretta_renderer_minimal.json ]; then
    echo "ERROR: must be run from the repository root, and both" >&2
    echo "       webui/profiles/diretta_renderer.json and" >&2
    echo "       webui/profiles/diretta_renderer_minimal.json must exist." >&2
    exit 1
fi

if ! git diff-index --quiet HEAD --; then
    echo "WARNING: working tree has uncommitted changes — tarball reflects HEAD only." >&2
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Export a clean tree from the current HEAD (or VERSION if it's a ref)
GIT_REF="HEAD"
if git rev-parse --verify --quiet "$VERSION" >/dev/null; then
    GIT_REF="$VERSION"
fi
git archive --prefix="${PREFIX}/" "$GIT_REF" | tar -x -C "$TMPDIR"

PROFILES_DIR="$TMPDIR/$PREFIX/webui/profiles"

# Replace the full profile with the minimal content, then drop the
# minimal file (it no longer has a separate purpose in this flavor).
cp "$PROFILES_DIR/diretta_renderer_minimal.json" "$PROFILES_DIR/diretta_renderer.json"
rm -f "$PROFILES_DIR/diretta_renderer_minimal.json"

# Optionally strip the " (Minimal)" suffix from product_name so the web UI
# doesn't display a cosmetic marker that's now redundant.
if [ "$STRIP_SUFFIX" = "1" ]; then
    sed -i 's/ (Minimal)//' "$PROFILES_DIR/diretta_renderer.json"
fi

# Re-tar
mkdir -p "$OUTPUT_DIR"
tar -czf "$OUTPUT" -C "$TMPDIR" "$PREFIX"

echo "Created: $OUTPUT"
echo "Profiles in tarball:"
tar -tzf "$OUTPUT" | grep -E '/profiles/.+\.json$'
