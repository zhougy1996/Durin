#include "DObject/Package.h"

#include "DObject/DObjectGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin
{
	static auto GetPackageRegistry() -> std::unordered_map<std::string, DPackage*>&
	{
		static std::unordered_map<std::string, DPackage*> Registry;
		return Registry;
	}

	static auto GetPendingCppPackages() -> std::vector<std::string>&
	{
		static std::vector<std::string> Packages;
		return Packages;
	}

	static auto RegisterPackage(DPackage* Package, const std::string& Path) -> void
	{
		auto [It, bInserted] = GetPackageRegistry().emplace(Path, Package);
		check((bInserted || It->second == Package) && "A different package is already registered for this path.");
	}

	DPackage::DPackage(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	DPackage::~DPackage()
	{
		if (!PackagePath.empty())
		{
			auto It = GetPackageRegistry().find(PackagePath);
			if (It != GetPackageRegistry().end() && It->second == this) GetPackageRegistry().erase(It);
		}
	}

	auto DPackage::InitializeAssetPackage(const FAssetPath& InPath) -> void
	{
		check(GetOuter() == nullptr);
		check(PackageFlags == EPackageFlags::None);
		PackagePath = InPath.ToString();
		PackageFlags = EPackageFlags::Asset;
		RegisterPackage(this, PackagePath);
	}

	auto DPackage::InitializeCppPackage(FName ModuleName) -> void
	{
		check(GetOuter() == nullptr);
		check(PackageFlags == EPackageFlags::None);
		check(!ModuleName.IsNone());
		PackagePath = "/Cpp/" + ModuleName.ToString();
		PackageFlags = EPackageFlags::Cpp;
		RegisterPackage(this, PackagePath);
	}

	auto DPackage::SetAsset(DObject* InAsset) -> bool
	{
		if (!IsAssetPackage()) return false;
		if (InAsset && InAsset->GetOuter() != this) return false;
		Asset = InAsset;
		bDirty = true;
		return true;
	}

	auto FindPackage(std::string_view PackagePath) -> DPackage*
	{
		auto It = GetPackageRegistry().find(std::string(PackagePath));
		return It == GetPackageRegistry().end() ? nullptr : It->second;
	}

	auto FindOrCreateCppPackage(FName ModuleName) -> DPackage*
	{
		const std::string Path = "/Cpp/" + ModuleName.ToString();
		if (DPackage* Existing = FindPackage(Path))
		{
			check(Existing->IsCppPackage() && "Package path is already used by a non-C++ package.");
			return Existing;
		}

		DPackage* Package = NewObject<DPackage>(nullptr, ModuleName);
		Package->InitializeCppPackage(ModuleName);
		AddToRoot(Package);
		return Package;
	}

	auto RegisterCompiledInPackage(const char* ModuleName) -> void
	{
		check(ModuleName && ModuleName[0] != '\0');
		for (const std::string& Existing : GetPendingCppPackages()) if (Existing == ModuleName) return;
		GetPendingCppPackages().emplace_back(ModuleName);
	}

	auto ProcessRegisteredCppPackages() -> void
	{
		for (const std::string& ModuleName : GetPendingCppPackages()) FindOrCreateCppPackage(FName(ModuleName));
		GetPendingCppPackages().clear();
	}

	auto AttachCoreIntrinsicTypesToCppPackage() -> void
	{
		DPackage* CorePackage = FindOrCreateCppPackage("CoreDObject");
		DClass* IntrinsicClasses[] = {
			DObject::StaticClass(), DType::StaticClass(), DStructBase::StaticClass(),
			DClass::StaticClass(), DStruct::StaticClass(), DEnum::StaticClass()
		};
		for (DClass* Class : IntrinsicClasses) DObjectForceRegistration(Class);
		for (DObject* Object : GDObjectArray.GetAll())
		{
			if (Cast<DType>(Object) && Object->GetOuter() == nullptr && EnumHasAnyFlags(Object->GetObjectFlags(), EObjectFlags::Intrinsic))
			{
				Object->SetOuterPrivate(CorePackage);
			}
		}
	}
}
