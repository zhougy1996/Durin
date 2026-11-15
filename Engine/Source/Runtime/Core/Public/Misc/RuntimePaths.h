#pragma once
#include "Misc/CoreStd.h"
#include "CoreAPI.h"

namespace Durin
{
    // One process bootstrap binding. Published only after validation, before path consumers.
    struct FRuntimePathContext
    {
        bool bStaged = false;
        std::filesystem::path Manifest;
        std::map<std::string, std::string> Bindings;
        std::string ManifestJson;
        std::map<std::string,std::string> NativeLibraries; // Canonical folded path -> verified SHA-256.
    };
    CORE_API auto ReadRuntimePathContext(const std::filesystem::path& Manifest,
        const std::filesystem::path& Executable, bool bValidateHost = true,
        const std::filesystem::path& LocalAppDataOverride = {}) -> std::expected<FRuntimePathContext, std::string>;
    CORE_API auto ResolveRuntimeUserDataRoot(std::string_view ApplicationId,
        const std::filesystem::path& LocalAppDataOverride = {}) -> std::filesystem::path;
    CORE_API auto PublishRuntimePathContext(FRuntimePathContext Context) -> bool;
    CORE_API auto GetRuntimePathContext() -> const FRuntimePathContext*;
    CORE_API auto RuntimePathBinding(std::string_view Key) -> std::string;
    // Resolves normalized relative paths and rejects canonical aliases outside Root.
    CORE_API auto ResolveDeploymentPath(const std::filesystem::path& Root, std::string_view Relative,
        bool bAllowRoot = false) -> std::filesystem::path;
    CORE_API auto BytesSha256(std::span<const std::byte> Bytes) -> std::string;
    CORE_API auto FileSha256(const std::filesystem::path& File) -> std::string;
}
