# Camouflage implementation checkpoint

Branch: `experimental/camo-feature`. Working baseline: `74a73501fe1867af0623e0e2e4d376d309ae00bf` (`steam-login`).

## Required behavior

- A separate Camouflage section in Extras, with a vehicle list, actual 3D preview, PNG import and saved presets.
- Adjustable offset, scale, rotation, blend mode and opacity.
- An action-menu button applies a preset to the selected vehicle instance only.
- Rendering changes are client-local; no simulation commands, global texture replacement or server dependency.
- Preserve the installed DLL before deployment. Do not deploy a build as the finished feature until the two-identical-vehicles isolation check passes.

## Current state

Only the alternate compiler support is implemented. No camouflage UI or runtime modification is implemented or installed.
`python tools/build.py camo` successfully builds a separate `build/camo/iphlpapi.dll` using portable LLVM/MinGW with `GOH_LLVM_MINGW` pointing to its root. The standard Zig route remains the default.

## Static findings (GoH 1.067; require live verification)

- Existing Extras layout: `resource/interface/interface.pak`, `layout/PageExtras.xml`.
- Native library layout: `layout/views/LibraryView.xml`: vehicle list `lb_entries`, category `cb_library`, preview `display`, statistics `pnl_info`.
- Native library class `eGUI_Library@GUI2`, main vtable RVA `103d930`; event handler `2831c0`, message handler `283350`. Preview controller appears at instance + `280`.
- Native library already exposes texture modifiers through `eBaseLibrary`: `129fb0`, localized labels in `12a380`. This does not establish support for arbitrary PNG images.
- Skeleton texture modifier routine `715ca0` takes a skeleton, modifier descriptor, notification-suppression boolean and deferred-execution boolean. It can queue operations; its immediate path traverses views through `55c300`.
- The modifier descriptor contains two 0x60-byte engine string objects followed by a vector of per-material overrides at +c0 (entry stride 0x88). It is not compatible with a C string or this toolchain's C++ std::string.
- Engine string construction from a C string: `556310`; copy `556270`; destruction `5564e0`. Ownership must stay with the game allocator.
- `55b480` resolves a modifier against the material-name registry (`154b338`); an arbitrary unregistered name may resolve to empty. Do not assume calling `715ca0` with a new name works.
- `55aef0` loads/caches available modifiers for a material; returned vector has 0x68-byte entries. `55c000` builds material names with `#modifier` and optionally `$`.
- `ePolyView` vtable `10baa08`, slot +90 -> `576200`; texture-modification code creates variant geometry names with `#modifier.ply`. Material texture setters are called via virtual slot +60. This requires ownership/lifetime checks before any patch.
- `eGameControl` vtable `11104f8`; constructor `aee9e0`. A validated selected-entity pointer and its lifetime have NOT been established.
- Example native vehicle: `entity/-vehicle.pak`, `-vehicle/ussr/tank_medium/t3476_41/`; `base.mtl` uses diffuse `$/_vehicle/t34_76_41`, separate bump/specular textures and separate track material.

## Required integration checks

1. Determine selected entity and preview entity without guessing offsets.
2. Establish a per-instance material clone and texture-resource lifetime. Test two identical vehicles; only one may change.
3. Implement PNG compositing while preserving original normal/specular data and alpha cutouts; share the same path between preview and battle.
4. Add Extras and action-menu UI with input capture and preset persistence.
5. Check deselection/reselection, death, loading another map, replay restore, invalid PNG, missing PNG and restoring the original skin.

## Launch recovery

The direct computer-use EXE launch inherited `C:/Program Files/WindowsApps` as its working directory, and the game failed with `Failed to mount localization`. The installed DLL was unchanged (SHA-256 `a5bb393fbcef25cec44ddbe69d1da4d883742c354ad833ed1f028240f8f65851`). The user verified normal launch through Steam. Use Steam for further game launches.
