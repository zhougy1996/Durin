#pragma once

#include "Asset/SourcePath.h"
#include "AssetCoreAPI.h"

namespace Durin::Asset
{
	enum class EMountedSourceMutationContext : uint8
	{
		DependencySafe,
		EngineAuthoring
	};

	enum class ESourceFileDisposition : uint8
	{
		ReferenceExisting,
		IngestedExternal,
		ReusedIdentical
	};

	struct FMountedSourceFile
	{
		FSourcePath SourcePath;
		std::filesystem::path PhysicalPath;
		ESourceFileDisposition Disposition = ESourceFileDisposition::ReferenceExisting;
		bool bCreatedFile = false;
	};

	// Classifies an existing physical file. Files already beneath an allowed
	// registered mount are referenced in place. External files are copied to
	// the explicit virtual destination after dependency and write checks.
	ASSETCORE_API auto PrepareMountedSourceFile(
		const std::filesystem::path& InputFile,
		std::string_view ReferencingAssetPath,
		std::string_view ExternalIngestDestination,
		FMountedSourceFile& OutSource,
		std::string& OutError,
		EMountedSourceMutationContext MutationContext =
			EMountedSourceMutationContext::DependencySafe) -> bool;
	// Publishes an immutable byte payload at an explicit mounted source path. This
	// is used for container-embedded authoring inputs that need an independently
	// reloadable source identity.
	ASSETCORE_API auto PrepareMountedSourceBytes(
		std::span<const std::byte> Bytes,
		std::string_view ReferencingAssetPath,
		std::string_view DestinationSourcePath,
		FMountedSourceFile& OutSource,
		std::string& OutError) -> bool;
	ASSETCORE_API auto CommitMountedSourceFile(FMountedSourceFile& Source) noexcept -> void;
	ASSETCORE_API auto RollbackMountedSourceFile(FMountedSourceFile& Source) noexcept -> void;

	// Owns rollback for a successfully prepared mounted source until Commit is
	// called. Move transfers that ownership; referenced and reused files remain
	// non-owning because their bCreatedFile flag is false.
	class ASSETCORE_API FScopedMountedSourceFile final : public FMountedSourceFile
	{
	public:
		FScopedMountedSourceFile() = default;
		~FScopedMountedSourceFile() noexcept;
		FScopedMountedSourceFile(const FScopedMountedSourceFile&) = delete;
		auto operator=(const FScopedMountedSourceFile&)
			-> FScopedMountedSourceFile& = delete;
		FScopedMountedSourceFile(FScopedMountedSourceFile&& Other) noexcept;
		auto operator=(FScopedMountedSourceFile&& Other) noexcept
			-> FScopedMountedSourceFile&;

		auto Commit() noexcept -> void;
		auto Reset() noexcept -> void;
	};

	// Resolves a persisted or user-selected virtual source reference without
	// mutating source files.
	ASSETCORE_API auto ResolveMountedSourceReference(
		std::string_view ReferencingAssetPath,
		std::string_view SourceVirtualPath,
		FMountedSourceFile& OutSource,
		std::string& OutError) -> bool;

	struct FMountedSourceReplacement
	{
		FSourcePath SourcePath;
		std::filesystem::path PhysicalPath;
		std::filesystem::path BackupPath;
		bool bPublished = false;
	};

	// Explicitly stages and publishes replacement bytes for a shared source.
	// The caller commits only after all referencing package changes succeed, or
	// rolls back to restore the prior source bytes.
	ASSETCORE_API auto PrepareMountedSourceReplacement(
		const std::filesystem::path& ReplacementFile,
		std::string_view AuthoringAssetPath,
		std::string_view SourceVirtualPath,
		FMountedSourceReplacement& OutReplacement,
		std::string& OutError,
		EMountedSourceMutationContext MutationContext =
			EMountedSourceMutationContext::DependencySafe) -> bool;
	ASSETCORE_API auto CommitMountedSourceReplacement(
		FMountedSourceReplacement& Replacement) -> void;
	ASSETCORE_API auto RollbackMountedSourceReplacement(
		FMountedSourceReplacement& Replacement) -> void;

	struct FMountedSourceRelocation
	{
		FSourcePath OriginalSourcePath;
		FSourcePath DestinationSourcePath;
		std::filesystem::path OriginalPhysicalPath;
		std::filesystem::path DestinationPhysicalPath;
		bool bPublished = false;
	};

	// Stages a non-destructive copy at the destination. Commit removes the
	// original only after every referencing package has been updated; rollback
	// removes only the staged destination.
	ASSETCORE_API auto PrepareMountedSourceRelocation(
		std::string_view AuthoringAssetPath,
		std::string_view OriginalSourceVirtualPath,
		std::string_view DestinationSourceVirtualPath,
		FMountedSourceRelocation& OutRelocation,
		std::string& OutError,
		EMountedSourceMutationContext MutationContext =
			EMountedSourceMutationContext::DependencySafe) -> bool;
	ASSETCORE_API auto CommitMountedSourceRelocation(
		FMountedSourceRelocation& Relocation, std::string& OutError) -> bool;
	ASSETCORE_API auto RollbackMountedSourceRelocation(
		FMountedSourceRelocation& Relocation) -> void;

}
