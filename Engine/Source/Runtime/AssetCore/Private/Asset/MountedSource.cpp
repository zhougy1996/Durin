#include "Asset/MountedSource.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::Asset
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

		auto FileEqualsBytes(
			const std::filesystem::path& File,
			std::span<const std::byte> Bytes,
			std::string& OutError) -> bool
		{
			std::error_code Error;
			if (std::filesystem::file_size(File, Error) != Bytes.size() || Error)
			{
				if (Error) OutError = Error.message();
				return false;
			}
			std::ifstream Stream(File, std::ios::binary);
			if (!Stream)
			{
				OutError = "Failed to read the mounted source for collision comparison.";
				return false;
			}
			constexpr size_t BufferSize = 64 * 1024;
			std::array<std::byte, BufferSize> Buffer{};
			size_t Offset = 0;
			while (Offset < Bytes.size())
			{
				const size_t Count = std::min(Buffer.size(), Bytes.size() - Offset);
				Stream.read(reinterpret_cast<char*>(Buffer.data()), Count);
				if (static_cast<size_t>(Stream.gcount()) != Count
					|| !std::equal(Buffer.begin(), Buffer.begin() + Count, Bytes.begin() + Offset))
					return false;
				Offset += Count;
			}
			return true;
		}
	}

	auto ResolveMountedSourceReference(
		std::string_view ReferencingAssetPath,
		std::string_view SourceVirtualPath,
		EMountedSourceExistencePolicy ExistencePolicy,
		FMountedSourceResolution& OutResolution,
		std::string& OutError) -> bool
	{
		OutResolution = {};
		const PathUtilities::FSourcePathResult Resolved =
			PathUtilities::ResolveSourcePath(
				SourceVirtualPath, PathUtilities::EPathExistence::AllowMissing);
		const bool bUnavailableRoot =
			Resolved.Error == PathUtilities::EMountPathError::UnavailableRoot
			&& ExistencePolicy == EMountedSourceExistencePolicy::AllowMissing;
		if (!Resolved && !bUnavailableRoot)
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

		const std::filesystem::path PhysicalPath = bUnavailableRoot
			? (Resolved.Mount->GetContentDir() / Resolved.RelativePath).lexically_normal()
			: Resolved.PhysicalPath;
		std::error_code Error;
		const bool bExists = !bUnavailableRoot
			&& std::filesystem::is_regular_file(PhysicalPath, Error);
		if (Error == std::errc::no_such_file_or_directory
			|| Error == std::errc::not_a_directory)
			Error.clear();
		if (Error)
		{
			OutError = Error.message();
			return false;
		}
		if (!bExists && ExistencePolicy == EMountedSourceExistencePolicy::RequireFile)
		{
			OutError = "The requested file does not exist.";
			return false;
		}

		OutResolution.SourcePath.Path = Resolved.NormalizedVirtualPath;
		OutResolution.PhysicalPath = PhysicalPath;
		OutResolution.bExists = bExists;
		OutError.clear();
		return true;
	}

	auto ResolveMountedSourceReference(
		std::string_view ReferencingAssetPath,
		std::string_view SourceVirtualPath,
		FMountedSourceFile& OutSource,
		std::string& OutError) -> bool
	{
		OutSource = {};
		FMountedSourceResolution Resolution;
		if (!ResolveMountedSourceReference(
			ReferencingAssetPath, SourceVirtualPath,
			EMountedSourceExistencePolicy::RequireFile, Resolution, OutError))
			return false;
		OutSource.SourcePath = std::move(Resolution.SourcePath);
		OutSource.PhysicalPath = std::move(Resolution.PhysicalPath);
		OutSource.Disposition = ESourceFileDisposition::ReferenceExisting;
		return true;
	}

	auto PrepareMountedSourceFile(
		const std::filesystem::path& InputFile,
		std::string_view ReferencingAssetPath,
		std::string_view ExternalIngestDestination,
		FMountedSourceFile& OutSource,
		std::string& OutError,
		EMountedSourceMutationContext MutationContext) -> bool
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
			PathUtilities::CheckAuthoringMutation(
				ReferencingAssetPath, Destination.NormalizedVirtualPath,
				MutationContext == EMountedSourceMutationContext::EngineAuthoring);
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

	auto PrepareMountedSourceBytes(
		std::span<const std::byte> Bytes,
		std::string_view ReferencingAssetPath,
		std::string_view DestinationSourcePath,
		FMountedSourceFile& OutSource,
		std::string& OutError) -> bool
	{
		OutSource = {};
		if (Bytes.empty())
		{
			OutError = "Mounted source bytes cannot be empty.";
			return false;
		}
		const PathUtilities::FSourcePathResult Destination =
			PathUtilities::ResolveSourcePath(
				DestinationSourcePath, PathUtilities::EPathExistence::AllowMissing);
		if (!Destination)
		{
			OutError = Destination.Message;
			return false;
		}
		const PathUtilities::FMountPolicyResult Mutation =
			PathUtilities::CheckAuthoringMutation(
				ReferencingAssetPath, Destination.NormalizedVirtualPath);
		if (!Mutation)
		{
			OutError = Mutation.Message;
			return false;
		}

		OutSource.SourcePath.Path = Destination.NormalizedVirtualPath;
		OutSource.PhysicalPath = Destination.PhysicalPath;
		std::error_code Error;
		if (std::filesystem::is_regular_file(Destination.PhysicalPath, Error))
		{
			std::string CompareError;
			if (!FileEqualsBytes(Destination.PhysicalPath, Bytes, CompareError))
			{
				OutError = CompareError.empty()
					? std::format(
						"A different source payload already exists at {}.",
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
		FFileHelper::FAtomicFileError SaveError;
		if (!FFileHelper::SaveArrayToFileAtomically(
			Bytes,
			Destination.PhysicalPath,
			&SaveError))
		{
			OutError = SaveError.ToString();
			OutSource = {};
			return false;
		}
		OutSource.Disposition = ESourceFileDisposition::IngestedExternal;
		OutSource.bCreatedFile = true;
		OutError.clear();
		return true;
	}

	auto CommitMountedSourceFile(FMountedSourceFile& Source) noexcept -> void
	{
		Source.bCreatedFile = false;
	}

	auto RollbackMountedSourceFile(FMountedSourceFile& Source) noexcept -> void
	{
		if (Source.bCreatedFile)
		{
			std::error_code Error;
			std::filesystem::remove(Source.PhysicalPath, Error);
		}
		Source = {};
	}

	FScopedMountedSourceFile::~FScopedMountedSourceFile() noexcept
	{
		Reset();
	}

	FScopedMountedSourceFile::FScopedMountedSourceFile(
		FScopedMountedSourceFile&& Other) noexcept
	{
		SourcePath = std::move(Other.SourcePath);
		PhysicalPath = std::move(Other.PhysicalPath);
		Disposition = Other.Disposition;
		bCreatedFile = std::exchange(Other.bCreatedFile, false);
		Other.SourcePath = {};
		Other.PhysicalPath.clear();
	}

	auto FScopedMountedSourceFile::operator=(
		FScopedMountedSourceFile&& Other) noexcept -> FScopedMountedSourceFile&
	{
		if (this == &Other) return *this;
		Reset();
		SourcePath = std::move(Other.SourcePath);
		PhysicalPath = std::move(Other.PhysicalPath);
		Disposition = Other.Disposition;
		bCreatedFile = std::exchange(Other.bCreatedFile, false);
		Other.SourcePath = {};
		Other.PhysicalPath.clear();
		return *this;
	}

	auto FScopedMountedSourceFile::Commit() noexcept -> void
	{
		CommitMountedSourceFile(*this);
	}

	auto FScopedMountedSourceFile::Reset() noexcept -> void
	{
		RollbackMountedSourceFile(*this);
	}

	auto PrepareMountedSourceReplacement(
		const std::filesystem::path& ReplacementFile,
		std::string_view AuthoringAssetPath,
		std::string_view SourceVirtualPath,
		FMountedSourceReplacement& OutReplacement,
		std::string& OutError,
		EMountedSourceMutationContext MutationContext) -> bool
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
			PathUtilities::CheckAuthoringMutation(
				AuthoringAssetPath, Target.NormalizedVirtualPath,
				MutationContext == EMountedSourceMutationContext::EngineAuthoring);
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
		EMountedSourceMutationContext MutationContext) -> bool
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
			PathUtilities::CheckAuthoringMutation(
				AuthoringAssetPath, Original.NormalizedVirtualPath,
				MutationContext == EMountedSourceMutationContext::EngineAuthoring);
		if (!OriginalMutation)
		{
			OutError = OriginalMutation.Message;
			return false;
		}
		const PathUtilities::FMountPolicyResult DestinationMutation =
			PathUtilities::CheckAuthoringMutation(
				AuthoringAssetPath, Destination.NormalizedVirtualPath,
				MutationContext == EMountedSourceMutationContext::EngineAuthoring);
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

}
