#pragma once

#include "AssetCoreAPI.h"
#include "Asset/BulkData.h"
#include "Serialization/Archive.h"

namespace Durin::Asset
{
	// Owns one atomic authored payload and exposes verified immutable bytes.
	class FEditorBulkData
	{
	public:
		FEditorBulkData() = default;
		ASSETCORE_API explicit FEditorBulkData(FGuid PayloadId);

		auto GetBulkData() const -> const FBulkData& { return Data; }
		ASSETCORE_API auto ReplaceBytes(std::span<const std::byte> Bytes) -> bool;
		ASSETCORE_API auto ReplaceBytes(
			FGuid PayloadId, std::span<const std::byte> Bytes) -> bool;
		ASSETCORE_API auto Serialize(FArchive& Ar) -> void;
		ASSETCORE_API auto Identical(const FEditorBulkData& Other) const -> bool;

	private:
		FBulkData Data;
	};
}
