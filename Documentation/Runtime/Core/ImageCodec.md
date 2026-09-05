# Core Image Codec

Summary: Defines the asset-independent encoded-image codec boundary owned by Core.

Modules: Core

`Image/ImageDecoder.h` is the only public bytes-to-pixels boundary for built-in
LDR images, Radiance HDR, and exact grayscale16 PNG input. Its declarations and
owned decoded values live in `Durin::Image`. Core does not attach asset,
package, source-provenance, import, DDC, Cook, or publication policy to those
values, and stb headers remain private to one Core implementation translation
unit.

`Image/ImageEncoder.h` owns the inverse asset-independent boundary for tightly
packed, top-left-origin RGBA8 pixels. `EncodeRgba8Png` produces a compressed
RGBA PNG and clears its output on invalid dimensions or byte counts. PNG chunk,
checksum, filtering, and DEFLATE details remain private to Core.

Memory decode is authoritative. File overloads are thin convenience wrappers
for non-transactional preview and test callers; direct family importers capture an
immutable byte snapshot first and uses those same bytes for hashing, decoding,
diagnostics, and build composition. Codec extension capability does not admit a
source to an asset workflow. Texture2D, TextureCube, and Scene each own
an independent family source-policy predicate.

LDR output is top-left-origin RGBA8 with the source channel count and derived
transparency fact. Radiance output is top-left-origin finite nonnegative linear
RGB float data and accepts only `-Y height +X width`. Grayscale16 PNG output is
top-left-origin row-major unsigned samples and requires color type 0, 16-bit
samples, standard compression/filtering, and non-interlaced rows. Every failure
clears the output value before returning a diagnostic.

Default decode admission limits are 512 MiB encoded input, 256 million decoded LDR or
grayscale pixels, and 32 million Radiance pixels with a 16,384 dimension bound.
Callers may select smaller limits. `ImageCodecTests` owns extension, output,
orientation, malformed/truncated input, limit, encode round-trip, compression,
and failure-state contracts.
