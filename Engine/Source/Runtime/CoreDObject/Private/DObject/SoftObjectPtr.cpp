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
			FAssetPath& OutPath,
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
			if (Package->GetAsset() != Object || Object->GetOuter() != Package)
				return FailSoftObject("Only a package main asset can be assigned as a soft object.", OutError);
			if (!FAssetPath::TryCreate(Package->GetPackagePath(), OutPath, OutError)) return false;
			return true;
		}
	}

	auto FSoftObjectPath::TryCreate(
		std::string_view InPath,
		FSoftObjectPath& OutPath,
		std::string* OutError) -> bool
	{
		FAssetPath Path;
		if (!FAssetPath::TryCreate(InPath, Path, OutError)) return false;
		OutPath = FSoftObjectPath(std::move(Path));
		return true;
	}

	FSoftObjectPtr::FSoftObjectPtr(FSoftObjectPtr&& Other) noexcept
		: SoftObjectPath(std::move(Other.SoftObjectPath))
		, ResolvedPackagePath(std::move(Other.ResolvedPackagePath))
		, WeakObject(Other.WeakObject)
	{
		Other.Reset();
	}

	auto FSoftObjectPtr::operator=(FSoftObjectPtr&& Other) noexcept -> FSoftObjectPtr&
	{
		if (this == &Other) return *this;
		SoftObjectPath = std::move(Other.SoftObjectPath);
		ResolvedPackagePath = std::move(Other.ResolvedPackagePath);
		WeakObject = Other.WeakObject;
		Other.Reset();
		return *this;
	}

	auto FSoftObjectPtr::SetPath(FSoftObjectPath InPath) -> void
	{
		SoftObjectPath = std::move(InPath);
		ResolvedPackagePath = {};
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

		FAssetPath ObjectPath;
		if (!ValidateSoftObject(InObject, ExpectedClass, ObjectPath, OutError)) return false;
		SoftObjectPath = FSoftObjectPath(std::move(ObjectPath));
		ResolvedPackagePath = SoftObjectPath.GetAssetPath();
		WeakObject.SetObject(InObject);
		return true;
	}

	auto FSoftObjectPtr::TrySetLoadedObject(
		DObject* InObject,
		const DClass* ExpectedClass,
		std::string* OutError) -> bool
	{
		FAssetPath ObjectPath;
		if (!ValidateSoftObject(InObject, ExpectedClass, ObjectPath, OutError)) return false;
		if (SoftObjectPath.IsNull() || SoftObjectPath.GetAssetPath() != ObjectPath)
			return FailSoftObject("The loaded object does not match the stored soft-object path.", OutError);
		ResolvedPackagePath = ObjectPath;
		WeakObject.SetObject(InObject);
		return true;
	}

	auto FSoftObjectPtr::TrySetResolvedObject(
		DObject* InObject,
		const FAssetPath& AuthoredPath,
		const FAssetPath& ResolvedPath,
		const DClass* ExpectedClass,
		std::string* OutError) -> bool
	{
		FAssetPath ObjectPath;
		if (!ValidateSoftObject(InObject, ExpectedClass, ObjectPath, OutError)) return false;
		if (SoftObjectPath.IsNull() || SoftObjectPath.GetAssetPath() != AuthoredPath)
			return FailSoftObject("The resolved object does not match the authored soft-object path.", OutError);
		if (!ResolvedPath.IsValid() || ObjectPath != ResolvedPath)
			return FailSoftObject("The resolved object does not match the resolved package path.", OutError);
		ResolvedPackagePath = ResolvedPath;
		WeakObject.SetObject(InObject);
		return true;
	}

	auto FSoftObjectPtr::Get(const DClass* ExpectedClass) const -> DObject*
	{
		DObject* Object = WeakObject.Get();
		if (!Object || SoftObjectPath.IsNull() || !ResolvedPackagePath.IsValid()) return nullptr;

		FAssetPath ObjectPath;
		if (!ValidateSoftObject(Object, ExpectedClass, ObjectPath, nullptr)) return nullptr;
		return ObjectPath == ResolvedPackagePath ? Object : nullptr;
	}
}
