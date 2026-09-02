#include "Asset/Redirector.h"

namespace Durin
{
	DAssetRedirector::DAssetRedirector(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto SetAssetRedirectorDestinationForTesting(
		DAssetRedirector& Redirector,
		DObject* Destination) -> void
	{
		Redirector.SetDestinationObject(Destination);
	}
}
