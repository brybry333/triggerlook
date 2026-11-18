#!/bin/sh
# Install the Linux build for the local CS:S:
#   install/libtriggerlook.so (the LD_PRELOAD loader, in css-launch) + install/triggerlook.so (the mod)
# The mod writes its game files itself at startup (cstrike/materials/triggerlook, cstrike/resource/triggerlook).
# Settings live in ~/.config/triggerlook/settings.txt (never touched here). Windows: win/build.sh -> triggerlook.dll.
set -e
cd "$(dirname "$0")"
CSS="${CSS:-$HOME/.local/share/Steam/steamapps/common/Counter-Strike Source/cstrike}"
mkdir -p install
for f in libtriggerlook.so triggerlook.so; do   # atomic: a running game keeps its old copy
  cp "build/$f" "install/.$f.tmp" && mv -f "install/.$f.tmp" "install/$f"
done
rm -f install/game_shader_generic_triggerlook.so    # the first version's mod (a shader library)
rm -rf "$CSS/custom/triggerlook"                     # the first version's game files
echo "installed: $(pwd)/install"
echo "preload:   $(pwd)/install/libtriggerlook.so (LD_PRELOAD it; the path must not contain spaces)"
