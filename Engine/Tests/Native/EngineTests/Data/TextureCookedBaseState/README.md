# Texture Cooked Base-State Fixtures

`CookedPackages/Game/` contains the corresponding source-free Win64 Game packages.
Their native `PlatformData` identities remain declared by the concrete family.
The Volume TXPL payload is external while the smaller 2D and Cube payloads are
inline. The packages use DAST v10. Regeneration on 2026-09-14 preserved all
platform payload bytes except the producer-version field, which now identifies
the current writer. Pixel data, mip tables, formats, and dimensions are unchanged.

Tests copy these read-only inputs into their process sandbox before loading
them. Regenerate them only when intentionally changing the cooked wire format.
