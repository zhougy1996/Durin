#include "StaticMesh/StaticMeshDerivedData.h"

#include "Misc/DerivedDataCache.h"

namespace Durin
{
	auto BuildStaticMeshDerivedDataKeyBytes(
		const FStaticMeshDerivedDataKeyInput& Input) -> std::vector<uint8>
	{
		DerivedDataCache::FWriter Writer;
		Writer.WriteU32(StaticMeshDerivedDataKeySchemaVersion);
		Writer.WriteU64(Input.SourceContentHash.HashLow);
		Writer.WriteU64(Input.SourceContentHash.HashHigh);
		Writer.WriteString(Input.ImporterId);
		Writer.WriteU32(Input.ImporterVersion);
		Writer.WriteU8(static_cast<uint8>(Input.ImportSettings.ForwardAxis));
		Writer.WriteU8(static_cast<uint8>(Input.ImportSettings.RightAxis));
		Writer.WriteU8(static_cast<uint8>(Input.ImportSettings.UpAxis));
		Writer.WriteU32(Input.BuilderVersion);
		Writer.WriteU32(Input.PayloadSchemaVersion);
		Writer.WriteU32(static_cast<uint32>(Input.TargetPlatform));
		return Writer.TakeBytes();
	}

	auto BuildStaticMeshDerivedDataKey(
		const FStaticMeshDerivedDataKeyInput& Input) -> std::string
	{
		return FXxHash128::HashBuffer(BuildStaticMeshDerivedDataKeyBytes(Input)).ToString();
	}
}
