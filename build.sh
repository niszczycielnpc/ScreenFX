#!/usr/bin/env bash
# Builds SCFX00001.elf inside an OnionHEN source checkout.
#
#   PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk \
#   ./build.sh /path/to/onionHEN /path/to/onionHEN-plugin-sdk
#
# The OnionHEN checkout gets one extra add_subdirectory() line and its own
# util/daemon/bootstrapper/unpacker targets are skipped (they need extra blobs).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ONION="${1:?path to an onionHEN checkout}"
SDK="${2:?path to an onionHEN-plugin-sdk checkout}"
: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK}"
LISTS="${ONION}/source/CMakeLists.txt"

sed -i -E 's/^add_subdirectory\((util|daemon|bootstrapper|unpacker)\)/# &/' "${LISTS}"
grep -q 'SCREENFX_DIR' "${LISTS}" ||
  printf '\nif(SCREENFX_DIR)\n\tadd_subdirectory("${SCREENFX_DIR}/intree" screenfx)\nendif()\n' >> "${LISTS}"

BUILD="${BUILD_DIR:-${HERE}/build}"
"${PS5_PAYLOAD_SDK}/bin/prospero-cmake" -S "${ONION}/source" -B "${BUILD}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSCREENFX_DIR="${HERE}" -DONIONHEN_PLUGIN_SDK_SOURCE="${SDK}"
cmake --build "${BUILD}" --target screenfx
echo "Built ${BUILD}/bin/SCFX00001.elf"
