#include "Assets/AssetDestinationValidation.h"

#include "AssetSystem.h"

namespace Durin
{
	namespace
	{
		auto QueryAssetDestinationOccupancy(const FAssetPath& AssetPath) -> FAssetDestinationOccupancy
		{
			return {
				.bRegistryAssetExists = Asset::GetAssetRegistry().FindAsset(AssetPath) != nullptr,
				.bLoadedPackageExists = Asset::FindLoadedPackage(AssetPath) != nullptr
			};
		}
	} // namespace

	auto InspectAssetDestination(
		std::string_view VirtualPath,
		FAssetDestinationOccupancyQuery OccupancyQuery
	) -> FAssetDestinationValidation
	{
		FAssetDestinationValidation Result;
		Result.bAssetPathValid = FAssetPath::TryCreate(VirtualPath, Result.AssetPath, &Result.Message);
		if (!Result.bAssetPathValid) return Result;

		const PathUtilities::FAssetPathResult Resolved =
			PathUtilities::ResolveAssetPath(Result.AssetPath.GetView());
		Result.Mount = Resolved.Mount;
		if (!Resolved)
		{
			Result.Message = "Choose a destination inside a package-enabled mount.";
			return Result;
		}

		Result.bMountedDestination = true;
		Result.PhysicalPath = Resolved.PhysicalPath.generic_string() + ".dasset";
		const FAssetDestinationOccupancy Occupancy =
			(OccupancyQuery != nullptr ? OccupancyQuery : QueryAssetDestinationOccupancy)(Result.AssetPath);
		Result.bRegistryAssetExists = Occupancy.bRegistryAssetExists;
		Result.bLoadedPackageExists = Occupancy.bLoadedPackageExists;
		if (Result.AssetExists()) Result.Message = "An asset already exists at this path.";
		return Result;
	}

	auto ClassifyAssetDestination(
		const std::filesystem::path& PhysicalPath,
		FAssetDestinationOccupancyQuery OccupancyQuery
	) -> FAssetDestinationValidation
	{
		const PathUtilities::FAssetPathResult Classified =
			PathUtilities::ClassifyAssetPath(PhysicalPath);
		if (!Classified)
		{
			FAssetDestinationValidation Result;
			Result.Message = Classified.Message;
			return Result;
		}

		std::filesystem::path VirtualPath(Classified.NormalizedVirtualPath);
		VirtualPath.replace_extension();
		return InspectAssetDestination(VirtualPath.generic_string(), OccupancyQuery);
	}

	auto InspectContentDirectory(std::string_view VirtualPath)
		-> FContentDirectoryValidation
	{
		FContentDirectoryValidation Result;
		Result.bDirectoryPathValid = FAssetPath::TryCreate(
			VirtualPath, Result.DirectoryPath, &Result.Message);
		if (!Result.bDirectoryPathValid) return Result;

		const PathUtilities::FAssetPathResult Resolved =
			PathUtilities::ResolveAssetPath(Result.DirectoryPath.GetView());
		Result.Mount = Resolved.Mount;
		if (!Resolved)
		{
			Result.Message = "Choose a directory inside a package-enabled mount.";
			return Result;
		}

		Result.bMountedDestination = true;
		Result.PhysicalPath = Resolved.PhysicalPath;
		return Result;
	}

	auto ClassifyContentDirectory(const std::filesystem::path& PhysicalPath)
		-> FContentDirectoryValidation
	{
		const PathUtilities::FAssetPathResult Classified =
			PathUtilities::ClassifyAssetPath(PhysicalPath);
		if (!Classified)
		{
			FContentDirectoryValidation Result;
			Result.Message = Classified.Message;
			return Result;
		}
		return InspectContentDirectory(Classified.NormalizedVirtualPath);
	}
} // namespace Durin
