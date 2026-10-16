# Platform Libraries

Summary: Define native dynamic-library loading, reference ownership, diagnostics, and physical-path behavior on Windows and macOS.
Modules: Core

Last reviewed: 2026-10-16

## Loading and Dependency Resolution

`HAL/PlatformLibrary.h` exposes `FPlatformLibrary::Load`, `FindSymbol`, and
`Unload`. Each returns `std::expected` with an owned `FPlatformLibraryError` on
failure. Errors retain the operation, requested load path or export name,
native error code where available, and diagnostic text. Native loader errors
are captured immediately; inspecting a later error cannot overwrite an earlier
result. macOS loader failures carry `dlerror` text without inventing an errno.
The platform layer does not log or choose caller recovery.

`Load` requires an absolute `std::filesystem::path` without embedded NULs.
It does not append a platform extension or search for the requested library by
name. UTF-8 strings must be decoded into native paths with
`std::filesystem::u8path`; constructing a Windows path directly from a narrow
string does not establish a UTF-8 contract.

Windows uses wide-character APIs and an extended-length drive or UNC path,
after lexical normalization. It searches library dependencies beside the loaded
DLL and through `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` (application, System32 and
registered user DLL directories), excluding implicit current-directory/PATH
search. Existing startup registration of third-party DLL directories remains
process-wide; registration is not a per-load scope.

macOS uses `dlopen` with `RTLD_NOW | RTLD_LOCAL`. Dependencies follow Mach-O
install names and rpaths, including `@loader_path` and `@rpath` as authored by
the producer. Windows DLL-directory registration does not provide equivalent
macOS search behavior. Packaging must supply correct install names/rpaths;
the platform layer does not rewrite them or mutate loader environment variables.

`FModuleManager::AddModule` accepts UTF-8 filenames. The manager resolves relative module filenames beside the executable,
then in the active project's runtime binary directory. Absolute registrations
load only their specified path. No initial ambient name search is performed.
Windows executable-path discovery grows its buffer instead of truncating at
`MAX_PATH`, and returns UTF-8; retrieval failure returns an empty string.

## Reference and Symbol Lifetime

Every successful `Load` transfers one native loader reference to its caller.
Repeated loads can return the same handle while owning separate references.
Copying a handle does not acquire another reference. The owner must either
call `Unload` once per successful load or deliberately retain the reference
until process exit. The API does not automatically unload process-resident code.

`FindSymbol` requires a live handle and a nonempty ASCII export name without
embedded NULs. It supports function and data exports. The returned address is
borrowed; callers must finish all calls and release code/data users before
unloading. On macOS a successful lookup can contain a null address; success is
determined by `dlerror`, not by the pointer value.

`Unload` requires an owned live reference. A successful result consumes that
reference; a failed result leaves it with the caller. Null handles are rejected.
The API cannot validate a stale non-null handle, track borrowed pointers, or
make concurrent lookup/calls versus unload safe. Owners synchronize lifetime.
Engine module startup, shutdown, code leases, and residency remain owned by
the module manager rather than the native loader.

## Contract Coverage

`PlatformLibraryContractTests` uses a real library with function and data exports
and an adjacent dependent library. It covers Unicode and paths longer than
`MAX_PATH`, repeated reference acquisition, missing libraries/exports, invalid
arguments, missing adjacent dependencies, and diagnostic independence. Fixtures are copied into per-process
test sandboxes; macOS fixtures use an `@rpath` dependency and `@loader_path` rpath.
