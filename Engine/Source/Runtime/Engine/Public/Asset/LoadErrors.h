#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"

namespace Durin::AssetLoadErrors
{
	inline constexpr UnifiedError::FErrorId InvalidPath{"AssetLoad", "InvalidPath"};
	inline constexpr UnifiedError::FErrorId AlreadyExists{"AssetLoad", "AlreadyExists"};
	inline constexpr UnifiedError::FErrorId NotFound{"AssetLoad", "NotFound"};
	inline constexpr UnifiedError::FErrorId UnknownClass{"AssetLoad", "UnknownClass"};
	inline constexpr UnifiedError::FErrorId TypeMismatch{"AssetLoad", "TypeMismatch"};
	inline constexpr UnifiedError::FErrorId MissingDependency{"AssetLoad", "MissingDependency"};
	inline constexpr UnifiedError::FErrorId CircularDependency{"AssetLoad", "CircularDependency"};
	inline constexpr UnifiedError::FErrorId InvalidObjectGraph{"AssetLoad", "InvalidObjectGraph"};
	inline constexpr UnifiedError::FErrorId UnsupportedProperty{"AssetLoad", "UnsupportedProperty"};
	inline constexpr UnifiedError::FErrorId InvalidPackageType{"AssetLoad", "InvalidPackageType"};
	inline constexpr UnifiedError::FErrorId InUse{"AssetLoad", "InUse"};
	inline constexpr UnifiedError::FErrorId StaleData{"AssetLoad", "StaleData"};
	inline constexpr UnifiedError::FErrorId NotInitialized{"AssetLoad", "NotInitialized"};
	inline constexpr UnifiedError::FErrorId ShuttingDown{"AssetLoad", "ShuttingDown"};
	inline constexpr UnifiedError::FErrorId Cancelled{"AssetLoad", "Cancelled"};
	inline constexpr UnifiedError::FErrorId ProjectionPending{"AssetLoad", "ProjectionPending"};
}

namespace Durin::PackageReadErrors
{
	inline constexpr UnifiedError::FErrorId IoError{"PackageRead", "IoError"};
	inline constexpr UnifiedError::FErrorId CorruptFile{"PackageRead", "CorruptFile"};
	inline constexpr UnifiedError::FErrorId UnsupportedVersion{"PackageRead", "UnsupportedVersion"};
}
