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

	auto FormatAssetDestinationValidation(const FAssetDestinationValidation& Result) -> std::string
	{
		switch (Result.Error)
		{
		case EAssetDestinationError::None: return {};
		case EAssetDestinationError::Path: return Result.PathCause ? FormatObjectError(*Result.PathCause) : "Invalid asset destination path.";
		case EAssetDestinationError::Mount: return "Choose a destination inside a package-enabled mount.";
		case EAssetDestinationError::ReadOnly: return "Choose a destination inside a content-writable mount.";
		case EAssetDestinationError::RegistryAsset: return "An asset already exists at this path. Choose another destination or delete the existing asset first.";
		case EAssetDestinationError::UnsavedPackage: return "A newly created unsaved package already uses this path. Save or explicitly discard it before reusing the destination.";
		case EAssetDestinationError::ResidentPackage: return "A resident package already uses this path. Close it or choose another destination.";
		case EAssetDestinationError::Redirector: return Result.RedirectDestination.IsValid()
			? std::format("A redirector already occupies this path and points to {}. Run Fix Up Redirectors or choose another destination.", Result.RedirectDestination.ToString())
			: "A redirector already occupies this path. Repair or Fix Up the redirector before reusing the destination.";
		}
		return {};
	}

	auto InspectAssetDestination(
		std::string_view VirtualPath,
		FAssetDestinationOccupancyQuery OccupancyQuery
	) -> FAssetDestinationValidation
	{
		FAssetDestinationValidation Result{.RequestedPath = std::string(VirtualPath)};
		const auto PathValidation = FPackagePath::TryCreate(VirtualPath, Result.AssetPath);
		Result.bAssetPathValid = PathValidation.Succeeded();
		if (!PathValidation)
		{
			Result.Error = EAssetDestinationError::Path;
			Result.PathCause = PathValidation.Error;
		}
		if (!Result.bAssetPathValid) return Result;

		const FAssetPathResult Resolved =
			FMountPaths::ResolveAssetPath(Result.AssetPath.GetView());
		Result.Mount = Resolved.Mount;
		if (!Resolved)
		{
			Result.Error = EAssetDestinationError::Mount;
			Result.MountCause = Resolved.Error;
			return Result;
		}

		Result.bMountedDestination = true;
		Result.bContentWritable = Result.Mount->bContentWritable;
		Result.PhysicalPath = Resolved.PhysicalPath.generic_string() + ".dasset";
		if (!Result.bContentWritable)
		{
			Result.Error = EAssetDestinationError::ReadOnly;
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
			Result.Error = EAssetDestinationError::Redirector;
		else if (Result.bRegistryAssetExists)
			Result.Error = EAssetDestinationError::RegistryAsset;
		else if (Result.bResidentPackageNewlyCreated)
			Result.Error = EAssetDestinationError::UnsavedPackage;
		else if (Result.bResidentPackageExists)
			Result.Error = EAssetDestinationError::ResidentPackage;
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
			Result.Error = EAssetDestinationError::Mount;
			Result.RequestedPath = PhysicalPath.generic_string();
			Result.MountCause = Classified.Error;
			return Result;
		}

		std::filesystem::path VirtualPath(Classified.NormalizedVirtualPath);
		VirtualPath.replace_extension();
		return InspectAssetDestination(VirtualPath.generic_string(), OccupancyQuery);
	}

	auto FormatContentDirectoryValidation(const FContentDirectoryValidation& Result) -> std::string
	{
		switch (Result.Error)
		{
		case EContentDirectoryError::None: return {};
		case EContentDirectoryError::Path: return Result.PathCause ? FormatObjectError(*Result.PathCause) : "Invalid content directory path.";
		case EContentDirectoryError::Mount: return "Choose a directory inside a package-enabled mount.";
		case EContentDirectoryError::ReadOnly: return "Choose a directory inside a content-writable mount.";
		}
		return {};
	}

	auto InspectContentDirectory(std::string_view VirtualPath)
		-> FContentDirectoryValidation
	{
		FContentDirectoryValidation Result{.RequestedPath = std::string(VirtualPath)};
		const auto PathValidation = FPackagePath::TryCreate(VirtualPath, Result.DirectoryPath);
		Result.bDirectoryPathValid = PathValidation.Succeeded();
		if (!PathValidation)
		{
			Result.Error = EContentDirectoryError::Path;
			Result.PathCause = PathValidation.Error;
		}
		if (!Result.bDirectoryPathValid) return Result;

		const FAssetPathResult Resolved =
			FMountPaths::ResolveAssetPath(Result.DirectoryPath.GetView());
		Result.Mount = Resolved.Mount;
		if (!Resolved)
		{
			Result.Error = EContentDirectoryError::Mount;
			Result.MountCause = Resolved.Error;
			return Result;
		}

		Result.bMountedDestination = true;
		Result.bContentWritable = Result.Mount->bContentWritable;
		Result.PhysicalPath = Resolved.PhysicalPath;
		if (!Result.bContentWritable)
			Result.Error = EContentDirectoryError::ReadOnly;
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
			Result.Error = EContentDirectoryError::Mount;
			Result.RequestedPath = PhysicalPath.generic_string();
			Result.MountCause = Classified.Error;
			return Result;
		}
		return InspectContentDirectory(Classified.NormalizedVirtualPath);
	}
} // namespace Durin::Editor
