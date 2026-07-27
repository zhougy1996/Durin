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

		const PathUtilities::FContentPathResult Resolved =
			PathUtilities::ResolveContentPath(Result.AssetPath.GetView());
		Result.Mount = Resolved.Mount;
		if (!Resolved)
		{
			Result.Message = "Choose a destination inside a mounted Content directory.";
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
		const PathUtilities::FContentPathResult Classified =
			PathUtilities::ClassifyContentPath(PhysicalPath);
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
} // namespace Durin
