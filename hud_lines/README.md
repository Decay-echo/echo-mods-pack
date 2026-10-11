# Hide the visor HUD lines

Removes the thin decorative lines on Echo VR's helmet visor and keeps the mute icon, speed and connection bars. Echo's own "heads-up display" setting hides all of them together.

Needs **NvrAssetPatches** (EchoLoader plugin, `bin\win10\asset_patches\manifest.json`).

```
py -3 install_hud_lines.py               # hide the lines (restart Echo VR)
py -3 install_hud_lines.py --uninstall   # bring them back
```

The installer adds `hud_lines_*` entries to the NvrAssetPatches manifest, backing it up once as `manifest.json.hudlines-backup`, and writes the patch files to `asset_patches\hud_lines\`. Every patch is the exact size of the buffer it replaces, because NvrAssetPatches only patches in place.

## What it changes

The line art is one 2048×1024 texture pair on the helmet (material `858d5ac84cc5eadf` in helmet prefab `e53a35d2892e5d88`):

| texture | format | role |
| --- | --- | --- |
| `c98ec7f37bb9c291` | BC1 sRGB | colour |
| `c98ec7f37bbfc38a` | BC3 sRGB | colour + alpha |

Both are streamed:
- **Preview:** the `CGTextureResource` "Win10" buffer (buffer 0 of type `0xB3EBE9E0BF0EB81F`) holds a 256-byte header plus an embedded DDS with mips 128×64 down to 1×1.
- **Full size:** the 256×128 to 2048×1024 mips are separate raw-block resources named `sym("_mip8".."_mip11", seed=texture)`, with manifest type `0xAE49FAD43254367A`.

**Approach 1 (didn't work):** patch the full-size mip resources directly. They never pass through NvrAssetPatches' hook, so they were never patched. Only the preview patch applied, and the lines stayed.

**Approach 2 (current):** `patch\*.desc` from `make_hud_lines_patch.py` replaces only the header buffer. The embedded mips become transparent black, and the full size at `+0xc4`/`+0xc8`/`+0xcc` is set to the embedded size (128×64, 8 mips), so nothing larger is streamed in.

## Readout layout

The readout is moved to the bottom of the view and all of it is hidden: the speed text, the mute icon and the connection bars are moved 2000 px off the canvas. EchoHudPlus draws all three itself, in your HUD colour:
- your speed with your last run's top speed under it;
- the mute icon and connection bars inside its wrist-thruster rings.

Echo always draws the connection bars white, and its speed can't be made bigger. Element `+0x94` is the text's character capacity (33), not a font size, and Echo has no bigger version of that font. The values are in `make_hud_lines_patch.py`:
- **`READOUT_HEIGHT`:** −0.13 m (stock is 0.198 m below eye level). EchoHudPlus's `hud_height_m` must match.
- **`HIDE_ICONS`:** True hides them as above. False keeps Echo's speed and spreads its icons `SPREAD` px either side (stock −24 for mute and +44 for connection).

The three files it writes are whole-resource replacements of the same size:

| file | type | what changes |
| --- | --- | --- |
| `r14_glb_global_mp.transforms` | `CTransformCR`, `0x9C72178D05ADB686` | transform `af0f60ef729284b5` on both player actors: local position `+0x90`, y `-0.198` → `READOUT_HEIGHT` |
| `d66fcf1b64026ebd.canvas` | `CUICanvasResource`, `0x46222C5D2AFEB3FF` | the speed text `41d2ca380e26150d` and the mute sprite `de495c45bdc1b51b` |
| `2d06e14794a11e31.canvas` | `CUICanvasResource` | the network panels `af05b299e02227c0` and `444647855627d557` |

Element x is stored as rect `+0x14`/`+0x1c`, offsets `+0x34`/`+0x3c` and centre `+0x4c`, all relative to the parent.

The mute icon, speed and connection bars are separate canvases (`8fcec01d07930956`, `d66fcf1b64026ebd`, `2d06e14794a11e31`) driven by script `ui_mp_helmet_meter_common` (`500851025e2505f4.dll`), so they're untouched. The cracked-visor damage textures in the same prefab are untouched too.

**If the lines still show** after a restart, check `bin\win10\plugin_logs\NvrAssetPatches\NvrAssetPatches.log` for `PATCHED in-place` lines for both `hud_lines_*` labels. If both applied and the lines remain, the streamer takes the full size from somewhere other than the header.

`dev\` holds the research scripts: canvas and texture rendering, using EchoArcade's `.venv` and its vendored `echovr_pkg.py`.
