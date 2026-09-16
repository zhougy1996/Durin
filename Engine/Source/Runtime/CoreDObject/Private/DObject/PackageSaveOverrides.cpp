#include "DObject/Class.h"
#include "DObject/PackageSaveOverrides.h"
#include "DObject/DurinPropertyTypes.h"

namespace Durin
{
	namespace
	{
		auto FailSaveOverride(std::string_view Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = Message;
			return false;
		}

		auto ObjectOwnsProperty(const DObject& Object, const FProperty& Property) -> bool
		{
			if (!Object.GetClass()) return false;
			bool bFound = false;
			Object.GetClass()->ForEachProperty([&](FProperty* Candidate) {
				bFound = bFound || Candidate == &Property;
			}, true);
			return bFound;
		}
	}

	auto FObjectSaveOverrides::FindMutableObject(const DObject& Object) -> FObjectSaveOverride*
	{
		auto It = std::ranges::find(Objects, &Object, &FObjectSaveOverride::Object);
		return It == Objects.end() ? nullptr : &*It;
	}

	auto FObjectSaveOverrides::FindObject(const DObject& Object) const -> const FObjectSaveOverride*
	{
		auto It = std::ranges::find(Objects, &Object, &FObjectSaveOverride::Object);
		return It == Objects.end() ? nullptr : &*It;
	}

	auto FObjectSaveOverrides::AddObjectOmission(
		const DObject& Object, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (FObjectSaveOverride* Existing = FindMutableObject(Object))
		{
			if (Existing->bOmitObject || !Existing->Properties.empty())
				return FailSaveOverride("A conflicting save override already exists for the object.", OutError);
			Existing->bOmitObject = true;
			return true;
		}
		Objects.push_back({.Object = &Object, .bOmitObject = true});
		return true;
	}

	auto FObjectSaveOverrides::AddPropertyOmission(
		const DObject& Object, const FProperty& Property, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (!ObjectOwnsProperty(Object, Property))
			return FailSaveOverride("The omitted property does not belong to the target object's reflected schema.", OutError);
		FObjectSaveOverride* ObjectOverride = FindMutableObject(Object);
		if (!ObjectOverride)
		{
			Objects.push_back({.Object = &Object});
			ObjectOverride = &Objects.back();
		}
		if (ObjectOverride->bOmitObject
			|| std::ranges::find(ObjectOverride->Properties, &Property,
				&FPropertySaveOverride::Property) != ObjectOverride->Properties.end())
			return FailSaveOverride("A conflicting save override already exists for the property.", OutError);
		ObjectOverride->Properties.push_back({.Property = &Property});
		return true;
	}

	auto FObjectSaveOverrides::AddPropertyValueRaw(
		const DObject& Object, const FProperty& Property, const void* Replacement,
		size_t ReplacementSize, size_t ReplacementAlignment,
		DurinCodeGen::EPropertyGenFlags ReplacementKind,
		const DStruct* ReplacementStruct, const DClass* ReplacementClass,
		std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (!ObjectOwnsProperty(Object, Property))
			return FailSaveOverride("The replacement property does not belong to the target object's reflected schema.", OutError);
		if (!Replacement || Property.GetArrayDim() != 1
			|| Property.GetValueSize() != ReplacementSize
			|| Property.GetValueAlignment() != ReplacementAlignment
			|| Property.GetKind() != ReplacementKind)
			return FailSaveOverride("The replacement value does not exactly match the reflected property storage type.", OutError);
		if (ReplacementKind == DurinCodeGen::EPropertyGenFlags::Struct
			&& static_cast<const FStructProperty&>(Property).GetStruct() != ReplacementStruct)
			return FailSaveOverride("The replacement Struct type does not match the reflected property type.", OutError);
		if ((ReplacementKind == DurinCodeGen::EPropertyGenFlags::Object
				|| ReplacementKind == DurinCodeGen::EPropertyGenFlags::SoftObject)
			&& (Property.GetReferencedClass() != ReplacementClass
				|| (ReplacementKind == DurinCodeGen::EPropertyGenFlags::Object
					&& !Property.IsObjectPtrWrapper())))
			return FailSaveOverride("The replacement object wrapper type does not match the reflected property type.", OutError);
		FObjectSaveOverride* ObjectOverride = FindMutableObject(Object);
		if (!ObjectOverride)
		{
			Objects.push_back({.Object = &Object});
			ObjectOverride = &Objects.back();
		}
		if (ObjectOverride->bOmitObject
			|| std::ranges::find(ObjectOverride->Properties, &Property,
				&FPropertySaveOverride::Property) != ObjectOverride->Properties.end())
			return FailSaveOverride("A conflicting save override already exists for the property.", OutError);

		FReflectedValueStorage Storage;
		std::string CaptureError;
		if (!Storage.CopyConstruct(&Property, Replacement, 0, &CaptureError))
			return FailSaveOverride(CaptureError, OutError);
		FPropertyValueSnapshot Snapshot;
		if (!CapturePropertyValue(&Property, Storage.GetContainer(), 0, Snapshot, &CaptureError))
			return FailSaveOverride(CaptureError, OutError);
		ObjectOverride->Properties.push_back({
			.Property = &Property,
			.Kind = EPropertySaveOverrideKind::Replace,
			.Replacement = std::move(Snapshot)});
		return true;
	}

}
