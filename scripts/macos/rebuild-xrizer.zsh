#!/bin/zsh
set -euo pipefail

xrizer_version="macos-alyx-stage-dds"
pull=1

while (( $# > 0 )); do
    case "$1" in
        --xrizer-version)
            if (( $# < 2 )); then
                print -u2 "Error: --xrizer-version requires a value"
                exit 2
            fi
            xrizer_version="$2"
            shift 2
            ;;
        --xrizer-version=*)
            xrizer_version="${1#*=}"
            shift
            ;;
        --no-pull)
            pull=0
            shift
            ;;
        -h|--help)
            cat <<EOF
Usage: ${0:t} [--no-pull] [--xrizer-version VERSION]

Options:
  --no-pull
      Build the current XRizer checkout without running git pull.
  --xrizer-version VERSION
      Set XRIZER_VERSION for the xrizer build.
      Default: macos-alyx-stage-dds
EOF
            exit 0
            ;;
        *)
            print -u2 "Unknown argument: $1"
            print -u2 "Usage: ${0:t} [--no-pull] [--xrizer-version VERSION]"
            exit 2
            ;;
    esac
done

owd=$(pwd)

cd "$HOME/Code/monado-2/build-wine-dxmt/xrizer-src"
if (( pull )); then
    git pull
fi

PATH="/opt/homebrew/opt/rustup/bin:$PATH" \
XRIZER_VERSION="$xrizer_version" \
BINDGEN_EXTRA_CLANG_ARGS_x86_64_pc_windows_gnu='-isystem /opt/homebrew/Cellar/mingw-w64/14.0.0_3/toolchain-x86_64/x86_64-w64-mingw32/include/c++/16.2.0 -isystem /opt/homebrew/Cellar/mingw-w64/14.0.0_3/toolchain-x86_64/x86_64-w64-mingw32/include/c++/16.2.0/x86_64-w64-mingw32 -isystem /opt/homebrew/Cellar/mingw-w64/14.0.0_3/toolchain-x86_64/x86_64-w64-mingw32/include/c++/16.2.0/backward -isystem /opt/homebrew/Cellar/mingw-w64/14.0.0_3/toolchain-x86_64/lib/gcc/x86_64-w64-mingw32/16.2.0/include -isystem /opt/homebrew/Cellar/mingw-w64/14.0.0_3/toolchain-x86_64/lib/gcc/x86_64-w64-mingw32/16.2.0/include-fixed -isystem /opt/homebrew/Cellar/mingw-w64/14.0.0_3/toolchain-x86_64/x86_64-w64-mingw32/include' cargo +stable xbuild --release --target x86_64-pc-windows-gnu --no-default-features

cd "$HOME/Code/monado-2"

MONADO_XRIZER_DLL_SOURCE="$HOME/Code/monado-2/build-wine-dxmt/xrizer-src/target/x86_64-pc-windows-gnu/release/openvr_api.dll" \
scripts/macos/provision-xrizer.zsh

cd "$owd"
