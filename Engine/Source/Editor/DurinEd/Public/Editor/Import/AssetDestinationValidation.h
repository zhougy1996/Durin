#pragma once

#include "Asset/Load.h"
#include "DurinEdAPI.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin::Editor
{
	enum class EAssetDestinationOccupantKind : uint8
	{
		None,
		Asset,
		Redirector
	};

	// Separates asset occupancy lookup from path validation for deterministic callers and tests.
	struct FAssetDestinationOccupancy
	{
		bool bRegistryAssetExists = false;
		bool bResidentPackageExists = false;
		bool bResidentPackageNewlyCreated = false;
		EAssetDestinationOccupantKind OccupantKind =
			EAssetDestinationOccupantKind::None;
		FPackagePath RedirectDestination;
	};

	using FAssetDestinationOccupancyQuery = FAssetDestinationOccupancy (*)(const FPackagePath&);

	// Carries the complete side-effect-free decision for one editor asset destination.
	struct FAssetDestinationValidation
	{
		FPackagePath AssetPath;
		const FMountPoint* Mount = nullptr;
		std::filesystem::path PhysicalPath;
		bool bAssetPathValid = false;
		bool bMountedDestination = false;
		bool bContentWritable = false;
		bool bRegistryAssetExists = false;
		bool bResidentPackageExists = false;
		bool bResidentPackageNewlyCreated = false;
		EAssetDestinationOccupantKind OccupantKind =
			EAssetDestinationOccupantKind::None;
		FPackagePath RedirectDestination;
		std::string Message;

		auto AssetExists() const -> bool
		{
			return bRegistryAssetExists || bResidentPackageExists;
		}
		explicit operator bool() const
		{
			return bAssetPathValid && bMountedDestination && bContentWritable
				&& !AssetExists() && Message.empty();
		}
	};

	// Carries the side-effect-free resolution of one virtual asset directory.
	struct FContentDirectoryValidation
	{
		FPackagePath DirectoryPath;
		const FMountPoint* Mount = nullptr;
		std::filesystem::path PhysicalPath;
		bool bDirectoryPathValid = false;
		bool bMountedDestination = false;
		bool bContentWritable = false;
		std::string Message;

		explicit operator bool() const
		{
			return bDirectoryPathValid && bMountedDestination
				&& bContentWritable && Message.empty();
		}
	};

	// Validates a virtual asset path and queries occupancy only after Content resolution succeeds.
	DURINED_API auto InspectAssetDestination(
		std::string_view VirtualPath,
		FAssetDestinationOccupancyQuery OccupancyQuery = nullptr
	) -> FAssetDestinationValidation;

	// Converts a selected package filename back to its extension-free virtual asset destination.
	DURINED_API auto ClassifyAssetDestination(
		const std::filesystem::path& PhysicalPath,
		FAssetDestinationOccupancyQuery OccupancyQuery = nullptr
	) -> FAssetDestinationValidation;

	DURINED_API auto InspectContentDirectory(std::string_view VirtualPath)
		-> FContentDirectoryValidation;
	DURINED_API auto ClassifyContentDirectory(const std::filesystem::path& PhysicalPath)
		-> FContentDirectoryValidation;
} // namespace Durin::Editor
