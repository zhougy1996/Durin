#pragma once

#include "EngineAPI.h"
#include "DObject/ObjectPtr.h"

#include "Redirector.gen.h"

namespace Durin
{
	// Persists an old package identity as one hard reference to its canonical asset.
	DCLASS()
	class DAssetRedirector final : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DAssetRedirector(const FObjectInitializer& ObjectInitializer);

		auto GetDestinationObject() const -> DObject* { return DestinationObject.Get(); }

	private:
		DPROPERTY()
		TObjectPtr<DObject> DestinationObject;

		auto SetDestinationObject(DObject* InDestination) -> void
		{
			DestinationObject = InDestination;
		}

		friend class FAssetLoadService;
	};
}
