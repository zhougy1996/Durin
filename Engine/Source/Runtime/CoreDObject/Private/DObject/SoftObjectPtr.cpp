#include "DObject/SoftObjectPtr.h"

#include "DObject/Class.h"
#include "DObject/Package.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GSoftObjectCacheEpoch = 1;
		auto FailSoftObject(ESoftObjectError Code, std::string Subject = {},
			std::string Expected = {}, std::string Actual = {}) -> FObjectOperationResult
		{
			FObjectError Error;
			Error.Code = Code;
			Error.Subject = std::move(Subject);
			Error.Expected = std::move(Expected);
			Error.Actual = std::move(Actual);
			return {std::move(Error)};
		}
		auto ValidateSoftObject(DObject* Object, const DClass* ExpectedClass, FObjectPath& OutPath) -> FObjectOperationResult
		{
			if (!Object) return FailSoftObject(ESoftObjectError::NullLoadedObject);
			if (Cast<DPackage>(Object)) return FailSoftObject(ESoftObjectError::PackageObject, Object->GetObjectPath());
			if (EnumHasAnyFlags(Object->GetObjectFlags(), EObjectFlags::Transient)) return FailSoftObject(ESoftObjectError::TransientObject, Object->GetObjectPath());
			if (ExpectedClass && !Object->IsA(ExpectedClass)) return FailSoftObject(ESoftObjectError::ClassMismatch, Object->GetObjectPath(), ExpectedClass->GetQualifiedName().ToString(), Object->GetClass()->GetQualifiedName().ToString());
			DPackage* Package = Object->GetPackage();
			if (!Package || !Package->IsAssetPackage()) return FailSoftObject(ESoftObjectError::UnpackagedObject, Object->GetObjectPath());
			return FObjectPath::TryCreateWithDiagnostic(Object->GetObjectPath(), OutPath);
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
	auto FSoftObjectPtr::TrySetObject(DObject* InObject, const DClass* ExpectedClass) -> FObjectOperationResult
	{
		if (!InObject) { Reset(); return {}; }
		FObjectPath ObjectPath; if (auto Result = ValidateSoftObject(InObject, ExpectedClass, ObjectPath); !Result) return Result;
		AuthoredPath = ObjectPath; WeakObject.SetObject(InObject); CacheEpoch = GetSoftObjectCacheEpoch(); return {};
	}
	auto FSoftObjectPtr::TrySetLoadedObject(DObject* InObject, const DClass* ExpectedClass) -> FObjectOperationResult
	{
		FObjectPath ObjectPath; if (auto Result = ValidateSoftObject(InObject, ExpectedClass, ObjectPath); !Result) return Result;
		if (!AuthoredPath.IsValid() || AuthoredPath != ObjectPath) return FailSoftObject(ESoftObjectError::LoadedPathMismatch, ObjectPath.ToString(), AuthoredPath.ToString(), ObjectPath.ToString());
		WeakObject.SetObject(InObject); CacheEpoch = GetSoftObjectCacheEpoch(); return {};
	}
	auto FSoftObjectPtr::TrySetResolvedObject(DObject* InObject, const FObjectPath& InAuthoredPath, const FObjectPath& ResolvedPath, const DClass* ExpectedClass) -> FObjectOperationResult
	{
		FObjectPath ObjectPath; if (auto Result = ValidateSoftObject(InObject, ExpectedClass, ObjectPath); !Result) return Result;
		if (!AuthoredPath.IsValid() || AuthoredPath != InAuthoredPath) return FailSoftObject(ESoftObjectError::AuthoredPathMismatch, ObjectPath.ToString(), AuthoredPath.ToString(), InAuthoredPath.ToString());
		if (!ResolvedPath.IsValid() || ObjectPath != ResolvedPath) return FailSoftObject(ESoftObjectError::ResolvedPathMismatch, ObjectPath.ToString(), ResolvedPath.ToString(), ObjectPath.ToString());
		WeakObject.SetObject(InObject); CacheEpoch = GetSoftObjectCacheEpoch(); return {};
	}
	auto FSoftObjectPtr::Get(const DClass* ExpectedClass) const -> DObject*
	{
		DObject* Object = WeakObject.Get();
		if (!AuthoredPath.IsValid() || CacheEpoch == 0 || CacheEpoch != GetSoftObjectCacheEpoch()) return nullptr;
		if (!Object) return nullptr;
		FObjectPath ObjectPath; return ValidateSoftObject(Object, ExpectedClass, ObjectPath) && ObjectPath.IsValid() ? Object : nullptr;
	}
	auto FSoftObjectPtr::GetState(const DClass* ExpectedClass) const -> ESoftObjectPtrState
	{
		if (!AuthoredPath.IsValid()) return ESoftObjectPtrState::Null;
		if (CacheEpoch == 0) return ESoftObjectPtrState::Pending;
		return Get(ExpectedClass) ? ESoftObjectPtrState::Valid : ESoftObjectPtrState::Stale;
	}
}
