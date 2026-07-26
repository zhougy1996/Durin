# Cooked-Bulk Logical Fixtures

These are the canonical logical inputs for DBLK container version 1 and cook
manifest version 1. Tests construct bytes from these values; they do not
serialize C++ structures or require an asset codec.

## `TwoPayloads`

- Target platform: `Win64(1)`.
- Target profile: `Game(1)`.
- Entry 0:
  - Payload ID: `53aa6a89-dc49-401a-b409-adc498ac4f8b`.
  - Flags: `Required(1)`.
  - Payload schema: `1`.
  - Compression: `None(0)`.
  - Alignment: `16`.
  - Uncompressed bytes, in order:
    `00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff`.
- Entry 1:
  - Payload ID: `d52878ce-8f50-48c7-a3c7-ff846e2c4c5a`.
  - Flags: `Required(1)`.
  - Payload schema: `1`.
  - Compression: `None(0)`.
  - Alignment: `64`.
  - Uncompressed bytes are UTF-8 `Durin cube payload\n`.

The encoder derives offsets, stored sizes, hashes, zero padding, table hash,
and complete file size. Reversing the input entries must produce identical
bytes because table order is by the unsigned GUID A/B/C/D tuple.

## `RelocatedCook`

The logical cooked files are:

| Kind | Relative path | Complete bytes |
| --- | --- | --- |
| cooked package | `Game/Textures/T_Test.dasset` | `44 41 53 53 45 54 00 01` |
| DBLK | `Game/Textures/T_Test.dbulk` | canonical `TwoPayloads` encoding |

The manifest writer receives the files in reverse order and must emit records
in raw relative-path UTF-8 order. Moving the complete cook root without
changing these relative paths must not change the manifest or descriptor
resolution.

## Malformed derivations

Readers derive these cases from the canonical encodings:

- every truncation length from zero through the final byte;
- bad DBLK or CMNF magic, version, platform, profile, header size, entry size,
  and record kind;
- nonzero reserved header, entry, record, gap, and trailing-padding bytes;
- zero, duplicate, and unsorted payload IDs;
- unknown DBLK flag, compression, and descriptor location values;
- zero, non-power-of-two, less-than-16, and greater-than-4096 alignments;
- `offset + stored size` overflow, out-of-file and overlapping ranges;
- `None` with unequal stored and uncompressed sizes;
- a payload or container one byte above its allocation limit;
- a compressed ratio above `64:1`;
- corrupted table, payload, manifest record, and published-file hashes;
- descriptor ID, offset, size, hash, schema, platform, profile, alignment, and
  compression mismatches, one field at a time;
- absolute, empty, backslash, `.`, `..`, DDC, and cook-root-escaping paths;
- duplicate or unsorted manifest paths, a 1,025-byte path, excess entry count,
  and excess record bytes;
- failure before DBLK publication, between DBLK and package publication, and
  before manifest publication.
