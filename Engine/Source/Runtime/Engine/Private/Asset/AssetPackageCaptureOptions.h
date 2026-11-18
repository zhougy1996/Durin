#pragma once

#include "CoreMinimal.h"
#include "Asset/PackageSerialization.h"
#include "DObject/Archive.h"
#include "DObject/ObjectKey.h"

namespace Durin { class ITargetPlatform; }

namespace Durin::AssetPrivate
{
	// Cook targets reach the encoder only after the Cook owner prepares its products.
	struct FAssetPackageCaptureOptions
	{
		FAssetPackageSerializationOptions Serialization;
		std::optional<FArchiveTarget> CookTarget;
		const ITargetPlatform* TargetPlatform = nullptr;
		std::vector<FObjectKey>* OutExports = nullptr;
	};
}
