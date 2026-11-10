# Texture Cooked Base-State Fixtures

`CookedPackages/Game/` contains the corresponding source-free Win64 Game packages.
Their native `PlatformData` identities remain declared by the concrete family.
The Volume TXPL payload is external while the smaller 2D and Cube payloads are
inline. The packages use DAST v11 and TXPL schema 3 with a 76-byte header.
The 2026-10-11 format cutover removed the producer word and regenerated each
package through the current DAST writer. Pixel bytes, mip ranges, formats,
dimensions and concrete-family field identities are unchanged.

Registry metadata was upgraded to section version 3 on 2026-10-25 through the
construct-free package reader and writer; bulk payload bytes remain identical.

The 2026-11-10 repository-wide upgrade re-emitted all three packages as DAST v11
with registry-v4 metadata. The detached linker values and bulk bytes were
preserved; the one-time conversion code was removed after regeneration.

Tests copy these read-only inputs into their process sandbox before loading
them. Regenerate them only when intentionally changing the cooked wire format.
