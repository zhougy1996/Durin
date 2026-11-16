#include "Asset/Redirector.h"

namespace Durin
{
	DAssetRedirector::DAssetRedirector(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}
	auto DAssetRedirector::SetDestinationObject(DObject* Object) -> void { DestinationObject = Object; MarkPackageDirty(); }
}
