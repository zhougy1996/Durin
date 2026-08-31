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
		// The live asset package has not completed its first persistent save.
		NewlyCreated = 1 << 2,
	};
	ENUM_CLASS_FLAGS(EPackageFlags);

	// Owns every direct asset export or a module's reflected metadata under one registered path.
	DCLASS(NoClassDefaultObject)
	class DPackage : public DObject
	{
		GENERATED_BODY()
	public:
		COREDOBJECT_API explicit DPackage(const FObjectInitializer& ObjectInitializer);
		COREDOBJECT_API ~DPackage() override;

		// Temporary string spelling shared with compiled-in packages during migration.
		auto GetPackagePath() const -> const std::string& { return RegisteredPath; }
		auto GetPackagePathIdentity() const -> const FPackagePath& { return PackagePath; }
		auto GetTopLevelAssets() const -> std::span<DObject* const> { return TopLevelAssets; }
		COREDOBJECT_API auto FindTopLevelAsset(FName Name) const -> DObject*;
		// Temporary v8 adapter. New code must select a named top-level asset.
		auto GetAsset() const -> DObject* { return LegacyMainAsset; }
		auto IsDirty() const -> bool { return bDirty; }
		auto IsCanonicalResaveRecommended() const -> bool { return bCanonicalResaveRecommended; }
		auto GetEditRevision() const -> uint64 { return EditRevision; }
		auto GetPackageFlags() const -> EPackageFlags { return PackageFlags; }
		auto IsAssetPackage() const -> bool { return EnumHasAnyFlags(PackageFlags, EPackageFlags::Asset); }
		auto IsCppPackage() const -> bool { return EnumHasAnyFlags(PackageFlags, EPackageFlags::Cpp); }
		auto IsNewlyCreated() const -> bool
		{
			return IsAssetPackage()
				&& EnumHasAnyFlags(PackageFlags, EPackageFlags::NewlyCreated);
		}

		// Initialization is one-shot and requires an unparented package with no existing kind.
		COREDOBJECT_API auto InitializeAssetPackage(const FPackagePath& InPath) -> void;
		COREDOBJECT_API auto RelocateAssetPackage(const FPackagePath& InPath) -> bool;
		COREDOBJECT_API auto InitializeCppPackage(FName ModuleName) -> void;
		// Controls ordinary-GC residency for an asset package. Unload attempts clear
		// this temporarily and restore it when another strong reference keeps the
		// package graph reachable.
		COREDOBJECT_API auto SetStandaloneResidency(bool bResident) -> void;
		COREDOBJECT_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;

		// Asset packages accept only an asset whose Outer is this package.
		COREDOBJECT_API auto SetAsset(DObject* InAsset) -> bool;
		auto MarkAsNewlyCreated() -> void
		{
			if (IsAssetPackage()) PackageFlags |= EPackageFlags::NewlyCreated;
		}
		auto MarkAsPublished() -> void
		{
			PackageFlags &= ~EPackageFlags::NewlyCreated;
		}
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
		// Mounted package identity; invalid only for compiled-in metadata packages.
		FPackagePath PackagePath;

		// Global registry key, including the separate /Cpp/<Module> namespace.
		DPROPERTY()
		std::string RegisteredPath;

		// Direct persistent exports are retained as one package residency closure.
		std::vector<DObject*> TopLevelAssets;

		// Non-owning v8 compatibility selector removed with the production v8 route.
		DObject* LegacyMainAsset = nullptr;

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

		COREDOBJECT_API auto RegisterTopLevelAsset(DObject* Asset) -> bool;
		COREDOBJECT_API auto UnregisterTopLevelAsset(DObject* Asset) -> void;
		COREDOBJECT_API auto CanUseTopLevelAssetName(FName Name, const DObject* Ignore = nullptr) const -> bool;

		friend class DObject;
	};

	// Creates a standalone asset package. Invalid paths and paths already owned
	// by another live package are rejected.
	COREDOBJECT_API auto CreatePackage(const FPackagePath& Path) -> DPackage*;
	COREDOBJECT_API auto FindPackage(std::string_view PackagePath) -> DPackage*;
	COREDOBJECT_API auto FindOrCreateCppPackage(FName ModuleName) -> DPackage*;
	COREDOBJECT_API auto RegisterCompiledInPackage(const char* ModuleName) -> void;
	COREDOBJECT_API auto ProcessRegisteredCppPackages() -> void;
	COREDOBJECT_API auto AttachCoreIntrinsicTypesToCppPackage() -> void;
}
