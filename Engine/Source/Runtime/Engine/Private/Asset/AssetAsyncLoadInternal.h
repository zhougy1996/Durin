#pragma once

#include "Asset/AssetReadResult.h"
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
		FAssetReadResult Result;
	};
	using FAsyncPackageInputs = std::unordered_map<FPackagePath, FAsyncPackageInput>;
}
