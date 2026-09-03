# Texture Cooked Base-State Fixtures

`CookedPackages/Game/` contains the corresponding source-free Win64 Game packages.
Their native `PlatformData` identities remain declared by the concrete family.
The Volume TXPL payload is external while the smaller 2D and Cube payloads are
inline.

Tests copy these read-only inputs into their process sandbox before loading
them. Regenerate them only when intentionally changing the cooked wire format.
