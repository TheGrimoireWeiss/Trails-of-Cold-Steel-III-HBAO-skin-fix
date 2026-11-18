# Trails of Cold Steel III — Character Face HBAO+ Fix

A lightweight, zero-dependency DirectX 11 proxy mod (`dxgi.dll`) for **The Legend of Heroes: Trails of Cold Steel III** (PC) that eliminates **HBAO+** dark smudging and blotches on character faces, eyes, and skin — while keeping **100% of HBAO+ ambient occlusion** intact on hair, hats, glasses, clothing, armor, monsters, and the entire world.

---

## The Problem

Enabling **HBAO+** in *Trails of Cold Steel III* adds rich contact shadows to environments and character outfits, but NVIDIA's screen-space horizon-based ambient occlusion treats the subtle 3D geometry of anime-styled noses, eye sockets, mouths, and chins like realistic creases. At certain camera angles and lighting conditions, this produces dark, muddy shadows ("smudged faces") across characters' skin.

Previously, the only workaround was turning HBAO+ off globally. **This mod fixes character faces and skin surgically so you can keep HBAO+ enabled everywhere else.**

<!-- Optional: Add before/after screenshots to your repo (e.g. in a screenshots/ folder) and uncomment below:
| Vanilla HBAO+ (Smudged Face) | With Face HBAO+ Fix (Clean Face, Full Scene HBAO+) |
| :---: | :---: |
| ![Before](screenshots/before.png) | ![After](screenshots/after.png) |
-->

---

## Features

- **Zero Game File Modifications** — Pure drop-in `dxgi.dll` proxy; does not modify `ed8_3_PC.exe`, `GFSDK_SSAO_D3D11.win64.dll`, or any `.pka` game archives.
- **Skin & Face Precision** — Audited across all 678 character/NPC/equipment models and 7,045 asset packages in CS3:
  - Cleanses faces, mouths, eyeballs, and exposed non-normal-mapped skin on all playable characters and NPCs (including asset edge cases such as Ennea).
  - Leaves **100% of HBAO+** untouched on clothing, ribbons, metallic trims, armor, monsters, props, and world geometry.
  - Preserves natural HBAO+ shading from foreground hair bangs, hats, and glasses frames overlapping the face.
- **Full MSAA & Resolution Support** — Subsample-accurate depth matching across `1x`, `2x`, `4x`, and `8x MSAA` (including TSSAA) at any resolution or aspect ratio (`1080p`, `1440p`, `4K`, Ultrawide).
- **Negligible Performance Impact** — Executes a single tiny fullscreen pass on the SSAO buffer only when characters are on screen (~`0.02–0.05 ms` on modern GPUs).
- **Live In-Game Hotkeys** — Toggle the fix ON/OFF instantly in-game (`F10`) for live comparisons, or hot-reload `hbao_face_fix.ini` (`F11`) without restarting.

---

## Installation

1. Download the latest release (or grab `dxgi.dll` and `hbao_face_fix.ini` from this repository).
2. Copy **`dxgi.dll`** and **`hbao_face_fix.ini`** into your *Trails of Cold Steel III* `bin\x64` folder (next to `ed8_3_PC.exe`):
   ```text
   <Game Folder>\bin\x64\
   ├── ed8_3_PC.exe
   ├── dxgi.dll            <-- place here
   └── hbao_face_fix.ini   <-- place here
   ```
   *(Default Steam path: `C:\Program Files (x86)\Steam\steamapps\common\The Legend of Heroes Trails of Cold Steel III\bin\x64\`)*
3. Make sure **SSAO** is set to **HBAO+** in the game's launcher / graphics settings, then launch the game normally.

### Uninstallation

Simply delete `dxgi.dll`, `hbao_face_fix.ini`, and (if generated) `hbao_face_fix.log` from `bin\x64\`.

---

## In-Game Hotkeys

| Default Key | Action | Audio Cue |
| :---: | :--- | :--- |
| **`F10`** | Toggle Face HBAO+ Fix **ON / OFF** live in-game | High tone = ON, Low tone = OFF |
| **`F11`** | Reload **`hbao_face_fix.ini`** live without restarting | Mid tone |

*(Hotkeys and audio cues can be customized or disabled in `hbao_face_fix.ini`.)*

---

## Configuration (`hbao_face_fix.ini`)

All settings are optional and pre-tuned out of the box:

```ini
[HBAOFaceFix]
; Enable or disable the character face HBAO+ smudge fix (1 = ON, 0 = OFF)
Enabled=1

; Strength of HBAO+ removal on character faces, mouths, and eyes
;   1.0  = completely clean anime faces (recommended)
;   0.85 = remove 85% of face HBAO while keeping 15% subtle shading
;   0.0  = vanilla HBAO+
MaskStrength=1.0

; Hardware depth match tolerance for 8x MSAA / 1x depth comparison
DepthEpsilon=0.000004

; Optional depth-gated seam dilation in pixels (0 = exact MSAA subpixel mask, 1 = 1px seam guard)
EdgeDilatePixels=0
DilateDepthEpsilon=0.00012

; Include character eyeballs / sclera (1 = ON, 0 = OFF)
MaskEyes=1

[Hotkeys]
; Virtual-Key codes in hex or decimal (0x79 = F10, 0x7A = F11; set to 0 to disable)
ToggleKey=0x79
ReloadIniKey=0x7A
ToggleBeep=1

[Debug]
; Write startup & detection diagnostics to hbao_face_fix.log (1 = ON, 0 = OFF)
Logging=1

; Debug view: render detected character face mask as solid black AO (0 = OFF, 1 = ON)
VisualizeMask=0
```

---

## Compatibility

- **Game Versions**: Compatible with Steam and GOG DirectX 11 builds (`bin\x64\ed8_3_PC.exe`).
- **SenPatcher**: Fully compatible (SenPatcher uses `dinput8.dll`, whereas this mod uses `dxgi.dll`).
- **ReShade / Other `dxgi.dll` Mods**: If you also use ReShade in `dxgi.dll` mode, install ReShade as `d3d11.dll` (or rename ReShade's DLL accordingly) so both can load side by side.

---

## How It Works (Technical Overview)

1. **Loader-Lock-Safe DXGI Proxy & Persistent Context Hooking**
   - Forwards all 20 Windows 10/11 `dxgi.dll` exports dynamically to `System32\dxgi.dll` via `InitOnceExecuteOnce`.
   - Installs a persistent 256-slot shadow `ID3D11DeviceContext` vtable that forwards dynamically through the driver's mutable dispatch table — remaining active across NVIDIA `nvwgf2umx.dll` amortized command-list fast-path swaps — and hooks `GFSDK_SSAO_Context_D3D11::RenderAO`.
2. **Shader Reflection & Texture Hash Classification**
   - Parses `DXBC` -> `RDEF` reflection metadata on `CreatePixelShader` to locate `CartoonMapSampler`, `SphereMapSampler`, `NormalMapSampler`, `CubeMapSampler`, and `DiffuseMapSampler` register bindings across PhyreEngine's `ed8_chr.fx` permutations.
   - Identifies character skin toon ramps (`dlight_skin`), eyeball sphere maps (`eyes_sphere`), and Ennea's `chr039_00` + `dlight_conv` face pair via Mip-0 FNV-1a hashes on `CreateTexture2D`, while rejecting normal-mapped/cube-mapped draws, depth-only shadow/fade passes, and back-face toon outlines (`OutlineDefaultFPShader`).
3. **MSAA-Accurate Depth Mask & SSAO Cleanse Pass**
   - Replays qualifying opaque character face/eye/skin draws into a matching single-sample or `Texture2DMS` `FaceDepth` buffer.
   - Immediately after `GFSDK_SSAO` writes `SSAOBuffer`, runs a fullscreen pixel shader comparing `SceneDepth` against `FaceDepth` per MSAA sample. Where foreground hair, hats, or glasses occlude a sample in front of the face, `SceneDepth != FaceDepth` and full HBAO+ shading is preserved down to the subpixel edge.

---

## Building from Source

No Visual Studio installation or external SDKs are required. Using [Zig](https://ziglang.org/download/) (`0.14+` / `0.17+`) on Windows:

```powershell
zig c++ -target x86_64-windows-gnu -shared -O2 -std=c++17 -fms-extensions -s src/dxgi_hbao_fix.cpp src/dxgi.def -o dxgi.dll -ld3d11 -luuid -luser32 -lkernel32
```

### Repository Structure

```text
├── dxgi.dll                # Prebuilt 64-bit drop-in proxy DLL
├── hbao_face_fix.ini       # Optional configuration & hotkey settings
├── README.md               # Documentation
└── src/
    ├── dxgi_hbao_fix.cpp   # Full C++17 / D3D11 / HLSL source
    └── dxgi.def            # Win64 DXGI export definition table
```

---

## License

Released under the [MIT License](https://opensource.org/licenses/MIT).
