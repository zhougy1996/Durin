#include "DObject/Package.h"

namespace Durin
{
	DPackage::DPackage(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DPackage::InitializePackage(const FAssetPath& InPath) -> void
	{
		check(GetOuter() == nullptr);
		PackagePath = InPath.ToString();
	}

	auto DPackage::SetAsset(DObject* InAsset) -> bool
	{
		if (InAsset && InAsset->GetOuter() != this) return false;
		Asset = InAsset;
		bDirty = true;
		return true;
	}
}
