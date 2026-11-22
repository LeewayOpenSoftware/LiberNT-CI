# INTEGRATION TABLE — Orosi Forma Icons

**Generated:** 2026-11-21
**Phase:** 1 (Sample integration, before bulk replacement)
**Source:** `LiberNT-v3-assets-2026-11-18/08_iconos/`
**Verified against:** forma/icons-sample on leeway/main c0f1366bb02 (tip 13af124072c1)

## Inventory Summary

| Category | Count | Status |
|----------|-------|--------|
| **Total logical entries** | 349 | Complete |
| **Proposed entries** | 345 | Ready for integration |
| **Reserved entries** | 4 | shell32/50–53 (transparent, untouched) |
| **Additional (new functions)** | 30 | Pending ID assignment / consumers |

**By module:**
- shell32: 234 proposed + 4 reserved
- explorer: 104 proposed
- user32: 7 proposed

## Phase 1 Sample Icons

Tested in the sample integration (forma/icons-sample). Sample set verified
built-in: shells 4/5/8/9/12, user32 101/103, explorer 258 — see
VERIFICATION-REPORT-RYAN (hive shared store) for the PE-level checks
(sizes, 32bpp DIB + AND mask, no PNG, reserves unchanged).

| Module | ID | Label | Symbol | Source ICO | Sizes |
|--------|-----|--------|--------|------------|-------|
| shell32 | 4 | Carpeta | IDI_SHELL_FOLDER | `propuestas_orosi/shell32/4.ico` | 16–256 |
| shell32 | 5 | Carpeta abierta | IDI_SHELL_FOLDER_OPEN | `propuestas_orosi/shell32/5.ico` | 16–256 |
| shell32 | 8 | Unidad USB | IDI_SHELL_REMOVEABLE | `propuestas_orosi/shell32/8.ico` | 16–256 |
| shell32 | 9 | Disco local | IDI_SHELL_DRIVE | `propuestas_orosi/shell32/9.ico` | 16–256 |
| shell32 | 12 | CD-ROM | IDI_SHELL_CDROM | `propuestas_orosi/shell32/12.ico` | 16–256 |
| user32 | 101 | Advertencia | IDI_WARNING | `propuestas_orosi/user32/oic_bang.ico` | 16–256 |
| user32 | 103 | Error | IDI_ERROR | `propuestas_orosi/user32/oic_hand.ico` | 16–256 |
| explorer | 258 | Botón Inicio | IDI_STARTORB | `propuestas_orosi/explorer/258.ico` | 16–256 |

### Additional Sample Set (from additionales/ batches)

| Module | Batch | Function | Source ICO | Status |
|--------|-------|----------|------------|--------|
| explorer | explorer_moderno_07 | Micrófono | `propuestas_orosi/adicionales/explorer_moderno_07/microphone.ico` | **Available without consumer** |
| explorer | explorer_moderno_07 | Micrófono silenciado | `propuestas_orosi/adicionales/explorer_moderno_07/microphone_muted.ico` | **Available without consumer** |

Wi-Fi and Volume are likewise not yet proposed. Per owner direction, no
placeholder buttons for missing functions; these ride a separate scope.

## Target Resource Locations

Verified against the tree (forma/icons-sample). **These paths are the
contract; the .rc files reference them verbatim — a plain build silently
embeds OLD art if new icons are dropped at unreferenced paths.**

| Module | Source Directory | RC File | Binary | Icon Storage Path |
|--------|------------------|---------|--------|-------------------|
| shell32 | `dll/win32/shell32` | `icon_res.rc` (included by shell32.rc) | **shell32.dll.mun** (neutral satellite via `add_neutral_resource_file`, NOT shell32.dll) | `res/icons/*.ico` |
| explorer | `base/shell/explorer` | `explorer.rc` | `explorer.exe` | `res/ico/*.ico` |
| user32 | `win32ss/user/user32` | `user32.rc` | `user32.dll` | `resources/*.ico` |

**Verification consequence:** shell32 icon checks must walk
`shell32_mun.dll` (built as the `shell32_mun` target, shipped as
`reactos/SystemResources/shell32.dll.mun`), not shell32.dll — shell32.dll's
.rsrc intentionally has no icons.

### Integration Pattern

1. **Locate source ICO** → `propuestas_orosi/<module>/<ID>.ico` (from manifest)
2. **Target path** → the module's Icon Storage Path above (rc-referenced)
3. **Replace ICO data in place** at the rc-referenced path (no rc edits; keep IDs/names/languages)
4. **Pre-build gate** → run `media/graphics/orosi-forma/check-ico-refs.py <base>..<tip>`; it fails if any added/modified .ico is not referenced by an .rc in its module
5. **Recompile** → build the module AND the `_mun` target where present
6. **Verify** → PE-walk the built binary (or .mun) for sizes/bpp/AND mask

## Important Constraints

### Reserved IDs (DO NOT TOUCH)

**shell32 IDs 50–53:** Transparent, reserved; ALL FOUR rc entries reference
the shared file `res/icons/50.ico` (1 image). Keep unchanged; do not
regenerate or assign functions.

### Additional Icons (IDs TBD)

30 entries in 7 batches + 2 hardware sets, no existing IDs/consumers.
Do not assign arbitrary IDs. Map to existing consumers only; document the
rest as "available without consumer".

## Discrepancies and Notes

| Issue | Severity | Status | Action |
|-------|----------|--------|--------|
| Small sizes (16–24px) resampled, not hand-optimized | Medium | Known | Optimization phase after bulk |
| Wi-Fi / Volume not proposed | Low | Noted | Separate scope on request |
| Mic active/muted (explorer_moderno_07) | Low | Known | **Available without consumer** (table) |
| Shell32 50–53 share one physical file | Low | Known | Preserve as-is |
| Dead-path drop risk (sample-phase defect, fixed in 13af124072c1) | Medium | FIXED | Mechanical gate script now enforced |

## Phase 2 (COMPLETE 2026-11-22)

Bulk replacement of all 345 proposed entries, executed on branch
forma/icons-bulk (3 module commits + gate fix + this table). Gate PASS
(repo-relative resolution), builds green amd64+i386 for shell32,
shell32_mun, explorer, user32, netshell, stobject. PE verification:
345 proposal image-sets present in the expected binaries (368 checks per
arch: 23 icons are dual-embedded via explorer.rc cross-module refs and
netshell/stobject own rc), shell32.dll has zero icon groups (art lives in
shell32.dll.mun), reserves 50-53 byte-identical to base.

Notes:
- Shared physical files 312.ico / 512.ico each feed two resource ids
  (230/312 and 239/512) whose layout proposals conflict; the primary id
  (312/512) proposal ships. Both ids render the same file either way.
- netshell/stobject tray icons ship in their own dlls AND in explorer.exe
  (cross-module references); all embedded copies verified.
- 30 additional icons remain "available without consumer" (not in scope).
