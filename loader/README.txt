EchoLoader (dbgcore.dll), the community plugin loader for Echo VR, as the community Echo VR
installer sets it up (the copy that runs the pack's plugins). INSTALL.cmd puts it in bin\win10
when the game has none, and in place of a different build (kept as dbgcore.dll.before-pack;
INSTALL.cmd --keep-loader leaves the game's own in),
keeping the old one as dbgcore.dll.before-pack. It is not part of this pack's own code.
