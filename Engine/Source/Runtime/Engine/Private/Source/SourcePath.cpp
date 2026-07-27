#include "Source/SourcePath.h"

#include "Misc/Paths.h"

#include <fstream>

namespace Durin
{
	namespace
	{
		auto FilesEqual(
			const std::filesystem::path& First,
			const std::filesystem::path& Second,
			std::string& OutError) -> bool
		{
			std::error_code Error;
			if (std::filesystem::file_size(First, Error)
				!= std::filesystem::file_size(Second, Error) || Error)
			{
				if (Error) OutError = Error.message();
				return false;
			}
			std::ifstream FirstStream(First, std::ios::binary);
			std::ifstream SecondStream(Second, std::ios::binary);
			if (!FirstStream || !SecondStream)
			{
				OutError = "Failed to read source files for collision comparison.";
				return false;
			}
			constexpr size_t BufferSize = 64 * 1024;
			std::array<char, BufferSize> FirstBuffer{};
			std::array<char, BufferSize> SecondBuffer{};
			while (FirstStream && SecondStream)
			{
				FirstStream.read(FirstBuffer.data(), FirstBuffer.size());
				SecondStream.read(SecondBuffer.data(), SecondBuffer.size());
				if (FirstStream.gcount() != SecondStream.gcount()
					|| !std::equal(
						FirstBuffer.begin(), FirstBuffer.begin() + FirstStream.gcount(),
						SecondBuffer.begin()))
					return false;
			}
			return true;
		}
	}

	auto ResolveMountedSourceReference(
		std::string_view ReferencingAssetPath,
		std::string_view SourceVirtualPath,
		FMountedSourceFile& OutSource,
		std::string& OutError) -> bool
	{
		OutSource = {};
		const PathUtilities::FSourcePathResult Resolved =
			PathUtilities::ResolveSourcePath(
				SourceVirtualPath, PathUtilities::EPathExistence::RequireFile);
		if (!Resolved)
		{
			OutError = Resolved.Message;
			return false;
		}
		const PathUtilities::FMountPolicyResult Dependency =
			PathUtilities::CheckMountDependency(
				ReferencingAssetPath, Resolved.NormalizedVirtualPath);
		if (!Dependency)
		{
			OutError = Dependency.Message;
			return false;
		}
		OutSource.SourcePath.Path = Resolved.NormalizedVirtualPath;
		OutSource.PhysicalPath = Resolved.PhysicalPath;
		OutSource.Disposition = ESourceFileDisposition::ReferenceExisting;
		OutError.clear();
		return true;
	}

	auto PrepareMountedSourceFile(
		const std::filesystem::path& InputFile,
		std::string_view ReferencingAssetPath,
		std::string_view ExternalIngestDestination,
		FMountedSourceFile& OutSource,
		std::string& OutError) -> bool
	{
		OutSource = {};
		std::error_code Error;
		const std::filesystem::path Input =
			std::filesystem::absolute(InputFile, Error).lexically_normal();
		if (Error || !std::filesystem::is_regular_file(Input, Error))
		{
			OutError = Error
				? Error.message()
				: std::format("Source file does not exist: {}", Input.generic_string());
			return false;
		}

		const PathUtilities::FSourcePathResult Classified =
			PathUtilities::ClassifySourcePath(Input);
		if (Classified)
			return ResolveMountedSourceReference(
				ReferencingAssetPath, Classified.NormalizedVirtualPath, OutSource, OutError);
		if (Classified.Error != PathUtilities::EMountPathError::UnknownMount)
		{
			OutError = Classified.Message;
			return false;
		}
		if (ExternalIngestDestination.empty())
		{
			OutError = "External source ingestion requires an explicit mounted destination.";
			return false;
		}

		const PathUtilities::FSourcePathResult Destination =
			PathUtilities::ResolveSourcePath(
				ExternalIngestDestination, PathUtilities::EPathExistence::AllowMissing);
		if (!Destination)
		{
			OutError = Destination.Message;
			return false;
		}
		const PathUtilities::FMountPolicyResult Mutation =
			PathUtilities::CheckSourceMutation(
				ReferencingAssetPath, Destination.NormalizedVirtualPath);
		if (!Mutation)
		{
			OutError = Mutation.Message;
			return false;
		}

		OutSource.SourcePath.Path = Destination.NormalizedVirtualPath;
		OutSource.PhysicalPath = Destination.PhysicalPath;
		if (std::filesystem::is_regular_file(Destination.PhysicalPath, Error))
		{
			std::string CompareError;
			if (!FilesEqual(Input, Destination.PhysicalPath, CompareError))
			{
				OutError = CompareError.empty()
					? std::format(
						"A different source file already exists at {}.",
						Destination.PhysicalPath.generic_string())
					: std::move(CompareError);
				OutSource = {};
				return false;
			}
			OutSource.Disposition = ESourceFileDisposition::ReusedIdentical;
			OutError.clear();
			return true;
		}
		Error.clear();
		std::filesystem::create_directories(Destination.PhysicalPath.parent_path(), Error);
		if (Error)
		{
			OutError = std::format(
				"Failed to create source directory {}: {}",
				Destination.PhysicalPath.parent_path().generic_string(), Error.message());
			OutSource = {};
			return false;
		}
		const std::filesystem::path Temporary =
			Destination.PhysicalPath.generic_string() + ".ingest.tmp";
		std::filesystem::remove(Temporary, Error);
		Error.clear();
		if (!std::filesystem::copy_file(
			Input, Temporary, std::filesystem::copy_options::none, Error))
		{
			OutError = std::format("Failed to stage source ingestion: {}", Error.message());
			OutSource = {};
			return false;
		}
		Error.clear();
		std::filesystem::rename(Temporary, Destination.PhysicalPath, Error);
		if (Error)
		{
			std::error_code CleanupError;
			std::filesystem::remove(Temporary, CleanupError);
			OutError = std::format("Failed to publish source ingestion: {}", Error.message());
			OutSource = {};
			return false;
		}
		OutSource.Disposition = ESourceFileDisposition::IngestedExternal;
		OutSource.bCreatedFile = true;
		OutError.clear();
		return true;
	}

	auto CommitMountedSourceFile(FMountedSourceFile& Source) -> void
	{
		Source.bCreatedFile = false;
	}

	auto RollbackMountedSourceFile(FMountedSourceFile& Source) -> void
	{
		if (Source.bCreatedFile)
		{
			std::error_code Error;
			std::filesystem::remove(Source.PhysicalPath, Error);
		}
		Source = {};
	}

	auto PrepareMountedSourceReplacement(
		const std::filesystem::path& ReplacementFile,
		std::string_view AuthoringAssetPath,
		std::string_view SourceVirtualPath,
		FMountedSourceReplacement& OutReplacement,
		std::string& OutError,
		bool bEngineAuthoringContext) -> bool
	{
		OutReplacement = {};
		std::error_code Error;
		const std::filesystem::path Input =
			std::filesystem::absolute(ReplacementFile, Error).lexically_normal();
		if (Error || !std::filesystem::is_regular_file(Input, Error))
		{
			OutError = Error
				? Error.message()
				: std::format("Replacement source does not exist: {}", Input.generic_string());
			return false;
		}
		const PathUtilities::FSourcePathResult Target =
			PathUtilities::ResolveSourcePath(
				SourceVirtualPath, PathUtilities::EPathExistence::RequireFile);
		if (!Target)
		{
			OutError = Target.Message;
			return false;
		}
		const PathUtilities::FMountPolicyResult Mutation =
			PathUtilities::CheckSourceMutation(
				AuthoringAssetPath, Target.NormalizedVirtualPath, bEngineAuthoringContext);
		if (!Mutation)
		{
			OutError = Mutation.Message;
			return false;
		}
		std::string CompareError;
		if (FilesEqual(Input, Target.PhysicalPath, CompareError))
		{
			OutReplacement.SourcePath.Path = Target.NormalizedVirtualPath;
			OutReplacement.PhysicalPath = Target.PhysicalPath;
			OutError.clear();
			return true;
		}
		if (!CompareError.empty())
		{
			OutError = std::move(CompareError);
			return false;
		}

		const std::filesystem::path Temporary =
			Target.PhysicalPath.generic_string() + ".replace.tmp";
		const std::filesystem::path Backup =
			Target.PhysicalPath.generic_string() + ".replace.backup";
		std::filesystem::remove(Temporary, Error);
		std::filesystem::remove(Backup, Error);
		Error.clear();
		if (!std::filesystem::copy_file(
			Input, Temporary, std::filesystem::copy_options::none, Error))
		{
			OutError = std::format("Failed to stage shared-source replacement: {}", Error.message());
			return false;
		}
		Error.clear();
		std::filesystem::rename(Target.PhysicalPath, Backup, Error);
		if (Error)
		{
			std::error_code CleanupError;
			std::filesystem::remove(Temporary, CleanupError);
			OutError = std::format("Failed to preserve shared source: {}", Error.message());
			return false;
		}
		Error.clear();
		std::filesystem::rename(Temporary, Target.PhysicalPath, Error);
		if (Error)
		{
			std::error_code RestoreError;
			std::filesystem::rename(Backup, Target.PhysicalPath, RestoreError);
			std::filesystem::remove(Temporary, RestoreError);
			OutError = std::format("Failed to publish shared-source replacement: {}", Error.message());
			return false;
		}
		OutReplacement.SourcePath.Path = Target.NormalizedVirtualPath;
		OutReplacement.PhysicalPath = Target.PhysicalPath;
		OutReplacement.BackupPath = Backup;
		OutReplacement.bPublished = true;
		OutError.clear();
		return true;
	}

	auto CommitMountedSourceReplacement(FMountedSourceReplacement& Replacement) -> void
	{
		if (!Replacement.BackupPath.empty())
		{
			std::error_code Error;
			std::filesystem::remove(Replacement.BackupPath, Error);
		}
		Replacement = {};
	}

	auto RollbackMountedSourceReplacement(FMountedSourceReplacement& Replacement) -> void
	{
		if (Replacement.bPublished && !Replacement.BackupPath.empty())
		{
			std::error_code Error;
			std::filesystem::remove(Replacement.PhysicalPath, Error);
			Error.clear();
			std::filesystem::rename(
				Replacement.BackupPath, Replacement.PhysicalPath, Error);
		}
		Replacement = {};
	}

	auto PrepareMountedSourceRelocation(
		std::string_view AuthoringAssetPath,
		std::string_view OriginalSourceVirtualPath,
		std::string_view DestinationSourceVirtualPath,
		FMountedSourceRelocation& OutRelocation,
		std::string& OutError,
		bool bEngineAuthoringContext) -> bool
	{
		OutRelocation = {};
		const PathUtilities::FSourcePathResult Original =
			PathUtilities::ResolveSourcePath(
				OriginalSourceVirtualPath,
				PathUtilities::EPathExistence::RequireFile);
		if (!Original)
		{
			OutError = Original.Message;
			return false;
		}
		const PathUtilities::FSourcePathResult Destination =
			PathUtilities::ResolveSourcePath(
				DestinationSourceVirtualPath,
				PathUtilities::EPathExistence::AllowMissing);
		if (!Destination)
		{
			OutError = Destination.Message;
			return false;
		}
		if (Original.NormalizedVirtualPath == Destination.NormalizedVirtualPath)
		{
			OutError = "Choose a different mounted source destination.";
			return false;
		}
		const PathUtilities::FMountPolicyResult OriginalMutation =
			PathUtilities::CheckSourceMutation(
				AuthoringAssetPath, Original.NormalizedVirtualPath,
				bEngineAuthoringContext);
		if (!OriginalMutation)
		{
			OutError = OriginalMutation.Message;
			return false;
		}
		const PathUtilities::FMountPolicyResult DestinationMutation =
			PathUtilities::CheckSourceMutation(
				AuthoringAssetPath, Destination.NormalizedVirtualPath,
				bEngineAuthoringContext);
		if (!DestinationMutation)
		{
			OutError = DestinationMutation.Message;
			return false;
		}
		std::error_code Error;
		if (std::filesystem::exists(Destination.PhysicalPath, Error))
		{
			OutError = std::format(
				"Source relocation destination already exists: {}.",
				Destination.PhysicalPath.generic_string());
			return false;
		}
		std::filesystem::create_directories(
			Destination.PhysicalPath.parent_path(), Error);
		if (Error)
		{
			OutError = std::format(
				"Failed to create source relocation directory: {}",
				Error.message());
			return false;
		}
		const std::filesystem::path Temporary =
			Destination.PhysicalPath.generic_string() + ".relocate.tmp";
		std::filesystem::remove(Temporary, Error);
		Error.clear();
		if (!std::filesystem::copy_file(
			Original.PhysicalPath, Temporary,
			std::filesystem::copy_options::none, Error))
		{
			OutError = std::format(
				"Failed to stage source relocation: {}", Error.message());
			return false;
		}
		Error.clear();
		std::filesystem::rename(
			Temporary, Destination.PhysicalPath, Error);
		if (Error)
		{
			std::error_code CleanupError;
			std::filesystem::remove(Temporary, CleanupError);
			OutError = std::format(
				"Failed to publish staged source relocation: {}",
				Error.message());
			return false;
		}
		OutRelocation = {
			.OriginalSourcePath = {.Path = Original.NormalizedVirtualPath},
			.DestinationSourcePath = {.Path = Destination.NormalizedVirtualPath},
			.OriginalPhysicalPath = Original.PhysicalPath,
			.DestinationPhysicalPath = Destination.PhysicalPath,
			.bPublished = true};
		OutError.clear();
		return true;
	}

	auto CommitMountedSourceRelocation(
		FMountedSourceRelocation& Relocation, std::string& OutError) -> bool
	{
		if (!Relocation.bPublished)
		{
			OutError.clear();
			Relocation = {};
			return true;
		}
		std::error_code Error;
		if (!std::filesystem::remove(Relocation.OriginalPhysicalPath, Error)
			|| Error)
		{
			OutError = std::format(
				"Failed to remove relocated source {}: {}",
				Relocation.OriginalPhysicalPath.generic_string(),
				Error ? Error.message() : "file was not removed");
			return false;
		}
		Relocation = {};
		OutError.clear();
		return true;
	}

	auto RollbackMountedSourceRelocation(
		FMountedSourceRelocation& Relocation) -> void
	{
		if (Relocation.bPublished)
		{
			std::error_code Error;
			std::filesystem::remove(
				Relocation.DestinationPhysicalPath, Error);
		}
		Relocation = {};
	}

	auto TryMigrateLegacySourcePath(
		std::string_view PackagePath,
		std::string_view LegacyPath,
		FSourcePath& OutSourcePath,
		std::filesystem::path& OutPhysicalPath,
		std::string& OutError) -> bool
	{
		OutSourcePath = {};
		OutPhysicalPath.clear();
		const PathUtilities::FMountLookupResult Owner =
			PathUtilities::FindMountForVirtualPath(PackagePath);
		if (!Owner)
		{
			OutError = std::format(
				"Legacy source owner '{}' is invalid: {}", PackagePath, Owner.Message);
			return false;
		}

		const std::filesystem::path Legacy(LegacyPath);
		const std::filesystem::path Normalized = Legacy.lexically_normal();
		const std::string NormalizedString = Normalized.generic_string();
		const bool bContainsParent = std::ranges::any_of(
			Legacy, [](const std::filesystem::path& Part) { return Part == ".."; });
		if (LegacyPath.empty() || Legacy.is_absolute() || LegacyPath.starts_with('/')
			|| LegacyPath.find('\\') != std::string_view::npos || bContainsParent
			|| NormalizedString != LegacyPath || !NormalizedString.starts_with("SourceAssets/"))
		{
			OutError = std::format(
				"Legacy source path '{}' is not a normalized SourceAssets-relative file path.",
				LegacyPath);
			return false;
		}

		const std::filesystem::path Relative = Normalized.lexically_relative("SourceAssets");
		if (Relative.empty() || Relative == ".")
		{
			OutError = "Legacy source path does not identify a file beneath SourceAssets.";
			return false;
		}
		const std::string VirtualPath = Owner.Mount->VirtualRoot + Relative.generic_string();
		const PathUtilities::FSourcePathResult Resolved =
			PathUtilities::ResolveSourcePath(
				VirtualPath, PathUtilities::EPathExistence::RequireFile);
		if (!Resolved)
		{
			OutError = Resolved.Message;
			return false;
		}
		OutSourcePath.Path = Resolved.NormalizedVirtualPath;
		OutPhysicalPath = Resolved.PhysicalPath;
		OutError.clear();
		return true;
	}
}
