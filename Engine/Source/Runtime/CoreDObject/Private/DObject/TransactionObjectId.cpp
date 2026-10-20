#include "DObject/TransactionObjectId.h"

#include "DObject/Class.h"
#include "DObject/Object.h"
#include "DObject/Package.h"

namespace Durin
{
	auto CaptureTransactionObjectIdentity(const DObject& Object) -> FTransactionObjectId
	{
		FTransactionObjectId Identity;
		Identity.OriginalObject = FObjectKey(&Object);
		Identity.OriginalName = Object.GetFName();
		if (auto* Outer = Object.GetOuter()) Identity.OriginalOuterPath = Outer->GetObjectPath();
		if (auto* Class = Object.GetClass()) Identity.OriginalClassPath = Class->GetQualifiedName().ToString();
		if (auto* Package = Object.GetPackage()) Identity.OriginalPackagePath = Package->IsAssetPackage() ? Package->GetPackagePath() : Package->GetObjectPath();
		return Identity;
	}
}
