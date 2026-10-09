#!/usr/bin/env bash
# Configure and build both payloads. Release is the default.
#   ./build.sh
#   ./build.sh Debug
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$root"

build_type=${1:-Release}
case "$build_type" in
  Release|Debug) ;;
  *)
    echo "Build type must be Release or Debug" >&2
    exit 1
    ;;
esac

export PS4_PAYLOAD_SDK="${PS4_PAYLOAD_SDK:-/opt/ps4-payload-sdk}"
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"

if [ ! -x "$PS4_PAYLOAD_SDK/bin/orbis-cmake" ] || [ ! -x "$PS5_PAYLOAD_SDK/bin/prospero-cmake" ]; then
  echo "PS4 and PS5 payload SDKs were not found. Run this from WSL, where they live under /opt." >&2
  exit 1
fi

jobs=$(nproc 2>/dev/null || echo 1)

echo "Building PS4 ($build_type)"
"$PS4_PAYLOAD_SDK/bin/orbis-cmake" -S "$root" -B "$root/build/ps4" -DCMAKE_BUILD_TYPE="$build_type"
cmake --build "$root/build/ps4" -j"$jobs"

echo "Building PS5 ($build_type)"
"$PS5_PAYLOAD_SDK/bin/prospero-cmake" -S "$root" -B "$root/build/ps5" -DCMAKE_BUILD_TYPE="$build_type"
cmake --build "$root/build/ps5" -j"$jobs"

echo "PS4: $root/build/bin/disc-ps4.elf"
echo "PS5: $root/build/bin/disc-ps5.elf"
