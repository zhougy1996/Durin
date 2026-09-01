#pragma once

#include "Asset/AssetDefinitions.h"

#include "EngineAPI.h"
#include "Asset/MutationTypes.h"

namespace Durin
{
	struct FAssetRelocationMapping
	{
		FPackagePath SourcePath;
		FPackagePath DestinationPath;

		auto operator==(const FAssetRelocationMapping&) const -> bool = default;
	};

	class FAssetRelocationSummary
	{
	public:
		FAssetRelocationSummary() = default;
		FAssetRelocationSummary(
			uint64 InRegistryRevision,
			std::vector<FPackagePath> InScope)
			: RegistryRevision(InRegistryRevision)
			, Scope(std::move(InScope))
		{
		}

		auto GetRegistryRevision() const -> uint64 { return RegistryRevision; }
		auto GetScope() const -> std::span<const FPackagePath> { return Scope; }

	private:
		uint64 RegistryRevision = 0;
		std::vector<FPackagePath> Scope;
	};

	ENGINE_API auto PrepareAssetRelocationJob(
		std::span<const FAssetRelocationMapping> Mappings,
		FAssetRelocationSummary& OutSummary,
		FAssetMutationJob& OutJob
	) -> FAssetResult;
} // namespace Durin
