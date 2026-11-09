#include "DObject/SoftObjectPtr.h"
#include "DObject/SoftObjectPathServices.h"

#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "Threading/RunnableThread.h"

#include "DObject/Class.h"
#include "DObject/Package.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GSoftObjectCacheEpoch = 1;
		Private::FSoftObjectPathOperations GSoftObjectPathOperations;
		auto IsSoftObjectTarget(DObject* Object, const DClass* ExpectedClass) -> bool
		{
			if (!Durin::IsValid(Object) || Cast<DPackage>(Object)
				|| EnumHasAnyFlags(Object->GetObjectFlags(), EObjectFlags::Transient)
				|| (ExpectedClass && !Object->IsA(ExpectedClass))) return false;
			const DPackage* Package = Object->GetPackage();
			return Package && Package->IsAssetPackage();
		}
		auto FailSoftObject(ESoftObjectError Code, std::string Subject = {},
			std::string Expected = {}, std::string Actual = {}) -> std::expected<void, FSoftObjectError>
		{
			FSoftObjectError Error;
			Error.Code = Code;
			Error.Subject = std::move(Subject);
			Error.Expected = std::move(Expected);
			Error.Actual = std::move(Actual);
			return std::unexpected(std::move(Error));
		}
		auto ValidateSoftObject(DObject* Object, const DClass* ExpectedClass, FObjectPath& OutPath) -> std::expected<void, FSoftObjectError>
		{
			check(Object);
			if (Cast<DPackage>(Object)) return FailSoftObject(ESoftObjectError::PackageObject, Object->GetObjectPath());
			if (EnumHasAnyFlags(Object->GetObjectFlags(), EObjectFlags::Transient)) return FailSoftObject(ESoftObjectError::TransientObject, Object->GetObjectPath());
			if (ExpectedClass && !Object->IsA(ExpectedClass)) return FailSoftObject(ESoftObjectError::ClassMismatch, Object->GetObjectPath(), ExpectedClass->GetQualifiedName().ToString(), Object->GetClass()->GetQualifiedName().ToString());
			DPackage* Package = Object->GetPackage();
			if (!Package || !Package->IsAssetPackage()) return FailSoftObject(ESoftObjectError::UnpackagedObject, Object->GetObjectPath());
			if (auto Path = FObjectPath::TryCreateWithDiagnostic(Object->GetObjectPath(), OutPath); !Path)
				return std::unexpected(FSoftObjectError{.Code = ESoftObjectError::InvalidObjectPath,
					.Subject = Object->GetObjectPath(), .PathCause = std::move(Path.error())});
			return {};
		}
	}

	namespace Private
	{
		auto SetSoftObjectPathOperations(FSoftObjectPathOperations Operations) -> void
		{
			GSoftObjectPathOperations = Operations;
			InvalidateSoftObjectCaches();
		}

		auto InvalidateSoftObjectCaches() -> void
		{
			if (GSoftObjectCacheEpoch.fetch_add(1, std::memory_order_acq_rel) == std::numeric_limits<uint64>::max())
				GSoftObjectCacheEpoch.store(1, std::memory_order_release);
		}
	}

	FSoftObjectPtr::FSoftObjectPtr(FSoftObjectPtr&& Other) noexcept
		: AuthoredPath(std::move(Other.AuthoredPath)), WeakObject(Other.WeakObject), CacheEpoch(Other.CacheEpoch)
	{
		Other.Reset();
	}

	auto FSoftObjectPtr::operator=(FSoftObjectPtr&& Other) noexcept -> FSoftObjectPtr&
	{
		if (this != &Other)
		{
			AuthoredPath = std::move(Other.AuthoredPath);
			WeakObject = Other.WeakObject;
			CacheEpoch = Other.CacheEpoch;
			Other.Reset();
		}
		return *this;
	}

	auto FSoftObjectPtr::TrySetObject(DObject* InObject, const DClass* ExpectedClass) -> std::expected<void, FSoftObjectError>
	{
		if (!InObject) { Reset(); return {}; }
		FObjectPath ObjectPath;
		if (auto Result = ValidateSoftObject(InObject, ExpectedClass, ObjectPath); !Result) return Result;
		AuthoredPath = std::move(ObjectPath);
		WeakObject.SetObject(InObject);
		CacheEpoch = GSoftObjectCacheEpoch.load(std::memory_order_acquire);
		return {};
	}

	auto FSoftObjectPtr::Get(const DClass* ExpectedClass) const -> DObject*
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (IsNull()) return nullptr;
		DObject* Object = CacheEpoch == GSoftObjectCacheEpoch.load(std::memory_order_acquire) ? WeakObject.Get() : nullptr;
		if (GSoftObjectPathOperations.Resolve)
		{
			Object = GSoftObjectPathOperations.Resolve(AuthoredPath, ExpectedClass, Object);
		}
		else if (!Object)
		{
			DPackage* Package = FindPackage(AuthoredPath.GetPackagePath().GetView());
			Object = Package ? Package->FindTopLevelAsset(FName(AuthoredPath.GetAssetPath().GetAssetName())) : nullptr;
			for (const std::string_view Name : AuthoredPath.GetSubobjectNames())
			{
				if (!Object) break;
				const auto Children = GDObjectArray.GetObjectsWithOuter(Object, EObjectQueryScope::LiveOnly);
				const auto Child = std::ranges::find(Children, FName(Name), &DObject::GetFName);
				Object = Child == Children.end() ? nullptr : *Child;
			}
		}
		if (!IsSoftObjectTarget(Object, ExpectedClass))
		{
			ResetWeakPtr();
			return nullptr;
		}
		WeakObject.SetObject(Object);
		CacheEpoch = GSoftObjectCacheEpoch.load(std::memory_order_acquire);
		return Object;
	}

	auto FSoftObjectPtr::LoadSynchronous(const DClass* ExpectedClass) const -> DObject*
	{
		if (DObject* Object = Get(ExpectedClass)) return Object;
		if (IsNull() || !GSoftObjectPathOperations.Load) return nullptr;
		DObject* Object = GSoftObjectPathOperations.Load(AuthoredPath, ExpectedClass);
		if (!IsSoftObjectTarget(Object, ExpectedClass))
		{
			ResetWeakPtr();
			return nullptr;
		}
		WeakObject.SetObject(Object);
		CacheEpoch = GSoftObjectCacheEpoch.load(std::memory_order_acquire);
		return Object;
	}
	auto ToString(const FSoftObjectError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case ESoftObjectError::InvalidObjectPath: return Error.PathCause ? ToString(*Error.PathCause) : "The soft object has an invalid path.";
		case ESoftObjectError::PackageObject: return "A package cannot be assigned as a soft object.";
		case ESoftObjectError::TransientObject: return "A transient object cannot be assigned as a soft object.";
		case ESoftObjectError::ClassMismatch: return std::format("Object {} is not a {} (actual class {}).", Error.Subject, Error.Expected, Error.Actual);
		case ESoftObjectError::UnpackagedObject: return "An unpackaged object cannot be assigned as a soft object.";
		}
		return "Unknown object error.";
	}

}
