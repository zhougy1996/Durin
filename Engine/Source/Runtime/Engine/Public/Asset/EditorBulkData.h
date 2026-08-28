#pragma once

#include "EngineAPI.h"
#include "Asset/BulkData.h"
#include "Serialization/Archive.h"

namespace Durin::Asset
{
	// Owns one atomic authored payload and exposes verified immutable bytes.
	class FEditorBulkData
	{
	public:
		FEditorBulkData() = default;
		ENGINE_API explicit FEditorBulkData(FGuid PayloadId);

		auto GetBulkData() const -> const FBulkData& { return Data; }
		ENGINE_API auto ReplaceBytes(std::span<const std::byte> Bytes) -> bool;
		ENGINE_API auto ReplaceBytes(
			FGuid PayloadId, std::span<const std::byte> Bytes) -> bool;
		ENGINE_API auto Serialize(FArchive& Ar) -> void;
		ENGINE_API auto Identical(const FEditorBulkData& Other) const -> bool;

	private:
		FBulkData Data;
	};
}
