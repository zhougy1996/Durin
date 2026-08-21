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
	namespace
	{
		constexpr uint32 ObjectGraphMagic = 0x4E524F44; // DORN
		constexpr uint32 ObjectGraphVersion = 2;
		constexpr uint64 MaximumSoftObjectPathBytes = 1024 * 1024;

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
			case DurinCodeGen::EPropertyGenFlags::Object:
				return FArchiveLogicalTypeDescriptor::Object(Property->GetReferencedClass()
					? Property->GetReferencedClass()->GetQualifiedName() : FName());
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
				return FArchiveLogicalTypeDescriptor::SoftObject(Property->GetReferencedClass()
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
			return !Context.Archive.HasError();
		}

		struct FArchiveMapEntry
		{
			std::vector<uint8> Token;
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
				std::string Error;
				if (!BuildCanonicalMapKeyToken(Context.Property->GetKeyProp(), Key, 0, Entry.Token, &Error))
				{
					Context.Archive.SetError(Error);
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
			return !Context.Archive.HasError();
		}

		auto SerializePropertyValue(FArchive& Ar, FProperty* Property, void* Container, uint32 ArrayIndex, bool bIncludeRawObjectReferences) -> void
		{
			if (Ar.HasError()) return;
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
				switch (Property->GetElementSize())
				{
				case 1: Ar << *static_cast<uint8*>(Property->GetValuePtr(Container, ArrayIndex)); break;
				case 2: Ar << *static_cast<uint16*>(Property->GetValuePtr(Container, ArrayIndex)); break;
				case 4: Ar << *static_cast<uint32*>(Property->GetValuePtr(Container, ArrayIndex)); break;
				case 8: Ar << *static_cast<uint64*>(Property->GetValuePtr(Container, ArrayIndex)); break;
				default: Ar.Fail(EArchiveFailureCode::UnsupportedType, "Enum underlying width is unsupported."); break;
				}
				break;
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
				if (Ar.IsLoading() && !Ar.HasError()) *Value = SerializedValue;
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
				if (Ar.IsLoading() && !Ar.HasError())
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
				FSoftObjectPath Path = Ar.IsSaving() ? Value->GetSoftObjectPath() : FSoftObjectPath();
				SerializeArchiveSoftObjectPath(Ar, Path);
				if (Ar.IsLoading() && !Ar.HasError())
				{
					if (Path.IsNull()) Value->Reset();
					else Value->SetPath(std::move(Path));
				}
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
						if (Ar.HasError() || !Field
							|| Field->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
						auto FieldScope = EnterArchiveField(Ar, MakeFieldDescriptor(
							Field, Struct->GetQualifiedName()));
						for (uint32 Index = 0; Index < Field->GetArrayDim() && !Ar.HasError(); ++Index)
						{
							auto FixedScope = Field->GetArrayDim() > 1
								? EnterArchiveFixedArrayElement(Ar, Index) : FArchivePathScope();
							SerializePropertyValue(
								Ar, Field, StructValue, Index, bIncludeRawObjectReferences);
						}
					}, false);
				};
				if (Ar.IsSaving())
				{
					SerializeStructValue(Property->GetValuePtr(Container, ArrayIndex));
					break;
				}

				std::string OperationError;
				if (!Struct->CanDefaultConstruct() || !Struct->CanDestroy()
					|| !Struct->CanCopyAssign())
				{
					Ar.SetError(std::format(
						"DStructOperationUnavailable: transactional loading requires "
						"DefaultConstruct, Destroy, and CopyAssign for '{}'.",
						Struct->GetQualifiedName().ToString()));
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
				FReflectedValueStorage Storage;
				if (!Storage.DefaultConstruct(StorageProperty, 0, &OperationError))
				{
					Ar.SetError(OperationError);
					break;
				}
				SerializeStructValue(Storage.GetValue());
				if (Ar.HasError()) break;
				if (Struct->HasPostDeserialize())
				{
					std::string PostDeserializeError;
					const FArchiveFormatVersion* DastVersion =
						Ar.GetVersionContext().FindFormat(FName("DAST"));
					FDStructPostDeserializeContext Context{
						.Source = Ar.GetPurpose() == EArchivePurpose::AuthoredPackage
							? EDStructDeserializeSource::AuthoredAsset
							: EDStructDeserializeSource::RuntimeArchive,
						.SourceVersion = DastVersion ? DastVersion->Version : 0,
						.Error = &PostDeserializeError};
					if (!Struct->GetOps().PostDeserialize(Storage.GetValue(), Context))
					{
						Ar.SetError(PostDeserializeError.empty()
							? std::format("PostDeserializeRejected: '{}' rejected the loaded value.",
								Struct->GetQualifiedName().ToString())
							: std::format("PostDeserializeRejected: {}", PostDeserializeError));
						break;
					}
				}
				if (!Property->CopyAssignValue(
					Property->GetValuePtr(Container, ArrayIndex), Storage.GetValue(), &OperationError))
					Ar.SetError(OperationError);
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				auto* ArrayProperty = static_cast<FArrayProperty*>(Property);
				FProperty* Inner = ArrayProperty->GetInner();
				if (!Inner || !ArrayProperty->HasArrayOps()
					|| !ArrayProperty->HasCapability(EArrayOpsFlags::Count))
				{
					Ar.SetError("ArrayOperationUnavailable: Count is required.");
					break;
				}

				uint64 Num = 0;
				if (Ar.IsSaving() && ArrayProperty->GetNum(Container, Num, ArrayIndex) != EContainerOpResult::Success)
				{
					Ar.SetError("ArrayOperationFailed: Count failed.");
					break;
				}
				Ar << Num;
				if (Ar.HasError()) break;
				if (Ar.IsLoading() && Num > 10000000)
				{
					Ar.SetError("Array element count exceeds the supported limit.");
					break;
				}
				if (Ar.IsSaving())
				{
					if (!ArrayProperty->HasCapability(EArrayOpsFlags::ConstTraversal))
					{
						Ar.SetError("ArrayOperationUnavailable: ConstTraversal is required for save.");
						break;
					}
					FArchiveArrayVisitContext Context{Ar, Inner, bIncludeRawObjectReferences};
					if (ArrayProperty->VisitElements(Container, &SerializeArrayElement, &Context, ArrayIndex)
						!= EContainerOpResult::Success && !Ar.HasError())
						Ar.SetError("ArrayOperationFailed: ConstTraversal failed.");
					break;
				}

				const FArrayOps& Ops = ArrayProperty->GetOps();
				if (!ArrayProperty->HasCapability(EArrayOpsFlags::DetachedStorage | EArrayOpsFlags::TransactionalCommit
					| EArrayOpsFlags::RandomAccess) || (Num > 0 && !ArrayProperty->HasCapability(EArrayOpsFlags::DefaultGrow)))
				{
					Ar.SetError("ArrayOperationUnavailable: transactional load requires DetachedStorage, RandomAccess, DefaultGrow, and TransactionalCommit.");
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
				for (uint64 Index = 0; Index < Num && !Ar.HasError(); ++Index)
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
				if (!Ar.HasError())
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
					Ar.SetError("MapOperationUnavailable: Count is required.");
					break;
				}
				uint64 Num = 0;
				if (Ar.IsSaving() && MapProperty->GetNum(Container, Num, ArrayIndex) != EContainerOpResult::Success)
				{
					Ar.SetError("MapOperationFailed: Count failed.");
					break;
				}
				Ar << Num;
				if (Ar.HasError()) break;
				if (Ar.IsLoading() && Num > 10000000)
				{
					Ar.SetError("Map element count exceeds the supported limit.");
					break;
				}
				if (Ar.IsSaving())
				{
					if (!MapProperty->HasCapability(EMapOpsFlags::ConstTraversal))
					{
						Ar.SetError("MapOperationUnavailable: ConstTraversal is required for save.");
						break;
					}
					FArchiveMapVisitContext Context{Ar, MapProperty, bIncludeRawObjectReferences,
						Ar.HasCapability(EArchiveCapability::CanonicalMapOrder)};
					if (MapProperty->VisitEntries(Container, &CollectOrSerializeMapEntry, &Context, ArrayIndex)
						!= EContainerOpResult::Success && !Ar.HasError())
						Ar.SetError("MapOperationFailed: ConstTraversal failed.");
					if (Ar.HasError() || !Context.bCanonical) break;
					std::ranges::sort(Context.Entries, {}, &FArchiveMapEntry::Token);
					for (size_t Index = 0; Index < Context.Entries.size() && !Ar.HasError(); ++Index)
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
						Ar.SetError("MapOperationUnavailable: transactional load requires DetachedStorage, Insert, and TransactionalCommit.");
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
					std::string Error;
					if (Num > 0
						&& (!KeyStorage.DefaultConstruct(MapProperty->GetKeyProp(), 0, &Error)
							|| !ValueStorage.DefaultConstruct(MapProperty->GetValueProp(), 0, &Error)))
					{
						Ar.SetError(Error);
						return;
					}
					for (uint64 Index = 0; Index < Num && !Ar.HasError(); ++Index)
					{
						if (Index > 0)
						{
							KeyStorage.Reset();
							ValueStorage.Reset();
							if (!KeyStorage.DefaultConstruct(MapProperty->GetKeyProp(), 0, &Error)
								|| !ValueStorage.DefaultConstruct(MapProperty->GetValueProp(), 0, &Error))
							{
								Ar.SetError(Error);
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
						if (Ar.HasError()) return;
						OpResult = Ops.InsertCopy(Detached.Get(), KeyStorage.GetValue(), ValueStorage.GetValue());
						if (OpResult != EContainerOpResult::Success)
						{
							Ar.SetError(OpResult == EContainerOpResult::DuplicateKey
								? std::format("MapDuplicateKey: MapEntry[{}].Key has a duplicate decoded key.", Index)
								: std::format("MapOperationFailed: Insert entry {} returned {}.", Index, static_cast<uint32>(OpResult)));
							return;
						}
					}
					if (!Ar.HasError())
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

		auto ValidateSnapshotProperty(const FProperty* Property, std::string* OutError) -> bool
		{
			if (!Property)
			{
				if (OutError) *OutError = "Cannot snapshot a null property.";
				return false;
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
			case DurinCodeGen::EPropertyGenFlags::Object:
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
				return true;
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				const auto* StructProperty = static_cast<const FStructProperty*>(Property);
				if (!StructProperty->GetStruct())
				{
					if (OutError) *OutError = "Cannot snapshot a struct property without reflected fields.";
					return false;
				}
				bool bValid = true;
				StructProperty->GetStruct()->ForEachProperty([&](FProperty* Field) {
					if (bValid && Field && !Field->HasAnyPropertyFlags(EPropertyFlags::Transient))
					{
						bValid = ValidateSnapshotProperty(Field, OutError);
					}
				}, false);
				return bValid;
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				const auto* ArrayProperty = static_cast<const FArrayProperty*>(Property);
				if (!ArrayProperty->HasArrayOps() || !ArrayProperty->GetInner()
					|| !ArrayProperty->HasCapability(EArrayOpsFlags::Count | EArrayOpsFlags::ConstTraversal
						| EArrayOpsFlags::RandomAccess | EArrayOpsFlags::DefaultGrow
						| EArrayOpsFlags::DetachedStorage | EArrayOpsFlags::TransactionalCommit))
				{
					if (OutError) *OutError = "Cannot snapshot an array without Count, ConstTraversal, RandomAccess, DefaultGrow, DetachedStorage, and TransactionalCommit.";
					return false;
				}
				return ValidateSnapshotProperty(ArrayProperty->GetInner(), OutError);
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				const auto* MapProperty = static_cast<const FMapProperty*>(Property);
				if (!MapProperty->HasMapOps() || !MapProperty->GetKeyProp() || !MapProperty->GetValueProp()
					|| !MapProperty->HasCapability(EMapOpsFlags::Count | EMapOpsFlags::ConstTraversal
						| EMapOpsFlags::Insert | EMapOpsFlags::DetachedStorage | EMapOpsFlags::TransactionalCommit))
				{
					if (OutError) *OutError = "Cannot snapshot a map without Count, ConstTraversal, Insert, DetachedStorage, and TransactionalCommit.";
					return false;
				}
				return ValidateCanonicalMapKeyProperty(MapProperty->GetKeyProp(), OutError)
					&& ValidateSnapshotProperty(MapProperty->GetKeyProp(), OutError)
					&& ValidateSnapshotProperty(MapProperty->GetValueProp(), OutError);
			}
			default:
				if (OutError) *OutError = "The reflected property kind does not support value snapshots.";
				return false;
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

			auto SerializeRawBytes(std::span<std::byte>) -> void override
			{
			}

			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				Context.Discover(Object);
			}

			auto SerializeSoftObjectPath(FSoftObjectPath&) -> void override {}

		private:
			FObjectGraphContext& Context;
		};

		class FObjectGraphWriter : public FObjectMemoryWriter
		{
		public:
			FObjectGraphWriter(std::vector<uint8>& InBytes, FObjectGraphContext& InContext)
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
				if (!Object || HasError()) return;
				uint64 Id = Context.FindId(Object);
				if (Id == 0)
				{
					Fail(EArchiveFailureCode::InvalidObjectReference,
						"Object graph grew after discovery was frozen.");
					return;
				}
				*this << Id;
			}

		private:
			FObjectGraphContext& Context;
		};

		class FObjectGraphReader : public FObjectMemoryReader
		{
		public:
			FObjectGraphReader(const std::vector<uint8>& InBytes, FObjectGraphContext& InContext)
				: FObjectMemoryReader(InBytes, EArchivePurpose::ObjectGraph)
				, Context(InContext)
			{
				EnableCapabilities(EArchiveCapability::ObjectReferences);
			}

			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint8 Kind = 0;
				*this << Kind;
				if (HasError()) return;
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
				if (HasError()) return;
				if (Id == 0 || Id > Context.IdToObject.size())
				{
					Fail(EArchiveFailureCode::InvalidObjectReference,
						"Invalid object graph reference identifier.");
					return;
				}
				Object = Context.ResolveId(Id);
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
	auto FArchiveLogicalTypeDescriptor::Object(FName Type) -> FArchiveLogicalTypeDescriptor { return {.Kind = EKind::Object, .QualifiedType = Type}; }
	auto FArchiveLogicalTypeDescriptor::SoftObject(FName Type) -> FArchiveLogicalTypeDescriptor { return {.Kind = EKind::SoftObject, .QualifiedType = Type}; }
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
		const std::string Identity = std::format("{}::{}", Field.DeclaringType.ToString(), Field.Name.ToString());
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

	auto FObjectArchive::NotifyCanonicalMapKey(uint64 Index, std::span<const uint8> Token) -> void
	{
		if (!HasError()) OnCanonicalMapKey(Index, Token);
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
	auto FObjectArchive::OnCanonicalMapKey(uint64, std::span<const uint8>) -> void {}
	auto FObjectArchive::OnLeavePath() -> void {}
	auto FObjectArchive::OnReflectedPropertyValue(FProperty&, const void*, uint32) -> void {}
	auto FObjectArchive::NotifyReflectedPropertyValue(
		FProperty& Property, const void* Container, uint32 ArrayIndex) -> void
	{
		OnReflectedPropertyValue(Property, Container, ArrayIndex);
	}

	auto FObjectArchive::SerializeObjectReference(DObject*&) -> void
	{
		Fail(EArchiveFailureCode::UnsupportedCapability, "This Archive does not support ObjectReferences.");
	}
	auto FObjectArchive::SerializeSoftObjectPath(FSoftObjectPath& Value) -> void
	{
		if (!IsCurrentFieldAvailable()) return;
		if (!HasCapability(EArchiveCapability::SoftObjectReferences))
		{
			Fail(EArchiveFailureCode::UnsupportedCapability, "This Archive does not support SoftObjectReferences.");
			return;
		}
		uint8 Kind = IsSaving() && !Value.IsNull() ? 1 : 0;
		*this << Kind;
		if (HasError()) return;
		if (Kind == 0) { if (IsLoading()) Value.Reset(); return; }
		if (Kind != 1) { Fail(EArchiveFailureCode::InvalidData, "Unknown soft object reference tag."); return; }
		std::string Path = IsSaving() ? Value.ToString() : std::string();
		uint64 PathBytes = static_cast<uint64>(Path.size());
		*this << PathBytes;
		if (HasError()) return;
		if (PathBytes == 0 || PathBytes > MaximumSoftObjectPathBytes
			|| (IsLoading() && PathBytes > GetRemainingPayloadBytes()))
		{
			Fail(EArchiveFailureCode::InvalidPath,
				"Soft object path payload is empty, truncated, or exceeds 1 MiB.");
			return;
		}
		if (IsLoading()) Path.resize(static_cast<size_t>(PathBytes));
		SerializeRawBytes(std::as_writable_bytes(std::span<char>(Path.data(), Path.size())));
		if (HasError()) return;
		if (IsLoading())
		{
			FSoftObjectPath Loaded;
			std::string Error;
			if (!FSoftObjectPath::TryCreate(Path, Loaded, &Error))
				Fail(EArchiveFailureCode::InvalidPath, Error.empty() ? "Archive contains an invalid soft object path." : Error);
			else Value = std::move(Loaded);
		}
	}

	FObjectMemoryWriter::FObjectMemoryWriter(std::vector<uint8>& InBytes, EArchivePurpose Purpose)
		: FObjectArchive({EArchiveDirection::Save, Purpose,
			EArchiveCapability::StructuredFields | EArchiveCapability::RawBytes
			| EArchiveCapability::Position
			| EArchiveCapability::SoftObjectReferences
			| (Purpose == EArchivePurpose::PropertySnapshot ? EArchiveCapability::CanonicalMapOrder : EArchiveCapability::None)})
		, Bytes(InBytes)
	{
	}

	auto FObjectMemoryWriter::SerializeRawBytes(std::span<std::byte> Data) -> void
	{
		if (HasError()) return;
		const auto* Source = reinterpret_cast<const uint8*>(Data.data());
		Bytes.insert(Bytes.end(), Source, Source + Data.size());
	}

	FObjectMemoryReader::FObjectMemoryReader(std::span<const uint8> InBytes, EArchivePurpose Purpose)
		: FObjectArchive({EArchiveDirection::Load, Purpose,
			EArchiveCapability::StructuredFields | EArchiveCapability::RawBytes
			| EArchiveCapability::Position
			| EArchiveCapability::SoftObjectReferences
			| EArchiveCapability::RemainingPayload
			| (Purpose == EArchivePurpose::PropertySnapshot ? EArchiveCapability::CanonicalMapOrder : EArchiveCapability::None)})
		, Bytes(InBytes)
	{
	}

	auto FObjectMemoryReader::SerializeRawBytes(std::span<std::byte> Data) -> void
	{
		if (HasError()) return;
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
		FArchive& Ar, uint64 Index, std::span<const uint8> Token) -> void
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

	auto SerializeArchiveSoftObjectPath(FArchive& Ar, FSoftObjectPath& Value) -> void
	{
		if (auto* ObjectArchive = RequireObjectArchive(Ar)) ObjectArchive->SerializeSoftObjectPath(Value);
	}

	FPropertyValueSnapshot::~FPropertyValueSnapshot()
	{
		ReleaseReferenceRoots();
	}

	FPropertyValueSnapshot::FPropertyValueSnapshot(const FPropertyValueSnapshot& Other)
		: Property(Other.Property)
		, Bytes(Other.Bytes)
		, ReferencedObjects(Other.ReferencedObjects)
	{
		AddReferenceRoots();
	}

	auto FPropertyValueSnapshot::operator=(const FPropertyValueSnapshot& Other) -> FPropertyValueSnapshot&
	{
		if (this == &Other) return *this;
		FPropertyValueSnapshot Copy(Other);
		*this = std::move(Copy);
		return *this;
	}

	FPropertyValueSnapshot::FPropertyValueSnapshot(FPropertyValueSnapshot&& Other) noexcept
		: Property(Other.Property)
		, Bytes(std::move(Other.Bytes))
		, ReferencedObjects(std::move(Other.ReferencedObjects))
	{
		Other.Property = nullptr;
		Other.ReferencedObjects.clear();
	}

	auto FPropertyValueSnapshot::operator=(FPropertyValueSnapshot&& Other) noexcept -> FPropertyValueSnapshot&
	{
		if (this == &Other) return *this;
		ReleaseReferenceRoots();
		Property = Other.Property;
		Bytes = std::move(Other.Bytes);
		ReferencedObjects = std::move(Other.ReferencedObjects);
		Other.Property = nullptr;
		Other.ReferencedObjects.clear();
		return *this;
	}

	auto FPropertyValueSnapshot::operator==(const FPropertyValueSnapshot& Other) const -> bool
	{
		if (this == &Other) return true;
		if (Property != Other.Property) return false;
		if (!Property) return true;

		FReflectedValueStorage Left;
		FReflectedValueStorage Right;
		std::string Error;
		if (Left.DefaultConstruct(Property, 0, &Error)
			&& Right.DefaultConstruct(Property, 0, &Error)
			&& RestorePropertyValue(Property, Left.GetContainer(), 0, *this, &Error)
			&& RestorePropertyValue(Property, Right.GetContainer(), 0, Other, &Error))
		{
			return ArePropertyValuesIdentical(
				Property, Left.GetContainer(), 0, Right.GetContainer(), 0);
		}
		return Bytes == Other.Bytes && ReferencedObjects == Other.ReferencedObjects;
	}

	auto FPropertyValueSnapshot::AddReferenceRoots() -> void
	{
		for (DObject* Object : ReferencedObjects) AddToRoot(Object);
	}

	auto FPropertyValueSnapshot::ReleaseReferenceRoots() -> void
	{
		for (DObject* Object : ReferencedObjects)
		{
			// Explicit destruction can invalidate an otherwise rooted reference. Avoid
			// dereferencing a removed object while tearing down the snapshot afterward.
			if (GDObjectArray.Contains(Object)) RemoveFromRoot(Object);
		}
		ReferencedObjects.clear();
	}

	auto CapturePropertyValue(
		const FProperty* Property,
		const void* Container,
		uint32 ArrayIndex,
		FPropertyValueSnapshot& OutSnapshot,
		std::string* OutError
	) -> bool
	{
		if (OutError) OutError->clear();
		if (!Container)
		{
			if (OutError) *OutError = "Cannot snapshot a property from a null container.";
			return false;
		}
		if (!ValidateSnapshotProperty(Property, OutError)) return false;
		if (ArrayIndex >= Property->GetArrayDim())
		{
			if (OutError) *OutError = "Property snapshot array index is out of range.";
			return false;
		}

		class FSnapshotWriter final : public FObjectMemoryWriter
		{
		public:
			FSnapshotWriter(std::vector<uint8>& InBytes, std::vector<DObject*>& InReferences)
				: FObjectMemoryWriter(InBytes, EArchivePurpose::PropertySnapshot), References(InReferences)
			{
				EnableCapabilities(EArchiveCapability::ObjectReferences);
			}
			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint64 Id = 0;
				if (Object)
				{
					auto It = std::find(References.begin(), References.end(), Object);
					if (It == References.end())
					{
						References.push_back(Object);
						Id = static_cast<uint64>(References.size());
					}
					else
					{
						Id = static_cast<uint64>(std::distance(References.begin(), It)) + 1;
					}
				}
				*this << Id;
			}
		private:
			std::vector<DObject*>& References;
		};

		FPropertyValueSnapshot Snapshot;
		Snapshot.Property = Property;
		FSnapshotWriter Writer(Snapshot.Bytes, Snapshot.ReferencedObjects);
		SerializePropertyValue(Writer, const_cast<FProperty*>(Property), const_cast<void*>(Container), ArrayIndex, true);
		if (Writer.HasError())
		{
			if (OutError) *OutError = Writer.GetError();
			return false;
		}
		class FSnapshotReferenceCollector final : public FReferenceCollector
		{
		public:
			explicit FSnapshotReferenceCollector(std::vector<DObject*>& InReferences)
				: References(InReferences) {}
			auto AddReferencedObject(DObject*& Object) -> void override
			{
				if (Object && std::ranges::find(References, Object) == References.end()) References.push_back(Object);
			}
		private:
			std::vector<DObject*>& References;
		};
		FSnapshotReferenceCollector ReferenceCollector(Snapshot.ReferencedObjects);
		Private::FGCReferenceSchemaRegistry::VisitProperty(
			const_cast<FProperty*>(Property), const_cast<void*>(Container), ArrayIndex, ReferenceCollector);
		Snapshot.AddReferenceRoots();
		OutSnapshot = std::move(Snapshot);
		return true;
	}

	auto RestorePropertyValue(
		const FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const FPropertyValueSnapshot& Snapshot,
		std::string* OutError
	) -> bool
	{
		if (OutError) OutError->clear();
		if (!Container)
		{
			if (OutError) *OutError = "Cannot restore a property into a null container.";
			return false;
		}
		if (!ValidateSnapshotProperty(Property, OutError)) return false;
		if (Snapshot.Property != Property)
		{
			if (OutError) *OutError = "Property snapshot was captured from a different reflected property.";
			return false;
		}
		if (ArrayIndex >= Property->GetArrayDim())
		{
			if (OutError) *OutError = "Property restore array index is out of range.";
			return false;
		}

		class FSnapshotReader final : public FObjectMemoryReader
		{
		public:
			FSnapshotReader(const std::vector<uint8>& InBytes, const std::vector<DObject*>& InReferences)
				: FObjectMemoryReader(InBytes, EArchivePurpose::PropertySnapshot), References(InReferences)
			{
				EnableCapabilities(EArchiveCapability::ObjectReferences);
			}
			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint64 Id = 0;
				*this << Id;
				if (HasError()) return;
				if (Id > References.size())
				{
					SetError("Invalid property snapshot reference identifier.");
					return;
				}
				Object = Id == 0 ? nullptr : References[static_cast<size_t>(Id - 1)];
			}
		private:
			const std::vector<DObject*>& References;
		};

		FSnapshotReader Reader(Snapshot.Bytes, Snapshot.ReferencedObjects);
		SerializeReflectedPropertyValue(
			Reader, *const_cast<FProperty*>(Property), Container, ArrayIndex, true);
		if (!Reader.HasError() && Reader.GetRemainingPayloadBytes() != 0)
			Reader.Fail(EArchiveFailureCode::InvalidData, "Property snapshot has trailing bytes.");
		if (!Reader.HasError()) return true;
		if (OutError) *OutError = Reader.GetError();
		return false;
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
		SerializePropertyValue(
			Ar, &Property, Container, ArrayIndex, bIncludeRawObjectReferences);
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
				if (!Property || Property->HasAnyPropertyFlags(EPropertyFlags::Transient))
				{
					return;
				}

				auto FieldScope = EnterArchiveField(Ar, MakeFieldDescriptor(
					Property, Object.GetClass()->GetQualifiedName()));
				for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
				{
					auto FixedScope = Property->GetArrayDim() > 1
						? EnterArchiveFixedArrayElement(Ar, Index) : FArchivePathScope();
					SerializePropertyValue(Ar, Property, &Object, Index, false);
				}
			},
			true
		);
	}

	auto SaveObjectGraphToMemory(DObject* RootObject, std::vector<uint8>& OutBytes) -> bool
	{
		if (!RootObject || RootObject->IsTemplateObject())
		{
			return false;
		}

		FObjectGraphContext Context;
		Context.Discover(RootObject);
		FObjectGraphDiscoveryArchive DiscoveryArchive(Context);
		for (size_t Index = 0; Index < Context.Objects.size() && !DiscoveryArchive.HasError(); ++Index)
		{
			DObject* Object = Context.Objects[Index];
			auto ObjectScope = DiscoveryArchive.EnterObject(*Object);
			Object->Serialize(DiscoveryArchive);
		}
		if (DiscoveryArchive.HasError()) return false;
		Context.Freeze();

		std::vector<uint8> Bytes;
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
			std::vector<uint8> PropertyBytes;
			FObjectGraphWriter PropertyWriter(PropertyBytes, Context);
			{
				auto ObjectScope = PropertyWriter.EnterObject(*Object);
				Object->Serialize(PropertyWriter);
			}
			if (PropertyWriter.HasError()) return false;
			uint64 PropertySize = static_cast<uint64>(PropertyBytes.size());

			HeaderWriter << Id << OuterId;
			WriteString(HeaderWriter, ClassName);
			WriteString(HeaderWriter, ObjectName);
			HeaderWriter << PropertySize;
			if (PropertySize > 0)
			{
				HeaderWriter.SerializeRawBytes(std::as_writable_bytes(
					std::span<uint8>(PropertyBytes.data(), PropertyBytes.size())));
			}
		}

		if (HeaderWriter.HasError()) return false;
		OutBytes = std::move(Bytes);
		return true;
	}

	auto LoadObjectGraphFromMemory(const std::vector<uint8>& Bytes) -> DObject*
	{
		FObjectMemoryReader Reader(Bytes);
		uint32 Magic = 0;
		uint32 Version = 0;
		uint64 RootId = 0;
		uint64 ObjectCount = 0;
		Reader << Magic << Version << RootId << ObjectCount;
		if (Reader.HasError() || Magic != ObjectGraphMagic || Version != ObjectGraphVersion || ObjectCount == 0
			|| ObjectCount > 1000000 || RootId == 0 || RootId > ObjectCount)
		{
			return nullptr;
		}

		struct FLoadedObjectRecord
		{
			uint64 Id = 0;
			uint64 OuterId = 0;
			std::string ClassName;
			std::string ObjectName;
			std::vector<uint8> PropertyBytes;
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
			if (Reader.HasError() || PropertySize > Reader.GetRemainingPayloadBytes()
				|| PropertySize > std::vector<uint8>().max_size())
			{
				DiscardLoadedObjects();
				return nullptr;
			}
			Record.PropertyBytes.resize(static_cast<size_t>(PropertySize));
			if (PropertySize > 0)
			{
				Reader.SerializeRawBytes(std::as_writable_bytes(
					std::span<uint8>(Record.PropertyBytes.data(), Record.PropertyBytes.size())));
			}
			if (Reader.HasError() || Record.Id == 0 || Record.Id > ObjectCount || Context.ResolveId(Record.Id)
				|| Record.OuterId > ObjectCount)
			{
				DiscardLoadedObjects();
				return nullptr;
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
			DObjectForceRegistration(Object);
			Context.IdToObject[static_cast<size_t>(Record.Id - 1)] = Object;
		}
		if (Reader.HasError() || Reader.GetRemainingPayloadBytes() != 0)
		{
			DiscardLoadedObjects();
			return nullptr;
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
					return nullptr;
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
				return nullptr;
			}
			Object->SetOuterPrivate(Outer);
			FObjectGraphReader PropertyReader(Record.PropertyBytes, Context);
			{
				auto ObjectScope = PropertyReader.EnterObject(*Object);
				Object->Serialize(PropertyReader);
			}
			if (PropertyReader.HasError() || PropertyReader.GetRemainingPayloadBytes() != 0)
			{
				DiscardLoadedObjects();
				return nullptr;
			}
		}

		DObject* LoadedRoot = Context.ResolveId(RootId);
		if (!LoadedRoot) DiscardLoadedObjects();
		return LoadedRoot;
	}

	auto DuplicateObjectGraph(DObject* RootObject, DObject* NewOuter, FName NewName, std::string* OutError, std::unordered_map<DObject*, DObject*>* OutDuplicates) -> DObject*
	{
		if (OutError) OutError->clear();
		if (OutDuplicates) OutDuplicates->clear();
		if (!RootObject)
		{
			if (OutError) *OutError = "Cannot duplicate a null object graph.";
			return nullptr;
		}
		if (RootObject->IsTemplateObject())
		{
			if (OutError) *OutError = "Cannot duplicate a class-default template as an ordinary object graph.";
			return nullptr;
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
				if (OutError) *OutError = "Object graph contains an inner object whose outer was not duplicated.";
				DiscardDuplicates();
				return nullptr;
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
					if (OutError) *OutError = std::format("Object '{}' has no constructible class.", Source->GetName());
					DiscardDuplicates();
					return nullptr;
				}
				FStaticConstructObjectParameters Params;
				Params.Class = Class;
				Params.Outer = DuplicateOuter;
				Params.Name = Source == RootObject && !NewName.IsNone() ? NewName : Source->GetFName();
				Params.Size = Class->PropertiesSize;
				Params.Purpose = EObjectConstructionPurpose::Duplication;
				Duplicate = StaticConstructObject(Params);
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
			FDuplicateWriter(std::vector<uint8>& Bytes,
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
		private:
			const std::unordered_map<DObject*, uint64>& Ids;
			std::vector<DObject*>& Externals;
		};

		class FDuplicateReader final : public FObjectMemoryReader
		{
		public:
			FDuplicateReader(const std::vector<uint8>& Bytes,
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
				if (HasError()) return;
				if (Kind == static_cast<uint8>(EArchiveObjectReferenceKind::Null))
				{
					Object = nullptr;
					return;
				}
				uint64 Id = 0;
				*this << Id;
				if (HasError()) return;
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
		private:
			const std::vector<DObject*>& DuplicateObjects;
			const std::vector<DObject*>& Externals;
		};

		for (DObject* Source : Sources)
		{
			std::vector<uint8> Bytes;
			FDuplicateWriter Writer(Bytes, SourceIds, ExternalReferences);
			{
				auto ObjectScope = Writer.EnterObject(*Source);
				Source->Serialize(Writer);
			}
			if (Writer.HasError())
			{
				if (OutError) *OutError = Writer.GetError();
				DiscardDuplicates();
				return nullptr;
			}
			FDuplicateReader Reader(Bytes, DuplicateById, ExternalReferences);
			{
				auto ObjectScope = Reader.EnterObject(*Duplicates[Source]);
				Duplicates[Source]->Serialize(Reader);
			}
			if (Reader.HasError() || Reader.GetRemainingPayloadBytes() != 0)
			{
				if (OutError) *OutError = Reader.HasError()
					? std::string(Reader.GetError()) : "Duplicate stream contains trailing bytes.";
				DiscardDuplicates();
				return nullptr;
			}
		}

		for (DObject* Source : Sources)
		{
			FAuthoredOverrideDiagnostic LedgerDiagnostic;
			if (!Duplicates[Source]->CopyAuthoredOverridesFrom(*Source, &LedgerDiagnostic))
			{
				if (OutError) *OutError = std::format(
					"Duplicated authored override path failed validation (reason {}).",
					static_cast<uint32>(LedgerDiagnostic.Reason));
				DiscardDuplicates();
				return nullptr;
			}
		}

		for (auto It = Sources.rbegin(); It != Sources.rend(); ++It)
		{
			std::string PostLoadError;
			if (!Duplicates[*It]->PostLoad(PostLoadError))
			{
				if (OutError) *OutError = PostLoadError.empty()
					? "Duplicated object graph failed PostLoad." : std::move(PostLoadError);
				DiscardDuplicates();
				return nullptr;
			}
		}
		if (OutDuplicates) *OutDuplicates = Duplicates;
		return DuplicateRoot;
	}

	auto CopyEditableObjectProperties(DObject* Source, DObject* Destination, const std::unordered_map<DObject*, DObject*>& ReferenceMap, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (!Source || !Destination || Source->GetClass() != Destination->GetClass())
		{
			if (OutError) *OutError = "Editable properties require matching source and destination classes.";
			return false;
		}

		class FEditableCopyWriter final : public FObjectMemoryWriter
		{
		public:
			FEditableCopyWriter(std::vector<uint8>& Bytes, std::vector<DObject*>& InReferences)
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
			FRemappingReader(const std::vector<uint8>& Bytes,
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
				if (HasError()) return;
				if (Id > References.size())
				{
					Fail(EArchiveFailureCode::InvalidObjectReference,
						"Editable-copy stream contains an invalid reference identifier.");
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
		auto RollBack = [&]() {
			for (auto It = OriginalValues.rbegin(); It != OriginalValues.rend(); ++It)
			{
				std::string IgnoredError;
				RestorePropertyValue(It->Property, Destination, It->Index, It->Snapshot, &IgnoredError);
			}
		};

		bool bSucceeded = true;
		Source->GetClass()->ForEachProperty([&](FProperty* Property) {
			if (!bSucceeded || !Property || !Property->HasAnyPropertyFlags(EPropertyFlags::Edit) || Property->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
			for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
			{
				FOriginalValue Original{Property, Index, {}};
				if (!CapturePropertyValue(Property, Destination, Index, Original.Snapshot, OutError))
				{
					bSucceeded = false;
					return;
				}
				OriginalValues.push_back(std::move(Original));

				std::vector<uint8> Bytes;
				std::vector<DObject*> References;
				FEditableCopyWriter Writer(Bytes, References);
				SerializeReflectedPropertyValue(Writer, *Property, Source, Index);
				if (Writer.HasError())
				{
					if (OutError) *OutError = Writer.GetError();
					bSucceeded = false;
					return;
				}
				FRemappingReader Reader(Bytes, References, ReferenceMap);
				SerializeReflectedPropertyValue(Reader, *Property, Destination, Index);
				if (Reader.HasError() || Reader.GetRemainingPayloadBytes() != 0)
				{
					if (OutError && OutError->empty()) *OutError = Reader.HasError()
						? std::string(Reader.GetError()) : "Editable-copy stream contains trailing bytes.";
					bSucceeded = false;
					return;
				}
			}
		}, true);
		if (!bSucceeded)
		{
			RollBack();
			return false;
		}
		Destination->MarkPackageDirty();
		return true;
	}
}
