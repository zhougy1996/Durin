#include "Misc/MountPaths.h"
#include "Editor/Import/AssetDestinationValidation.h"

#include "Asset/Asset.h"
#include "DObject/Package.h"

namespace Durin::Editor
{
	namespace
	{
		auto QueryAssetDestinationOccupancy(const FPackagePath& AssetPath) -> FAssetDestinationOccupancy
		{
			const FAssetCatalogEntry Entry = FindAssetExact(AssetPath);
			DPackage* ResidentPackage = FindResidentPackage(AssetPath);
			return {
				.bRegistryAssetExists = Entry.Succeeded(),
				.bResidentPackageExists = ResidentPackage != nullptr,
				.bResidentPackageNewlyCreated =
					ResidentPackage && ResidentPackage->IsNewlyCreated(),
				.OccupantKind = !Entry
					? EAssetDestinationOccupantKind::None
					: Entry->EntryKind == EAssetRegistryEntryKind::Redirector
						? EAssetDestinationOccupantKind::Redirector
						: EAssetDestinationOccupantKind::Asset,
				.RedirectDestination = Entry
					? Entry->RedirectDestination : FPackagePath{}
			};
		}
	} // namespace

	auto InspectAssetDestination(
		std::string_view VirtualPath,
		FAssetDestinationOccupancyQuery OccupancyQuery
	) -> FAssetDestinationValidation
	{
		FAssetDestinationValidation Result;
		Result.bAssetPathValid = FPackagePath::TryCreate(VirtualPath, Result.AssetPath, &Result.Message);
		if (!Result.bAssetPathValid) return Result;

		const FAssetPathResult Resolved =
			FMountPaths::ResolveAssetPath(Result.AssetPath.GetView());
		Result.Mount = Resolved.Mount;
		if (!Resolved)
		{
			Result.Message = "Choose a destination inside a package-enabled mount.";
			return Result;
		}

		Result.bMountedDestination = true;
		Result.bContentWritable = Result.Mount->bContentWritable;
		Result.PhysicalPath = Resolved.PhysicalPath.generic_string() + ".dasset";
		if (!Result.bContentWritable)
		{
			Result.Message = "Choose a destination inside a content-writable mount.";
			return Result;
		}
		const FAssetDestinationOccupancy Occupancy =
			(OccupancyQuery != nullptr ? OccupancyQuery : QueryAssetDestinationOccupancy)(Result.AssetPath);
		Result.bRegistryAssetExists = Occupancy.bRegistryAssetExists;
		Result.bResidentPackageExists = Occupancy.bResidentPackageExists;
		Result.bResidentPackageNewlyCreated =
			Occupancy.bResidentPackageNewlyCreated;
		Result.OccupantKind = Occupancy.OccupantKind;
		Result.RedirectDestination = Occupancy.RedirectDestination;
		if (Result.bRegistryAssetExists
			&& Result.OccupantKind == EAssetDestinationOccupantKind::Redirector)
			Result.Message = Result.RedirectDestination.IsValid()
				? std::format(
					"A redirector already occupies this path and points to {}. Run Fix Up Redirectors or choose another destination.",
					Result.RedirectDestination.ToString())
				: "A redirector already occupies this path. Repair or Fix Up the redirector before reusing the destination.";
		else if (Result.bRegistryAssetExists)
			Result.Message = "An asset already exists at this path. Choose another destination or delete the existing asset first.";
		else if (Result.bResidentPackageNewlyCreated)
			Result.Message = "A newly created unsaved package already uses this path. Save or explicitly discard it before reusing the destination.";
		else if (Result.bResidentPackageExists)
			Result.Message = "A resident package already uses this path. Close it or choose another destination.";
		return Result;
	}

	auto ClassifyAssetDestination(
		const std::filesystem::path& PhysicalPath,
		FAssetDestinationOccupancyQuery OccupancyQuery
	) -> FAssetDestinationValidation
	{
		const FAssetPathResult Classified =
			FMountPaths::ClassifyAssetPath(PhysicalPath);
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
		Result.bDirectoryPathValid = FPackagePath::TryCreate(
			VirtualPath, Result.DirectoryPath, &Result.Message);
		if (!Result.bDirectoryPathValid) return Result;

		const FAssetPathResult Resolved =
			FMountPaths::ResolveAssetPath(Result.DirectoryPath.GetView());
		Result.Mount = Resolved.Mount;
		if (!Resolved)
		{
			Result.Message = "Choose a directory inside a package-enabled mount.";
			return Result;
		}

		Result.bMountedDestination = true;
		Result.bContentWritable = Result.Mount->bContentWritable;
		Result.PhysicalPath = Resolved.PhysicalPath;
		if (!Result.bContentWritable)
			Result.Message = "Choose a directory inside a content-writable mount.";
		return Result;
	}

	auto ClassifyContentDirectory(const std::filesystem::path& PhysicalPath)
		-> FContentDirectoryValidation
	{
		const FAssetPathResult Classified =
			FMountPaths::ClassifyAssetPath(PhysicalPath);
		if (!Classified)
		{
			FContentDirectoryValidation Result;
			Result.Message = Classified.Message;
			return Result;
		}
		return InspectContentDirectory(Classified.NormalizedVirtualPath);
	}
} // namespace Durin::Editor
