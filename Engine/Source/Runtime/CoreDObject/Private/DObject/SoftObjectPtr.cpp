#include "DObject/SoftObjectPtr.h"

#include "DObject/Class.h"
#include "DObject/Package.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GSoftObjectCacheEpoch = 1;
		auto FailSoftObject(std::string Message, std::string* OutError) -> bool { if (OutError) *OutError = std::move(Message); return false; }
		auto ValidateSoftObject(DObject* Object, const DClass* ExpectedClass, FObjectPath& OutPath, std::string* OutError) -> bool
		{
			if (!Object) return FailSoftObject("A loaded soft-object cache cannot be null.", OutError);
			if (Cast<DPackage>(Object)) return FailSoftObject("A package cannot be assigned as a soft object.", OutError);
			if (EnumHasAnyFlags(Object->GetObjectFlags(), EObjectFlags::Transient)) return FailSoftObject("A transient object cannot be assigned as a soft object.", OutError);
			if (ExpectedClass && !Object->IsA(ExpectedClass)) return FailSoftObject(std::format("Object {} is not a {}.", Object->GetObjectPath(), ExpectedClass->GetQualifiedName().ToString()), OutError);
			DPackage* Package = Object->GetPackage();
			if (!Package || !Package->IsAssetPackage()) return FailSoftObject("An unpackaged object cannot be assigned as a soft object.", OutError);
			return FObjectPath::TryCreate(Object->GetObjectPath(), OutPath, OutError);
		}
	}

	auto GetSoftObjectCacheEpoch() -> uint64 { return GSoftObjectCacheEpoch.load(std::memory_order_acquire); }
	auto InvalidateSoftObjectCaches() -> void
	{
		if (GSoftObjectCacheEpoch.fetch_add(1, std::memory_order_acq_rel) == std::numeric_limits<uint64>::max()) GSoftObjectCacheEpoch.store(1, std::memory_order_release);
	}

	FSoftObjectPtr::FSoftObjectPtr(FSoftObjectPtr&& Other) noexcept
		: AuthoredPath(std::move(Other.AuthoredPath)), WeakObject(Other.WeakObject), CacheEpoch(Other.CacheEpoch) { Other.Reset(); }
	auto FSoftObjectPtr::operator=(FSoftObjectPtr&& Other) noexcept -> FSoftObjectPtr&
	{
		if (this != &Other) { AuthoredPath = std::move(Other.AuthoredPath); WeakObject = Other.WeakObject; CacheEpoch = Other.CacheEpoch; Other.Reset(); } return *this;
	}
	auto FSoftObjectPtr::SetPath(FObjectPath InPath) -> void { AuthoredPath = std::move(InPath); ResetCache(); }
	auto FSoftObjectPtr::TrySetObject(DObject* InObject, const DClass* ExpectedClass, std::string* OutError) -> bool
	{
		if (!InObject) { Reset(); return true; }
		FObjectPath ObjectPath; if (!ValidateSoftObject(InObject, ExpectedClass, ObjectPath, OutError)) return false;
		AuthoredPath = ObjectPath; WeakObject.SetObject(InObject); CacheEpoch = GetSoftObjectCacheEpoch(); return true;
	}
	auto FSoftObjectPtr::TrySetLoadedObject(DObject* InObject, const DClass* ExpectedClass, std::string* OutError) -> bool
	{
		FObjectPath ObjectPath; if (!ValidateSoftObject(InObject, ExpectedClass, ObjectPath, OutError)) return false;
		if (!AuthoredPath.IsValid() || AuthoredPath != ObjectPath) return FailSoftObject("The loaded object does not match the stored soft-object path.", OutError);
		WeakObject.SetObject(InObject); CacheEpoch = GetSoftObjectCacheEpoch(); return true;
	}
	auto FSoftObjectPtr::TrySetResolvedObject(DObject* InObject, const FObjectPath& InAuthoredPath, const FObjectPath& ResolvedPath, const DClass* ExpectedClass, std::string* OutError) -> bool
	{
		FObjectPath ObjectPath; if (!ValidateSoftObject(InObject, ExpectedClass, ObjectPath, OutError)) return false;
		if (!AuthoredPath.IsValid() || AuthoredPath != InAuthoredPath) return FailSoftObject("The resolved object does not match the authored soft-object path.", OutError);
		if (!ResolvedPath.IsValid() || ObjectPath != ResolvedPath) return FailSoftObject("The resolved object does not match the exact resolved object path.", OutError);
		WeakObject.SetObject(InObject); CacheEpoch = GetSoftObjectCacheEpoch(); return true;
	}
	auto FSoftObjectPtr::Get(const DClass* ExpectedClass) const -> DObject*
	{
		DObject* Object = WeakObject.Get();
		if (!AuthoredPath.IsValid() || CacheEpoch == 0 || CacheEpoch != GetSoftObjectCacheEpoch()) return nullptr;
		if (!Object) return nullptr;
		FObjectPath ObjectPath; return ValidateSoftObject(Object, ExpectedClass, ObjectPath, nullptr) && ObjectPath.IsValid() ? Object : nullptr;
	}
	auto FSoftObjectPtr::GetState(const DClass* ExpectedClass) const -> ESoftObjectPtrState
	{
		if (!AuthoredPath.IsValid()) return ESoftObjectPtrState::Null;
		if (CacheEpoch == 0) return ESoftObjectPtrState::Pending;
		return Get(ExpectedClass) ? ESoftObjectPtrState::Valid : ESoftObjectPtrState::Stale;
	}
}
