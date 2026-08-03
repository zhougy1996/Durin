#pragma once

#include "Misc/Paths.h"

namespace Durin
{
	enum class EMountedSourceImportMode : uint8
	{
		ReferenceExisting,
		IngestExternal
	};

	struct FMountedSourceImportDiagnostic
	{
		const PathUtilities::FMountPoint* Mount = nullptr;
		std::string VirtualPath;
		std::string Message;
		bool bValid = false;
	};

	inline auto DescribeMountOwner(PathUtilities::EMountOwner Owner) -> const char*
	{
		switch (Owner)
		{
		case PathUtilities::EMountOwner::Engine: return "Engine";
		case PathUtilities::EMountOwner::ActiveProject: return "Project";
		case PathUtilities::EMountOwner::Extension: return "Extension";
		case PathUtilities::EMountOwner::ExternalSources: return "External sources";
		case PathUtilities::EMountOwner::Test: return "Test";
		}
		return "Unknown";
	}

	inline auto InspectMountedSourceImport(
		std::string_view InputFile,
		std::string_view AssetPath,
		std::string_view IngestTarget,
		EMountedSourceImportMode Mode) -> FMountedSourceImportDiagnostic
	{
		FMountedSourceImportDiagnostic Result;
		if (InputFile.empty())
		{
			Result.Message = "Select a source file to continue.";
			return Result;
		}
		const std::filesystem::path Input =
			std::filesystem::absolute(InputFile).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
		{
			Result.Message = "The selected source file no longer exists.";
			return Result;
		}

		const PathUtilities::FSourcePathResult Classified =
			PathUtilities::ClassifySourcePath(Input);
		if (Mode == EMountedSourceImportMode::ReferenceExisting)
		{
			if (!Classified)
			{
				Result.Message = Classified.Error == PathUtilities::EMountPathError::UnknownMount
					? "This file is external. Choose Ingest External Source to copy it into a writable mount."
					: Classified.Message;
				return Result;
			}
			const PathUtilities::FMountPolicyResult Dependency =
				PathUtilities::CheckMountDependency(AssetPath, Classified.NormalizedVirtualPath);
			if (!Dependency)
			{
				Result.Message = Dependency.Message;
				return Result;
			}
			Result.Mount = Classified.Mount;
			Result.VirtualPath = Classified.NormalizedVirtualPath;
			Result.bValid = true;
			return Result;
		}

		if (Classified)
		{
			Result.Mount = Classified.Mount;
			Result.VirtualPath = Classified.NormalizedVirtualPath;
			Result.Message =
				"This file is already mounted. Choose Reference Existing Source to keep it in place.";
			return Result;
		}
		if (Classified.Error != PathUtilities::EMountPathError::UnknownMount)
		{
			Result.Message = Classified.Message;
			return Result;
		}
		if (IngestTarget.empty())
		{
			Result.Message = "Choose a complete mounted source destination.";
			return Result;
		}
		const PathUtilities::FSourcePathResult Destination =
			PathUtilities::ResolveSourcePath(
				IngestTarget, PathUtilities::EPathExistence::AllowMissing);
		if (!Destination)
		{
			Result.Message = Destination.Message;
			return Result;
		}
		const PathUtilities::FMountPolicyResult Mutation =
			PathUtilities::CheckAuthoringMutation(AssetPath, Destination.NormalizedVirtualPath);
		if (!Mutation)
		{
			Result.Message = Mutation.Message;
			return Result;
		}
		Result.Mount = Destination.Mount;
		Result.VirtualPath = Destination.NormalizedVirtualPath;
		Result.bValid = true;
		return Result;
	}

	inline auto MakeDefaultSourceVirtualPath(
		std::string_view AssetPath,
		std::string_view Category,
		std::string_view FileName) -> std::string
	{
		const PathUtilities::FMountLookupResult Lookup =
			PathUtilities::FindMountForVirtualPath(AssetPath);
		if (!Lookup) return {};
		return Lookup.Mount->VirtualRoot + std::string(Category) + "/" + std::string(FileName);
	}
} // namespace Durin
