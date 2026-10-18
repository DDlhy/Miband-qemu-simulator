#!/usr/bin/env bash
set -euo pipefail
MODEL=${1:?Usage: bash tools/build-qemu.sh band11|band10pro}
case "$MODEL" in band11|band10pro) ;; *) exit 2 ;; esac
PROJECT_ROOT=$(cd "${BASH_SOURCE[0]%/*}/.." && pwd)
TOOLCHAIN_ROOT=${TOOLCHAIN_ROOT:-/c/msys64}
COMPILE_ROOT=${QEMU_BUILD_ROOT:-$PROJECT_ROOT/.build/$MODEL}
export PATH="$TOOLCHAIN_ROOT/ucrt64/bin:$TOOLCHAIN_ROOT/usr/bin:$PATH"
# MinGW tools can emit the Windows ACP even under a UTF-8 Python locale.
# Meson's legacy decoder tolerates these compiler diagnostics.
export PYTHONUTF8=0 PYTHONIOENCODING=utf-8 LC_ALL=C LANG=C
for dep in gcc g++ python ninja pkg-config; do
  command -v "$dep" >/dev/null || { printf 'Missing build dependency: %s\n' "$dep" >&2; exit 1; }
done
mkdir -p "$COMPILE_ROOT/build" "$COMPILE_ROOT/tmp"
export TMPDIR="$COMPILE_ROOT/tmp"
export TMP="$(cygpath -m "$TMPDIR")" TEMP="$(cygpath -m "$TMPDIR")"
if [[ ! -d "$COMPILE_ROOT/qemu" ]]; then
  python "$PROJECT_ROOT/tools/prepare_source.py" --model "$MODEL" --output "$(cygpath -m "$COMPILE_ROOT/qemu")"
fi
cd "$COMPILE_ROOT/build"
if [[ ! -f build.ninja ]]; then
  if [[ "$MODEL" == band11 ]]; then
    "$COMPILE_ROOT/qemu/configure" --cc=gcc --cxx=g++ --target-list=arm-softmmu --with-devices-arm=q66 \
      --without-default-devices --without-default-features --enable-tcg --enable-sdl \
      --enable-vnc --enable-pixman --disable-rust --disable-tools --disable-docs \
      --disable-tests --disable-install-blobs --enable-hmp
  else
    "$COMPILE_ROOT/qemu/configure" --cc=gcc --cxx=g++ --target-list=arm-softmmu \
      --without-default-devices --disable-docs --disable-tools --disable-user \
      --disable-guest-agent --disable-slirp --enable-sdl --disable-gtk --disable-curses \
      --disable-opengl --disable-virglrenderer --disable-spice --disable-gnutls \
      --disable-capstone --disable-libnfs --disable-libiscsi --disable-rbd \
      --disable-xen --disable-kvm --disable-werror --disable-rust
  fi
fi
if [[ "${CONFIGURE_ONLY:-0}" == 1 ]]; then
  printf 'Configured %s\n' "$COMPILE_ROOT/build"
  exit 0
fi
ninja -j"${BUILD_JOBS:-4}" qemu-system-arm.exe
printf 'Built %s\n' "$COMPILE_ROOT/build/qemu-system-arm.exe"
