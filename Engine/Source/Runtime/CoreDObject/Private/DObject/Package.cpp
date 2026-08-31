#include "DObject/Package.h"

#include "DObject/DObjectGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/SoftObjectPtr.h"
#include "Misc/StringHelper.h"

namespace Durin
{
	struct FPackageRegistryHash
	{
		auto operator()(std::string_view Value) const noexcept -> size_t
		{
			size_t Hash = 1469598103934665603ull;
			for (const char Character : Value)
				Hash = (Hash ^ static_cast<uint8>(StringUtils::ToLowerAscii(Character)))
					* 1099511628211ull;
			return Hash;
		}
	};

	struct FPackageRegistryEqual
	{
		auto operator()(std::string_view Left, std::string_view Right) const noexcept -> bool
		{
			if (Left.size() != Right.size()) return false;
			for (size_t Index = 0; Index < Left.size(); ++Index)
				if (StringUtils::ToLowerAscii(Left[Index])
					!= StringUtils::ToLowerAscii(Right[Index])) return false;
			return true;
		}
	};

	using FPackageRegistry = std::unordered_map<
		std::string, DPackage*, FPackageRegistryHash, FPackageRegistryEqual>;

	static auto GetPackageRegistry() -> FPackageRegistry&
	{
		static FPackageRegistry Registry;
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
		if (!RegisteredPath.empty())
		{
			auto It = GetPackageRegistry().find(RegisteredPath);
			if (It != GetPackageRegistry().end() && It->second == this) GetPackageRegistry().erase(It);
		}
	}

	auto DPackage::InitializeAssetPackage(const FPackagePath& InPath) -> void
	{
		check(GetOuter() == nullptr);
		check(PackageFlags == EPackageFlags::None);
		PackagePath = InPath;
		RegisteredPath = InPath.ToString();
		PackageFlags = EPackageFlags::Asset;
		RegisterPackage(this, RegisteredPath);
	}

	auto DPackage::RelocateAssetPackage(const FPackagePath& InPath) -> bool
	{
		if (!IsAssetPackage()) return false;
		const std::string NewPath = InPath.ToString();
		if (NewPath == RegisteredPath) return true;
		auto& Registry = GetPackageRegistry();
		if (auto Existing = Registry.find(NewPath); Existing != Registry.end() && Existing->second != this) return false;
		auto Old = Registry.find(RegisteredPath);
		if (Old == Registry.end() || Old->second != this) return false;
		Registry.erase(Old);
		PackagePath = InPath;
		RegisteredPath = NewPath;
		Registry.emplace(RegisteredPath, this);
		MarkDirty();
		InvalidateSoftObjectCaches();
		return true;
	}

	auto DPackage::InitializeCppPackage(FName ModuleName) -> void
	{
		check(GetOuter() == nullptr);
		check(PackageFlags == EPackageFlags::None);
		check(!ModuleName.IsNone());
		RegisteredPath = "/Cpp/" + ModuleName.ToString();
		PackageFlags = EPackageFlags::Cpp;
		RegisterPackage(this, RegisteredPath);
	}

	auto DPackage::SetStandaloneResidency(bool bResident) -> void
	{
		check(IsAssetPackage());
		if (bResident) ObjectFlags |= EObjectFlags::Standalone;
		else ObjectFlags &= ~EObjectFlags::Standalone;
	}

	auto DPackage::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		Super::AddReferencedObjects(Collector);
		for (DObject*& Asset : TopLevelAssets) Collector.AddReferencedObject(Asset);
	}

	auto DPackage::FindTopLevelAsset(FName Name) const -> DObject*
	{
		const auto It = std::ranges::find(TopLevelAssets, Name, &DObject::GetFName);
		return It == TopLevelAssets.end() ? nullptr : *It;
	}

	auto DPackage::CanUseTopLevelAssetName(FName Name, const DObject* Ignore) const -> bool
	{
		return !Name.IsNone() && std::ranges::none_of(TopLevelAssets, [&](const DObject* Existing) {
			return Existing != Ignore && !Existing->IsGarbage() && Existing->GetFName() == Name;
		});
	}

	auto DPackage::RegisterTopLevelAsset(DObject* InAsset) -> bool
	{
		if (!IsAssetPackage() || !InAsset || InAsset->GetOuter() != this
			|| InAsset->IsTemplateObject()
			|| EnumHasAnyFlags(InAsset->GetObjectFlags(), EObjectFlags::Transient)) return false;
		if (std::ranges::find(TopLevelAssets, InAsset) != TopLevelAssets.end()) return true;
		if (!CanUseTopLevelAssetName(InAsset->GetFName())) return false;
		TopLevelAssets.push_back(InAsset);
		std::ranges::sort(TopLevelAssets, [](const DObject* Left, const DObject* Right) {
			return Left->GetName() < Right->GetName();
		});
		return true;
	}

	auto DPackage::UnregisterTopLevelAsset(DObject* InAsset) -> void
	{
		std::erase(TopLevelAssets, InAsset);
	}

	auto CreatePackage(const FPackagePath& Path) -> DPackage*
	{
		if (!Path.IsValid() || FindPackage(Path.GetView())) return nullptr;

		DPackage* Package = NewObject<DPackage>(
			nullptr, FName(Path.GetAssetName()), EObjectFlags::Standalone);
		Package->InitializeAssetPackage(Path);
		return Package;
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
		const std::pair<DClass*, FName> IntrinsicClasses[] = {
			{DObject::StaticClass(), FName("Durin::DObject")},
			{DType::StaticClass(), FName("Durin::DType")},
			{DStructBase::StaticClass(), FName("Durin::DStructBase")},
			{DClass::StaticClass(), FName("Durin::DClass")},
			{DStruct::StaticClass(), FName("Durin::DStruct")},
			{DEnum::StaticClass(), FName("Durin::DEnum")}
		};
		for (const auto& [Class, QualifiedName] : IntrinsicClasses)
		{
			DObjectForceRegistration(Class);
			Class->SetQualifiedName(QualifiedName);
		}
		for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates))
		{
			if (Cast<DType>(Object) && Object->GetOuter() == nullptr && EnumHasAnyFlags(Object->GetObjectFlags(), EObjectFlags::Intrinsic))
			{
				Object->SetOuterPrivate(CorePackage);
			}
		}
	}
}
