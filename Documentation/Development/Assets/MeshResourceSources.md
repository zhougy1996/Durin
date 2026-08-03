# Mesh Resource Sources

Last reviewed: 2026-07-28

Use this guide when selecting static meshes for renderer, material, UV, and
import-pipeline testing. Prefer a small, intentional test set over replacing
the teapot with one visually complex model: each committed asset should cover a
named behavior and have reproducible provenance.

## Recommended Sources

| Source | Best use | License posture | Notes |
| --- | --- | --- | --- |
| [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) | Importer and renderer conformance | Varies by asset; inspect the model directory's `README.md` | Curated glTF/GLB models with previews, feature tags, and per-model credits. Start here for deterministic test fixtures. |
| [Poly Haven Models](https://polyhaven.com/models) | Realistic PBR props and material/texture validation | [All assets are CC0](https://polyhaven.com/license) | Useful for higher-detail meshes and real-world texture sets. Download only the resolutions needed for the test. |
| [Kenney 3D Assets](https://kenney.nl/assets/category%3A3D) | Lightweight, game-ready props and modular kits | Asset pages state the license; packs such as [Factory Kit](https://kenney.nl/assets/factory-kit) are CC0 | Good for quick editor scenes, batching, repeated props, and simple material-atlas coverage. Verify the selected pack page before committing it. |
| [Quaternius](https://quaternius.com/) | Low-poly props, environments, rigs, and animations | [All models are CC0](https://quaternius.com/faq.html) | Many packs provide Blender and FBX files and use atlas textures, which makes them useful for UV and shared-texture tests. |
| [Sketchfab downloadable models](https://sketchfab.com/search?features=downloadable&type=models) | Broad subject coverage and scanned objects | Per-model Creative Commons or store license | Filter for downloadable models and prefer CC0 or CC BY. Preserve the generated attribution text. Avoid NC, ND, SA, editorial, and store-licensed assets unless the project has explicitly accepted their obligations. |
| [OpenGameArt 3D Art](https://opengameart.org/art-search-advanced?field_art_type_tid%5B%5D=10) | Community-authored game assets | Varies by submission | The site supports several licenses and explains attribution in its [FAQ](https://opengameart.org/content/faq). Prefer CC0; otherwise review every asset, texture, and derivative dependency separately. |

These links are discovery sources, not blanket approval for every download.
User-uploaded platforms can contain incorrect ownership claims, trademarked
objects, or textures with a different license from the mesh.

## Suggested Initial Test Set

The [Khronos model catalog](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/Models.md)
is the most useful starting point because it labels the behavior and license of
each model:

| Coverage | Candidate | Why |
| --- | --- | --- |
| Basic textured mesh | `WaterBottle` | Compact CC0 model with metal/roughness material data and a conventional UV path. |
| Multiple UV channels | `MultiUVTest` | Explicitly exercises a second texture-coordinate set; CC BY 4.0 attribution is required. |
| Texture and raster-state combinations | `TextureSettingsTest` | Exercises texturing modes, sidedness, and texture settings; CC BY 4.0 attribution is required. |
| Rich PBR reference | `FlightHelmet` | CC0 showcase model with multiple materials and textures; useful after the basic path works. |
| Small real-world prop | A Poly Haven model | Adds a realistic mesh and texture set under CC0 without user-specific licensing. |
| Atlas and repeated props | A Kenney or Quaternius pack | Exercises shared atlas UVs, many mesh files, and practical editor-scene assembly. |

Do not commit the complete Khronos repository or an entire high-resolution pack
for one test. Select the smallest variant that retains the behavior under test.
For a Durin-owned regression fixture, pin the upstream commit or release and
record the selected file paths.

## Intake Checklist

Before committing or ingesting a model:

1. Confirm that at least one UV channel exists and inspect it with a checker
   texture. For multi-UV coverage, record which material input uses each
   channel.
2. Check normals, tangents, winding, degenerate triangles, material slots,
   texture paths, and whether transforms are baked or stored on scene nodes.
3. Record units, up axis, forward axis, and any import settings needed to
   reproduce the intended orientation and scale.
4. Prefer glTF 2.0 for glTF-path coverage and OBJ for the simplest geometry/UV
   baseline. Add FBX only when the format itself, hierarchy, or animation is
   part of the test.
5. Keep only required texture resolutions and model variants. Do not commit
   preview renders or unrelated files from an asset pack.
6. Store source models under the owning mount's effective `Models` directory
   and textures under `Textures`; built-in Engine and Game mounts place these
   beneath `Content`. See [Content Version Control](../VersionControl/ContentVersionControl.md).
7. Commit model and texture sources through Git LFS according to the repository
   attributes.

## Provenance Record

Add a small Markdown record beside each imported third-party asset or asset
group. Use a stable upstream URL and preserve the license text supplied with
the download:

```markdown
# Asset provenance

- Asset: Water Bottle
- Author: Microsoft
- Source: https://github.com/KhronosGroup/glTF-Sample-Assets/...
- Downloaded: YYYY-MM-DD
- Upstream revision/version: <tag or commit>
- License: CC0-1.0
- Local modifications: Removed unused variants; no geometry changes
- Test purpose: Baseline UV0 and metal/roughness import
```

For CC BY assets, also preserve the title, creator, source URL, license name and
URL, and a description of local changes. A link alone is insufficient because
the page or its terms can change.
