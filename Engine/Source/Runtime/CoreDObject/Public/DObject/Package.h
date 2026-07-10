#pragma once

#include "CoreDObjectAPI.h"
#include "DObject/AssetPath.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"

#include "Package.gen.h"

namespace Durin
{
	DCLASS()
	class COREDOBJECT_API DPackage : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DPackage(const FObjectInitializer& ObjectInitializer);

		auto GetPackagePath() const -> const std::string& { return PackagePath; }
		auto GetAsset() const -> DObject* { return Asset.Get(); }
		auto IsDirty() const -> bool { return bDirty; }

		auto InitializePackage(const FAssetPath& InPath) -> void;
		auto SetAsset(DObject* InAsset) -> bool;
		auto MarkDirty() -> void { bDirty = true; }
		auto ClearDirty() -> void { bDirty = false; }

	private:
		DPROPERTY()
		std::string PackagePath;

		DPROPERTY()
		TObjectPtr<DObject> Asset;

		DPROPERTY(Transient)
		bool bDirty = false;
	};
}
