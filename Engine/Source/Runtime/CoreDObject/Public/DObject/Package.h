#pragma once

#include "CoreDObjectAPI.h"
#include "DObject/AssetPath.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"

#include "Package.gen.h"

namespace Durin
{
	enum class EPackageFlags : uint8
	{
		None = 0,
		Asset = 1 << 0,
		Cpp = 1 << 1,
	};
	ENUM_CLASS_FLAGS(EPackageFlags);

	DCLASS()
	class COREDOBJECT_API DPackage : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DPackage(const FObjectInitializer& ObjectInitializer);
		~DPackage() override;

		auto GetPackagePath() const -> const std::string& { return PackagePath; }
		auto GetAsset() const -> DObject* { return Asset.Get(); }
		auto IsDirty() const -> bool { return bDirty; }
		auto GetPackageFlags() const -> EPackageFlags { return PackageFlags; }
		auto IsAssetPackage() const -> bool { return EnumHasAnyFlags(PackageFlags, EPackageFlags::Asset); }
		auto IsCppPackage() const -> bool { return EnumHasAnyFlags(PackageFlags, EPackageFlags::Cpp); }

		auto InitializeAssetPackage(const FAssetPath& InPath) -> void;
		auto RelocateAssetPackage(const FAssetPath& InPath) -> bool;
		auto InitializeCppPackage(FName ModuleName) -> void;
		auto SetAsset(DObject* InAsset) -> bool;
		auto MarkDirty() -> void { if (IsAssetPackage()) bDirty = true; }
		auto ClearDirty() -> void { bDirty = false; }

	private:
		DPROPERTY()
		std::string PackagePath;

		DPROPERTY()
		TObjectPtr<DObject> Asset;

		EPackageFlags PackageFlags = EPackageFlags::None;

		DPROPERTY(Transient)
		bool bDirty = false;
	};

	COREDOBJECT_API auto FindPackage(std::string_view PackagePath) -> DPackage*;
	COREDOBJECT_API auto FindOrCreateCppPackage(FName ModuleName) -> DPackage*;
	COREDOBJECT_API auto RegisterCompiledInPackage(const char* ModuleName) -> void;
	COREDOBJECT_API auto ProcessRegisteredCppPackages() -> void;
	COREDOBJECT_API auto AttachCoreIntrinsicTypesToCppPackage() -> void;
}
