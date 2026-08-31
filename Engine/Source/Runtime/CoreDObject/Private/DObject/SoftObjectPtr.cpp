#include "DObject/SoftObjectPtr.h"

#include "DObject/Class.h"
#include "DObject/Package.h"

namespace Durin
{
	namespace
	{
		auto FailSoftObject(std::string Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto ValidateSoftObject(
			DObject* Object,
			const DClass* ExpectedClass,
			FObjectPath& OutPath,
			std::string* OutError) -> bool
		{
			if (!Object) return FailSoftObject("A loaded soft-object cache cannot be null.", OutError);
			if (Cast<DPackage>(Object)) return FailSoftObject("A package cannot be assigned as a soft object.", OutError);
			if (EnumHasAnyFlags(Object->GetObjectFlags(), EObjectFlags::Transient))
				return FailSoftObject("A transient object cannot be assigned as a soft object.", OutError);
			if (ExpectedClass && !Object->IsA(ExpectedClass))
				return FailSoftObject(std::format(
					"Object {} is not a {}.",
					Object->GetObjectPath(),
					ExpectedClass->GetQualifiedName().ToString()), OutError);

			DPackage* Package = Object->GetPackage();
			if (!Package || !Package->IsAssetPackage())
				return FailSoftObject("An unpackaged object cannot be assigned as a soft object.", OutError);
			if (!FObjectPath::TryCreate(Object->GetObjectPath(), OutPath, OutError)) return false;
			return true;
		}
	}

	FSoftObjectPath::FSoftObjectPath(FPackagePath InPath)
	{
		FTopLevelAssetPath AssetPath;
		if (!FTopLevelAssetPath::TryCreate(
			InPath, InPath.GetPackageName(), AssetPath)) return;
		(void)FObjectPath::TryCreate(
			AssetPath, std::span<const std::string>{}, ObjectPath);
	}

	auto FSoftObjectPath::TryCreate(
		std::string_view InPath,
		FSoftObjectPath& OutPath,
		std::string* OutError) -> bool
	{
		if (InPath.empty())
		{
			OutPath = {};
			return true;
		}
		FObjectPath Path;
		if (!FObjectPath::TryCreate(InPath, Path, OutError)) return false;
		OutPath = FSoftObjectPath(std::move(Path));
		return true;
	}

	FSoftObjectPtr::FSoftObjectPtr(FSoftObjectPtr&& Other) noexcept
		: SoftObjectPath(std::move(Other.SoftObjectPath))
		, ResolvedObjectPath(std::move(Other.ResolvedObjectPath))
		, WeakObject(Other.WeakObject)
	{
		Other.Reset();
	}

	auto FSoftObjectPtr::operator=(FSoftObjectPtr&& Other) noexcept -> FSoftObjectPtr&
	{
		if (this == &Other) return *this;
		SoftObjectPath = std::move(Other.SoftObjectPath);
		ResolvedObjectPath = std::move(Other.ResolvedObjectPath);
		WeakObject = Other.WeakObject;
		Other.Reset();
		return *this;
	}

	auto FSoftObjectPtr::SetPath(FSoftObjectPath InPath) -> void
	{
		SoftObjectPath = std::move(InPath);
		ResolvedObjectPath = {};
		WeakObject.Reset();
	}

	auto FSoftObjectPtr::TrySetObject(
		DObject* InObject,
		const DClass* ExpectedClass,
		std::string* OutError) -> bool
	{
		if (!InObject)
		{
			Reset();
			return true;
		}

		FObjectPath ObjectPath;
		if (!ValidateSoftObject(InObject, ExpectedClass, ObjectPath, OutError)) return false;
		SoftObjectPath = FSoftObjectPath(std::move(ObjectPath));
		ResolvedObjectPath = SoftObjectPath.GetObjectPath();
		WeakObject.SetObject(InObject);
		return true;
	}

	auto FSoftObjectPtr::TrySetLoadedObject(
		DObject* InObject,
		const DClass* ExpectedClass,
		std::string* OutError) -> bool
	{
		FObjectPath ObjectPath;
		if (!ValidateSoftObject(InObject, ExpectedClass, ObjectPath, OutError)) return false;
		if (SoftObjectPath.IsNull() || SoftObjectPath.GetObjectPath() != ObjectPath)
			return FailSoftObject("The loaded object does not match the stored soft-object path.", OutError);
		ResolvedObjectPath = ObjectPath;
		WeakObject.SetObject(InObject);
		return true;
	}

	auto FSoftObjectPtr::TrySetResolvedObject(
		DObject* InObject,
		const FPackagePath& AuthoredPath,
		const FPackagePath& ResolvedPath,
		const DClass* ExpectedClass,
		std::string* OutError) -> bool
	{
		FObjectPath ObjectPath;
		if (!ValidateSoftObject(InObject, ExpectedClass, ObjectPath, OutError)) return false;
		if (SoftObjectPath.IsNull() || SoftObjectPath.GetAssetPath() != AuthoredPath)
			return FailSoftObject("The resolved object does not match the authored soft-object path.", OutError);
		if (!ResolvedPath.IsValid() || ObjectPath.GetPackagePath() != ResolvedPath)
			return FailSoftObject("The resolved object does not match the resolved package path.", OutError);
		ResolvedObjectPath = ObjectPath;
		WeakObject.SetObject(InObject);
		return true;
	}

	auto FSoftObjectPtr::Get(const DClass* ExpectedClass) const -> DObject*
	{
		DObject* Object = WeakObject.Get();
		if (!Object || SoftObjectPath.IsNull() || !ResolvedObjectPath.IsValid()) return nullptr;

		FObjectPath ObjectPath;
		if (!ValidateSoftObject(Object, ExpectedClass, ObjectPath, nullptr)) return nullptr;
		return ObjectPath == ResolvedObjectPath ? Object : nullptr;
	}
}
