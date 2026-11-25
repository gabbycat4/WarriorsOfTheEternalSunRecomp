#!/usr/bin/env bash
# Builds "Warriors of the Eternal Sun Recompiled: Easy on the Eyes" as an
# AppImage on this PC.
#
#   bash tools/release/make_appimage.sh
#
# Writes WarriorsOfTheEternalSun-EasyOnTheEyes-x86_64.AppImage in the
# repository folder. Needs: cmake, ninja, gcc, SDL2 development files, curl,
# and your ROM at game/wotes.bin (the build generates the game's code from it;
# the ROM itself is never put in the AppImage - checked below).
#
# Release rules (segagenesisrecomp/RELEASING.md): a separate build with the
# developer diagnostics OFF, the PolyForm Noncommercial LICENSE and the
# third-party notices inside, the launcher assets, and no ROM/saves/logs.
# SDL2 is NOT bundled (every desktop distribution has it); the program is
# built against this system's C library, so it runs on distributions as new
# as this one.
set -euo pipefail
cd "$(dirname "$0")/../.."
build=build-release
app="$build/AppDir"
out=WarriorsOfTheEternalSun-EasyOnTheEyes-x86_64.AppImage

[ -f game/wotes.bin ] || { echo "Put your ROM at game/wotes.bin first." >&2; exit 1; }

cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DGEN_ENABLE_TRACE=OFF -DSONIC_REVERSE_DEBUG=OFF
grep -q '^GEN_ENABLE_TRACE:BOOL=OFF' "$build/CMakeCache.txt" &&
grep -q '^SONIC_REVERSE_DEBUG:BOOL=OFF' "$build/CMakeCache.txt" ||
    { echo "Developer diagnostics are not off; stopping." >&2; exit 1; }
cmake --build "$build" --target WotESRecomp --parallel

rm -rf "$app"
mkdir -p "$app/usr/bin" "$app/usr/share/wotes" "$app/usr/share/doc/wotes"
cp "$build/WotESRecomp" "$app/usr/bin/"
strip "$app/usr/bin/WotESRecomp"
cp -R "$build/assets" "$app/usr/share/wotes/assets"
cp LICENSE "$app/usr/share/doc/wotes/LICENSE"
cp segagenesisrecomp/THIRD-PARTY-LICENSES.md "$app/usr/share/doc/wotes/"
cp packaging/linux/README.txt "$app/usr/share/doc/wotes/README.txt"
cp packaging/linux/AppRun "$app/AppRun"
chmod +x "$app/AppRun"
cp packaging/linux/wotes-easyontheeyes.desktop packaging/linux/wotes-easyontheeyes.png "$app/"
ln -s wotes-easyontheeyes.png "$app/.DirIcon"
echo "$(git describe --tags --always --dirty)-$(date +%s)" > "$app/VERSION"

# Nothing from the game, no saves, no logs, no recordings.
bad=$(find "$app" -type f \( -iname '*.bin' -o -iname '*.gen' -o -iname '*.smd' -o -iname '*.md.rom' \
      -o -iname '*.srm' -o -iname '*.sav' -o -iname '*.log' -o -iname '*.replay*' -o -iname 'ramdump*' \
      -o -iname 'savestate*' -o -iname 'native_save*' -o -iname 'settings.ini' -o -iname 'rom.cfg' \) )
if [ -n "$bad" ]; then echo "Refusing: these must not be in the package:" >&2; echo "$bad" >&2; exit 1; fi
romsize=$(stat -c %s game/wotes.bin)
copy=$(find "$app" -type f -size "${romsize}c" -exec cmp -s {} game/wotes.bin \; -print)
if [ -n "$copy" ]; then echo "The ROM is in the package ($copy); stopping." >&2; exit 1; fi

# Everyone must be able to read the package (and run the program / AppRun),
# whatever the permissions of the source files were.
chmod -R u+rwX,go+rX,go-w "$app"
chmod 755 "$app/AppRun" "$app/usr/bin/WotESRecomp"

tool="$build/appimagetool"
if [ ! -x "$tool" ]; then
    curl -fsSL -o "$tool" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$tool"
fi
ARCH=x86_64 "$tool" --appimage-extract-and-run "$app" "$out"
echo
echo "Built $out"
echo "Contents:"; (cd "$app" && find . -type f | sort | sed 's/^/  /' | grep -v '/assets/' ; echo "  ./usr/share/wotes/assets/... (launcher fonts and pictures)")
