#include "DObject/DObjectGlobals.h"
#include "QualifiedTypeRegistry.h"
#include "Misc/Name.h"

#include "Misc/AppConfig.h"
#include "Misc/Time.h"
#include "Modules/ModuleManager.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/GarbageCollectionScheduler.h"
#include "DObject/MathStructs.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "GCReferenceSchema.h"

namespace Durin
{
	namespace
	{
		std::atomic_bool GDObjectInitialized = false;

		auto GetGeneratedPropertyOwnerName(const FFieldVariant& Owner) -> std::string
		{
			if (DObject* Object = Owner.ToDObject()) return Object->GetName();
			if (FField* Field = Owner.ToField()) return Field->NamePrivate.ToString();
			return "<null>";
		}

		auto GetExpectedPropertyLayout(DurinCodeGen::EPropertyGenFlags Kind) -> DurinCodeGen::EPropertyParamLayout
		{
			switch (Kind)
			{
			case DurinCodeGen::EPropertyGenFlags::Bool:
			case DurinCodeGen::EPropertyGenFlags::Int8:
			case DurinCodeGen::EPropertyGenFlags::Int16:
			case DurinCodeGen::EPropertyGenFlags::Int32:
			case DurinCodeGen::EPropertyGenFlags::Int64:
			case DurinCodeGen::EPropertyGenFlags::UInt8:
			case DurinCodeGen::EPropertyGenFlags::UInt16:
			case DurinCodeGen::EPropertyGenFlags::UInt32:
			case DurinCodeGen::EPropertyGenFlags::UInt64:
			case DurinCodeGen::EPropertyGenFlags::Float:
			case DurinCodeGen::EPropertyGenFlags::Double:
			case DurinCodeGen::EPropertyGenFlags::String:
			case DurinCodeGen::EPropertyGenFlags::Name:
			case DurinCodeGen::EPropertyGenFlags::Guid:
			case DurinCodeGen::EPropertyGenFlags::Byte:
				return DurinCodeGen::EPropertyParamLayout::Plain;
			case DurinCodeGen::EPropertyGenFlags::Enum: return DurinCodeGen::EPropertyParamLayout::Enum;
			case DurinCodeGen::EPropertyGenFlags::Object: return DurinCodeGen::EPropertyParamLayout::Object;
			case DurinCodeGen::EPropertyGenFlags::Array: return DurinCodeGen::EPropertyParamLayout::Array;
			case DurinCodeGen::EPropertyGenFlags::Map: return DurinCodeGen::EPropertyParamLayout::Map;
			case DurinCodeGen::EPropertyGenFlags::Struct: return DurinCodeGen::EPropertyParamLayout::Struct;
			case DurinCodeGen::EPropertyGenFlags::SoftObject: return DurinCodeGen::EPropertyParamLayout::SoftObject;
			case DurinCodeGen::EPropertyGenFlags::WeakObject: return DurinCodeGen::EPropertyParamLayout::WeakObject;
			case DurinCodeGen::EPropertyGenFlags::Blob: return DurinCodeGen::EPropertyParamLayout::Blob;
			case DurinCodeGen::EPropertyGenFlags::None: return DurinCodeGen::EPropertyParamLayout::Generic;
			default: return DurinCodeGen::EPropertyParamLayout::Invalid;
			}
		}

		auto IsValidValueOps(const DurinCodeGen::FPropertyValueOps& Ops) -> bool
		{
			return Ops.ValueSize > 0
				   && Ops.ValueAlignment > 0
				   && (Ops.ValueAlignment & (Ops.ValueAlignment - 1)) == 0
				   && Ops.InitializeValue
				   && Ops.DestroyValue
				   && Ops.CopyConstructValue
				   && Ops.CopyAssignValue;
		}

		auto HasTransientPropertyContext(const FFieldVariant& Owner, EPropertyFlags Flags) -> bool
		{
			if (EnumHasAnyFlags(Flags, EPropertyFlags::Transient)) return true;
			for (FField* Field = Owner.ToField(); Field; Field = Field->Owner.ToField())
			{
				// Generated property descriptors only nest fields beneath container properties.
				if (static_cast<FProperty*>(Field)->HasAnyPropertyFlags(EPropertyFlags::Transient)) return true;
			}
			return false;
		}

		auto SetValueOps(FProperty* Property, const DurinCodeGen::FPropertyValueOps& Ops) -> void
		{
			Property->SetValueLifecycle(
				Ops.ValueSize,
				Ops.ValueAlignment,
				Ops.InitializeValue,
				Ops.DestroyValue,
				Ops.CopyConstructValue,
				Ops.CopyAssignValue
			);
		}

		template<typename TValue, typename TProperty>
		auto ConstructPlainProperty(
			const FFieldVariant& Owner,
			const DurinCodeGen::FPropertyParamsBase* Params
		) -> FProperty*
		{
			static_assert(sizeof(TValue) <= std::numeric_limits<uint16>::max());
			auto* Property = new TProperty(
				Owner,
				FName(Params->NameUTF8),
				EObjectFlags::NoFlags,
				Params->Flags,
				Params->ArrayDim,
				Params->Offset,
				static_cast<uint16>(sizeof(TValue)),
				Params->Kind,
				nullptr
			);
			SetValueOps(Property, DurinCodeGen::MakePropertyValueOps<TValue>());
			return Property;
		}

		template<typename TValue>
		auto SetEnumValueOps(FProperty* Property) -> void
		{
			SetValueOps(Property, DurinCodeGen::MakePropertyValueOps<TValue>());
		}

		auto InstallEnumValueOps(
			FProperty* Property,
			DurinCodeGen::EEnumUnderlyingType UnderlyingType
		) -> bool
		{
			switch (UnderlyingType)
			{
			case DurinCodeGen::EEnumUnderlyingType::Int8: SetEnumValueOps<int8>(Property); return true;
			case DurinCodeGen::EEnumUnderlyingType::Int16: SetEnumValueOps<int16>(Property); return true;
			case DurinCodeGen::EEnumUnderlyingType::Int32: SetEnumValueOps<int32>(Property); return true;
			case DurinCodeGen::EEnumUnderlyingType::Int64: SetEnumValueOps<int64>(Property); return true;
			case DurinCodeGen::EEnumUnderlyingType::UInt8: SetEnumValueOps<uint8>(Property); return true;
			case DurinCodeGen::EEnumUnderlyingType::UInt16: SetEnumValueOps<uint16>(Property); return true;
			case DurinCodeGen::EEnumUnderlyingType::UInt32: SetEnumValueOps<uint32>(Property); return true;
			case DurinCodeGen::EEnumUnderlyingType::UInt64: SetEnumValueOps<uint64>(Property); return true;
			default: return false;
			}
		}

		auto IsMatchingEnumSize(DurinCodeGen::EEnumUnderlyingType UnderlyingType, uint16 UnderlyingSize) -> bool
		{
			switch (UnderlyingType)
			{
			case DurinCodeGen::EEnumUnderlyingType::Int8: return UnderlyingSize == sizeof(int8);
			case DurinCodeGen::EEnumUnderlyingType::Int16: return UnderlyingSize == sizeof(int16);
			case DurinCodeGen::EEnumUnderlyingType::Int32: return UnderlyingSize == sizeof(int32);
			case DurinCodeGen::EEnumUnderlyingType::Int64: return UnderlyingSize == sizeof(int64);
			case DurinCodeGen::EEnumUnderlyingType::UInt8: return UnderlyingSize == sizeof(uint8);
			case DurinCodeGen::EEnumUnderlyingType::UInt16: return UnderlyingSize == sizeof(uint16);
			case DurinCodeGen::EEnumUnderlyingType::UInt32: return UnderlyingSize == sizeof(uint32);
			case DurinCodeGen::EEnumUnderlyingType::UInt64: return UnderlyingSize == sizeof(uint64);
			default: return false;
			}
		}

		auto ExpectedMetadataNumericKind(const FProperty* Property) -> EPropertyMetadataNumericKind
		{
			if (!Property) return EPropertyMetadataNumericKind::None;
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Int8:
			case DurinCodeGen::EPropertyGenFlags::Int16:
			case DurinCodeGen::EPropertyGenFlags::Int32:
			case DurinCodeGen::EPropertyGenFlags::Int64: return EPropertyMetadataNumericKind::Signed;
			case DurinCodeGen::EPropertyGenFlags::UInt8:
			case DurinCodeGen::EPropertyGenFlags::UInt16:
			case DurinCodeGen::EPropertyGenFlags::UInt32:
			case DurinCodeGen::EPropertyGenFlags::UInt64: return EPropertyMetadataNumericKind::Unsigned;
			case DurinCodeGen::EPropertyGenFlags::Float: return EPropertyMetadataNumericKind::Float;
			case DurinCodeGen::EPropertyGenFlags::Double: return EPropertyMetadataNumericKind::Double;
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				const DStruct* Struct = static_cast<const FStructProperty*>(Property)->GetStruct();
				if (Struct == Z_Construct_DStruct_Durin_FVector2f()
					|| Struct == Z_Construct_DStruct_Durin_FVector3f()
					|| Struct == Z_Construct_DStruct_Durin_FVector4f()
					|| Struct == Z_Construct_DStruct_Durin_FQuatf()) return EPropertyMetadataNumericKind::Float;
				if (Struct == Z_Construct_DStruct_Durin_FVector2()
					|| Struct == Z_Construct_DStruct_Durin_FVector3()
					|| Struct == Z_Construct_DStruct_Durin_FVector4()
					|| Struct == Z_Construct_DStruct_Durin_FQuat()) return EPropertyMetadataNumericKind::Double;
				return EPropertyMetadataNumericKind::None;
			}
			default: return EPropertyMetadataNumericKind::None;
			}
		}

		template<typename T>
		auto MetadataNumbersFitIntegralProperty(const FPropertyMetadataParams& Metadata) -> bool
		{
			for (const FPropertyMetadataNumber* Number : {&Metadata.Step, &Metadata.ClampMin,
				&Metadata.ClampMax, &Metadata.UIMin, &Metadata.UIMax})
			{
				if (Number->Kind == EPropertyMetadataNumericKind::None) continue;
				if constexpr (std::is_signed_v<T>)
				{
					if (Number->Signed < std::numeric_limits<T>::min()
						|| Number->Signed > std::numeric_limits<T>::max()) return false;
				}
				else if (Number->Unsigned > std::numeric_limits<T>::max()) return false;
			}
			return true;
		}

		auto MetadataNumbersFitProperty(const FProperty* Property,
			const FPropertyMetadataParams& Metadata) -> bool
		{
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Int8: return MetadataNumbersFitIntegralProperty<int8>(Metadata);
			case DurinCodeGen::EPropertyGenFlags::Int16: return MetadataNumbersFitIntegralProperty<int16>(Metadata);
			case DurinCodeGen::EPropertyGenFlags::Int32: return MetadataNumbersFitIntegralProperty<int32>(Metadata);
			case DurinCodeGen::EPropertyGenFlags::Int64: return MetadataNumbersFitIntegralProperty<int64>(Metadata);
			case DurinCodeGen::EPropertyGenFlags::UInt8: return MetadataNumbersFitIntegralProperty<uint8>(Metadata);
			case DurinCodeGen::EPropertyGenFlags::UInt16: return MetadataNumbersFitIntegralProperty<uint16>(Metadata);
			case DurinCodeGen::EPropertyGenFlags::UInt32: return MetadataNumbersFitIntegralProperty<uint32>(Metadata);
			case DurinCodeGen::EPropertyGenFlags::UInt64: return MetadataNumbersFitIntegralProperty<uint64>(Metadata);
			default: return true;
			}
		}

		template<typename T>
		auto MetadataNumberValue(const FPropertyMetadataNumber& Number) -> T
		{
			if constexpr (std::is_same_v<T, int64>) return Number.Signed;
			if constexpr (std::is_same_v<T, uint64>) return Number.Unsigned;
			if constexpr (std::is_same_v<T, float>) return Number.Float;
			return Number.Double;
		}

		template<typename T>
		auto ValidateMetadataNumbers(const FPropertyMetadataParams& Metadata) -> bool
		{
			auto Present = [](const FPropertyMetadataNumber& Number) {
				return Number.Kind != EPropertyMetadataNumericKind::None;
			};
			auto Finite = [](T Value) {
				if constexpr (std::is_floating_point_v<T>) return std::isfinite(Value);
				return true;
			};
			if (Present(Metadata.Step)
				&& (!Finite(MetadataNumberValue<T>(Metadata.Step)) || MetadataNumberValue<T>(Metadata.Step) <= T{})) return false;
			for (const FPropertyMetadataNumber* Number : {&Metadata.ClampMin, &Metadata.ClampMax, &Metadata.UIMin, &Metadata.UIMax})
				if (Present(*Number) && !Finite(MetadataNumberValue<T>(*Number))) return false;
			if (Present(Metadata.ClampMin) && Present(Metadata.ClampMax)
				&& MetadataNumberValue<T>(Metadata.ClampMin) > MetadataNumberValue<T>(Metadata.ClampMax)) return false;
			if (Present(Metadata.UIMin) && Present(Metadata.UIMax)
				&& MetadataNumberValue<T>(Metadata.UIMin) > MetadataNumberValue<T>(Metadata.UIMax)) return false;
			if (Present(Metadata.ClampMin) && Present(Metadata.UIMin)
				&& MetadataNumberValue<T>(Metadata.UIMin) < MetadataNumberValue<T>(Metadata.ClampMin)) return false;
			if (Present(Metadata.ClampMax) && Present(Metadata.UIMax)
				&& MetadataNumberValue<T>(Metadata.UIMax) > MetadataNumberValue<T>(Metadata.ClampMax)) return false;
			return true;
		}

		auto ValidateTypedMetadataRegistration(const FProperty* Property,
			const FPropertyMetadataParams* Metadata) -> bool
		{
			if (!Metadata) return true;
			if ((Metadata->DisplayName && Metadata->DisplayName[0] == '\0')
				|| (Metadata->ToolTip && Metadata->ToolTip[0] == '\0')
				|| (Metadata->Category && Metadata->Category[0] == '\0')) return false;
			if (Metadata->Units > EPropertyUnit::Kilometers || Metadata->Precision < -1) return false;

			const EPropertyMetadataNumericKind Expected = ExpectedMetadataNumericKind(Property);
			const bool bHasNumericMetadata = Metadata->Units != EPropertyUnit::None
				|| Metadata->Precision >= 0 || Metadata->Step.Kind != EPropertyMetadataNumericKind::None
				|| Metadata->ClampMin.Kind != EPropertyMetadataNumericKind::None
				|| Metadata->ClampMax.Kind != EPropertyMetadataNumericKind::None
				|| Metadata->UIMin.Kind != EPropertyMetadataNumericKind::None
				|| Metadata->UIMax.Kind != EPropertyMetadataNumericKind::None;
			if (!bHasNumericMetadata) return true;
			if (Expected == EPropertyMetadataNumericKind::None
				|| !Property->HasAnyPropertyFlags(EPropertyFlags::Edit)) return false;
			for (const FPropertyMetadataNumber* Number : {&Metadata->Step, &Metadata->ClampMin,
				&Metadata->ClampMax, &Metadata->UIMin, &Metadata->UIMax})
				if (Number->Kind != EPropertyMetadataNumericKind::None && Number->Kind != Expected) return false;
			if (!MetadataNumbersFitProperty(Property, *Metadata)) return false;
			if ((Expected == EPropertyMetadataNumericKind::Signed || Expected == EPropertyMetadataNumericKind::Unsigned)
				&& Metadata->Precision >= 0) return false;
			if (Expected == EPropertyMetadataNumericKind::Float && Metadata->Precision > 9) return false;
			if (Expected == EPropertyMetadataNumericKind::Double && Metadata->Precision > 17) return false;
			switch (Expected)
			{
			case EPropertyMetadataNumericKind::Signed: return ValidateMetadataNumbers<int64>(*Metadata);
			case EPropertyMetadataNumericKind::Unsigned: return ValidateMetadataNumbers<uint64>(*Metadata);
			case EPropertyMetadataNumericKind::Float: return ValidateMetadataNumbers<float>(*Metadata);
			case EPropertyMetadataNumericKind::Double: return ValidateMetadataNumbers<double>(*Metadata);
			default: return false;
			}
		}

		auto ValidateDeprecationRegistration(const FProperty* Property,
			const FPropertyDeprecationParams* Deprecation) -> bool
		{
			if (!Deprecation)
				return !Property->HasAnyPropertyFlags(EPropertyFlags::Deprecated);
			if (!Property->HasAnyPropertyFlags(EPropertyFlags::Deprecated)
				|| Property->HasAnyPropertyFlags(EPropertyFlags::Edit | EPropertyFlags::Transient)
				|| !Deprecation->CustomVersionGuid.IsValid()
				|| Deprecation->DeprecatedBefore <= 0
				|| Deprecation->LatestVersion < Deprecation->DeprecatedBefore
				|| !Deprecation->HistoricalName || Deprecation->HistoricalName[0] == '\0'
				|| !Deprecation->MigrationTargets || Deprecation->NumMigrationTargets == 0) return false;
			const std::string Name = Property->NamePrivate.ToString();
			if (!Name.ends_with("_DEPRECATED")) return false;
			for (size_t Index = 0; Index < Deprecation->NumMigrationTargets; ++Index)
				if (!Deprecation->MigrationTargets[Index] || Deprecation->MigrationTargets[Index][0] == '\0') return false;
			return true;
		}

		auto ConstructGeneratedProperty(
			const FFieldVariant& Owner,
			const DurinCodeGen::FPropertyParamsBase* PropertyParams
		) -> FProperty*
		{
			check(PropertyParams);
			FProperty* Property = nullptr;
			const DurinCodeGen::EPropertyParamLayout ExpectedLayout = GetExpectedPropertyLayout(PropertyParams->Kind);
			if (PropertyParams->Layout != ExpectedLayout)
			{
				checkf(
					false,
					"PropertyRegistration.KindLayoutMismatch owner '{}' property '{}'.",
					GetGeneratedPropertyOwnerName(Owner),
					PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>"
				);
				return nullptr;
			}
			const bool bHasMutableAccessor = PropertyParams->MutableValueAccessor != nullptr;
			const bool bHasConstAccessor = PropertyParams->ConstValueAccessor != nullptr;
			if (bHasMutableAccessor != bHasConstAccessor || (bHasMutableAccessor && PropertyParams->Offset != 0))
			{
				checkf(false, "PropertyRegistration.AccessorPairMismatch owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
				return nullptr;
			}
			if ((PropertyParams->MetaData == nullptr) != (PropertyParams->NumMetaData == 0))
			{
				checkf(false, "PropertyRegistration.MetadataMismatch owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
				return nullptr;
			}
			if ((PropertyParams->LegacyNames == nullptr) != (PropertyParams->NumLegacyNames == 0))
			{
				checkf(false, "PropertyRegistration.LegacyNamesMismatch owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
				return nullptr;
			}

			switch (PropertyParams->Kind)
			{
			case DurinCodeGen::EPropertyGenFlags::Bool:
				Property = ConstructPlainProperty<bool, FBoolProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::String:
				Property = ConstructPlainProperty<std::string, FStringProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::Name:
				Property = ConstructPlainProperty<FName, FNameProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::Guid:
				Property = ConstructPlainProperty<FGuid, FGuidProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::Enum:
				{
					const auto* EnumParams = static_cast<const DurinCodeGen::FEnumPropertyParams*>(PropertyParams);
					DEnum* ReferencedEnum = EnumParams->EnumResolver ? EnumParams->EnumResolver() : nullptr;
					if (!ReferencedEnum || !IsMatchingEnumSize(ReferencedEnum->GetUnderlyingType(), ReferencedEnum->GetUnderlyingSize()))
					{
						checkf(false, "EnumPropertyRegistration.InvalidDescriptor owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
						return nullptr;
					}
					Property = new FEnumProperty(
						Owner,
						FName(PropertyParams->NameUTF8),
						EObjectFlags::NoFlags,
						PropertyParams->Flags,
						PropertyParams->ArrayDim,
						PropertyParams->Offset,
						ReferencedEnum->GetUnderlyingSize(),
						PropertyParams->Kind,
						nullptr,
						ReferencedEnum
					);
					requiref(InstallEnumValueOps(Property, ReferencedEnum->GetUnderlyingType()),
						"A reflected enum property has an unsupported underlying type.");
					break;
				}
			case DurinCodeGen::EPropertyGenFlags::Object:
				{
					const auto* ObjectParams = static_cast<const DurinCodeGen::FObjectPropertyParams*>(PropertyParams);
					DClass* ExpectedClass = ObjectParams->ClassResolver ? ObjectParams->ClassResolver() : nullptr;
					const bool bValidStorage = ObjectParams->Storage == DurinCodeGen::FObjectPropertyParams::EStorage::Raw
											   || ObjectParams->Storage == DurinCodeGen::FObjectPropertyParams::EStorage::ObjectPtr;
					if (!ExpectedClass || !bValidStorage || !IsValidValueOps(ObjectParams->ValueOps)
						|| ObjectParams->ValueOps.ValueSize > std::numeric_limits<uint16>::max()
						|| !ObjectParams->ReadObjectValue || !ObjectParams->WriteObjectValue)
					{
						checkf(false, "ObjectPropertyRegistration.InvalidDescriptor owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
						return nullptr;
					}
					Property = new FObjectProperty(
						Owner,
						FName(PropertyParams->NameUTF8),
						EObjectFlags::NoFlags,
						PropertyParams->Flags,
						PropertyParams->ArrayDim,
						PropertyParams->Offset,
						static_cast<uint16>(ObjectParams->ValueOps.ValueSize),
						PropertyParams->Kind,
						ExpectedClass,
						ObjectParams->Storage == DurinCodeGen::FObjectPropertyParams::EStorage::ObjectPtr,
						ObjectParams->ReadObjectValue,
						ObjectParams->WriteObjectValue
					);
					SetValueOps(Property, ObjectParams->ValueOps);
					break;
				}
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
				{
					const auto* SoftParams = static_cast<const DurinCodeGen::FSoftObjectPropertyParams*>(PropertyParams);
					const std::string OwnerName = GetGeneratedPropertyOwnerName(Owner);
					const char* PropertyName = PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>";
					DClass* ExpectedClass = SoftParams->ExpectedClassResolver ? SoftParams->ExpectedClassResolver() : nullptr;
					if (!ExpectedClass || !SoftParams->MutableSoftValueAccessor || !SoftParams->ConstSoftValueAccessor
						|| !IsValidValueOps(SoftParams->ValueOps)
						|| SoftParams->ValueOps.ValueSize > std::numeric_limits<uint16>::max())
					{
						checkf(false, "SoftObjectPropertyRegistration.InvalidDescriptor owner '{}' property '{}'.", OwnerName, PropertyName);
						return nullptr;
					}
					Property = new FSoftObjectProperty(
						Owner,
						FName(PropertyParams->NameUTF8),
						EObjectFlags::NoFlags,
						PropertyParams->Flags,
						PropertyParams->ArrayDim,
						PropertyParams->Offset,
						static_cast<uint16>(SoftParams->ValueOps.ValueSize),
						ExpectedClass,
						SoftParams->MutableSoftValueAccessor,
						SoftParams->ConstSoftValueAccessor
					);
					SetValueOps(Property, SoftParams->ValueOps);
					break;
				}
			case DurinCodeGen::EPropertyGenFlags::WeakObject:
				{
					const auto* WeakParams = static_cast<const DurinCodeGen::FWeakObjectPropertyParams*>(PropertyParams);
					const std::string OwnerName = GetGeneratedPropertyOwnerName(Owner);
					const char* PropertyName = PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>";
					DClass* ExpectedClass = WeakParams->ExpectedClassResolver ? WeakParams->ExpectedClassResolver() : nullptr;
					if (!HasTransientPropertyContext(Owner, PropertyParams->Flags))
					{
						checkf(false, "WeakObjectPropertyRegistration.NonTransient owner '{}' property '{}'.", OwnerName, PropertyName);
						return nullptr;
					}
					if (!ExpectedClass || !WeakParams->MutableWeakValueAccessor || !WeakParams->ConstWeakValueAccessor
						|| !IsValidValueOps(WeakParams->ValueOps)
						|| WeakParams->ValueOps.ValueSize > std::numeric_limits<uint16>::max())
					{
						checkf(false, "WeakObjectPropertyRegistration.InvalidDescriptor owner '{}' property '{}'.", OwnerName, PropertyName);
						return nullptr;
					}
					Property = new FWeakObjectProperty(
						Owner, FName(PropertyParams->NameUTF8), EObjectFlags::NoFlags,
						PropertyParams->Flags, PropertyParams->ArrayDim, PropertyParams->Offset,
						static_cast<uint16>(WeakParams->ValueOps.ValueSize), ExpectedClass,
						WeakParams->MutableWeakValueAccessor, WeakParams->ConstWeakValueAccessor);
					SetValueOps(Property, WeakParams->ValueOps);
					break;
				}
			case DurinCodeGen::EPropertyGenFlags::Struct:
				{
					const auto* StructParams = static_cast<const DurinCodeGen::FStructPropertyParams*>(PropertyParams);
					const std::string OwnerName = GetGeneratedPropertyOwnerName(Owner);
					const char* PropertyName = PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>";
					if (!StructParams->StructResolver)
					{
						checkf(false, "StructPropertyRegistration.MissingResolver owner '{}' property '{}'.", OwnerName, PropertyName);
						return nullptr;
					}
					DStruct* ReferencedStruct = StructParams->StructResolver();
					if (!ReferencedStruct)
					{
						checkf(false, "StructPropertyRegistration.NullDescriptor owner '{}' property '{}'.", OwnerName, PropertyName);
						return nullptr;
					}
					if (ReferencedStruct->PropertiesSize == 0 || ReferencedStruct->PropertiesSize > std::numeric_limits<uint16>::max())
					{
						checkf(false, "StructPropertyRegistration.InvalidSize owner '{}' property '{}'.", OwnerName, PropertyName);
						return nullptr;
					}
					const uint32 Alignment = ReferencedStruct->MinAlignment;
					if (Alignment == 0 || (Alignment & (Alignment - 1)) != 0)
					{
						checkf(false, "StructPropertyRegistration.InvalidAlignment owner '{}' property '{}'.", OwnerName, PropertyName);
						return nullptr;
					}
					const bool bHasMutableAccessor = PropertyParams->MutableValueAccessor != nullptr;
					const bool bHasConstAccessor = PropertyParams->ConstValueAccessor != nullptr;
					if (bHasMutableAccessor != bHasConstAccessor || (bHasMutableAccessor && PropertyParams->Offset != 0))
					{
						checkf(false, "StructPropertyRegistration.AccessorPairMismatch owner '{}' property '{}'.", OwnerName, PropertyName);
						return nullptr;
					}
					if ((PropertyParams->MetaData == nullptr) != (PropertyParams->NumMetaData == 0))
					{
						checkf(false, "StructPropertyRegistration.MetadataMismatch owner '{}' property '{}'.", OwnerName, PropertyName);
						return nullptr;
					}
					Property = new FStructProperty(
						Owner,
						FName(PropertyParams->NameUTF8),
						EObjectFlags::NoFlags,
						PropertyParams->Flags,
						PropertyParams->ArrayDim,
						PropertyParams->Offset,
						ReferencedStruct
					);
					break;
				}
			case DurinCodeGen::EPropertyGenFlags::Array:
				{
					const auto* ArrayParams = static_cast<const DurinCodeGen::FArrayPropertyParams*>(PropertyParams);
					const FArrayOps* Ops = ArrayParams->OpsResolver ? ArrayParams->OpsResolver() : nullptr;
					if (!IsValidArrayOps(Ops) || !ArrayParams->InnerParams || Ops->ContainerSize > std::numeric_limits<uint16>::max())
					{
						checkf(false, "ArrayPropertyRegistration.InvalidDescriptor owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
						return nullptr;
					}
					Property = new FArrayProperty(
						Owner,
						FName(PropertyParams->NameUTF8),
						EObjectFlags::NoFlags,
						PropertyParams->Flags,
						PropertyParams->ArrayDim,
						PropertyParams->Offset,
						static_cast<uint16>(Ops->ContainerSize),
						PropertyParams->Kind,
						nullptr,
						Ops
					);
					static_cast<FArrayProperty*>(Property)->SetInner(ConstructGeneratedProperty(FFieldVariant(Property), ArrayParams->InnerParams));
					break;
				}
			case DurinCodeGen::EPropertyGenFlags::Map:
				{
					const auto* MapParams = static_cast<const DurinCodeGen::FMapPropertyParams*>(PropertyParams);
					if (MapParams->KeyParams && MapParams->KeyParams->Kind == DurinCodeGen::EPropertyGenFlags::WeakObject)
					{
						checkf(false, "MapPropertyRegistration.WeakKeyUnsupported owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
						return nullptr;
					}
					const FMapOps* Ops = MapParams->OpsResolver ? MapParams->OpsResolver() : nullptr;
					if (!IsValidMapOps(Ops) || !MapParams->KeyParams || !MapParams->ValueParams || Ops->ContainerSize > std::numeric_limits<uint16>::max())
					{
						checkf(false, "MapPropertyRegistration.InvalidDescriptor owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
						return nullptr;
					}
					Property = new FMapProperty(
						Owner,
						FName(PropertyParams->NameUTF8),
						EObjectFlags::NoFlags,
						PropertyParams->Flags,
						PropertyParams->ArrayDim,
						PropertyParams->Offset,
						static_cast<uint16>(Ops->ContainerSize),
						PropertyParams->Kind,
						nullptr,
						Ops
					);
					FProperty* KeyProperty = ConstructGeneratedProperty(FFieldVariant(Property), MapParams->KeyParams);
					if (KeyProperty && KeyProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::WeakObject)
					{
						checkf(false, "MapPropertyRegistration.WeakKeyUnsupported owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
						return nullptr;
					}
					static_cast<FMapProperty*>(Property)->SetKeyProp(KeyProperty);
					static_cast<FMapProperty*>(Property)->SetValueProp(ConstructGeneratedProperty(FFieldVariant(Property), MapParams->ValueParams));
					break;
				}
			case DurinCodeGen::EPropertyGenFlags::Int8:
				Property = ConstructPlainProperty<int8, FNumericProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::Int16:
				Property = ConstructPlainProperty<int16, FNumericProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::Int32:
				Property = ConstructPlainProperty<int32, FNumericProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::Int64:
				Property = ConstructPlainProperty<int64, FNumericProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::UInt8:
				Property = ConstructPlainProperty<uint8, FNumericProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::UInt16:
				Property = ConstructPlainProperty<uint16, FNumericProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::UInt32:
				Property = ConstructPlainProperty<uint32, FNumericProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::UInt64:
				Property = ConstructPlainProperty<uint64, FNumericProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::Byte:
				Property = ConstructPlainProperty<std::byte, FProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::Blob:
				{
					const auto* BlobParams = static_cast<const DurinCodeGen::FBlobPropertyParams*>(PropertyParams);
					if (!IsValidValueOps(BlobParams->ValueOps)
						|| BlobParams->ValueOps.ValueSize > std::numeric_limits<uint16>::max())
					{
						checkf(false, "BlobPropertyRegistration.InvalidDescriptor owner '{}' property '{}'.",
							GetGeneratedPropertyOwnerName(Owner),
							PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
						return nullptr;
					}
					Property = new FProperty(
						Owner, FName(PropertyParams->NameUTF8), EObjectFlags::NoFlags,
						PropertyParams->Flags, PropertyParams->ArrayDim, PropertyParams->Offset,
						static_cast<uint16>(BlobParams->ValueOps.ValueSize), PropertyParams->Kind, nullptr);
					SetValueOps(Property, BlobParams->ValueOps);
					break;
				}
			case DurinCodeGen::EPropertyGenFlags::Float:
				Property = ConstructPlainProperty<float, FNumericProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::Double:
				Property = ConstructPlainProperty<double, FNumericProperty>(Owner, PropertyParams);
				break;
			case DurinCodeGen::EPropertyGenFlags::None:
				{
					const auto* GenericParams = static_cast<const DurinCodeGen::FGenericPropertyParams*>(PropertyParams);
					if (GenericParams->ElementSize == 0 || !IsValidValueOps(GenericParams->ValueOps)
						|| GenericParams->ValueOps.ValueSize > GenericParams->ElementSize)
					{
						checkf(false, "GenericPropertyRegistration.InvalidDescriptor owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
						return nullptr;
					}
					Property = new FProperty(
						Owner,
						FName(PropertyParams->NameUTF8),
						EObjectFlags::NoFlags,
						PropertyParams->Flags,
						PropertyParams->ArrayDim,
						PropertyParams->Offset,
						GenericParams->ElementSize,
						PropertyParams->Kind,
						nullptr
					);
					SetValueOps(Property, GenericParams->ValueOps);
					break;
				}
			default:
				check(false);
				return nullptr;
			}

			if (!ValidateTypedMetadataRegistration(Property, PropertyParams->TypedMetadata))
			{
				checkf(false, "PropertyRegistration.InvalidTypedMetadata owner '{}' property '{}'.",
					GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
				delete Property;
				return nullptr;
			}
			if (!ValidateDeprecationRegistration(Property, PropertyParams->Deprecation))
			{
				checkf(false, "PropertyRegistration.InvalidDeprecation owner '{}' property '{}'.",
					GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
				delete Property;
				return nullptr;
			}
			Property->SetValueAccessors(PropertyParams->MutableValueAccessor, PropertyParams->ConstValueAccessor);
			Property->SetTypedMetadata(PropertyParams->TypedMetadata);
			Property->SetDeprecation(PropertyParams->Deprecation);
			if (PropertyParams->Kind == DurinCodeGen::EPropertyGenFlags::Array)
			{
				const FArrayOps& Ops = static_cast<FArrayProperty*>(Property)->GetOps();
				Property->SetValueLifecycle(Ops.ContainerSize, Ops.ContainerAlignment, Ops.Initialize, Ops.Destroy);
			}
			else if (PropertyParams->Kind == DurinCodeGen::EPropertyGenFlags::Map)
			{
				const FMapOps& Ops = static_cast<FMapProperty*>(Property)->GetOps();
				Property->SetValueLifecycle(Ops.ContainerSize, Ops.ContainerAlignment, Ops.Initialize, Ops.Destroy);
			}
			for (size_t Index = 0; PropertyParams->MetaData && Index < PropertyParams->NumMetaData; ++Index)
			{
				const DurinCodeGen::FMetaDataPair& Pair = PropertyParams->MetaData[Index];
				if (Pair.Key && Pair.Key[0] != '\0') Property->SetMetaData(FName(Pair.Key), Pair.Value ? Pair.Value : "");
			}
			Property->SetLegacyNames(std::span(PropertyParams->LegacyNames, PropertyParams->NumLegacyNames));
			return Property;
		}
	} // namespace

	auto FObjectInitializer::Get() -> const FObjectInitializer&
	{
		static thread_local FObjectInitializer Instance;
		return Instance;
	}

	auto DObjectInit() -> void
	{
		check(IsFNameInitialized() && "FNameInit must run before reflected type initialization.");
		ProcessNewlyLoadedDObjects();
		DObjectProcessRegistrants();
		AttachCoreIntrinsicTypesToCppPackage();

		FModuleManager::Get().SetProcessLoadedObjectsCallback(ProcessNewlyLoadedDObjects);
		FModuleManager::Get().SetPreShutdownModuleCallback([](FName ModuleName) {
			ReleaseDStructDefaultsForModule(ModuleName);
			return ReleaseClassDefaultObjectsForModule(ModuleName);
		});
		FModuleManager::Get().StartProcessingNewlyLoadedObjects();

		auto& array = GDObjectArray;

		// CoreDObject owns its schema so Launch only coordinates subsystem initialization.
		const FYamlNodeView GCConfig = GetModuleConfig("CoreDObject").GetView("GC");
		FGarbageCollectionSettings GCSettings;
		GCSettings.bEnabled = GCConfig.GetView("Enabled").GetBool(true);
		GCSettings.IntervalSeconds = GCConfig.GetView("IntervalSeconds").GetDouble(60.0);
		GCSettings.MaxIntervalSeconds = GCConfig.GetView("MaxIntervalSeconds").GetDouble(600.0);
		GCSettings.IntervalBackoffMultiplier = GCConfig.GetView("IntervalBackoffMultiplier").GetDouble(2.0);
		GCSettings.PendingKillThreshold = GCConfig.GetView("PendingKillThreshold").GetUInt(128);
		GCSettings.ObjectGrowthThreshold = GCConfig.GetView("ObjectGrowthThreshold").GetUInt(1024);
		ConfigureAutomaticGarbageCollection(GCSettings, FTime::Seconds());
		GDObjectInitialized.store(true, std::memory_order_release);
	}

	auto IsDObjectInitialized() -> bool
	{
		return GDObjectInitialized.load(std::memory_order_acquire);
	}

	auto StaticAllocateObject(DClass* Class, DObject* Outer, FName Name, size_t Size) -> DObject*
	{
		// Allocate memory and zero it out
		DObject* Obj = nullptr;
		Obj = static_cast<DObject*>(::operator new(Size));
		assert(Obj && "Memory allocation failed");
		new (Obj) DObject(Class, Outer, Name);

		return Obj;
	}

	auto StaticConstructObject(const FStaticConstructObjectParameters& Params) -> DObject*
	{
		check(Params.Purpose != EObjectConstructionPurpose::ClassDefaultObject
			|| (Params.Class && Params.Outer == Params.Class));
		check(Params.Purpose != EObjectConstructionPurpose::ClassDefaultSubobject
			|| (Params.Outer && Params.Outer->IsClassDefaultObject()));
		DObject* Obj = StaticAllocateObject(Params.Class, Params.Outer, Params.Name, Params.Size);

		DClass* InClass = Params.Class;

		assert(InClass && InClass->ClassConstructor);

		FObjectInitializer ObjectInitializer;
		ObjectInitializer.Obj = Obj;
		ObjectInitializer.Class = InClass;
		ObjectInitializer.Outer = Params.Outer;
		ObjectInitializer.Name = Params.Name;
		ObjectInitializer.Purpose = Params.Purpose;

		InClass->ClassConstructor(ObjectInitializer);

		Obj->SetOuterPrivate(Params.Outer);
		Obj->AddObject(Params.Name);

		return Obj;
	}

	auto NewObject(DClass* Class, DObject* Outer, FName Name) -> DObject*
	{
		if (!CanConstructObjectOfClass(Class, DObject::StaticClass())) return nullptr;
		FStaticConstructObjectParameters Params;
		Params.Class = Class;
		Params.Outer = Outer;
		Params.Name = Name;
		Params.Size = Class->PropertiesSize;
		DObject* Object = StaticConstructObject(Params);
		DObjectForceRegistration(Object);
		return Object;
	}

	auto CanConstructObjectOfClass(const DClass* Class, const DClass* RequiredBaseClass) -> bool
	{
		return Class && RequiredBaseClass && Class->IsChildOf(RequiredBaseClass) && Class->ClassConstructor
			   && !Class->HasAnyClassFlags(EClassFlags::Abstract) && Class->PropertiesSize >= sizeof(DObject);
	}


	auto DurinCodeGen::ConstructDClass(const FClassParams& Params) -> DClass*
	{
		DClass* Class = Params.ClassNoRegisterFunc();

		DObjectForceRegistration(Class);
		Class->SetQualifiedName(FName(Params.QualifiedClassName));
		Class->SetTypeNames(
			Params.ShortClassName ? Params.ShortClassName : "",
			Params.DisplayName ? Params.DisplayName : "",
			Params.DefaultObjectName ? Params.DefaultObjectName : ""
		);
		Private::RegisterLegacyClassNames(
			Class, std::span(Params.LegacyNames, Params.NumLegacyNames));

		if (!Class->ChildProperties && Params.PropertyParams && Params.NumProperties > 0)
		{
			FField* LastProperty = nullptr;
			for (size_t Index = 0; Index < Params.NumProperties; ++Index)
			{
				const FPropertyParamsBase* PropertyParams = Params.PropertyParams[Index];
				FProperty* Property = ConstructGeneratedProperty(FFieldVariant(Class), PropertyParams);

				if (LastProperty)
				{
					LastProperty->Next = Property;
				}
				else
				{
					Class->ChildProperties = Property;
				}
				LastProperty = Property;
			}
		}
		Class->ValidateSerializedPropertyNames();
		Private::FGCReferenceSchemaRegistry::FinalizeAndAssemble(Class);
		return Class;
	}

	auto DurinCodeGen::ConstructDEnum(const FEnumParams& Params) -> DEnum*
	{
		DEnum* Enum = Params.EnumNoRegisterFunc();

		DObjectForceRegistration(Enum);
		Private::RegisterQualifiedEnum(Enum);
		Private::RegisterLegacyEnumNames(
			Enum, std::span(Params.LegacyNames, Params.NumLegacyNames));

		return Enum;
	}

	auto DurinCodeGen::ConstructDStruct(const FStructParams& Params) -> DStruct*
	{
		DStruct* Struct = Params.StructNoRegisterFunc();
		DObjectForceRegistration(Struct);
		Private::RegisterQualifiedStruct(Struct);
		Private::RegisterLegacyStructNames(
			Struct, std::span(Params.LegacyNames, Params.NumLegacyNames));
		Struct->InitializeOps(Params.Ops);
		if (!Struct->ChildProperties && Params.PropertyParams && Params.NumProperties > 0)
		{
			FField* LastProperty = nullptr;
			for (size_t Index = 0; Index < Params.NumProperties; ++Index)
			{
				FProperty* Property = ConstructGeneratedProperty(FFieldVariant(Struct), Params.PropertyParams[Index]);
				if (LastProperty)
					LastProperty->Next = Property;
				else
					Struct->ChildProperties = Property;
				LastProperty = Property;
			}
		}
		Struct->ValidateSerializedPropertyNames();
		Private::FGCReferenceSchemaRegistry::FinalizeAndAssemble(Struct);
		if (!Private::IsDStructRegistrationBatchActive())
		{
			const std::array Batch{Struct};
			(void)Private::CreateDStructDefaultsForBatch(Batch);
		}
		return Struct;
	}
} // namespace Durin
