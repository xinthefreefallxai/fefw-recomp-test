Obelisk Chamber @VERSION@ (Linux x86_64, the game through Proton)

A static recompilation of Fire Emblem: Fortune's Weave for PC. This package contains no game code or data: you need your own
decrypted dump of the game (update 131072), as a folder with exefs/ and romfs/ or as a .nup package.

fefw.exe is a Windows program: the launcher builds it on this PC and starts it through Proton (Steam's own, else umu-run) or Wine.

1. Run fefw-launcher.AppImage (chmod +x it if your file manager did not keep the bit) and pick your dump (Setup page).
2. Press Set up. It checks your dump, recompiles the game code into fefw.exe and translates the shaders, with the tools in
   tools/. Nothing else needs to be installed for that. The first run takes a while; you can pause and continue later.
3. Press Play. This needs Steam with a Proton installed, umu-launcher (umu-run) or Wine; Settings > Runner sets another command.
   Or press Add to Steam on the Play page: Steam then runs fefw.exe with Proton, also in the Steam Deck's Game Mode.

Updating: the launcher's About page installs a new release (or extract the new package over this folder). Then press Set up again
in the launcher if it asks (Desktop Mode on a Steam Deck) before playing from Steam. Your saves are kept (save/).

Licences: LICENSE.txt, LEGAL.txt and licenses/.
