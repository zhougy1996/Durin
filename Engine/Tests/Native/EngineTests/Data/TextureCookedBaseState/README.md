# Texture Cooked Base-State Fixtures

`CookedPackages/Game/` contains the corresponding source-free Win64 Game packages.
Their native `PlatformData` identities remain declared by the concrete family.
The Volume TXPL payload is external while the smaller 2D and Cube payloads are
inline. The packages use DAST v10 and TXPL schema 3 with a 76-byte header.
The 2026-10-11 format cutover removed the producer word and regenerated each
package through the current DAST writer. Pixel bytes, mip ranges, formats,
dimensions and concrete-family field identities are unchanged.

Tests copy these read-only inputs into their process sandbox before loading
them. Regenerate them only when intentionally changing the cooked wire format.
