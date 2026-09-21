#include "DObject/Class.h"
#include "DObject/PackageSaveOverrides.h"
#include "DObject/DurinPropertyTypes.h"

namespace Durin
{
	namespace
	{
		auto FailSaveOverride(ESaveOverrideError Code, const DObject& Object,
			const FProperty* Property = nullptr) -> std::expected<void, FSaveOverrideError>
		{
			FSaveOverrideError Error;
			Error.Code = Code;
			Error.ObjectPath = Object.GetObjectPath();
			if (Property) Error.PropertyName = Property->NamePrivate.ToString();
			return std::unexpected(std::move(Error));
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
		const DObject& Object) -> std::expected<void, FSaveOverrideError>
	{
		if (FObjectSaveOverride* Existing = FindMutableObject(Object))
		{
			if (Existing->bOmitObject || !Existing->Properties.empty())
				return FailSaveOverride(ESaveOverrideError::ObjectConflict, Object);
			Existing->bOmitObject = true;
			return {};
		}
		Objects.push_back({.Object = &Object, .bOmitObject = true});
		return {};
	}

	auto FObjectSaveOverrides::AddPropertyOmission(
		const DObject& Object, const FProperty& Property) -> std::expected<void, FSaveOverrideError>
	{
		if (!ObjectOwnsProperty(Object, Property))
			return FailSaveOverride(ESaveOverrideError::ForeignOmissionProperty, Object, &Property);
		FObjectSaveOverride* ObjectOverride = FindMutableObject(Object);
		if (!ObjectOverride)
		{
			Objects.push_back({.Object = &Object});
			ObjectOverride = &Objects.back();
		}
		if (ObjectOverride->bOmitObject
			|| std::ranges::find(ObjectOverride->Properties, &Property,
				&FPropertySaveOverride::Property) != ObjectOverride->Properties.end())
			return FailSaveOverride(ESaveOverrideError::PropertyConflict, Object, &Property);
		ObjectOverride->Properties.push_back({.Property = &Property});
		return {};
	}

	auto FObjectSaveOverrides::AddPropertyValueRaw(
		const DObject& Object, const FProperty& Property, const void* Replacement,
		size_t ReplacementSize, size_t ReplacementAlignment,
		DurinCodeGen::EPropertyGenFlags ReplacementKind,
		const DStruct* ReplacementStruct, const DClass* ReplacementClass) -> std::expected<void, FSaveOverrideError>
	{
		if (!ObjectOwnsProperty(Object, Property))
			return FailSaveOverride(ESaveOverrideError::ForeignReplacementProperty, Object, &Property);
		if (!Replacement || Property.GetArrayDim() != 1
			|| Property.GetValueSize() != ReplacementSize
			|| Property.GetValueAlignment() != ReplacementAlignment
			|| Property.GetKind() != ReplacementKind)
		{
			auto Result = FailSaveOverride(ESaveOverrideError::StorageMismatch, Object, &Property);
			Result.error().ExpectedSize = Property.GetValueSize();
			Result.error().ActualSize = ReplacementSize;
			Result.error().ExpectedAlignment = Property.GetValueAlignment();
			Result.error().ActualAlignment = ReplacementAlignment;
			Result.error().ArrayDim = Property.GetArrayDim();
			Result.error().ExpectedKind = Property.GetKind();
			Result.error().ActualKind = ReplacementKind;
			return Result;
		}
		if (ReplacementKind == DurinCodeGen::EPropertyGenFlags::Struct
			&& static_cast<const FStructProperty&>(Property).GetStruct() != ReplacementStruct)
		{
			auto Result = FailSaveOverride(ESaveOverrideError::StructMismatch, Object, &Property);
			if (const auto* Expected = static_cast<const FStructProperty&>(Property).GetStruct())
				Result.error().ExpectedType = Expected->GetQualifiedName().ToString();
			if (ReplacementStruct) Result.error().ActualType = ReplacementStruct->GetQualifiedName().ToString();
			return Result;
		}
		if ((ReplacementKind == DurinCodeGen::EPropertyGenFlags::Object
				|| ReplacementKind == DurinCodeGen::EPropertyGenFlags::SoftObject)
			&& (Property.GetReferencedClass() != ReplacementClass
				|| (ReplacementKind == DurinCodeGen::EPropertyGenFlags::Object
					&& !Property.IsObjectPtrWrapper())))
		{
			auto Result = FailSaveOverride(ESaveOverrideError::ObjectWrapperMismatch, Object, &Property);
			if (const auto* Expected = Property.GetReferencedClass())
				Result.error().ExpectedType = Expected->GetQualifiedName().ToString();
			if (ReplacementClass) Result.error().ActualType = ReplacementClass->GetQualifiedName().ToString();
			return Result;
		}
		FObjectSaveOverride* ObjectOverride = FindMutableObject(Object);
		if (ObjectOverride && (ObjectOverride->bOmitObject
			|| std::ranges::find(ObjectOverride->Properties, &Property,
				&FPropertySaveOverride::Property) != ObjectOverride->Properties.end()))
			return FailSaveOverride(ESaveOverrideError::PropertyConflict, Object, &Property);

		FReflectedValueStorage Storage;
		if (const auto ValueResult = Storage.CopyConstruct(&Property, Replacement, 0); !ValueResult)
		{
			auto Result = FailSaveOverride(ESaveOverrideError::ValueCopyFailed, Object, &Property);
			Result.error().Cause = ValueResult.error();
			return Result;
		}
		FPropertyValueSnapshot Snapshot;
		if (const auto SnapshotResult = CapturePropertyValue(&Property, Storage.GetContainer(), 0, Snapshot); !SnapshotResult)
		{
			auto Result = FailSaveOverride(ESaveOverrideError::SnapshotFailed, Object, &Property);
			Result.error().Cause = SnapshotResult.error();
			return Result;
		}
		if (!ObjectOverride)
		{
			Objects.push_back({.Object = &Object});
			ObjectOverride = &Objects.back();
		}
		ObjectOverride->Properties.push_back({
			.Property = &Property,
			.Kind = EPropertySaveOverrideKind::Replace,
			.Replacement = std::move(Snapshot)});
		return {};
	}

	auto ToString(const FSaveOverrideError& Error) -> std::string
	{
		if (!std::holds_alternative<std::monostate>(Error.Cause))
			return std::visit([](const auto& Cause) -> std::string {
				using T = std::decay_t<decltype(Cause)>;
				if constexpr (std::is_same_v<T, std::monostate>) return {};
				else return ToString(Cause);
			}, Error.Cause);
		switch (Error.Code)
		{
		case ESaveOverrideError::None: return {};
		case ESaveOverrideError::ObjectConflict: return "A conflicting save override already exists for the object.";
		case ESaveOverrideError::ForeignOmissionProperty: return "The omitted property does not belong to the target object's reflected schema.";
		case ESaveOverrideError::PropertyConflict: return "A conflicting save override already exists for the property.";
		case ESaveOverrideError::ForeignReplacementProperty: return "The replacement property does not belong to the target object's reflected schema.";
		case ESaveOverrideError::StorageMismatch: return "The replacement value does not exactly match the reflected property storage type.";
		case ESaveOverrideError::StructMismatch: return "The replacement Struct type does not match the reflected property type.";
		case ESaveOverrideError::ObjectWrapperMismatch: return "The replacement object wrapper type does not match the reflected property type.";
		case ESaveOverrideError::ValueCopyFailed: return "The save override value could not be copied.";
		case ESaveOverrideError::SnapshotFailed: return "The save override value could not be captured.";
		}
		return {};
	}

}
