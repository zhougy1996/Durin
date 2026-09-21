#include "DObject/Archive.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "GCReferenceSchema.h"

namespace Durin
{
	auto FObjectArchive::FailValidation(const FObjectValidationError& Error) -> void
	{
		if (IsError()) return;
		ValueFailureCause = Error;
		Fail(EArchiveFailureCode::InvalidData, ToString(Error));
	}
	auto FObjectArchive::FailPropertyValue(const FPropertyValueError& Error) -> void
	{
		if (IsError() || !Error.HasError()) return;
		ValueFailureCause = Error;
		Fail(EArchiveFailureCode::UnsupportedOperation, ToString(Error));
	}
	auto FObjectArchive::FailMapKey(const FReflectedMapKeyError& Error) -> void
	{
		if (IsError() || !Error.HasError()) return;
		ValueFailureCause = Error;
		Fail(EArchiveFailureCode::UnsupportedType, ToString(Error));
	}

	auto FObjectKey::SerializeForSnapshot(FArchive& Archive) -> void
	{
		Archive << ObjectIndex << ObjectSerialNumber;
	}

	auto FPropertyValueSnapshotPayload::TryGetAllocatedSize(size_t& OutBytes) const -> bool
	{
		const size_t ByteCapacity = Bytes.capacity();
		if (ReferencedObjectKeys.capacity() > std::numeric_limits<size_t>::max()
			/ sizeof(FObjectKey)) return false;
		const size_t HandleBytes = ReferencedObjectKeys.capacity() * sizeof(FObjectKey);
		if (ByteCapacity > std::numeric_limits<size_t>::max() - HandleBytes) return false;
		OutBytes = ByteCapacity + HandleBytes;
		return true;
	}

	auto FPropertyValueSnapshotPayload::operator==(
		const FPropertyValueSnapshotPayload& Other) const -> bool
	{
		if (this == &Other) return true;
		if (Property != Other.Property) return false;
		if (!Property) return true;

		FReflectedValueStorage Left;
		FReflectedValueStorage Right;
		if (Left.DefaultConstruct(Property, 0)
			&& Right.DefaultConstruct(Property, 0)
			&& RestorePropertyValuePayload(Property, Left.GetContainer(), 0, *this)
			&& RestorePropertyValuePayload(Property, Right.GetContainer(), 0, Other))
		{
			return ArePropertyValuesIdentical(
				Property, Left.GetContainer(), 0, Right.GetContainer(), 0);
		}
		return Bytes == Other.Bytes
			&& ReferencedObjectKeys == Other.ReferencedObjectKeys;
	}

	namespace
	{
		constexpr uint32 ObjectGraphMagic = 0x4E524F44; // DORN
		constexpr uint32 ObjectGraphVersion = 2;
		constexpr uint64 MaximumSoftObjectPathBytes = 1024 * 1024;

		auto ShouldSerializeTransientWeakProperty(const FArchive& Ar, const FProperty* Property) -> bool
		{
			if (!Property || (Ar.GetPurpose() != EArchivePurpose::ObjectGraph
				&& Ar.GetPurpose() != EArchivePurpose::Duplicate)) return false;
			std::unordered_set<const DStruct*> VisitedStructs;
			std::function<bool(const FProperty*)> ContainsWeak = [&](const FProperty* Candidate) {
				if (!Candidate) return false;
				switch (Candidate->GetKind())
				{
				case DurinCodeGen::EPropertyGenFlags::WeakObject:
					return true;
				case DurinCodeGen::EPropertyGenFlags::Struct:
				{
					const DStruct* Struct = static_cast<const FStructProperty*>(Candidate)->GetStruct();
					if (!Struct || !VisitedStructs.emplace(Struct).second) return false;
					bool bContainsWeak = false;
					Struct->ForEachProperty([&](FProperty* Field) {
						if (!bContainsWeak) bContainsWeak = ContainsWeak(Field);
					}, false);
					return bContainsWeak;
				}
				case DurinCodeGen::EPropertyGenFlags::Array:
					return ContainsWeak(static_cast<const FArrayProperty*>(Candidate)->GetInner());
				case DurinCodeGen::EPropertyGenFlags::Map:
					return ContainsWeak(static_cast<const FMapProperty*>(Candidate)->GetValueProp());
				default:
					return false;
				}
			};
			return ContainsWeak(Property);
		}

		auto ShouldSerializeReflectedProperty(const FArchive& Ar, const FProperty* Property) -> bool
		{
			if (!Property) return false;
			if (Property->HasAnyPropertyFlags(EPropertyFlags::Transient)
				&& !ShouldSerializeTransientWeakProperty(Ar, Property)) return false;
			if (Property->IsDeprecated()
				&& (Ar.IsSaving()
					|| !Ar.HasCapability(EArchiveCapability::CustomVersions))) return false;
			return !Ar.IsFilterEditorOnly()
				|| !Property->HasAnyPropertyFlags(EPropertyFlags::EditorOnly);
		}

		auto ResolvePropertySaveValue(
			FArchive& Ar,
			FProperty& Property,
			const void* Container,
			uint32 ArrayIndex,
			FArchivePropertySaveValue& OutValue) -> EArchivePropertySaveDisposition
		{
			OutValue = {Container, ArrayIndex};
			if (!Ar.IsSaving()) return EArchivePropertySaveDisposition::LiveValue;
			auto* ObjectArchive = dynamic_cast<FObjectArchive*>(&Ar);
			return ObjectArchive
				? ObjectArchive->ResolvePropertySaveValue(
					Property, Container, ArrayIndex, OutValue)
				: EArchivePropertySaveDisposition::LiveValue;
		}

		auto MakeLogicalTypeDescriptor(FProperty* Property) -> FArchiveLogicalTypeDescriptor
		{
			if (!Property) return FArchiveLogicalTypeDescriptor::Bytes();
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Bool: return FArchiveLogicalTypeDescriptor::Scalar(false, 8);
			case DurinCodeGen::EPropertyGenFlags::Int8: return FArchiveLogicalTypeDescriptor::Scalar(true, 8);
			case DurinCodeGen::EPropertyGenFlags::Int16: return FArchiveLogicalTypeDescriptor::Scalar(true, 16);
			case DurinCodeGen::EPropertyGenFlags::Int32: return FArchiveLogicalTypeDescriptor::Scalar(true, 32);
			case DurinCodeGen::EPropertyGenFlags::Int64: return FArchiveLogicalTypeDescriptor::Scalar(true, 64);
			case DurinCodeGen::EPropertyGenFlags::UInt8: return FArchiveLogicalTypeDescriptor::Scalar(false, 8);
			case DurinCodeGen::EPropertyGenFlags::UInt16: return FArchiveLogicalTypeDescriptor::Scalar(false, 16);
			case DurinCodeGen::EPropertyGenFlags::UInt32: return FArchiveLogicalTypeDescriptor::Scalar(false, 32);
			case DurinCodeGen::EPropertyGenFlags::UInt64: return FArchiveLogicalTypeDescriptor::Scalar(false, 64);
			case DurinCodeGen::EPropertyGenFlags::Float: return FArchiveLogicalTypeDescriptor::Scalar(true, 32, true);
			case DurinCodeGen::EPropertyGenFlags::Double: return FArchiveLogicalTypeDescriptor::Scalar(true, 64, true);
			case DurinCodeGen::EPropertyGenFlags::String: return FArchiveLogicalTypeDescriptor::String();
			case DurinCodeGen::EPropertyGenFlags::Name: return FArchiveLogicalTypeDescriptor::Name();
			case DurinCodeGen::EPropertyGenFlags::Guid: return FArchiveLogicalTypeDescriptor::Guid();
			case DurinCodeGen::EPropertyGenFlags::Byte: return FArchiveLogicalTypeDescriptor::Bytes();
			case DurinCodeGen::EPropertyGenFlags::Blob: return FArchiveLogicalTypeDescriptor::Bytes();
			case DurinCodeGen::EPropertyGenFlags::BulkData: return FArchiveLogicalTypeDescriptor::BulkData();
			case DurinCodeGen::EPropertyGenFlags::Object:
				return FArchiveLogicalTypeDescriptor::Object(Property->GetReferencedClass()
					? Property->GetReferencedClass()->GetQualifiedName() : FName());
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
				return FArchiveLogicalTypeDescriptor::SoftObject(Property->GetReferencedClass()
					? Property->GetReferencedClass()->GetQualifiedName() : FName());
			case DurinCodeGen::EPropertyGenFlags::WeakObject:
				return FArchiveLogicalTypeDescriptor::WeakObject(Property->GetReferencedClass()
					? Property->GetReferencedClass()->GetQualifiedName() : FName());
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				auto* Struct = static_cast<FStructProperty*>(Property)->GetStruct();
				return FArchiveLogicalTypeDescriptor::Struct(Struct ? Struct->GetQualifiedName() : FName());
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
				return FArchiveLogicalTypeDescriptor::Array(MakeLogicalTypeDescriptor(static_cast<FArrayProperty*>(Property)->GetInner()));
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				auto* Map = static_cast<FMapProperty*>(Property);
				return FArchiveLogicalTypeDescriptor::Map(
					MakeLogicalTypeDescriptor(Map->GetKeyProp()), MakeLogicalTypeDescriptor(Map->GetValueProp()));
			}
			case DurinCodeGen::EPropertyGenFlags::Enum:
			{
				auto* EnumProperty = static_cast<FEnumProperty*>(Property);
				DEnum* Enum = EnumProperty->GetEnum();
				const auto Underlying = EnumProperty->GetUnderlyingType();
				const bool bSigned = Underlying == DurinCodeGen::EEnumUnderlyingType::Int8
					|| Underlying == DurinCodeGen::EEnumUnderlyingType::Int16
					|| Underlying == DurinCodeGen::EEnumUnderlyingType::Int32
					|| Underlying == DurinCodeGen::EEnumUnderlyingType::Int64;
				return FArchiveLogicalTypeDescriptor::Enum(
					Enum ? Enum->GetQualifiedName() : FName(), bSigned,
					static_cast<uint8>(Property->GetElementSize() * 8));
			}
			default: return FArchiveLogicalTypeDescriptor::Bytes();
			}
		}

		auto MakeFieldDescriptor(FProperty* Property, FName FallbackOwner = {})
			-> FArchiveFieldDescriptor
		{
			FArchiveLogicalTypeDescriptor LogicalType = MakeLogicalTypeDescriptor(Property);
			DStruct* DeclaringStruct = Property
				? Cast<DStruct>(Property->Owner.ToDObject()) : nullptr;
			DClass* DeclaringClass = Property
				? Cast<DClass>(Property->Owner.ToDObject()) : nullptr;
			if (Property && Property->GetArrayDim() > 1)
				LogicalType = FArchiveLogicalTypeDescriptor::FixedArray(
					std::move(LogicalType), Property->GetArrayDim());
			return {
				.DeclaringType = DeclaringClass ? DeclaringClass->GetQualifiedName()
					: (DeclaringStruct ? DeclaringStruct->GetQualifiedName()
						: FallbackOwner),
				.Name = Property ? Property->NamePrivate : FName(),
				.LogicalType = std::move(LogicalType),
				.ArrayDimension = Property ? static_cast<uint32>(Property->GetArrayDim()) : 1u,
				.PropertyFlags = Property ? Property->GetPropertyFlags() : EPropertyFlags::None
			};
		}

		auto WriteString(FArchive& Ar, std::string& Value) -> void { Ar << Value; }

		auto FindClassByName(const std::string& ClassName) -> DClass*
		{
			if (ClassName == "DObject")
			{
				return DObject::StaticClass();
			}

			for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates))
			{
				auto* Class = Cast<DClass>(Object);
				if (Class && Class->GetName() == ClassName)
				{
					return Class;
				}
			}
			return nullptr;
		}

		auto GetSerializableClass(DObject* Object) -> DClass*
		{
			if (!Object)
			{
				return nullptr;
			}

			DClass* Class = Object->GetClass();
			if (!Class || Class->GetClass() != DClass::StaticClass())
			{
				return DObject::StaticClass();
			}
			return Class;
		}

		auto CheckPropertyValueResult(FArchive& Ar, const std::expected<void, FPropertyValueError>& Result) -> bool
		{
			if (Result) return true;
			if (auto* ObjectArchive = dynamic_cast<FObjectArchive*>(&Ar))
				ObjectArchive->FailPropertyValue(Result.error());
			else Ar.Fail(EArchiveFailureCode::UnsupportedOperation, ToString(Result.error()));
			return false;
		}

		auto SerializePropertyValue(FArchive& Ar, FProperty* Property, void* Container, uint32 ArrayIndex, bool bIncludeRawObjectReferences = false) -> void;

		struct FArchiveArrayVisitContext
		{
			FArchive& Archive;
			FProperty* Inner;
			bool bIncludeRawObjectReferences;
		};

		auto SerializeArrayElement(void* RawContext, uint64 Index, const void* Element) -> bool
		{
			auto& Context = *static_cast<FArchiveArrayVisitContext*>(RawContext);
			auto ElementScope = EnterArchiveArrayElement(Context.Archive, Index);
			SerializePropertyValue(Context.Archive, Context.Inner, const_cast<void*>(Element), 0,
				Context.bIncludeRawObjectReferences);
			return !Context.Archive.IsError();
		}

		struct FArchiveMapEntry
		{
			FByteBuffer Token;
			const void* Key = nullptr;
			const void* Value = nullptr;
		};

		struct FArchiveMapVisitContext
		{
			FArchive& Archive;
			FMapProperty* Property;
			bool bIncludeRawObjectReferences;
			bool bCanonical;
			uint64 NextIndex = 0;
			std::vector<FArchiveMapEntry> Entries;
		};

		auto CollectOrSerializeMapEntry(void* RawContext, const void* Key, const void* Value) -> bool
		{
			auto& Context = *static_cast<FArchiveMapVisitContext*>(RawContext);
			if (Context.bCanonical)
			{
				FArchiveMapEntry Entry;
				const auto Result = BuildCanonicalMapKeyToken(Context.Property->GetKeyProp(), Key, 0, Entry.Token);
				if (!Result)
				{
					if (auto* ObjectArchive = dynamic_cast<FObjectArchive*>(&Context.Archive))
						ObjectArchive->FailMapKey(Result.error());
					else Context.Archive.SetError(ToString(Result.error()));
					return false;
				}
				Entry.Key = Key;
				Entry.Value = Value;
				Context.Entries.push_back(std::move(Entry));
				return true;
			}
			const uint64 Index = Context.NextIndex++;
			{
				auto KeyScope = EnterArchiveMapKey(Context.Archive, Index);
				SerializePropertyValue(Context.Archive, Context.Property->GetKeyProp(), const_cast<void*>(Key), 0,
					Context.bIncludeRawObjectReferences);
			}
			{
				auto ValueScope = EnterArchiveMapValue(Context.Archive, Index);
				SerializePropertyValue(Context.Archive, Context.Property->GetValueProp(), const_cast<void*>(Value), 0,
					Context.bIncludeRawObjectReferences);
			}
			return !Context.Archive.IsError();
		}

		auto SerializePropertyValue(FArchive& Ar, FProperty* Property, void* Container, uint32 ArrayIndex, bool bIncludeRawObjectReferences) -> void
		{
			if (Ar.IsError()) return;
			if (!Ar.IsCurrentFieldAvailable()) return;
			if (!Property || !Container)
			{
				Ar.SetError("Invalid reflected property serialization request.");
				return;
			}
			NotifyArchiveReflectedPropertyValue(Ar, *Property, Container, ArrayIndex);
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Bool: Ar << *static_cast<bool*>(Property->GetValuePtr(Container, ArrayIndex)); break;
			case DurinCodeGen::EPropertyGenFlags::Int8: Ar << *static_cast<int8*>(Property->GetValuePtr(Container, ArrayIndex)); break;
			case DurinCodeGen::EPropertyGenFlags::Int16: Ar << *static_cast<int16*>(Property->GetValuePtr(Container, ArrayIndex)); break;
			case DurinCodeGen::EPropertyGenFlags::Int32: Ar << *static_cast<int32*>(Property->GetValuePtr(Container, ArrayIndex)); break;
			case DurinCodeGen::EPropertyGenFlags::Int64: Ar << *static_cast<int64*>(Property->GetValuePtr(Container, ArrayIndex)); break;
			case DurinCodeGen::EPropertyGenFlags::UInt8: Ar << *static_cast<uint8*>(Property->GetValuePtr(Container, ArrayIndex)); break;
			case DurinCodeGen::EPropertyGenFlags::UInt16: Ar << *static_cast<uint16*>(Property->GetValuePtr(Container, ArrayIndex)); break;
			case DurinCodeGen::EPropertyGenFlags::UInt32: Ar << *static_cast<uint32*>(Property->GetValuePtr(Container, ArrayIndex)); break;
			case DurinCodeGen::EPropertyGenFlags::UInt64: Ar << *static_cast<uint64*>(Property->GetValuePtr(Container, ArrayIndex)); break;
			case DurinCodeGen::EPropertyGenFlags::Float: Ar << *static_cast<float*>(Property->GetValuePtr(Container, ArrayIndex)); break;
			case DurinCodeGen::EPropertyGenFlags::Double: Ar << *static_cast<double*>(Property->GetValuePtr(Container, ArrayIndex)); break;
			case DurinCodeGen::EPropertyGenFlags::Enum:
			{
				using U = DurinCodeGen::EEnumUnderlyingType;
				switch (static_cast<FEnumProperty*>(Property)->GetUnderlyingType())
				{
				case U::Int8: Ar << *static_cast<int8*>(Property->GetValuePtr(Container, ArrayIndex)); break;
				case U::Int16: Ar << *static_cast<int16*>(Property->GetValuePtr(Container, ArrayIndex)); break;
				case U::Int32: Ar << *static_cast<int32*>(Property->GetValuePtr(Container, ArrayIndex)); break;
				case U::Int64: Ar << *static_cast<int64*>(Property->GetValuePtr(Container, ArrayIndex)); break;
				case U::UInt8: Ar << *static_cast<uint8*>(Property->GetValuePtr(Container, ArrayIndex)); break;
				case U::UInt16: Ar << *static_cast<uint16*>(Property->GetValuePtr(Container, ArrayIndex)); break;
				case U::UInt32: Ar << *static_cast<uint32*>(Property->GetValuePtr(Container, ArrayIndex)); break;
				case U::UInt64: Ar << *static_cast<uint64*>(Property->GetValuePtr(Container, ArrayIndex)); break;
				default: Ar.Fail(EArchiveFailureCode::UnsupportedType, "Enum underlying width is unsupported."); break;
				}
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::String:
			{
				auto* StringProperty = static_cast<FStringProperty*>(Property);
				std::string* Value = StringProperty->GetStringValuePtr(Container, ArrayIndex);
				Ar << *Value;
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Name:
			{
				auto* NameProperty = static_cast<FNameProperty*>(Property);
				FName* Value = NameProperty->GetNameValuePtr(Container, ArrayIndex);
				Ar << *Value;
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Guid:
			{
				auto* GuidProperty = static_cast<FGuidProperty*>(Property);
				FGuid* Value = GuidProperty->GetGuidValuePtr(Container, ArrayIndex);
				FGuid SerializedValue = Ar.IsSaving() ? *Value : FGuid();
				Ar << SerializedValue;
				if (Ar.IsLoading() && !Ar.IsError()) *Value = SerializedValue;
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Byte:
			{
				auto* Value = static_cast<std::byte*>(Property->GetValuePtr(Container, ArrayIndex));
				std::byte Encoded = Ar.IsSaving() ? *Value : std::byte{};
				Ar.SerializeRawBytes(std::span(&Encoded, 1));
				if (Ar.IsLoading() && !Ar.IsError()) *Value = Encoded;
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Blob:
			{
				auto* Value = static_cast<FByteBuffer*>(
					Property->GetValuePtr(Container, ArrayIndex));
				Ar.SerializeByteBlob(*Value);
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::BulkData:
			{
				if (!Property->SerializeBulkDataValue(
					Ar, Property->GetValuePtr(Container, ArrayIndex)))
					Ar.Fail(EArchiveFailureCode::UnsupportedType,
						"Bulk data property has no semantic Archive operation.");
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Object:
			{
				auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
				if (!bIncludeRawObjectReferences && !ObjectProperty->IsObjectPtrWrapper())
				{
					if (Ar.GetPurpose() == EArchivePurpose::AuthoredPackage
						|| (Ar.GetPurpose() == EArchivePurpose::Discovery
							&& Ar.HasCapability(EArchiveCapability::CanonicalMapOrder)))
						Ar.Fail(EArchiveFailureCode::UnsupportedType,
							"Raw object pointer properties are not serializable in authored packages.");
					break;
				}
				DObject* ReferencedObject = ObjectProperty->GetObjectPropertyValue(Container, ArrayIndex);
				SerializeArchiveObjectReference(Ar, ReferencedObject);
				if (Ar.IsLoading() && !Ar.IsError())
				{
					ObjectProperty->SetObjectPropertyValue(Container, ReferencedObject, ArrayIndex);
				}
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
			{
				auto* SoftProperty = static_cast<FSoftObjectProperty*>(Property);
				FSoftObjectPtr* Value = SoftProperty->GetSoftObjectPtr(Container, ArrayIndex);
				if (!Value)
				{
					Ar.SetError("Soft object property has no typed value accessor.");
					break;
				}
				FObjectPath Path = Ar.IsSaving() ? Value->GetPath() : FObjectPath();
				SerializeArchiveSoftObjectValue(Ar, Path);
				if (Ar.IsLoading() && !Ar.IsError())
				{
					if (!Path.IsValid()) Value->Reset();
					else Value->SetPath(std::move(Path));
				}
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::WeakObject:
			{
				auto* WeakProperty = static_cast<FWeakObjectProperty*>(Property);
				FWeakObjectPtr* Value = WeakProperty->GetWeakObjectPtr(Container, ArrayIndex);
				if (!Value)
				{
					Ar.SetError("Weak object property has no typed value accessor.");
					break;
				}
				SerializeArchiveWeakObjectReference(Ar, *Value);
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				auto* StructProperty = static_cast<FStructProperty*>(Property);
				DStruct* Struct = StructProperty->GetStruct();
				if (!Struct)
				{
					Ar.SetError("Struct property has no reflected type.");
					break;
				}
				auto SerializeStructValue = [&](void* StructValue) {
					if (Struct->HasSerializer())
					{
						if (Ar.IsFilterEditorOnly())
						{
							bool bContainsEditorOnly = false;
							Struct->ForEachProperty([&](FProperty* Field) {
								bContainsEditorOnly = bContainsEditorOnly || (Field
									&& Field->HasAnyPropertyFlags(EPropertyFlags::EditorOnly));
							}, false);
							if (bContainsEditorOnly)
							{
								Ar.Fail(EArchiveFailureCode::MalformedSerializer,
									"CustomStructEditorOnlyFilterRequired: a custom struct serializer bypasses automatic EditorOnly filtering.");
								return;
							}
						}
						Struct->GetOps().Serialize(Ar, StructValue);
						return;
					}
					if (Ar.GetPurpose() == EArchivePurpose::AuthoredPackage
						&& !Struct->HasCompleteAuthoredFields())
					{
						Ar.Fail(EArchiveFailureCode::MalformedSerializer,
							"CustomStructCodecRequired: authored struct fallback does not declare AuthoredFieldsComplete.");
						return;
					}
					Struct->ForEachProperty([&](FProperty* Field) {
						if (Ar.IsError() || !ShouldSerializeReflectedProperty(Ar, Field)) return;
						FArchivePropertySaveValue EffectiveValue;
						const EArchivePropertySaveDisposition Disposition = ResolvePropertySaveValue(
							Ar, *Field, StructValue, 0, EffectiveValue);
						if (Disposition == EArchivePropertySaveDisposition::Omit) return;
						auto FieldScope = EnterArchiveField(Ar, MakeFieldDescriptor(
							Field, Struct->GetQualifiedName()));
						for (uint32 Index = 0; Index < Field->GetArrayDim() && !Ar.IsError(); ++Index)
						{
							EffectiveValue = {StructValue, Index};
							const EArchivePropertySaveDisposition ElementDisposition = ResolvePropertySaveValue(
								Ar, *Field, StructValue, Index, EffectiveValue);
							if (ElementDisposition == EArchivePropertySaveDisposition::Omit)
							{
								Ar.Fail(EArchiveFailureCode::MalformedSerializer,
									"A reflected fixed-array property cannot be partially omitted.");
								return;
							}
							auto FixedScope = Field->GetArrayDim() > 1
								? EnterArchiveFixedArrayElement(Ar, Index) : FArchivePathScope();
							SerializePropertyValue(
								Ar, Field, const_cast<void*>(EffectiveValue.Container),
								EffectiveValue.ArrayIndex, bIncludeRawObjectReferences);
						}
					}, false);
				};
				if (Ar.IsSaving())
				{
					SerializeStructValue(Property->GetValuePtr(Container, ArrayIndex));
					break;
				}

				if (!Struct->CanDefaultConstruct() || !Struct->CanDestroy()
					|| !Struct->CanCopyAssign())
				{
					Ar.Fail(EArchiveFailureCode::UnsupportedOperation, std::format(
						"DStructOperationUnavailable: transactional loading requires "
						"DefaultConstruct, Destroy, and CopyAssign for '{}'.",
						Struct->GetQualifiedName()));
					break;
				}
				std::optional<FStructProperty> DetachedProperty;
				const FProperty* StorageProperty = Property;
				if (Property->HasValueAccessors())
				{
					DetachedProperty.emplace(
						FFieldVariant(), Property->NamePrivate, EObjectFlags::Transient,
						EPropertyFlags::Transient, 1, 0, Struct);
					StorageProperty = &*DetachedProperty;
				}
				const EArchiveStructBaseline Baseline = Ar.GetStructBaseline();
				if (Ar.IsError()) break;
				FReflectedValueStorage Storage;
				if (!CheckPropertyValueResult(Ar, Storage.DefaultConstruct(StorageProperty, 0)))
				{
					break;
				}
				const void* BaselineValue = Baseline == EArchiveStructBaseline::Parent
					? Property->GetValuePtr(Container, ArrayIndex) : nullptr;
				if (Baseline == EArchiveStructBaseline::TypeDefault)
				{
					BaselineValue = Struct->GetDefaultValue();
					if (!BaselineValue || !Struct->HasCompleteAuthoredFields() || Struct->HasSerializer())
					{
						Ar.Fail(EArchiveFailureCode::UnsupportedOperation, "Struct type default is unavailable.");
						break;
					}
				}
				if (BaselineValue && !CheckPropertyValueResult(Ar, StorageProperty->CopyAssignValue(Storage.GetValue(), BaselineValue)))
				{
					break;
				}
				SerializeStructValue(Storage.GetValue());
				if (Ar.IsError()) break;
				if (Struct->HasPostDeserialize())
				{
					const FArchiveFormatVersion* DastVersion =
						Ar.GetVersionContext().FindFormat(FName("DAST"));
					FDStructPostDeserializeContext Context{
						.Source = Ar.GetPurpose() == EArchivePurpose::AuthoredPackage
							? EDStructDeserializeSource::AuthoredAsset
							: EDStructDeserializeSource::RuntimeArchive,
						.SourceVersion = DastVersion ? DastVersion->Version : 0,
						.VersionContext = &Ar.GetVersionContext(),
						.LoadedDeprecatedProperties = Ar.GetLoadedDeprecatedProperties(
							Struct->GetQualifiedName())};
					auto Validation = Struct->GetOps().PostDeserialize(Storage.GetValue(), Context);
					if (!Validation)
					{
						Validation.error().StructName = Struct->GetQualifiedName().ToString();
						Validation.error().SourceVersion = Context.SourceVersion;
						if (auto* ObjectArchive = dynamic_cast<FObjectArchive*>(&Ar)) ObjectArchive->FailValidation(Validation.error());
						else Ar.Fail(EArchiveFailureCode::InvalidData, ToString(Validation.error()));
						break;
					}
				}
				(void)CheckPropertyValueResult(Ar, Property->CopyAssignValue(Property->GetValuePtr(Container, ArrayIndex), Storage.GetValue()));
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				auto* ArrayProperty = static_cast<FArrayProperty*>(Property);
				FProperty* Inner = ArrayProperty->GetInner();
				if (!Inner || !ArrayProperty->HasArrayOps()
					|| !ArrayProperty->HasCapability(EArrayOpsFlags::Count))
				{
					Ar.Fail(EArchiveFailureCode::UnsupportedOperation,
						"ArrayOperationUnavailable: Count is required.");
					break;
				}

				uint64 Num = 0;
				if (Ar.IsSaving() && ArrayProperty->GetNum(Container, Num, ArrayIndex) != EContainerOpResult::Success)
				{
					Ar.SetError("ArrayOperationFailed: Count failed.");
					break;
				}
				Ar << Num;
				if (Ar.IsError()) break;
				if (Ar.IsLoading() && Num > 10000000)
				{
					Ar.SetError("Array element count exceeds the supported limit.");
					break;
				}
				if (Ar.IsSaving())
				{
					if (!ArrayProperty->HasCapability(EArrayOpsFlags::ConstTraversal))
					{
						Ar.Fail(EArchiveFailureCode::UnsupportedOperation,
							"ArrayOperationUnavailable: ConstTraversal is required for save.");
						break;
					}
					FArchiveArrayVisitContext Context{Ar, Inner, bIncludeRawObjectReferences};
					if (ArrayProperty->VisitElements(Container, &SerializeArrayElement, &Context, ArrayIndex)
						!= EContainerOpResult::Success && !Ar.IsError())
						Ar.SetError("ArrayOperationFailed: ConstTraversal failed.");
					break;
				}

				const FArrayOps& Ops = ArrayProperty->GetOps();
				if (!ArrayProperty->HasCapability(EArrayOpsFlags::DetachedStorage | EArrayOpsFlags::TransactionalCommit
					| EArrayOpsFlags::RandomAccess) || (Num > 0 && !ArrayProperty->HasCapability(EArrayOpsFlags::DefaultGrow)))
				{
					Ar.Fail(EArchiveFailureCode::UnsupportedOperation,
						"ArrayOperationUnavailable: transactional load requires DetachedStorage, RandomAccess, DefaultGrow, and TransactionalCommit.");
					break;
				}
				FDetachedContainerStorage Detached;
				EContainerOpResult Result = Detached.Create(Ops);
				if (Result == EContainerOpResult::Success) Result = Ops.Resize(Detached.Get(), Num);
				if (Result != EContainerOpResult::Success)
				{
					Ar.SetError(std::format("ArrayOperationFailed: detached allocation/resize returned {}.", static_cast<uint32>(Result)));
					break;
				}
				for (uint64 Index = 0; Index < Num && !Ar.IsError(); ++Index)
				{
					void* Element = nullptr;
					Result = Ops.GetMutableAt(Detached.Get(), Index, &Element);
					if (Result != EContainerOpResult::Success)
					{
						Ar.SetError(std::format("ArrayOperationFailed: element {} access returned {}.", Index, static_cast<uint32>(Result)));
						break;
					}
					auto ElementScope = EnterArchiveArrayElement(Ar, Index);
					SerializePropertyValue(Ar, Inner, Element, 0, bIncludeRawObjectReferences);
				}
				if (!Ar.IsError())
				{
					Result = Ops.Commit(ArrayProperty->GetValuePtr(Container, ArrayIndex), Detached.Get());
					if (Result != EContainerOpResult::Success)
						Ar.SetError(std::format("ArrayOperationFailed: Commit returned {}.", static_cast<uint32>(Result)));
				}
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				auto* MapProperty = static_cast<FMapProperty*>(Property);
				if (!MapProperty->HasMapOps() || !MapProperty->GetKeyProp() || !MapProperty->GetValueProp()
					|| !MapProperty->HasCapability(EMapOpsFlags::Count))
				{
					Ar.Fail(EArchiveFailureCode::UnsupportedOperation,
						"MapOperationUnavailable: Count is required.");
					break;
				}
				uint64 Num = 0;
				if (Ar.IsSaving() && MapProperty->GetNum(Container, Num, ArrayIndex) != EContainerOpResult::Success)
				{
					Ar.SetError("MapOperationFailed: Count failed.");
					break;
				}
				Ar << Num;
				if (Ar.IsError()) break;
				if (Ar.IsLoading() && Num > 10000000)
				{
					Ar.SetError("Map element count exceeds the supported limit.");
					break;
				}
				if (Ar.IsSaving())
				{
					if (!MapProperty->HasCapability(EMapOpsFlags::ConstTraversal))
					{
						Ar.Fail(EArchiveFailureCode::UnsupportedOperation,
							"MapOperationUnavailable: ConstTraversal is required for save.");
						break;
					}
					FArchiveMapVisitContext Context{Ar, MapProperty, bIncludeRawObjectReferences,
						Ar.HasCapability(EArchiveCapability::CanonicalMapOrder)};
					if (MapProperty->VisitEntries(Container, &CollectOrSerializeMapEntry, &Context, ArrayIndex)
						!= EContainerOpResult::Success && !Ar.IsError())
						Ar.SetError("MapOperationFailed: ConstTraversal failed.");
					if (Ar.IsError() || !Context.bCanonical) break;
					std::ranges::sort(Context.Entries, {}, &FArchiveMapEntry::Token);
					for (size_t Index = 0; Index < Context.Entries.size() && !Ar.IsError(); ++Index)
					{
						if (Index > 0 && Context.Entries[Index - 1].Token == Context.Entries[Index].Token)
						{
							Ar.SetError("CanonicalMapKeyCollision: distinct entries produced the same canonical token.");
							break;
						}
						NotifyArchiveCanonicalMapKey(Ar, Index, Context.Entries[Index].Token);
						{
							auto KeyScope = EnterArchiveMapKey(Ar, Index);
							SerializePropertyValue(Ar, MapProperty->GetKeyProp(), const_cast<void*>(Context.Entries[Index].Key), 0, bIncludeRawObjectReferences);
						}
						{
							auto ValueScope = EnterArchiveMapValue(Ar, Index);
							SerializePropertyValue(Ar, MapProperty->GetValueProp(), const_cast<void*>(Context.Entries[Index].Value), 0, bIncludeRawObjectReferences);
						}
					}
				}
				else
				{
					if (!MapProperty->HasCapability(EMapOpsFlags::DetachedStorage | EMapOpsFlags::TransactionalCommit | EMapOpsFlags::Insert))
					{
						Ar.Fail(EArchiveFailureCode::UnsupportedOperation,
							"MapOperationUnavailable: transactional load requires DetachedStorage, Insert, and TransactionalCommit.");
						break;
					}
					const FMapOps& Ops = MapProperty->GetOps();
					FDetachedContainerStorage Detached;
					EContainerOpResult OpResult = Detached.Create(Ops);
					if (OpResult != EContainerOpResult::Success)
					{
						Ar.SetError(std::format("MapOperationFailed: detached allocation returned {}.", static_cast<uint32>(OpResult)));
						break;
					}
					if (Ops.Reserve && (OpResult = Ops.Reserve(Detached.Get(), Num)) != EContainerOpResult::Success)
					{
						Ar.SetError(std::format("MapOperationFailed: Reserve returned {}.", static_cast<uint32>(OpResult)));
						break;
					}
					FReflectedValueStorage KeyStorage;
					FReflectedValueStorage ValueStorage;

					if (Num > 0
						&& (!CheckPropertyValueResult(Ar, KeyStorage.DefaultConstruct(MapProperty->GetKeyProp(), 0))
							|| !CheckPropertyValueResult(Ar, ValueStorage.DefaultConstruct(MapProperty->GetValueProp(), 0))))
					{
						return;
					}
					for (uint64 Index = 0; Index < Num && !Ar.IsError(); ++Index)
					{
						if (Index > 0)
						{
							KeyStorage.Reset();
							ValueStorage.Reset();
							if (!CheckPropertyValueResult(Ar, KeyStorage.DefaultConstruct(MapProperty->GetKeyProp(), 0))
								|| !CheckPropertyValueResult(Ar, ValueStorage.DefaultConstruct(MapProperty->GetValueProp(), 0)))
							{
								return;
							}
						}
						{
							auto KeyScope = EnterArchiveMapKey(Ar, Index);
							SerializePropertyValue(Ar, MapProperty->GetKeyProp(), KeyStorage.GetContainer(), 0, bIncludeRawObjectReferences);
						}
						{
							auto ValueScope = EnterArchiveMapValue(Ar, Index);
							SerializePropertyValue(Ar, MapProperty->GetValueProp(), ValueStorage.GetContainer(), 0, bIncludeRawObjectReferences);
						}
						if (Ar.IsError()) return;
						OpResult = Ops.InsertCopy(Detached.Get(), KeyStorage.GetValue(), ValueStorage.GetValue());
						if (OpResult != EContainerOpResult::Success)
						{
							Ar.SetError(OpResult == EContainerOpResult::DuplicateKey
								? std::format("MapDuplicateKey: MapEntry[{}].Key has a duplicate decoded key.", Index)
								: std::format("MapOperationFailed: Insert entry {} returned {}.", Index, static_cast<uint32>(OpResult)));
							return;
						}
					}
					if (!Ar.IsError())
					{
						OpResult = Ops.Commit(MapProperty->GetValuePtr(Container, ArrayIndex), Detached.Get());
						if (OpResult != EContainerOpResult::Success)
							Ar.SetError(std::format("MapOperationFailed: Commit returned {}.", static_cast<uint32>(OpResult)));
					}
				}
				break;
			}
			default:
				Ar.SetError("Unsupported reflected property kind.");
				break;
			}
		}


		auto SnapshotFailure(EPropertySnapshotError Code, const FProperty* Property,
			uint32 ArrayIndex = 0, EPropertySnapshotOperation Operation = EPropertySnapshotOperation::Capture)
			-> std::expected<void, FPropertySnapshotError>
		{
			FPropertySnapshotError Error;
			Error.Code = Code;
			Error.Operation = Operation;
			Error.ArrayIndex = ArrayIndex;
			if (Property)
			{
				Error.PropertyName = Property->NamePrivate.ToString();
				Error.ActualKind = Property->GetKind();
				Error.ArrayDim = Property->GetArrayDim();
				Error.PropertyRoute.push_back(Error.PropertyName);
			}
			return std::unexpected(std::move(Error));
		}

		auto ValidateSnapshotProperty(const FProperty* Property) -> std::expected<void, FPropertySnapshotError>
		{
			if (!Property)
			{
				return SnapshotFailure(EPropertySnapshotError::NullProperty, Property);
			}

			switch (Property->GetKind())
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
			case DurinCodeGen::EPropertyGenFlags::Enum:
			case DurinCodeGen::EPropertyGenFlags::String:
			case DurinCodeGen::EPropertyGenFlags::Name:
			case DurinCodeGen::EPropertyGenFlags::Guid:
			case DurinCodeGen::EPropertyGenFlags::Byte:
			case DurinCodeGen::EPropertyGenFlags::Blob:
			case DurinCodeGen::EPropertyGenFlags::BulkData:
			case DurinCodeGen::EPropertyGenFlags::Object:
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
			case DurinCodeGen::EPropertyGenFlags::WeakObject:
				return {};
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				const auto* StructProperty = static_cast<const FStructProperty*>(Property);
				if (!StructProperty->GetStruct())
				{
					return SnapshotFailure(EPropertySnapshotError::MissingStruct, Property);
				}
				std::expected<void, FPropertySnapshotError> Result;
				StructProperty->GetStruct()->ForEachProperty([&](FProperty* Field) {
					if (Result && Field && !Field->HasAnyPropertyFlags(EPropertyFlags::Transient))
					{
						Result = ValidateSnapshotProperty(Field);
						if (!Result) Result.error().PropertyRoute.insert(Result.error().PropertyRoute.begin(), Property->NamePrivate.ToString());
					}
				}, false);
				return Result;
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				const auto* ArrayProperty = static_cast<const FArrayProperty*>(Property);
				if (!ArrayProperty->HasArrayOps() || !ArrayProperty->GetInner()
					|| !ArrayProperty->HasCapability(EArrayOpsFlags::Count | EArrayOpsFlags::ConstTraversal
						| EArrayOpsFlags::RandomAccess | EArrayOpsFlags::DefaultGrow
						| EArrayOpsFlags::DetachedStorage | EArrayOpsFlags::TransactionalCommit))
				{
					return SnapshotFailure(EPropertySnapshotError::MissingArrayOperations, Property);
				}
				auto Result = ValidateSnapshotProperty(ArrayProperty->GetInner());
				if (!Result) Result.error().PropertyRoute.insert(Result.error().PropertyRoute.begin(), Property->NamePrivate.ToString());
				return Result;
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				const auto* MapProperty = static_cast<const FMapProperty*>(Property);
				if (!MapProperty->HasMapOps() || !MapProperty->GetKeyProp() || !MapProperty->GetValueProp()
					|| !MapProperty->HasCapability(EMapOpsFlags::Count | EMapOpsFlags::ConstTraversal
						| EMapOpsFlags::Insert | EMapOpsFlags::DetachedStorage | EMapOpsFlags::TransactionalCommit))
				{
					return SnapshotFailure(EPropertySnapshotError::MissingMapOperations, Property);
				}
				if (const auto Result = ValidateCanonicalMapKeyProperty(MapProperty->GetKeyProp()); !Result)
				{
					auto Failure = SnapshotFailure(EPropertySnapshotError::InvalidMapKey, Property);
					Failure.error().Cause = Result.error();
					return Failure;
				}
				auto Result = ValidateSnapshotProperty(MapProperty->GetKeyProp());
				if (Result) Result = ValidateSnapshotProperty(MapProperty->GetValueProp());
				if (!Result) Result.error().PropertyRoute.insert(Result.error().PropertyRoute.begin(), Property->NamePrivate.ToString());
				return Result;
			}
			default:
				return SnapshotFailure(EPropertySnapshotError::UnsupportedKind, Property);
			}
		}

		class FObjectGraphContext
		{
		public:
			struct FObjectMetadata
			{
				std::string ClassName;
				std::string ObjectName;
				DObject* Outer = nullptr;
			};

			auto Discover(DObject* Object) -> void
			{
				if (!Object || bFrozen || ObjectToId.contains(Object)) return;

				const uint64 Id = static_cast<uint64>(Objects.size()) + 1;
				ObjectToId.emplace(Object, Id);
				Objects.push_back(Object);
				Metadata.emplace(
					Object,
					FObjectMetadata{
						GetSerializableClass(Object) ? GetSerializableClass(Object)->GetName() : std::string(),
						Object->GetName(),
						Object->GetOuter()
					}
				);

				Discover(Object->GetOuter());
				for (DObject* InnerObject : GDObjectArray.GetObjectsWithOuter(Object, EObjectQueryScope::LiveOnly)) Discover(InnerObject);
			}

			auto FindId(DObject* Object) const -> uint64
			{
				if (!Object) return 0;
				const auto It = ObjectToId.find(Object);
				return It == ObjectToId.end() ? 0 : It->second;
			}

			auto Freeze() -> void { bFrozen = true; }

			auto ResolveId(uint64 Id) const -> DObject*
			{
				if (Id == 0 || Id > IdToObject.size())
				{
					return nullptr;
				}
				return IdToObject[static_cast<size_t>(Id - 1)];
			}

			std::unordered_map<DObject*, uint64> ObjectToId;
			std::unordered_map<DObject*, FObjectMetadata> Metadata;
			std::vector<DObject*> Objects;
			std::vector<DObject*> IdToObject;
			bool bFrozen = false;
		};

		class FObjectGraphDiscoveryArchive final : public FObjectArchive
		{
		public:
			explicit FObjectGraphDiscoveryArchive(FObjectGraphContext& InContext)
				: FObjectArchive({EArchiveDirection::Save, EArchivePurpose::Discovery,
					EArchiveCapability::StructuredFields | EArchiveCapability::RawBytes
					| EArchiveCapability::ObjectReferences | EArchiveCapability::SoftObjectReferences
					| EArchiveCapability::MultiPassDiscovery})
				, Context(InContext)
			{
			}

			auto SerializeRawBytes(FMutableByteView) -> void override
			{
			}

			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				Context.Discover(Object);
			}

			auto SerializeSoftObjectValue(FObjectPath&) -> void override {}
			auto SerializeWeakObjectReference(FWeakObjectPtr&) -> void override {}

		private:
			FObjectGraphContext& Context;
		};

		class FObjectGraphWriter : public FObjectMemoryWriter
		{
		public:
			FObjectGraphWriter(FByteBuffer& InBytes, FObjectGraphContext& InContext)
				: FObjectMemoryWriter(InBytes, EArchivePurpose::ObjectGraph)
				, Context(InContext)
			{
				EnableCapabilities(EArchiveCapability::ObjectReferences);
			}

			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint8 Kind = static_cast<uint8>(Object
					? EArchiveObjectReferenceKind::Internal : EArchiveObjectReferenceKind::Null);
				*this << Kind;
				if (!Object || IsError()) return;
				uint64 Id = Context.FindId(Object);
				if (Id == 0)
				{
					Fail(EArchiveFailureCode::InvalidObjectReference,
						"Object graph grew after discovery was frozen.");
					return;
				}
				*this << Id;
			}

			auto SerializeWeakObjectReference(FWeakObjectPtr& Value) -> void override
			{
				uint64 Id = Context.FindId(Value.Get());
				*this << Id;
			}

		private:
			FObjectGraphContext& Context;
		};

		class FObjectGraphReader : public FObjectMemoryReader
		{
		public:
			FObjectGraphReader(const FByteBuffer& InBytes, FObjectGraphContext& InContext)
				: FObjectMemoryReader(InBytes, EArchivePurpose::ObjectGraph)
				, Context(InContext)
			{
				EnableCapabilities(EArchiveCapability::ObjectReferences);
			}

			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint8 Kind = 0;
				*this << Kind;
				if (IsError()) return;
				if (Kind == static_cast<uint8>(EArchiveObjectReferenceKind::Null))
				{
					Object = nullptr;
					return;
				}
				if (Kind != static_cast<uint8>(EArchiveObjectReferenceKind::Internal))
				{
					Fail(EArchiveFailureCode::InvalidObjectReference,
						"Object graph reference kind must be Null or Internal.");
					return;
				}
				uint64 Id = 0;
				*this << Id;
				if (IsError()) return;
				if (Id == 0 || Id > Context.IdToObject.size())
				{
					Fail(EArchiveFailureCode::InvalidObjectReference,
						"Invalid object graph reference identifier.");
					return;
				}
				Object = Context.ResolveId(Id);
			}

			auto SerializeWeakObjectReference(FWeakObjectPtr& Value) -> void override
			{
				uint64 Id = 0;
				*this << Id;
				if (IsError()) return;
				if (Id > Context.IdToObject.size())
				{
					Fail(EArchiveFailureCode::InvalidObjectReference, "Invalid weak object graph reference identifier.");
					return;
				}
				Value.SetObject(Context.ResolveId(Id));
			}

		private:
			FObjectGraphContext& Context;
		};
	}

	auto FArchiveLogicalTypeDescriptor::Scalar(bool bInSigned, uint8 InBitWidth, bool bInFloating) -> FArchiveLogicalTypeDescriptor
	{
		return {.Kind = EKind::Scalar, .bSigned = bInSigned, .bFloating = bInFloating, .BitWidth = InBitWidth};
	}
	auto FArchiveLogicalTypeDescriptor::Enum(FName InQualifiedType, bool bInSigned, uint8 InBitWidth) -> FArchiveLogicalTypeDescriptor
	{
		return {.Kind = EKind::Enum, .bSigned = bInSigned, .BitWidth = InBitWidth, .QualifiedType = InQualifiedType};
	}
	auto FArchiveLogicalTypeDescriptor::String() -> FArchiveLogicalTypeDescriptor { return {.Kind = EKind::String}; }
	auto FArchiveLogicalTypeDescriptor::Name() -> FArchiveLogicalTypeDescriptor { return {.Kind = EKind::Name}; }
	auto FArchiveLogicalTypeDescriptor::Guid() -> FArchiveLogicalTypeDescriptor { return {.Kind = EKind::Guid}; }
	auto FArchiveLogicalTypeDescriptor::Bytes() -> FArchiveLogicalTypeDescriptor { return {.Kind = EKind::Bytes}; }
	auto FArchiveLogicalTypeDescriptor::BulkData() -> FArchiveLogicalTypeDescriptor { return {.Kind = EKind::BulkData}; }
	auto FArchiveLogicalTypeDescriptor::Object(FName Type) -> FArchiveLogicalTypeDescriptor { return {.Kind = EKind::Object, .QualifiedType = Type}; }
	auto FArchiveLogicalTypeDescriptor::SoftObject(FName Type) -> FArchiveLogicalTypeDescriptor { return {.Kind = EKind::SoftObject, .QualifiedType = Type}; }
	auto FArchiveLogicalTypeDescriptor::WeakObject(FName Type) -> FArchiveLogicalTypeDescriptor { return {.Kind = EKind::WeakObject, .QualifiedType = Type}; }
	auto FArchiveLogicalTypeDescriptor::Struct(FName Type, uint32 Version) -> FArchiveLogicalTypeDescriptor
	{
		return {.Kind = EKind::Struct, .QualifiedType = Type, .NativeFieldVersion = Version};
	}
	auto FArchiveLogicalTypeDescriptor::Array(FArchiveLogicalTypeDescriptor Element) -> FArchiveLogicalTypeDescriptor
	{
		FArchiveLogicalTypeDescriptor Result{.Kind = EKind::Array};
		Result.ElementType = std::make_shared<FArchiveLogicalTypeDescriptor>(std::move(Element));
		return Result;
	}
	auto FArchiveLogicalTypeDescriptor::Map(FArchiveLogicalTypeDescriptor Key, FArchiveLogicalTypeDescriptor Value) -> FArchiveLogicalTypeDescriptor
	{
		FArchiveLogicalTypeDescriptor Result{.Kind = EKind::Map};
		Result.KeyType = std::make_shared<FArchiveLogicalTypeDescriptor>(std::move(Key));
		Result.ValueType = std::make_shared<FArchiveLogicalTypeDescriptor>(std::move(Value));
		return Result;
	}
	auto FArchiveLogicalTypeDescriptor::FixedArray(FArchiveLogicalTypeDescriptor Element, uint32 Dimension) -> FArchiveLogicalTypeDescriptor
	{
		FArchiveLogicalTypeDescriptor Result{.Kind = EKind::FixedArray, .FixedArrayDimension = Dimension};
		Result.ElementType = std::make_shared<FArchiveLogicalTypeDescriptor>(std::move(Element));
		return Result;
	}

	FObjectArchive::FObjectArchive(FArchiveState State, FArchiveVersionContext Versions)
		: FArchive(std::move(State), std::move(Versions))
	{
		EnableCapabilities(EArchiveCapability::StructuredFields
			| EArchiveCapability::SoftObjectReferences);
	}

	auto FObjectArchive::EnterObject(DObject& Object) -> FArchiveObjectScope
	{
		if (!HasCapability(EArchiveCapability::StructuredFields))
		{
			Fail(EArchiveFailureCode::UnsupportedCapability, "Object scopes require StructuredFields.");
			return {};
		}
		const std::string ClassName = Object.GetClass() ? Object.GetClass()->GetQualifiedName().ToString() : "?";
		PushPath(std::format("Object[?:{}:{}]", ClassName, Object.GetObjectPath()));
		ObjectScopes.emplace_back();
		OnEnterObject(Object);
		return FArchiveObjectScope(this);
	}

	auto FObjectArchive::EnterField(const FArchiveFieldDescriptor& Field) -> FArchiveFieldScope
	{
		if (!HasCapability(EArchiveCapability::StructuredFields))
		{
			Fail(EArchiveFailureCode::UnsupportedCapability, "Field scopes require StructuredFields.");
			return {};
		}
		const std::string Identity = std::format("{}::{}", Field.DeclaringType, Field.Name);
		if (!ObjectScopes.empty())
		{
			auto& Scope = ObjectScopes.back();
			std::string NestedIdentity;
			NestedIdentity = GetPathString();
			NestedIdentity += Identity;
			if (!Scope.Fields.insert(NestedIdentity).second)
				Fail(EArchiveFailureCode::DuplicateField, std::format("Field '{}' was serialized more than once.", Identity));
			++Scope.ActiveFieldDepth;
		}
		PushPath(std::format(".Field[{}]", Identity));
		FieldScopes.push_back(true);
		OnEnterField(Field);
		return FArchiveFieldScope(this);
	}
	auto FObjectArchive::EnterFixedArrayElement(uint64 Index) -> FArchivePathScope
	{
		PushPath(std::format(".Fixed[{}]", Index));
		OnEnterFixedArrayElement(Index);
		return FArchivePathScope(this);
	}
	auto FObjectArchive::EnterArrayElement(uint64 Index) -> FArchivePathScope
	{
		PushPath(std::format(".Array[{}]", Index));
		OnEnterArrayElement(Index);
		return FArchivePathScope(this);
	}
	auto FObjectArchive::EnterMapKey(uint64 Index) -> FArchivePathScope
	{
		PushPath(std::format(".MapKey[{}]", Index));
		OnEnterMapKey(Index);
		return FArchivePathScope(this);
	}
	auto FObjectArchive::EnterMapValue(uint64 Index) -> FArchivePathScope
	{
		PushPath(std::format(".MapValue[{}]", Index));
		OnEnterMapValue(Index);
		return FArchivePathScope(this);
	}

	auto FObjectArchive::NotifyCanonicalMapKey(uint64 Index, FByteView Token) -> void
	{
		if (!IsError()) OnCanonicalMapKey(Index, Token);
	}

	auto FObjectArchive::MarkBaseReflectedFieldsSerialized() -> void
	{
		if (ObjectScopes.empty()) return;
		auto& Scope = ObjectScopes.back();
		if (Scope.bBaseMarked)
		{
			Fail(EArchiveFailureCode::DuplicateBaseReflectedFields, "Base reflected fields were serialized more than once.");
			return;
		}
		Scope.bBaseMarked = true;
	}

	auto FObjectArchive::CloseFieldScope() -> void
	{
		if (FieldScopes.empty())
		{
			Fail(EArchiveFailureCode::UnbalancedScope, "A field scope closed without a matching open scope.");
			return;
		}
		OnLeaveField();
		FieldScopes.pop_back();
		if (!ObjectScopes.empty() && ObjectScopes.back().ActiveFieldDepth > 0) --ObjectScopes.back().ActiveFieldDepth;
		PopPath();
	}
	auto FObjectArchive::ClosePathScope() -> void { OnLeavePath(); PopPath(); }

	auto FObjectArchive::CloseObjectScope() -> void
	{
		if (ObjectScopes.empty())
		{
			Fail(EArchiveFailureCode::UnbalancedScope, "An object scope closed without a matching open scope.");
			return;
		}
		const FObjectScopeState& Scope = ObjectScopes.back();
		if (Scope.ActiveFieldDepth != 0)
			Fail(EArchiveFailureCode::UnbalancedScope, "An object scope closed while a field scope remained active.");
		else if (!Scope.bBaseMarked)
			Fail(EArchiveFailureCode::MissingBaseReflectedFields, "The base reflected field walk was not serialized.");
		OnLeaveObject();
		ObjectScopes.pop_back();
		PopPath();
	}

	FArchiveObjectScope::FArchiveObjectScope(FArchiveObjectScope&& Other) noexcept : Archive(std::exchange(Other.Archive, nullptr)) {}
	auto FArchiveObjectScope::operator=(FArchiveObjectScope&& Other) noexcept -> FArchiveObjectScope&
	{
		if (this != &Other) { if (Archive) Archive->CloseObjectScope(); Archive = std::exchange(Other.Archive, nullptr); }
		return *this;
	}
	FArchiveObjectScope::~FArchiveObjectScope() { if (Archive) Archive->CloseObjectScope(); }
	FArchiveFieldScope::FArchiveFieldScope(FArchiveFieldScope&& Other) noexcept : Archive(std::exchange(Other.Archive, nullptr)) {}
	auto FArchiveFieldScope::operator=(FArchiveFieldScope&& Other) noexcept -> FArchiveFieldScope&
	{
		if (this != &Other) { if (Archive) Archive->CloseFieldScope(); Archive = std::exchange(Other.Archive, nullptr); }
		return *this;
	}
	FArchiveFieldScope::~FArchiveFieldScope() { if (Archive) Archive->CloseFieldScope(); }
	FArchivePathScope::FArchivePathScope(FArchivePathScope&& Other) noexcept : Archive(std::exchange(Other.Archive, nullptr)) {}
	auto FArchivePathScope::operator=(FArchivePathScope&& Other) noexcept -> FArchivePathScope&
	{
		if (this != &Other) { if (Archive) Archive->ClosePathScope(); Archive = std::exchange(Other.Archive, nullptr); }
		return *this;
	}
	FArchivePathScope::~FArchivePathScope() { if (Archive) Archive->ClosePathScope(); }

	auto FObjectArchive::OnEnterObject(DObject&) -> void {}
	auto FObjectArchive::OnLeaveObject() -> void {}
	auto FObjectArchive::OnEnterField(const FArchiveFieldDescriptor&) -> void {}
	auto FObjectArchive::OnLeaveField() -> void {}
	auto FObjectArchive::OnEnterFixedArrayElement(uint64) -> void {}
	auto FObjectArchive::OnEnterArrayElement(uint64) -> void {}
	auto FObjectArchive::OnEnterMapKey(uint64) -> void {}
	auto FObjectArchive::OnEnterMapValue(uint64) -> void {}
	auto FObjectArchive::OnCanonicalMapKey(uint64, FByteView) -> void {}
	auto FObjectArchive::OnLeavePath() -> void {}
	auto FObjectArchive::OnReflectedPropertyValue(FProperty&, const void*, uint32) -> void {}
	auto FObjectArchive::OnResolvePropertySaveValue(
		FProperty&, const void* Container, uint32 ArrayIndex,
		FArchivePropertySaveValue& OutValue) -> EArchivePropertySaveDisposition
	{
		OutValue = {Container, ArrayIndex};
		return EArchivePropertySaveDisposition::LiveValue;
	}
	auto FObjectArchive::ResolvePropertySaveValue(
		FProperty& Property, const void* Container, uint32 ArrayIndex,
		FArchivePropertySaveValue& OutValue) -> EArchivePropertySaveDisposition
	{
		return OnResolvePropertySaveValue(Property, Container, ArrayIndex, OutValue);
	}
	auto FObjectArchive::NotifyReflectedPropertyValue(
		FProperty& Property, const void* Container, uint32 ArrayIndex) -> void
	{
		OnReflectedPropertyValue(Property, Container, ArrayIndex);
	}

	auto FObjectArchive::SerializeObjectReference(DObject*&) -> void
	{
		Fail(EArchiveFailureCode::UnsupportedCapability, "This Archive does not support ObjectReferences.");
	}
	auto FObjectArchive::SerializeSoftObjectValue(FObjectPath& Value) -> void
	{
		if (!IsCurrentFieldAvailable()) return;
		if (!HasCapability(EArchiveCapability::SoftObjectReferences))
		{
			Fail(EArchiveFailureCode::UnsupportedCapability, "This Archive does not support SoftObjectReferences.");
			return;
		}
		uint8 Kind = IsSaving() && Value.IsValid() ? 1 : 0;
		*this << Kind;
		if (IsError()) return;
		if (Kind == 0) { if (IsLoading()) Value = {}; return; }
		if (Kind != 1) { Fail(EArchiveFailureCode::InvalidData, "Unknown soft object reference tag."); return; }
		std::string Path = IsSaving() ? Value.ToString() : std::string();
		uint64 PathBytes = static_cast<uint64>(Path.size());
		*this << PathBytes;
		if (IsError()) return;
		if (PathBytes == 0 || PathBytes > MaximumSoftObjectPathBytes
			|| (IsLoading() && PathBytes > GetRemainingPayloadBytes()))
		{
			Fail(EArchiveFailureCode::InvalidPath,
				"Soft object path payload is empty, truncated, or exceeds 1 MiB.");
			return;
		}
		if (IsLoading()) Path.resize(static_cast<size_t>(PathBytes));
		SerializeRawBytes(std::as_writable_bytes(std::span<char>(Path.data(), Path.size())));
		if (IsError()) return;
		if (IsLoading())
		{
			FObjectPath Loaded;
			if (const auto PathValidation = FObjectPath::TryCreateWithDiagnostic(Path, Loaded); !PathValidation)
			{
				ValueFailureCause = PathValidation.error();
				Fail(EArchiveFailureCode::InvalidPath, ToString(PathValidation.error()));
			}
			else Value = std::move(Loaded);
		}
	}

	auto FObjectArchive::SerializeWeakObjectReference(FWeakObjectPtr& Value) -> void
	{
		DObject* Object = IsSaving() ? Value.Get() : nullptr;
		SerializeObjectReference(Object);
		if (IsLoading() && !IsError()) Value.SetObject(Object);
	}

	FObjectMemoryWriter::FObjectMemoryWriter(FByteBuffer& InBytes, EArchivePurpose Purpose)
		: FObjectArchive({EArchiveDirection::Save, Purpose,
			EArchiveCapability::StructuredFields | EArchiveCapability::RawBytes
			| EArchiveCapability::Position
			| EArchiveCapability::SoftObjectReferences
			| (Purpose == EArchivePurpose::PropertySnapshot ? EArchiveCapability::CanonicalMapOrder : EArchiveCapability::None)})
		, Bytes(InBytes)
	{
	}

	auto FObjectMemoryWriter::SerializeRawBytes(FMutableByteView Data) -> void
	{
		if (IsError()) return;
		Bytes.insert(Bytes.end(), Data.begin(), Data.end());
	}

	FObjectMemoryReader::FObjectMemoryReader(FByteView InBytes, EArchivePurpose Purpose)
		: FObjectArchive({EArchiveDirection::Load, Purpose,
			EArchiveCapability::StructuredFields | EArchiveCapability::RawBytes
			| EArchiveCapability::Position
			| EArchiveCapability::SoftObjectReferences
			| EArchiveCapability::RemainingPayload
			| (Purpose == EArchivePurpose::PropertySnapshot ? EArchiveCapability::CanonicalMapOrder : EArchiveCapability::None)})
		, Bytes(InBytes)
	{
	}

	auto FObjectMemoryReader::SerializeRawBytes(FMutableByteView Data) -> void
	{
		if (IsError()) return;
		if (Data.size() > GetRemainingPayloadBytes())
		{
			Fail(EArchiveFailureCode::TruncatedPayload, "Truncated byte payload.");
			return;
		}
		if (!Data.empty()) std::memcpy(Data.data(), Bytes.data() + Offset, Data.size());
		Offset += Data.size();
	}

	auto RequireObjectArchive(FArchive& Ar) -> FObjectArchive*
	{
		auto* ObjectArchive = dynamic_cast<FObjectArchive*>(&Ar);
		if (!ObjectArchive)
			Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
				"Reflected object serialization requires a CoreDObject object Archive.");
		return ObjectArchive;
	}

	auto EnterArchiveObject(FArchive& Ar, DObject& Object) -> FArchiveObjectScope
	{
		auto* ObjectArchive = RequireObjectArchive(Ar);
		return ObjectArchive ? ObjectArchive->EnterObject(Object) : FArchiveObjectScope{};
	}

	auto EnterArchiveField(FArchive& Ar, const FArchiveFieldDescriptor& Field) -> FArchiveFieldScope
	{
		auto* ObjectArchive = RequireObjectArchive(Ar);
		return ObjectArchive ? ObjectArchive->EnterField(Field) : FArchiveFieldScope{};
	}

	auto EnterArchiveFixedArrayElement(FArchive& Ar, uint64 Index) -> FArchivePathScope
	{
		auto* ObjectArchive = RequireObjectArchive(Ar);
		return ObjectArchive ? ObjectArchive->EnterFixedArrayElement(Index) : FArchivePathScope{};
	}

	auto EnterArchiveArrayElement(FArchive& Ar, uint64 Index) -> FArchivePathScope
	{
		auto* ObjectArchive = RequireObjectArchive(Ar);
		return ObjectArchive ? ObjectArchive->EnterArrayElement(Index) : FArchivePathScope{};
	}

	auto EnterArchiveMapKey(FArchive& Ar, uint64 Index) -> FArchivePathScope
	{
		auto* ObjectArchive = RequireObjectArchive(Ar);
		return ObjectArchive ? ObjectArchive->EnterMapKey(Index) : FArchivePathScope{};
	}

	auto EnterArchiveMapValue(FArchive& Ar, uint64 Index) -> FArchivePathScope
	{
		auto* ObjectArchive = RequireObjectArchive(Ar);
		return ObjectArchive ? ObjectArchive->EnterMapValue(Index) : FArchivePathScope{};
	}

	auto NotifyArchiveCanonicalMapKey(
		FArchive& Ar, uint64 Index, FByteView Token) -> void
	{
		if (auto* ObjectArchive = RequireObjectArchive(Ar))
			ObjectArchive->NotifyCanonicalMapKey(Index, Token);
	}

	auto MarkArchiveBaseReflectedFieldsSerialized(FArchive& Ar) -> void
	{
		if (auto* ObjectArchive = RequireObjectArchive(Ar))
			ObjectArchive->MarkBaseReflectedFieldsSerialized();
	}

	auto NotifyArchiveReflectedPropertyValue(
		FArchive& Ar, FProperty& Property, const void* Container, uint32 ArrayIndex) -> void
	{
		if (auto* ObjectArchive = RequireObjectArchive(Ar))
			ObjectArchive->NotifyReflectedPropertyValue(Property, Container, ArrayIndex);
	}

	auto SerializeArchiveObjectReference(FArchive& Ar, DObject*& Value) -> void
	{
		if (auto* ObjectArchive = RequireObjectArchive(Ar)) ObjectArchive->SerializeObjectReference(Value);
	}

	auto SerializeArchiveSoftObjectValue(FArchive& Ar, FObjectPath& Value) -> void
	{
		if (auto* ObjectArchive = RequireObjectArchive(Ar)) ObjectArchive->SerializeSoftObjectValue(Value);
	}

	auto SerializeArchiveWeakObjectReference(FArchive& Ar, FWeakObjectPtr& Value) -> void
	{
		if (auto* ObjectArchive = RequireObjectArchive(Ar)) ObjectArchive->SerializeWeakObjectReference(Value);
	}

	FPropertyValueSnapshot::~FPropertyValueSnapshot()
	{
		ReleaseStrongReferences();
	}

	FPropertyValueSnapshot::FPropertyValueSnapshot(const FPropertyValueSnapshot& Other)
		: Payload(Other.Payload)
		, ReferencedObjects(Other.ReferencedObjects)
	{
		AddStrongReferences();
	}

	auto FPropertyValueSnapshot::operator=(const FPropertyValueSnapshot& Other) -> FPropertyValueSnapshot&
	{
		if (this == &Other) return *this;
		FPropertyValueSnapshot Copy(Other);
		*this = std::move(Copy);
		return *this;
	}

	FPropertyValueSnapshot::FPropertyValueSnapshot(FPropertyValueSnapshot&& Other) noexcept
		: Payload(std::move(Other.Payload))
		, ReferencedObjects(std::move(Other.ReferencedObjects))
		, StrongReferences(std::move(Other.StrongReferences))
	{
		Other.Payload = {};
		Other.ReferencedObjects.clear();
		Other.StrongReferences.clear();
	}

	auto FPropertyValueSnapshot::operator=(FPropertyValueSnapshot&& Other) noexcept -> FPropertyValueSnapshot&
	{
		if (this == &Other) return *this;
		ReleaseStrongReferences();
		Payload = std::move(Other.Payload);
		ReferencedObjects = std::move(Other.ReferencedObjects);
		StrongReferences = std::move(Other.StrongReferences);
		Other.Payload = {};
		Other.ReferencedObjects.clear();
		Other.StrongReferences.clear();
		return *this;
	}

	auto FPropertyValueSnapshot::operator==(const FPropertyValueSnapshot& Other) const -> bool
	{
		if (this == &Other) return true;
		const FProperty* Property = Payload.GetProperty();
		if (Property != Other.Payload.GetProperty()) return false;
		if (!Property) return true;

		FReflectedValueStorage Left;
		FReflectedValueStorage Right;
		if (Left.DefaultConstruct(Property, 0)
			&& Right.DefaultConstruct(Property, 0)
			&& RestorePropertyValue(Property, Left.GetContainer(), 0, *this)
			&& RestorePropertyValue(Property, Right.GetContainer(), 0, Other))
		{
			return ArePropertyValuesIdentical(
				Property, Left.GetContainer(), 0, Right.GetContainer(), 0);
		}
		return Payload.GetBytes() == Other.Payload.GetBytes()
			&& ReferencedObjects == Other.ReferencedObjects;
	}

	auto FPropertyValueSnapshot::AddStrongReferences() -> void
	{
		StrongReferences.reserve(ReferencedObjects.size());
		for (DObject* Object : ReferencedObjects) StrongReferences.emplace_back(Object);
	}

	auto FPropertyValueSnapshot::ReleaseStrongReferences() -> void
	{
		StrongReferences.clear();
		ReferencedObjects.clear();
	}

	auto ArePropertySnapshotTypesCompatible(
		const FProperty* CapturedProperty,
		const FProperty* CandidateProperty
	) -> bool
	{
		if (CapturedProperty == CandidateProperty) return CapturedProperty != nullptr;
		if (!CapturedProperty || !CandidateProperty
			|| CapturedProperty->GetKind() != CandidateProperty->GetKind()
			|| CapturedProperty->GetArrayDim() != CandidateProperty->GetArrayDim()
			|| CapturedProperty->GetElementSize() != CandidateProperty->GetElementSize())
		{
			return false;
		}

		auto ReferencedClassName = [](const FProperty* Property) {
			const DClass* Class = Property->GetReferencedClass();
			return Class ? Class->GetQualifiedName() : FName();
		};
		switch (CapturedProperty->GetKind())
		{
		case DurinCodeGen::EPropertyGenFlags::Object:
		case DurinCodeGen::EPropertyGenFlags::SoftObject:
		case DurinCodeGen::EPropertyGenFlags::WeakObject:
			return ReferencedClassName(CapturedProperty)
				== ReferencedClassName(CandidateProperty);
		case DurinCodeGen::EPropertyGenFlags::Struct:
		{
			const DStruct* CapturedStruct =
				static_cast<const FStructProperty*>(CapturedProperty)->GetStruct();
			const DStruct* CandidateStruct =
				static_cast<const FStructProperty*>(CandidateProperty)->GetStruct();
			return CapturedStruct && CandidateStruct
				&& CapturedStruct->GetQualifiedName() == CandidateStruct->GetQualifiedName();
		}
		case DurinCodeGen::EPropertyGenFlags::Array:
			return ArePropertySnapshotTypesCompatible(
				static_cast<const FArrayProperty*>(CapturedProperty)->GetInner(),
				static_cast<const FArrayProperty*>(CandidateProperty)->GetInner());
		case DurinCodeGen::EPropertyGenFlags::Map:
		{
			const auto* CapturedMap = static_cast<const FMapProperty*>(CapturedProperty);
			const auto* CandidateMap = static_cast<const FMapProperty*>(CandidateProperty);
			return ArePropertySnapshotTypesCompatible(
				CapturedMap->GetKeyProp(), CandidateMap->GetKeyProp())
				&& ArePropertySnapshotTypesCompatible(
					CapturedMap->GetValueProp(), CandidateMap->GetValueProp());
		}
		case DurinCodeGen::EPropertyGenFlags::Enum:
		{
			const DEnum* CapturedEnum =
				static_cast<const FEnumProperty*>(CapturedProperty)->GetEnum();
			const DEnum* CandidateEnum =
				static_cast<const FEnumProperty*>(CandidateProperty)->GetEnum();
			return CapturedEnum && CandidateEnum
				&& CapturedEnum->GetQualifiedName() == CandidateEnum->GetQualifiedName();
		}
		default:
			return true;
		}
	}

	static auto SnapshotArchiveFailure(const FObjectArchive& Archive, const FProperty* Property,
		uint32 ArrayIndex, EPropertySnapshotOperation Operation) -> std::expected<void, FPropertySnapshotError>
	{
		auto Result = SnapshotFailure(EPropertySnapshotError::ArchiveFailure, Property, ArrayIndex, Operation);
		if (const auto* Failure = Archive.GetFailure())
		{
			Result.error().ArchiveCode = Failure->Code;
			Result.error().ArchivePath = Failure->Path;
		}
		Result.error().Cause = Archive.GetValueFailureCause();
		if (const auto* Failure = Archive.GetFailure(); Failure && std::holds_alternative<std::monostate>(Result.error().Cause))
			Result.error().Message = Failure->Message;
		return Result;
	}

	auto CapturePropertyValuePayload(
		const FProperty* Property,
		const void* Container,
		uint32 ArrayIndex,
		FPropertyValueSnapshotPayload& OutPayload
	) -> std::expected<void, FPropertySnapshotError>
	{
		constexpr auto Operation = EPropertySnapshotOperation::Capture;
		if (!Container)
		{
			return SnapshotFailure(EPropertySnapshotError::NullContainer, Property, ArrayIndex, Operation);
		}
		if (auto Result = ValidateSnapshotProperty(Property); !Result)
		{
			Result.error().Operation = Operation;
			return Result;
		}
		if (ArrayIndex >= Property->GetArrayDim())
		{
			return SnapshotFailure(EPropertySnapshotError::InvalidArrayIndex, Property, ArrayIndex, Operation);
		}

		class FSnapshotWriter final : public FObjectMemoryWriter
		{
		public:
			FSnapshotWriter(FByteBuffer& InBytes, std::vector<FObjectKey>& InReferences)
				: FObjectMemoryWriter(InBytes, EArchivePurpose::PropertySnapshot), References(InReferences)
			{
				EnableCapabilities(EArchiveCapability::ObjectReferences);
			}
			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint64 Id = 0;
				if (Object)
				{
					const FObjectKey Handle = FObjectKey(Object);
					auto It = std::find(References.begin(), References.end(), Handle);
					if (It == References.end())
					{
						References.push_back(Handle);
						Id = static_cast<uint64>(References.size());
					}
					else
					{
						Id = static_cast<uint64>(std::distance(References.begin(), It)) + 1;
					}
				}
				*this << Id;
			}
			auto SerializeWeakObjectReference(FWeakObjectPtr& Value) -> void override
			{
				FObjectKey Handle = Value.GetKey();
				Handle.SerializeForSnapshot(*this);
			}
		private:
			std::vector<FObjectKey>& References;
		};

		FPropertyValueSnapshotPayload Payload;
		Payload.Property = Property;
		FSnapshotWriter Writer(Payload.Bytes, Payload.ReferencedObjectKeys);
		SerializePropertyValue(Writer, const_cast<FProperty*>(Property), const_cast<void*>(Container), ArrayIndex, true);
		if (Writer.IsError())
		{
			return SnapshotArchiveFailure(Writer, Property, ArrayIndex, Operation);
		}
		class FSnapshotReferenceCollector final : public FReferenceCollector
		{
		public:
			explicit FSnapshotReferenceCollector(std::vector<FObjectKey>& InReferences)
				: References(InReferences) {}
			auto AddReferencedObject(DObject*& Object) -> void override
			{
				const FObjectKey Handle = FObjectKey(Object);
				if (!IsObjectKeyNull(Handle)
					&& std::ranges::find(References, Handle) == References.end())
				{
					References.push_back(Handle);
				}
			}
		private:
			std::vector<FObjectKey>& References;
		};
		FSnapshotReferenceCollector ReferenceCollector(Payload.ReferencedObjectKeys);
		Private::FGCReferenceSchemaRegistry::VisitProperty(
			const_cast<FProperty*>(Property), const_cast<void*>(Container), ArrayIndex, ReferenceCollector);
		OutPayload = std::move(Payload);
		return {};
	}

	auto RestorePropertyValuePayload(
		const FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const FPropertyValueSnapshotPayload& Payload
	) -> std::expected<void, FPropertySnapshotError>
	{
		constexpr auto Operation = EPropertySnapshotOperation::Restore;
		if (!Container)
		{
			return SnapshotFailure(EPropertySnapshotError::NullContainer, Property, ArrayIndex, Operation);
		}
		if (auto Result = ValidateSnapshotProperty(Property); !Result)
		{
			Result.error().Operation = Operation;
			return Result;
		}
		if (!ArePropertySnapshotTypesCompatible(Payload.Property, Property))
		{
			auto Result = SnapshotFailure(EPropertySnapshotError::IncompatibleType, Property, ArrayIndex, Operation);
			if (Payload.Property)
			{
				Result.error().ExpectedPropertyName = Payload.Property->NamePrivate.ToString();
				Result.error().ExpectedKind = Payload.Property->GetKind();
			}
			return Result;
		}
		if (ArrayIndex >= Property->GetArrayDim())
		{
			return SnapshotFailure(EPropertySnapshotError::InvalidArrayIndex, Property, ArrayIndex, Operation);
		}

		class FSnapshotReader final : public FObjectMemoryReader
		{
		public:
			FSnapshotReader(const FByteBuffer& InBytes, const std::vector<FObjectKey>& InReferences)
				: FObjectMemoryReader(InBytes, EArchivePurpose::PropertySnapshot), References(InReferences)
			{
				EnableCapabilities(EArchiveCapability::ObjectReferences);
			}
			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint64 Id = 0;
				*this << Id;
				if (IsError()) return;
				if (Id > References.size())
				{
					SnapshotCode = EPropertySnapshotError::InvalidReferenceIndex;
					ReferenceIndex = Id;
					Fail(EArchiveFailureCode::InvalidObjectReference, "Invalid property snapshot reference identifier.");
					return;
				}
				Object = Id == 0 ? nullptr
					: ResolveObjectKey(References[static_cast<size_t>(Id - 1)]);
				if (Object && Object->IsPendingKill()) Object = nullptr;
				if (Id != 0 && !Object)
				{
					SnapshotCode = EPropertySnapshotError::UnresolvedReference;
					ReferenceIndex = Id;
					Fail(EArchiveFailureCode::InvalidObjectReference, "Property snapshot hard reference no longer resolves.");
				}
			}
			auto SerializeWeakObjectReference(FWeakObjectPtr& Value) -> void override
			{
				FObjectKey Handle;
				Handle.SerializeForSnapshot(*this);
				if (!IsError()) Value.SetKey(Handle);
			}
			EPropertySnapshotError SnapshotCode = EPropertySnapshotError::ArchiveFailure;
			uint64 ReferenceIndex = 0;
		private:
			const std::vector<FObjectKey>& References;
		};

		FSnapshotReader Reader(Payload.Bytes, Payload.ReferencedObjectKeys);
		SerializeReflectedPropertyValue(
			Reader, *const_cast<FProperty*>(Property), Container, ArrayIndex, true);
		if (!Reader.IsError() && Reader.GetRemainingPayloadBytes() != 0)
		{
			Reader.SnapshotCode = EPropertySnapshotError::TrailingBytes;
			Reader.Fail(EArchiveFailureCode::TrailingData, "Property snapshot has trailing bytes.");
		}
		if (!Reader.IsError()) return {};
		auto Result = SnapshotArchiveFailure(Reader, Property, ArrayIndex, Operation);
		Result.error().Code = Reader.SnapshotCode;
		Result.error().ActualCount = Reader.SnapshotCode == EPropertySnapshotError::TrailingBytes
			? Reader.GetRemainingPayloadBytes() : Reader.ReferenceIndex;
		Result.error().ExpectedCount = (Reader.SnapshotCode == EPropertySnapshotError::InvalidReferenceIndex
			|| Reader.SnapshotCode == EPropertySnapshotError::UnresolvedReference)
			? Payload.ReferencedObjectKeys.size() : 0;
		return Result;
	}

	auto CapturePropertyValue(
		const FProperty* Property,
		const void* Container,
		uint32 ArrayIndex,
		FPropertyValueSnapshot& OutSnapshot
	) -> std::expected<void, FPropertySnapshotError>
	{
		FPropertyValueSnapshot Snapshot;
		if (auto Result = CapturePropertyValuePayload(
			Property, Container, ArrayIndex, Snapshot.Payload); !Result) return Result;
		for (FObjectKey Handle : Snapshot.Payload.GetReferencedObjectKeys())
		{
			if (DObject* Object = ResolveObjectKey(Handle))
				Snapshot.ReferencedObjects.push_back(Object);
		}
		Snapshot.AddStrongReferences();
		OutSnapshot = std::move(Snapshot);
		return {};
	}

	auto RestorePropertyValue(
		const FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const FPropertyValueSnapshot& Snapshot
	) -> std::expected<void, FPropertySnapshotError>
	{
		return RestorePropertyValuePayload(
			Property, Container, ArrayIndex, Snapshot.Payload);
	}

	auto ToString(const FPropertySnapshotError& Error) -> std::string
	{
		if (!std::holds_alternative<std::monostate>(Error.Cause))
			return std::visit([](const auto& Cause) -> std::string {
				using T = std::decay_t<decltype(Cause)>;
				if constexpr (std::is_same_v<T, std::monostate>) return {};
				else return ToString(Cause);
			}, Error.Cause);
		if (!Error.Message.empty()) return Error.Message;
		if (!Error.HasError()) return {};
		switch (Error.Code)
		{
		case EPropertySnapshotError::NullProperty: return "Cannot snapshot a null property.";
		case EPropertySnapshotError::MissingStruct: return "Cannot snapshot a struct property without reflected fields.";
		case EPropertySnapshotError::MissingArrayOperations: return "Cannot snapshot an array without Count, ConstTraversal, RandomAccess, DefaultGrow, DetachedStorage, and TransactionalCommit.";
		case EPropertySnapshotError::MissingMapOperations: return "Cannot snapshot a map without Count, ConstTraversal, Insert, DetachedStorage, and TransactionalCommit.";
		case EPropertySnapshotError::UnsupportedKind: return "The reflected property kind does not support value snapshots.";
		case EPropertySnapshotError::None: return {};
		case EPropertySnapshotError::NullContainer: return Error.Operation == EPropertySnapshotOperation::Capture
			? "Cannot snapshot a property from a null container." : "Cannot restore a property into a null container.";
		case EPropertySnapshotError::InvalidArrayIndex: return Error.Operation == EPropertySnapshotOperation::Capture
			? "Property snapshot array index is out of range." : "Property restore array index is out of range.";
		case EPropertySnapshotError::IncompatibleType: return "Property snapshot is incompatible with the requested reflected property.";
		case EPropertySnapshotError::InvalidReferenceIndex: return "Invalid property snapshot reference identifier.";
		case EPropertySnapshotError::UnresolvedReference: return "Property snapshot hard reference no longer resolves.";
		case EPropertySnapshotError::TrailingBytes: return "Property snapshot has trailing bytes.";
		case EPropertySnapshotError::InvalidMapKey: return "Property snapshot Map key is unsupported.";
		case EPropertySnapshotError::ArchiveFailure:
			return std::format("Property snapshot Archive failure {} at '{}'.",
				Error.ArchiveCode ? static_cast<uint32>(*Error.ArchiveCode) : 0, Error.ArchivePath);
		}
		return {};
	}

	auto SerializeReflectedPropertyValue(
		FArchive& Ar,
		FProperty& Property,
		void* Container,
		uint32 ArrayIndex,
		bool bIncludeRawObjectReferences) -> void
	{
		auto FieldScope = EnterArchiveField(Ar, MakeFieldDescriptor(&Property));
		auto FixedScope = Property.GetArrayDim() > 1
			? EnterArchiveFixedArrayElement(Ar, ArrayIndex) : FArchivePathScope();
		SerializePropertyValue(Ar, &Property, Container, ArrayIndex, bIncludeRawObjectReferences);
	}

	auto SerializeDObjectProperties(FArchive& Ar, DObject& Object) -> void
	{
		MarkArchiveBaseReflectedFieldsSerialized(Ar);
		if (!Object.GetClass())
		{
			return;
		}

		Object.GetClass()->ForEachProperty(
			[&](FProperty* Property)
			{
				if (!ShouldSerializeReflectedProperty(Ar, Property))
				{
					return;
				}
				FArchivePropertySaveValue EffectiveValue;
				const EArchivePropertySaveDisposition Disposition = ResolvePropertySaveValue(
					Ar, *Property, &Object, 0, EffectiveValue);
				if (Disposition == EArchivePropertySaveDisposition::Omit) return;

				auto FieldScope = EnterArchiveField(Ar, MakeFieldDescriptor(
					Property, Object.GetClass()->GetQualifiedName()));
				for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
				{
					EffectiveValue = {&Object, Index};
					const EArchivePropertySaveDisposition ElementDisposition = ResolvePropertySaveValue(
						Ar, *Property, &Object, Index, EffectiveValue);
					if (ElementDisposition == EArchivePropertySaveDisposition::Omit)
					{
						Ar.Fail(EArchiveFailureCode::MalformedSerializer,
							"A reflected fixed-array property cannot be partially omitted.");
						return;
					}
					auto FixedScope = Property->GetArrayDim() > 1
						? EnterArchiveFixedArrayElement(Ar, Index) : FArchivePathScope();
					SerializePropertyValue(Ar, Property,
						const_cast<void*>(EffectiveValue.Container), EffectiveValue.ArrayIndex, false);
				}
			},
			true
		);
	}

	static auto ObjectGraphFailure(FObjectGraphError Error, const FObjectArchive* Archive = nullptr) -> std::unexpected<FObjectGraphError>
	{
		if (Archive)
		{
			if (const auto* Failure = Archive->GetFailure())
			{
				Error.ArchiveCode = Failure->Code;
				Error.ArchivePath = Failure->Path;
			}
			Error.Cause = Archive->GetValueFailureCause();
			if (const auto* Failure = Archive->GetFailure(); Failure && std::holds_alternative<std::monostate>(Error.Cause)) Error.Message = Failure->Message;
		}
		return std::unexpected(std::move(Error));
	}

	auto ToString(const FObjectGraphError& Error) -> std::string
	{
		if (Error.Code == EObjectGraphError::None) return {};
		std::string_view Reason;
		switch (Error.Code)
		{
		case EObjectGraphError::None: break;
		case EObjectGraphError::InvalidRoot: Reason = "Missing graph root"; break;
		case EObjectGraphError::TemplateRoot: Reason = "Template root cannot be serialized"; break;
		case EObjectGraphError::Discovery: Reason = "Graph discovery failed"; break;
		case EObjectGraphError::PropertyWrite: Reason = "Graph property serialization failed"; break;
		case EObjectGraphError::HeaderWrite: Reason = "Graph record serialization failed"; break;
		case EObjectGraphError::Header: Reason = "Invalid graph header"; break;
		case EObjectGraphError::RecordPayload: Reason = "Invalid graph record payload"; break;
		case EObjectGraphError::RecordIdentity: Reason = "Invalid or duplicate graph record identity"; break;
		case EObjectGraphError::Construction: Reason = "Graph object construction failed"; break;
		case EObjectGraphError::TrailingBytes: Reason = "Graph contains trailing bytes"; break;
		case EObjectGraphError::OuterCycle: Reason = "Graph outer chain is cyclic or unresolved"; break;
		case EObjectGraphError::OuterIdentity: Reason = "Graph outer identity is unresolved"; break;
		case EObjectGraphError::PropertyRead: Reason = "Graph property deserialization failed"; break;
		case EObjectGraphError::Validation: Reason = "Loaded graph validation rejected the object"; break;
		case EObjectGraphError::RootIdentity: Reason = "Graph root identity is unresolved"; break;
		case EObjectGraphError::MissingClass: Reason = "Graph class has no constructor"; break;
		case EObjectGraphError::AuthoredOverrides: Reason = "Graph authored override copy failed"; break;
		}
		std::string Message = std::format("{}: object={}, class={}, id={}, outer={}, archive path={}", Reason,
			Error.ObjectName, Error.ClassName, Error.ObjectId, Error.OuterId, Error.ArchivePath);
		if (!std::holds_alternative<std::monostate>(Error.Cause))
			Message += ": " + std::visit([](const auto& Cause) -> std::string {
				if constexpr (std::is_same_v<std::decay_t<decltype(Cause)>, std::monostate>) return {};
				else return ToString(Cause);
			}, Error.Cause);
		else if (Error.OverrideCause) Message += std::format(": Authored override failure {} at '{}'",
			static_cast<uint32>(Error.OverrideCause->Reason), Error.OverrideCause->LogicalPath);
		else if (!Error.Message.empty()) Message += ": " + Error.Message;
		return Message;
	}

	auto SaveObjectGraphToMemory(DObject* RootObject, FByteBuffer& OutBytes) -> std::expected<void, FObjectGraphError>
	{
		if (!RootObject) return ObjectGraphFailure({.Code = EObjectGraphError::InvalidRoot});
		if (RootObject->IsTemplateObject()) return ObjectGraphFailure({.Code = EObjectGraphError::TemplateRoot,
			.ObjectName = RootObject->GetObjectPath()});

		FObjectGraphContext Context;
		Context.Discover(RootObject);
		FObjectGraphDiscoveryArchive DiscoveryArchive(Context);
		for (size_t Index = 0; Index < Context.Objects.size() && !DiscoveryArchive.IsError(); ++Index)
		{
			DObject* Object = Context.Objects[Index];
			auto ObjectScope = DiscoveryArchive.EnterObject(*Object);
			Object->Serialize(DiscoveryArchive);
		}
		if (DiscoveryArchive.IsError()) return ObjectGraphFailure({.Code = EObjectGraphError::Discovery,
			.ObjectName = RootObject->GetObjectPath()}, &DiscoveryArchive);
		Context.Freeze();

		FByteBuffer Bytes;
		FObjectMemoryWriter HeaderWriter(Bytes, EArchivePurpose::ObjectGraph);
		uint32 Magic = ObjectGraphMagic;
		uint32 Version = ObjectGraphVersion;
		uint64 RootId = Context.FindId(RootObject);
		uint64 ObjectCount = static_cast<uint64>(Context.Objects.size());
		HeaderWriter << Magic << Version << RootId << ObjectCount;

		for (DObject* Object : Context.Objects)
		{
			uint64 Id = Context.FindId(Object);
			const FObjectGraphContext::FObjectMetadata& Metadata = Context.Metadata[Object];
			uint64 OuterId = 0;
			if (Metadata.Outer)
			{
				auto OuterIt = Context.ObjectToId.find(Metadata.Outer);
				OuterId = OuterIt != Context.ObjectToId.end() ? OuterIt->second : 0;
			}
			std::string ClassName = Metadata.ClassName;
			std::string ObjectName = Metadata.ObjectName;
			FByteBuffer PropertyBytes;
			FObjectGraphWriter PropertyWriter(PropertyBytes, Context);
			{
				auto ObjectScope = PropertyWriter.EnterObject(*Object);
				Object->Serialize(PropertyWriter);
			}
			if (PropertyWriter.IsError()) return ObjectGraphFailure({.Code = EObjectGraphError::PropertyWrite,
				.ObjectName = ObjectName, .ClassName = ClassName, .ObjectId = Id}, &PropertyWriter);
			uint64 PropertySize = static_cast<uint64>(PropertyBytes.size());

			HeaderWriter << Id << OuterId;
			WriteString(HeaderWriter, ClassName);
			WriteString(HeaderWriter, ObjectName);
			HeaderWriter << PropertySize;
			if (PropertySize > 0)
			{
				HeaderWriter.SerializeRawBytes(std::as_writable_bytes(
					FMutableByteView(PropertyBytes.data(), PropertyBytes.size())));
			}
		}

		if (HeaderWriter.IsError()) return ObjectGraphFailure({.Code = EObjectGraphError::HeaderWrite,
			.ObjectId = RootId, .ObjectCount = ObjectCount}, &HeaderWriter);
		OutBytes = std::move(Bytes);
		return {};
	}

	auto LoadObjectGraphFromMemory(const FByteBuffer& Bytes) -> std::expected<DObject*, FObjectGraphError>
	{
		FObjectMemoryReader Reader(Bytes);
		uint32 Magic = 0;
		uint32 Version = 0;
		uint64 RootId = 0;
		uint64 ObjectCount = 0;
		Reader << Magic << Version << RootId << ObjectCount;
		if (Reader.IsError() || Magic != ObjectGraphMagic || Version != ObjectGraphVersion || ObjectCount == 0
			|| ObjectCount > 1000000 || RootId == 0 || RootId > ObjectCount)
		{
			return ObjectGraphFailure({.Code = EObjectGraphError::Header, .ObjectId = RootId, .ObjectCount = ObjectCount,
				.Magic = Magic, .ExpectedMagic = ObjectGraphMagic, .Version = Version, .ExpectedVersion = ObjectGraphVersion}, &Reader);
		}

		struct FLoadedObjectRecord
		{
			uint64 Id = 0;
			uint64 OuterId = 0;
			std::string ClassName;
			std::string ObjectName;
			FByteBuffer PropertyBytes;
		};

		std::vector<FLoadedObjectRecord> Records;
		Records.resize(static_cast<size_t>(ObjectCount));
		std::vector<uint64> OuterIds(static_cast<size_t>(ObjectCount));
		FObjectGraphContext Context;
		Context.IdToObject.resize(static_cast<size_t>(ObjectCount));
		auto DiscardLoadedObjects = [&Context]() {
			for (DObject* Object : Context.IdToObject) MarkObjectHierarchyAsGarbage(Object);
		};

		for (FLoadedObjectRecord& Record : Records)
		{
			uint64 PropertySize = 0;
			Reader << Record.Id << Record.OuterId;
			Reader << Record.ClassName;
			Reader << Record.ObjectName;
			Reader << PropertySize;
			if (Reader.IsError() || PropertySize > Reader.GetRemainingPayloadBytes()
				|| PropertySize > FByteBuffer().max_size())
			{
				DiscardLoadedObjects();
				return ObjectGraphFailure({.Code = EObjectGraphError::RecordPayload, .ObjectName = Record.ObjectName, .ClassName = Record.ClassName,
					.ObjectId = Record.Id, .OuterId = Record.OuterId, .ObjectCount = ObjectCount,
					.ActualBytes = PropertySize, .RemainingBytes = Reader.GetRemainingPayloadBytes()}, &Reader);
			}
			Record.PropertyBytes.resize(static_cast<size_t>(PropertySize));
			if (PropertySize > 0)
			{
				Reader.SerializeRawBytes(std::as_writable_bytes(
					FMutableByteView(Record.PropertyBytes.data(), Record.PropertyBytes.size())));
			}
			if (Reader.IsError() || Record.Id == 0 || Record.Id > ObjectCount || Context.ResolveId(Record.Id)
				|| Record.OuterId > ObjectCount)
			{
				DiscardLoadedObjects();
				return ObjectGraphFailure({.Code = EObjectGraphError::RecordIdentity, .ObjectName = Record.ObjectName, .ClassName = Record.ClassName,
					.ObjectId = Record.Id, .OuterId = Record.OuterId, .ObjectCount = ObjectCount}, &Reader);
			}
			OuterIds[static_cast<size_t>(Record.Id - 1)] = Record.OuterId;

			DClass* Class = FindClassByName(Record.ClassName);
			if (!Class || !Class->ClassConstructor)
			{
				Class = DObject::StaticClass();
			}

			FStaticConstructObjectParameters Params;
			Params.Class = Class;
			Params.Name = FName(Record.ObjectName);
			Params.Size = Class->PropertiesSize;
			Params.Purpose = EObjectConstructionPurpose::AssetLoad;
			DObject* Object = StaticConstructObject(Params);
			if (!Object)
			{
				DiscardLoadedObjects();
				return ObjectGraphFailure({.Code = EObjectGraphError::Construction, .ObjectName = Record.ObjectName,
					.ClassName = Record.ClassName, .ObjectId = Record.Id});
			}
			DObjectForceRegistration(Object);
			Context.IdToObject[static_cast<size_t>(Record.Id - 1)] = Object;
		}
		if (Reader.IsError() || Reader.GetRemainingPayloadBytes() != 0)
		{
			DiscardLoadedObjects();
			return ObjectGraphFailure({.Code = EObjectGraphError::TrailingBytes, .RemainingBytes = Reader.GetRemainingPayloadBytes()}, &Reader);
		}

		for (uint64 Id = 1; Id <= ObjectCount; ++Id)
		{
			std::unordered_set<uint64> VisitedOuterIds;
			for (uint64 OuterId = OuterIds[static_cast<size_t>(Id - 1)]; OuterId != 0;
				OuterId = OuterIds[static_cast<size_t>(OuterId - 1)])
			{
				if (!Context.ResolveId(OuterId) || !VisitedOuterIds.insert(OuterId).second)
				{
					DiscardLoadedObjects();
					return ObjectGraphFailure({.Code = EObjectGraphError::OuterCycle, .ObjectId = Id, .OuterId = OuterId, .ObjectCount = ObjectCount});
				}
			}
		}

		for (const FLoadedObjectRecord& Record : Records)
		{
			DObject* Object = Context.ResolveId(Record.Id);
			DObject* Outer = Context.ResolveId(Record.OuterId);
			if (!Object || (Record.OuterId != 0 && !Outer))
			{
				DiscardLoadedObjects();
				return ObjectGraphFailure({.Code = EObjectGraphError::OuterIdentity, .ObjectName = Record.ObjectName, .ClassName = Record.ClassName,
					.ObjectId = Record.Id, .OuterId = Record.OuterId, .ObjectCount = ObjectCount});
			}
			Object->SetOuterPrivate(Outer);
			FObjectGraphReader PropertyReader(Record.PropertyBytes, Context);
			{
				auto ObjectScope = PropertyReader.EnterObject(*Object);
				Object->Serialize(PropertyReader);
			}
			if (PropertyReader.IsError() || PropertyReader.GetRemainingPayloadBytes() != 0)
			{
				DiscardLoadedObjects();
				return ObjectGraphFailure({.Code = EObjectGraphError::PropertyRead, .ObjectName = Record.ObjectName, .ClassName = Record.ClassName,
					.ObjectId = Record.Id, .ActualBytes = Record.PropertyBytes.size(), .RemainingBytes = PropertyReader.GetRemainingPayloadBytes()}, &PropertyReader);
			}
		}

		for (const FLoadedObjectRecord& Record : Records)
		{
			if (const auto Validated = Context.ResolveId(Record.Id)->ValidateLoadedObjectGraph({}); !Validated)
			{
				DiscardLoadedObjects();
				return ObjectGraphFailure({.Code = EObjectGraphError::Validation, .ObjectName = Record.ObjectName, .ClassName = Record.ClassName,
					.ObjectId = Record.Id, .Cause = Validated.error()});
			}
		}
		DObject* LoadedRoot = Context.ResolveId(RootId);
		if (!LoadedRoot)
		{
			DiscardLoadedObjects();
			return ObjectGraphFailure({.Code = EObjectGraphError::RootIdentity, .ObjectId = RootId, .ObjectCount = ObjectCount});
		}
		return LoadedRoot;
	}

	static auto DuplicateObjectInternal(DObject* RootObject, DObject* NewOuter,
		FName NewName,
		std::unordered_map<DObject*, DObject*>* OutDuplicates) -> std::expected<DObject*, FObjectGraphError>
	{
		if (OutDuplicates) OutDuplicates->clear();
		if (!RootObject)
		{
			return ObjectGraphFailure({.Code = EObjectGraphError::InvalidRoot});
		}
		if (RootObject->IsTemplateObject())
		{
			return ObjectGraphFailure({.Code = EObjectGraphError::TemplateRoot, .ObjectName = RootObject->GetObjectPath()});
		}

		std::vector<DObject*> Sources;
		std::unordered_set<DObject*> Visited;
		std::function<void(DObject*)> GatherInnerTree = [&](DObject* Object) {
			if (!Object || !Visited.insert(Object).second) return;
			if (Object != RootObject && Object->HasAnyObjectFlags(EObjectFlags::Transient)) return;
			Sources.push_back(Object);
			for (DObject* Inner : GDObjectArray.GetObjectsWithOuter(Object, EObjectQueryScope::LiveOnly)) GatherInnerTree(Inner);
		};
		GatherInnerTree(RootObject);

		std::unordered_map<DObject*, DObject*> Duplicates;
		std::unordered_set<DObject*> ClaimedConstructedInners;
		DObject* DuplicateRoot = nullptr;
		auto DiscardDuplicates = [&DuplicateRoot]() { MarkObjectHierarchyAsGarbage(DuplicateRoot); };
		for (DObject* Source : Sources)
		{
			DObject* DuplicateOuter = Source == RootObject ? NewOuter : Duplicates[Source->GetOuter()];
			if (Source != RootObject && !DuplicateOuter)
			{
				DiscardDuplicates();
				return ObjectGraphFailure({.Code = EObjectGraphError::OuterIdentity, .ObjectName = Source->GetObjectPath()});
			}

			DObject* Duplicate = nullptr;
			if (Source != RootObject)
			{
				// Actor constructors create their default components. Reuse those matching inners
				// instead of constructing a second component with the same identity.
				for (DObject* Existing : GDObjectArray.GetObjectsWithOuter(DuplicateOuter, EObjectQueryScope::LiveOnly))
				{
					if (!ClaimedConstructedInners.contains(Existing) && Existing->GetClass() == Source->GetClass() && Existing->GetFName() == Source->GetFName())
					{
						Duplicate = Existing;
						ClaimedConstructedInners.insert(Existing);
						break;
					}
				}
			}

			if (!Duplicate)
			{
				DClass* Class = Source->GetClass();
				if (!Class || !Class->ClassConstructor)
				{
					DiscardDuplicates();
					return ObjectGraphFailure({.Code = EObjectGraphError::MissingClass, .ObjectName = Source->GetObjectPath()});
				}
				FStaticConstructObjectParameters Params;
				Params.Class = Class;
				Params.Outer = DuplicateOuter;
				Params.Name = Source == RootObject && !NewName.IsNone() ? NewName : Source->GetFName();
				Params.Size = Class->PropertiesSize;
				Params.Purpose = EObjectConstructionPurpose::Duplication;
				Duplicate = StaticConstructObject(Params);
				if (!Duplicate)
				{
					DiscardDuplicates();
					return ObjectGraphFailure({.Code = EObjectGraphError::Construction, .ObjectName = Source->GetObjectPath()});
				}
				DObjectForceRegistration(Duplicate);
			}
			Duplicates.emplace(Source, Duplicate);
			if (Source == RootObject) DuplicateRoot = Duplicate;
		}

		std::unordered_map<DObject*, uint64> SourceIds;
		std::vector<DObject*> DuplicateById;
		DuplicateById.reserve(Sources.size());
		for (size_t Index = 0; Index < Sources.size(); ++Index)
		{
			SourceIds.emplace(Sources[Index], static_cast<uint64>(Index) + 1);
			DuplicateById.push_back(Duplicates[Sources[Index]]);
		}
		std::vector<DObject*> ExternalReferences;

			class FDuplicateWriter final : public FObjectMemoryWriter
		{
		public:
			FDuplicateWriter(FByteBuffer& Bytes,
				const std::unordered_map<DObject*, uint64>& InSourceIds,
				std::vector<DObject*>& InExternalReferences)
				: FObjectMemoryWriter(Bytes, EArchivePurpose::Duplicate)
				, Ids(InSourceIds)
				, Externals(InExternalReferences)
			{
				EnableCapabilities(EArchiveCapability::ObjectReferences);
			}

			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				if (!Object)
				{
					uint8 Kind = static_cast<uint8>(EArchiveObjectReferenceKind::Null);
					*this << Kind;
					return;
				}
				if (const auto It = Ids.find(Object); It != Ids.end())
				{
					uint8 Kind = static_cast<uint8>(EArchiveObjectReferenceKind::Internal);
					uint64 Id = It->second;
					*this << Kind << Id;
					return;
				}
				uint8 Kind = static_cast<uint8>(EArchiveObjectReferenceKind::External);
				auto It = std::ranges::find(Externals, Object);
				if (It == Externals.end())
				{
					Externals.push_back(Object);
					It = std::prev(Externals.end());
				}
				uint64 Id = static_cast<uint64>(std::distance(Externals.begin(), It)) + 1;
				*this << Kind << Id;
			}
			auto SerializeWeakObjectReference(FWeakObjectPtr& Value) -> void override
			{
				const auto It = Ids.find(Value.Get());
				uint64 Id = It == Ids.end() ? 0 : It->second;
				*this << Id;
			}
		private:
			const std::unordered_map<DObject*, uint64>& Ids;
			std::vector<DObject*>& Externals;
		};

			class FDuplicateReader final : public FObjectMemoryReader
		{
		public:
			FDuplicateReader(const FByteBuffer& Bytes,
				const std::vector<DObject*>& InDuplicates,
				const std::vector<DObject*>& InExternalReferences)
				: FObjectMemoryReader(Bytes, EArchivePurpose::Duplicate)
				, DuplicateObjects(InDuplicates)
				, Externals(InExternalReferences)
			{
				EnableCapabilities(EArchiveCapability::ObjectReferences);
			}
			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint8 Kind = 0;
				*this << Kind;
				if (IsError()) return;
				if (Kind == static_cast<uint8>(EArchiveObjectReferenceKind::Null))
				{
					Object = nullptr;
					return;
				}
				uint64 Id = 0;
				*this << Id;
				if (IsError()) return;
				if (Kind == static_cast<uint8>(EArchiveObjectReferenceKind::Internal)
					&& Id > 0 && Id <= DuplicateObjects.size())
				{
					Object = DuplicateObjects[static_cast<size_t>(Id - 1)];
					return;
				}
				if (Kind == static_cast<uint8>(EArchiveObjectReferenceKind::External)
					&& Id > 0 && Id <= Externals.size())
				{
					Object = Externals[static_cast<size_t>(Id - 1)];
					return;
				}
				Fail(EArchiveFailureCode::InvalidObjectReference,
					"Duplicate stream contains an invalid reference token.");
			}
			auto SerializeWeakObjectReference(FWeakObjectPtr& Value) -> void override
			{
				uint64 Id = 0;
				*this << Id;
				if (IsError()) return;
				if (Id > DuplicateObjects.size())
				{
					Fail(EArchiveFailureCode::InvalidObjectReference, "Duplicate stream contains an invalid weak reference token.");
					return;
				}
				Value.SetObject(Id == 0 ? nullptr : DuplicateObjects[static_cast<size_t>(Id - 1)]);
			}
		private:
			const std::vector<DObject*>& DuplicateObjects;
			const std::vector<DObject*>& Externals;
		};

		for (DObject* Source : Sources)
		{
			FByteBuffer Bytes;
			FDuplicateWriter Writer(Bytes, SourceIds, ExternalReferences);
			{
				auto ObjectScope = Writer.EnterObject(*Source);
				Source->Serialize(Writer);
			}
			if (Writer.IsError())
			{
				DiscardDuplicates();
				return ObjectGraphFailure({.Code = EObjectGraphError::PropertyWrite, .ObjectName = Source->GetObjectPath()}, &Writer);
			}
			FDuplicateReader Reader(Bytes, DuplicateById, ExternalReferences);
			{
				auto ObjectScope = Reader.EnterObject(*Duplicates[Source]);
				Duplicates[Source]->Serialize(Reader);
			}
			if (Reader.IsError() || Reader.GetRemainingPayloadBytes() != 0)
			{
				DiscardDuplicates();
				return ObjectGraphFailure({.Code = EObjectGraphError::PropertyRead, .ObjectName = Source->GetObjectPath(),
					.ActualBytes = Bytes.size(), .RemainingBytes = Reader.GetRemainingPayloadBytes()}, &Reader);
			}
		}

		for (DObject* Source : Sources)
		{
			FAuthoredOverrideDiagnostic LedgerDiagnostic;
			if (!Duplicates[Source]->CopyAuthoredOverridesFrom(*Source, &LedgerDiagnostic))
			{
				DiscardDuplicates();
				return ObjectGraphFailure({.Code = EObjectGraphError::AuthoredOverrides, .ObjectName = Source->GetObjectPath(),
					.OverrideCause = LedgerDiagnostic});
			}
		}

		for (DObject* Source : Sources)
		{
			if (const auto Validated = Duplicates[Source]->ValidateLoadedObjectGraph({}); !Validated)
			{
				DiscardDuplicates();
				return ObjectGraphFailure({.Code = EObjectGraphError::Validation, .ObjectName = Source->GetObjectPath(),
					.Cause = Validated.error()});
			}
		}
		for (auto It = Sources.rbegin(); It != Sources.rend(); ++It)
		{
			Duplicates[*It]->PostLoad();
		}
		if (OutDuplicates) *OutDuplicates = Duplicates;
		return DuplicateRoot;
	}

	auto DuplicateObject(const DObject* SourceObject, DObject* NewOuter,
		FName NewName,
		std::unordered_map<DObject*, DObject*>* OutDuplicates) -> std::expected<DObject*, FObjectGraphError>
	{
		return DuplicateObjectInternal(
			const_cast<DObject*>(SourceObject), NewOuter, NewName, OutDuplicates);
	}

	namespace
	{
		auto MakeCopyError(EObjectPropertyCopyError Code, EObjectPropertyCopyOperation Operation,
			const DObject* Source, const DObject* Destination, const FProperty* Property = nullptr,
			uint32 Index = 0) -> std::expected<void, FObjectPropertyCopyError>
		{
			FObjectPropertyCopyError Error{.Code = Code, .Operation = Operation};
			if (Source) { Error.SourcePath = Source->GetObjectPath(); Error.SourceType = Source->GetClass()->GetQualifiedName().ToString(); }
			if (Destination) { Error.DestinationPath = Destination->GetObjectPath(); Error.DestinationType = Destination->GetClass()->GetQualifiedName().ToString(); }
			if (Property) { Error.PropertyName = Property->NamePrivate.ToString(); Error.ArrayIndex = Index; }
			return std::unexpected(std::move(Error));
		}
		auto CopyArchiveDiagnostic(FObjectPropertyCopyError& Error, const FObjectArchive& Archive) -> void
		{
			if (const auto* Failure = Archive.GetFailure())
			{
				Error.ArchiveCode = Failure->Code;
				Error.ArchivePath = Failure->Path;
			}
			std::visit([&](const auto& Cause) { Error.Cause = Cause; }, Archive.GetValueFailureCause());
			if (const auto* Failure = Archive.GetFailure(); Failure && std::holds_alternative<std::monostate>(Error.Cause)) Error.Message = Failure->Message;
		}
	}

	auto ToString(const FObjectPropertyCopyError& Error) -> std::string
	{
		if (!std::holds_alternative<std::monostate>(Error.Cause))
			return std::visit([](const auto& Cause) -> std::string {
				using T = std::decay_t<decltype(Cause)>;
				if constexpr (std::is_same_v<T, std::monostate>) return {};
				else if constexpr (std::is_same_v<T, EContainerOpResult>) return std::format("Default reference traversal failed: {}", static_cast<uint32>(Cause));
				else return ToString(Cause);
			}, Error.Cause);
		if (!Error.Message.empty()) return Error.Message;
		switch (Error.Code)
		{
		case EObjectPropertyCopyError::None: return {};
		case EObjectPropertyCopyError::InvalidObjects: return "Property copying requires matching source and destination classes.";
		case EObjectPropertyCopyError::UnmappedDefaultReference: return "Unmapped default subobject reference.";
		case EObjectPropertyCopyError::ContainerTraversal: return "Default reference container traversal failed.";
		case EObjectPropertyCopyError::ValueCopy:
			return "Default property value copy failed.";
		case EObjectPropertyCopyError::Snapshot:
			return "Editable property snapshot failed.";
		case EObjectPropertyCopyError::ArchiveWrite: case EObjectPropertyCopyError::ArchiveRead:
			return std::format("Property copy Archive failed at '{}', code={}." , Error.ArchivePath,
				static_cast<uint32>(Error.ArchiveCode.value_or(EArchiveFailureCode::InvalidData)));
		case EObjectPropertyCopyError::InvalidReferenceIndex: return "Editable-copy stream contains an invalid reference identifier.";
		case EObjectPropertyCopyError::TrailingBytes: return "Editable-copy stream contains trailing bytes.";
		}
		return {};
	}

	auto InitializeObjectFromDefaults(const DObject* Defaults, DObject* Destination,
		const std::unordered_map<DObject*, DObject*>& ReferenceMap) -> std::expected<void, FObjectPropertyCopyError>
	{
		using E = EObjectPropertyCopyError;
		auto Fail = [&](E Code, FProperty* Property = nullptr, uint32 Index = 0) {
			return MakeCopyError(Code, EObjectPropertyCopyOperation::Defaults, Defaults, Destination, Property, Index);
		};
		if (!Defaults || !Destination || Defaults->GetClass() != Destination->GetClass()) return Fail(E::InvalidObjects);
		using FRemap = std::function<std::expected<void, FObjectPropertyCopyError>(FProperty*, void*, uint32)>;
		struct FRemapContext { FRemap* Function; FProperty* Property; std::expected<void, FObjectPropertyCopyError> Result; };
		FRemap Remap;
		Remap = [&](FProperty* Property, void* Container, uint32 Index) -> std::expected<void, FObjectPropertyCopyError> {
			auto Value = [&]() -> std::expected<void, FObjectPropertyCopyError> {
				switch (Property->GetKind())
				{
				case DurinCodeGen::EPropertyGenFlags::Object:
				{
					auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
					DObject* Object = ObjectProperty->GetObjectPropertyValue(Container, Index);
					if (const auto It = ReferenceMap.find(Object); It != ReferenceMap.end()) Object = It->second;
					if (Object && Object->IsTemplateObject())
					{
						auto Result = Fail(E::UnmappedDefaultReference, Property, Index);
						Result.error().ReferencePath = Object->GetObjectPath();
						return Result;
					}
					ObjectProperty->SetObjectPropertyValue(Container, Object, Index);
					return {};
				}
				case DurinCodeGen::EPropertyGenFlags::Struct:
				{
					auto* Struct = static_cast<FStructProperty*>(Property);
					std::expected<void, FObjectPropertyCopyError> Result;
					Struct->GetStruct()->ForEachProperty([&](FProperty* Field) {
						for (uint32 Element = 0; Result && Element < Field->GetArrayDim(); ++Element)
							Result = Remap(Field, Struct->GetValuePtr(Container, Index), Element);
					});
					return Result;
				}
				case DurinCodeGen::EPropertyGenFlags::Array:
				case DurinCodeGen::EPropertyGenFlags::Map:
				{
					FRemapContext Context{&Remap, nullptr, {}};
					EContainerOpResult Traversal;
					if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Array)
					{
						auto* Array = static_cast<FArrayProperty*>(Property);
						Context.Property = Array->GetInner();
						Traversal = Array->VisitMutableElements(Container, [](void* Raw, uint64 ElementIndex, void* Value) {
							auto& C = *static_cast<FRemapContext*>(Raw);
							C.Result = (*C.Function)(C.Property, Value, 0);
							if (!C.Result) C.Result.error().PropertyRoute.insert(C.Result.error().PropertyRoute.begin(), std::to_string(ElementIndex));
							return C.Result.has_value();
						}, &Context, Index);
					}
					else
					{
						auto* Map = static_cast<FMapProperty*>(Property);
						Context.Property = Map->GetValueProp();
						Traversal = Map->VisitMutableEntries(Container, [](void* Raw, const void*, void* Value) {
							auto& C = *static_cast<FRemapContext*>(Raw);
							C.Result = (*C.Function)(C.Property, Value, 0);
							return C.Result.has_value();
						}, &Context, Index);
					}
					if (!Context.Result) return Context.Result;
					if (Traversal != EContainerOpResult::Success)
					{
						auto Result = Fail(E::ContainerTraversal, Property, Index);
						Result.error().Cause = Traversal;
						return Result;
					}
					return {};
				}
				default: return {};
				}
			}();
			if (!Value) Value.error().PropertyRoute.insert(Value.error().PropertyRoute.begin(),
				std::format("{}[{}]", Property->NamePrivate.ToString(), Index));
			return Value;
		};
		std::expected<void, FObjectPropertyCopyError> Result;
		Defaults->GetClass()->ForEachProperty([&](FProperty* Property) {
			if (!Result || !Property || Property->IsDeprecated()
				|| Property->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
			for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
			{
				const auto Copied = Property->CopyAssignValue(Property->ContainerPtrToValuePtr<void>(Destination, Index),
					Property->ContainerPtrToValuePtr<void>(Defaults, Index));
				if (!Copied)
				{
					Result = Fail(E::ValueCopy, Property, Index);
					Result.error().Cause = Copied.error();
					return;
				}
				Result = Remap(Property, Destination, Index);
				if (!Result) return;
			}
		});
		return Result;
	}

	auto CopyEditableObjectProperties(DObject* Source, DObject* Destination, const std::unordered_map<DObject*, DObject*>& ReferenceMap) -> std::expected<void, FObjectPropertyCopyError>
	{
		using E = EObjectPropertyCopyError;
		auto Fail = [&](E Code, FProperty* Property = nullptr, uint32 Index = 0) {
			return MakeCopyError(Code, EObjectPropertyCopyOperation::Editable, Source, Destination, Property, Index);
		};
		if (!Source || !Destination || Source->GetClass() != Destination->GetClass()) return Fail(E::InvalidObjects);

		class FEditableCopyWriter final : public FObjectMemoryWriter
		{
		public:
			FEditableCopyWriter(FByteBuffer& Bytes, std::vector<DObject*>& InReferences)
				: FObjectMemoryWriter(Bytes, EArchivePurpose::EditableCopy), References(InReferences)
			{
				EnableCapabilities(EArchiveCapability::ObjectReferences);
			}
			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint64 Id = 0;
				if (Object)
				{
					auto It = std::ranges::find(References, Object);
					if (It == References.end())
					{
						References.push_back(Object);
						It = std::prev(References.end());
					}
					Id = static_cast<uint64>(std::distance(References.begin(), It)) + 1;
				}
				*this << Id;
			}
		private:
			std::vector<DObject*>& References;
		};

		class FRemappingReader final : public FObjectMemoryReader
		{
		public:
			std::optional<uint64> InvalidReference;
			FRemappingReader(const FByteBuffer& Bytes,
				const std::vector<DObject*>& InReferences,
				const std::unordered_map<DObject*, DObject*>& InReferenceMap)
				: FObjectMemoryReader(Bytes, EArchivePurpose::EditableCopy)
				, References(InReferences)
				, Map(InReferenceMap)
			{
				EnableCapabilities(EArchiveCapability::ObjectReferences);
			}
			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint64 Id = 0;
				*this << Id;
				if (IsError()) return;
				if (Id > References.size())
				{
					InvalidReference = Id;
					Fail(EArchiveFailureCode::InvalidObjectReference, ToString(FObjectPropertyCopyError{.Code = EObjectPropertyCopyError::InvalidReferenceIndex}));
					return;
				}
				DObject* SourceReference = Id == 0 ? nullptr : References[static_cast<size_t>(Id - 1)];
				const auto It = Map.find(SourceReference);
				Object = It == Map.end() ? SourceReference : It->second;
			}
		private:
			const std::vector<DObject*>& References;
			const std::unordered_map<DObject*, DObject*>& Map;
		};

		struct FOriginalValue
		{
			FProperty* Property = nullptr;
			uint32 Index = 0;
			FPropertyValueSnapshot Snapshot;
		};
		std::vector<FOriginalValue> OriginalValues;
		std::expected<void, FObjectPropertyCopyError> Result;
		auto RollBack = [&]() {
			for (auto It = OriginalValues.rbegin(); It != OriginalValues.rend(); ++It)
			{
				const auto Restored = RestorePropertyValue(It->Property, Destination, It->Index, It->Snapshot);
				if (!Restored && !Result.error().RollbackCause) Result.error().RollbackCause = Restored.error();
			}
		};

		Source->GetClass()->ForEachProperty([&](FProperty* Property) {
			if (!Result || !Property || !Property->HasAnyPropertyFlags(EPropertyFlags::Edit) || Property->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
			for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
			{
				FOriginalValue Original{Property, Index, {}};
				const auto Captured = CapturePropertyValue(Property, Destination, Index, Original.Snapshot);
				if (!Captured)
				{
					Result = Fail(E::Snapshot, Property, Index);
					Result.error().Cause = Captured.error();
					return;
				}
				OriginalValues.push_back(std::move(Original));

				FByteBuffer Bytes;
				std::vector<DObject*> References;
				FEditableCopyWriter Writer(Bytes, References);
				SerializeReflectedPropertyValue(Writer, *Property, Source, Index);
				if (Writer.IsError())
				{
					Result = Fail(E::ArchiveWrite, Property, Index);
					CopyArchiveDiagnostic(Result.error(), Writer);
					return;
				}
				FRemappingReader Reader(Bytes, References, ReferenceMap);
				SerializeReflectedPropertyValue(Reader, *Property, Destination, Index);
				if (Reader.IsError() || Reader.GetRemainingPayloadBytes() != 0)
				{
					Result = Fail(Reader.InvalidReference ? E::InvalidReferenceIndex : Reader.IsError() ? E::ArchiveRead : E::TrailingBytes, Property, Index);
					CopyArchiveDiagnostic(Result.error(), Reader);
					Result.error().ActualCount = Reader.InvalidReference.value_or(Reader.GetRemainingPayloadBytes());
					Result.error().ExpectedCount = Reader.InvalidReference ? References.size() : 0;
					return;
				}
			}
		}, true);
		if (!Result)
		{
			RollBack();
			return Result;
		}
		Destination->MarkPackageDirty();
		return {};
	}
}
