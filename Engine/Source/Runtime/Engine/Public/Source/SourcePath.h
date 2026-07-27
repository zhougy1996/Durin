#pragma once

#include "DObject/CoreDObject.h"
#include "EngineAPI.h"

#include "SourcePath.gen.h"

namespace Durin
{
	// Stores one portable file path in a mount's SourceAssets domain.
	DSTRUCT()
	struct FSourcePath
	{
		GENERATED_BODY()

		// Empty means no source dependency; otherwise this is a complete normalized virtual file path.
		DPROPERTY()
		std::string Path;

		auto IsEmpty() const -> bool { return Path.empty(); }
		auto operator==(const FSourcePath&) const -> bool = default;
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
	// SourceAssets domain are referenced in place. External files are copied to
	// the explicit virtual destination after dependency and write checks.
	ENGINE_API auto PrepareMountedSourceFile(
		const std::filesystem::path& InputFile,
		std::string_view ReferencingAssetPath,
		std::string_view ExternalIngestDestination,
		FMountedSourceFile& OutSource,
		std::string& OutError) -> bool;
	ENGINE_API auto CommitMountedSourceFile(FMountedSourceFile& Source) -> void;
	ENGINE_API auto RollbackMountedSourceFile(FMountedSourceFile& Source) -> void;

	// Resolves a persisted or user-selected virtual source reference without
	// mutating source files.
	ENGINE_API auto ResolveMountedSourceReference(
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
	ENGINE_API auto PrepareMountedSourceReplacement(
		const std::filesystem::path& ReplacementFile,
		std::string_view AuthoringAssetPath,
		std::string_view SourceVirtualPath,
		FMountedSourceReplacement& OutReplacement,
		std::string& OutError,
		bool bEngineAuthoringContext = false) -> bool;
	ENGINE_API auto CommitMountedSourceReplacement(
		FMountedSourceReplacement& Replacement) -> void;
	ENGINE_API auto RollbackMountedSourceReplacement(
		FMountedSourceReplacement& Replacement) -> void;

	// Converts one pre-mounted SourceAssets-relative carrier using the owning
	// package's Content mount and requires the referenced source file to exist.
	ENGINE_API auto TryMigrateLegacySourcePath(
		std::string_view PackagePath,
		std::string_view LegacyPath,
		FSourcePath& OutSourcePath,
		std::filesystem::path& OutPhysicalPath,
		std::string& OutError) -> bool;
}
