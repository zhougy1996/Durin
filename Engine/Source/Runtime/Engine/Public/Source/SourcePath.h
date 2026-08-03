#pragma once

#include "Asset/SourcePath.h"
#include "EngineAPI.h"

namespace Durin
{
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
	ENGINE_API auto PrepareMountedSourceFile(
		const std::filesystem::path& InputFile,
		std::string_view ReferencingAssetPath,
		std::string_view ExternalIngestDestination,
		FMountedSourceFile& OutSource,
		std::string& OutError) -> bool;
	// Publishes an immutable byte payload at an explicit mounted source path. This
	// is used for container-embedded authoring inputs that need an independently
	// reloadable source identity.
	ENGINE_API auto PrepareMountedSourceBytes(
		std::span<const uint8> Bytes,
		std::string_view ReferencingAssetPath,
		std::string_view DestinationSourcePath,
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
	ENGINE_API auto PrepareMountedSourceRelocation(
		std::string_view AuthoringAssetPath,
		std::string_view OriginalSourceVirtualPath,
		std::string_view DestinationSourceVirtualPath,
		FMountedSourceRelocation& OutRelocation,
		std::string& OutError,
		bool bEngineAuthoringContext = false) -> bool;
	ENGINE_API auto CommitMountedSourceRelocation(
		FMountedSourceRelocation& Relocation, std::string& OutError) -> bool;
	ENGINE_API auto RollbackMountedSourceRelocation(
		FMountedSourceRelocation& Relocation) -> void;

}
