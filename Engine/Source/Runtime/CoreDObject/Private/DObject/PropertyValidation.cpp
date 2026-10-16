#include "DObject/PropertyValidation.h"

#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectPtr.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Misc/Guid.h"

namespace Durin
{
	auto ValidateNativeProperty(const FProperty& Property, FNativePropertyValidationOptions Options) -> std::optional<FPropertyValidationError>
	{
		using E = EPropertyValidationError;
		using K = DurinCodeGen::EPropertyGenFlags;
		auto Fail = [&](E Code, std::string Reason) -> std::optional<FPropertyValidationError> {
			return FPropertyValidationError{Code, Property.NamePrivate.ToString(), std::move(Reason)};
		};
		const auto Size = Property.GetValueSize(), Alignment = Property.GetValueAlignment();
		if (!Property.GetArrayDim() || !Size || !Alignment || (Alignment & (Alignment - 1))
			|| Size > Property.GetElementSize()) return Fail(E::InvalidStorage, "native value size/alignment is invalid or exceeds the element stride");
		auto Matches = [&](uint32 ExpectedSize, uint32 ExpectedAlignment) {
			return Size == ExpectedSize && Alignment == ExpectedAlignment;
		};
		auto TypeIs = [&](const FFieldClass* Class) { return Property.ClassPrivate && Property.ClassPrivate->IsChildOf(Class); };
		auto Scalar = [&]<typename T>(const FFieldClass* Class) -> std::optional<FPropertyValidationError> {
			if (!TypeIs(Class)) return Fail(E::InvalidType, "property kind does not match its concrete native field class");
			if (!Matches(sizeof(T), alignof(T))) return Fail(E::InvalidStorage,
				std::format("native scalar storage does not match the reflected kind (size/alignment {}/{}, expected {}/{})", Size, Alignment, sizeof(T), alignof(T)));
			return {};
		};
		std::optional<FPropertyValidationError> Error;
		switch (Property.GetKind())
		{
		case K::Bool: Error = Scalar.operator()<bool>(FBoolProperty::StaticClass()); break;
		case K::Int8: Error = Scalar.operator()<int8>(FNumericProperty::StaticClass()); break;
		case K::Int16: Error = Scalar.operator()<int16>(FNumericProperty::StaticClass()); break;
		case K::Int32: Error = Scalar.operator()<int32>(FNumericProperty::StaticClass()); break;
		case K::Int64: Error = Scalar.operator()<int64>(FNumericProperty::StaticClass()); break;
		case K::UInt8: Error = Scalar.operator()<uint8>(FNumericProperty::StaticClass()); break;
		case K::UInt16: Error = Scalar.operator()<uint16>(FNumericProperty::StaticClass()); break;
		case K::UInt32: Error = Scalar.operator()<uint32>(FNumericProperty::StaticClass()); break;
		case K::UInt64: Error = Scalar.operator()<uint64>(FNumericProperty::StaticClass()); break;
		case K::Float: Error = Scalar.operator()<float>(FNumericProperty::StaticClass()); break;
		case K::Double: Error = Scalar.operator()<double>(FNumericProperty::StaticClass()); break;
		case K::String: Error = Scalar.operator()<std::string>(FStringProperty::StaticClass()); break;
		case K::Name: Error = Scalar.operator()<FName>(FNameProperty::StaticClass()); break;
		case K::Guid: Error = Scalar.operator()<FGuid>(FGuidProperty::StaticClass()); break;
		case K::Byte: Error = Scalar.operator()<std::byte>(FProperty::StaticClass()); break;
		case K::Enum:
		{
			if (!TypeIs(FEnumProperty::StaticClass())) return Fail(E::InvalidType, "enum kind requires FEnumProperty");
			const auto& EnumProperty = static_cast<const FEnumProperty&>(Property);
			const auto* Enum = EnumProperty.GetEnum();
			if (!Enum) return Fail(E::MissingReferencedType, "enum descriptor is missing");
			using U = DurinCodeGen::EEnumUnderlyingType;
			bool Valid = false;
			switch (Enum->GetUnderlyingType())
			{
			case U::Int8: Valid = Matches(sizeof(int8), alignof(int8)); break;
			case U::Int16: Valid = Matches(sizeof(int16), alignof(int16)); break;
			case U::Int32: Valid = Matches(sizeof(int32), alignof(int32)); break;
			case U::Int64: Valid = Matches(sizeof(int64), alignof(int64)); break;
			case U::UInt8: Valid = Matches(sizeof(uint8), alignof(uint8)); break;
			case U::UInt16: Valid = Matches(sizeof(uint16), alignof(uint16)); break;
			case U::UInt32: Valid = Matches(sizeof(uint32), alignof(uint32)); break;
			case U::UInt64: Valid = Matches(sizeof(uint64), alignof(uint64)); break;
			default: break;
			}
			if (!Valid || Enum->GetUnderlyingSize() != Size || EnumProperty.GetUnderlyingType() != Enum->GetUnderlyingType())
				return Fail(E::InvalidStorage, "enum storage does not match its underlying native type");
			break;
		}
		case K::Object:
			if (!TypeIs(FObjectProperty::StaticClass())) return Fail(E::InvalidType, "object kind requires FObjectProperty");
			if (!Property.GetReferencedClass()) return Fail(E::MissingReferencedType, "referenced object class is missing");
			if (!Matches(Property.IsObjectPtrWrapper() ? sizeof(TObjectPtr<DObject>) : sizeof(DObject*),
				Property.IsObjectPtrWrapper() ? alignof(TObjectPtr<DObject>) : alignof(DObject*)))
				return Fail(E::InvalidStorage, "object pointer storage does not match its native representation");
			if (!static_cast<const FObjectProperty&>(Property).HasObjectValueWriter()) return Fail(E::MissingValueOperations, "object value writer is missing");
			break;
		case K::SoftObject:
			if (!TypeIs(FSoftObjectProperty::StaticClass())) return Fail(E::InvalidType, "soft object kind requires FSoftObjectProperty");
			if (!Property.GetReferencedClass()) return Fail(E::MissingReferencedType, "soft object class is missing");
			if (!Matches(sizeof(TSoftObjectPtr<DObject>), alignof(TSoftObjectPtr<DObject>))) return Fail(E::InvalidStorage, "soft object storage does not match its native representation");
			if (!static_cast<const FSoftObjectProperty&>(Property).HasSoftValueAccessors()) return Fail(E::MissingValueOperations, "soft object accessors are missing");
			break;
		case K::WeakObject:
			if (!TypeIs(FWeakObjectProperty::StaticClass())) return Fail(E::InvalidType, "weak object kind requires FWeakObjectProperty");
			if (!Property.GetReferencedClass()) return Fail(E::MissingReferencedType, "weak object class is missing");
			if (!Matches(sizeof(TWeakObjectPtr<DObject>), alignof(TWeakObjectPtr<DObject>))) return Fail(E::InvalidStorage, "weak object storage does not match its native representation");
			if (!static_cast<const FWeakObjectProperty&>(Property).HasWeakValueAccessors()) return Fail(E::MissingValueOperations, "weak object accessors are missing");
			break;
		case K::Struct:
		{
			if (!TypeIs(FStructProperty::StaticClass())) return Fail(E::InvalidType, "struct kind requires FStructProperty");
			const auto* Struct = static_cast<const FStructProperty&>(Property).GetStruct();
			if (!Struct) return Fail(E::MissingReferencedType, "struct descriptor is missing");
			if (!Matches(Struct->GetStructureSize(), Struct->GetMinAlignment())) return Fail(E::InvalidStorage, "struct storage does not match its native layout");
			break;
		}
		case K::Array:
		{
			if (!TypeIs(FArrayProperty::StaticClass())) return Fail(E::InvalidType, "array kind requires FArrayProperty");
			const auto& Array = static_cast<const FArrayProperty&>(Property);
			if (!Array.HasArrayOps() || !IsValidArrayOps(&Array.GetOps())) return Fail(E::InvalidContainerOperations, "array operations are missing or invalid");
			if (!Array.GetInner()) return Fail(E::MissingContainerField, "array element property is missing");
			if (!Matches(Array.GetOps().ContainerSize, Array.GetOps().ContainerAlignment)) return Fail(E::InvalidStorage, "array storage does not match its native operations");
			break;
		}
		case K::Map:
		{
			if (!TypeIs(FMapProperty::StaticClass())) return Fail(E::InvalidType, "map kind requires FMapProperty");
			const auto& Map = static_cast<const FMapProperty&>(Property);
			if (!Map.HasMapOps() || !IsValidMapOps(&Map.GetOps())) return Fail(E::InvalidContainerOperations, "map operations are missing or invalid");
			if (!Map.GetKeyProp() || !Map.GetValueProp()) return Fail(E::MissingContainerField, "map key/value property is missing");
			if (!Matches(Map.GetOps().ContainerSize, Map.GetOps().ContainerAlignment)) return Fail(E::InvalidStorage, "map storage does not match its native operations");
			break;
		}
		case K::Blob: case K::BulkData: case K::None: break; // Explicit native operations define these ordinary field layouts.
		default: return Fail(E::InvalidType, "unknown native property kind");
		}
		if (Error) return Error;
		if (Options.RequireValueOperations && (!Property.HasValueLifecycle() || !Property.CanCopyConstructValue() || !Property.CanCopyAssignValue()))
			return Fail(E::MissingValueOperations, "native initialization, destruction or copy operations are missing");
		return {};
	}
}
