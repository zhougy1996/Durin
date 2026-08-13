#pragma once

#include "CoreDObjectAPI.h"
#include "DObject/AssetPath.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"

#include "Package.gen.h"

namespace Durin
{
	// Distinguishes persistent asset packages from permanent compiled-in metadata packages.
	enum class EPackageFlags : uint8
	{
		None = 0,
		Asset = 1 << 0,
		Cpp = 1 << 1,
	};
	ENUM_CLASS_FLAGS(EPackageFlags);

	// Owns an asset or a module's reflected metadata under one globally registered path.
	DCLASS(NoClassDefaultObject)
	class DPackage : public DObject
	{
		GENERATED_BODY()
	public:
		COREDOBJECT_API explicit DPackage(const FObjectInitializer& ObjectInitializer);
		COREDOBJECT_API ~DPackage() override;

		auto GetPackagePath() const -> const std::string& { return PackagePath; }
		auto GetAsset() const -> DObject* { return Asset.Get(); }
		auto IsDirty() const -> bool { return bDirty; }
		auto IsCanonicalResaveRecommended() const -> bool { return bCanonicalResaveRecommended; }
		auto GetEditRevision() const -> uint64 { return EditRevision; }
		auto GetPackageFlags() const -> EPackageFlags { return PackageFlags; }
		auto IsAssetPackage() const -> bool { return EnumHasAnyFlags(PackageFlags, EPackageFlags::Asset); }
		auto IsCppPackage() const -> bool { return EnumHasAnyFlags(PackageFlags, EPackageFlags::Cpp); }

		// Initialization is one-shot and requires an unparented package with no existing kind.
		COREDOBJECT_API auto InitializeAssetPackage(const FAssetPath& InPath) -> void;
		COREDOBJECT_API auto RelocateAssetPackage(const FAssetPath& InPath) -> bool;
		COREDOBJECT_API auto InitializeCppPackage(FName ModuleName) -> void;

		// Asset packages accept only an asset whose Outer is this package.
		COREDOBJECT_API auto SetAsset(DObject* InAsset) -> bool;
		auto MarkDirty() -> void
		{
			if (!IsAssetPackage()) return;
			bDirty = true;
			++EditRevision;
		}
		auto ClearDirty() -> void { bDirty = false; }
		auto SetCanonicalResaveRecommended(bool bRecommended) -> void
		{
			bCanonicalResaveRecommended = IsAssetPackage() && bRecommended;
		}

	private:
		// Global registry key, using an asset path or the /Cpp/<Module> namespace.
		DPROPERTY()
		std::string PackagePath;

		// Main persistent asset; structural children remain reachable through Outer relationships.
		DPROPERTY()
		TObjectPtr<DObject> Asset;

		EPackageFlags PackageFlags = EPackageFlags::None;

		// Tracks unsaved asset-package changes; compiled-in packages never become dirty.
		DPROPERTY(Transient)
		bool bDirty = false;

		// Maintenance state is deliberately independent from authored Dirty state.
		DPROPERTY(Transient)
		bool bCanonicalResaveRecommended = false;

		// Monotonic process-local token for optimistic editor plans. Unlike dirty
		// state, repeated edits remain distinguishable before the next save.
		uint64 EditRevision = 1;
	};

	COREDOBJECT_API auto FindPackage(std::string_view PackagePath) -> DPackage*;
	COREDOBJECT_API auto FindOrCreateCppPackage(FName ModuleName) -> DPackage*;
	COREDOBJECT_API auto RegisterCompiledInPackage(const char* ModuleName) -> void;
	COREDOBJECT_API auto ProcessRegisteredCppPackages() -> void;
	COREDOBJECT_API auto AttachCoreIntrinsicTypesToCppPackage() -> void;
}
