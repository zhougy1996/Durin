#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

#include "DObject/AssetPath.h"
#include "Misc/PackageWriter.h"

namespace Durin
{
	struct FAsyncPackageInput
	{
		std::string PhysicalPath;
		std::shared_ptr<FPackageFileAccess> Access;
		FByteBuffer Bytes;
		uint64 BulkBytes = 0;
		std::expected<void, Durin::UnifiedError::FError> Result;
	};
	using FAsyncPackageInputs = std::unordered_map<FPackagePath, FAsyncPackageInput>;
}
