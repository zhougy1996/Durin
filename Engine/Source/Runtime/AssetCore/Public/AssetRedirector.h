#pragma once

#include "AssetCoreAPI.h"
#include "DObject/CoreDObject.h"

#include "AssetRedirector.gen.h"

namespace Durin::Asset
{
	// Persists an old package identity as one hard reference to its canonical asset.
	DCLASS()
	class DAssetRedirector final : public DObject
	{
		GENERATED_BODY()
	public:
		ASSETCORE_API explicit DAssetRedirector(const FObjectInitializer& ObjectInitializer);

		auto GetDestinationObject() const -> DObject* { return DestinationObject.Get(); }

	private:
		DPROPERTY()
		TObjectPtr<DObject> DestinationObject;

		auto SetDestinationObject(DObject* InDestination) -> void
		{
			DestinationObject = InDestination;
		}

		friend class FAssetManager;
	};
}
