# Versioning

Durin's engine release version is defined once in `Engine/Build/Build.version`. The current development version is `0.1.0-dev`. CMake validates that file, exposes the numeric core as the workspace project version, and generates the header consumed by Core's public version API.

Runtime code should use `GetEngineVersion()` for numeric components and `GetEngineVersionString()` for display. Do not duplicate the engine version in module sources, window titles, API integration metadata, or packaging scripts.

## Independent Version Domains

The engine release version identifies a Durin release for users, logs, diagnostics, source tags, and distributable binaries. It does not define serialization compatibility. Formats that evolve independently keep their own versions, including:

- `.dasset` package files
- transient object graphs
- DurinHeaderTool schemas and tool output
- shader metadata, cache records, and variant keys
- editor layout and user-setting schemas
- project descriptor schemas

Changing the engine release version alone must not rewrite assets or invalidate caches. A format version changes only when that format's reader or writer contract changes. A saved-by engine version may be added later for diagnostics, but readers must make compatibility decisions from the relevant format and custom schema versions.

## Release Convention

Durin follows Semantic Versioning for the engine release identifier. While the public API and long-lived content compatibility policy are still developing, releases remain under major version zero. Development builds use a prerelease channel such as `0.1.0-dev`; release tags use the corresponding stable form such as `v0.1.0` when that release is ready.
