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
		constexpr uint32 ObjectGraphVersion = 1;

		auto WriteString(FArchive& Ar, std::string& Value) -> void
		{
			Ar.SerializeString(Value);
		}

		auto FindClassByName(const std::string& ClassName) -> DClass*
		{
			if (ClassName == "DObject")
			{
				return DObject::StaticClass();
			}

			for (DObject* Object : GDObjectArray.GetAll())
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

		auto SerializeArrayElement(void* RawContext, uint64, const void* Element) -> bool
		{
			auto& Context = *static_cast<FArchiveArrayVisitContext*>(RawContext);
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
			SerializePropertyValue(Context.Archive, Context.Property->GetKeyProp(), const_cast<void*>(Key), 0,
				Context.bIncludeRawObjectReferences);
			SerializePropertyValue(Context.Archive, Context.Property->GetValueProp(), const_cast<void*>(Value), 0,
				Context.bIncludeRawObjectReferences);
			return !Context.Archive.HasError();
		}

		auto SerializePropertyValue(FArchive& Ar, FProperty* Property, void* Container, uint32 ArrayIndex, bool bIncludeRawObjectReferences) -> void
		{
			if (Ar.HasError()) return;
			if (!Property || !Container)
			{
				Ar.SetError("Invalid reflected property serialization request.");
				return;
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
				Ar.SerializeBytes(Property->GetValuePtr(Container, ArrayIndex), Property->GetElementSize());
				break;
			case DurinCodeGen::EPropertyGenFlags::String:
			{
				auto* StringProperty = static_cast<FStringProperty*>(Property);
				std::string* Value = StringProperty->GetStringValuePtr(Container, ArrayIndex);
				Ar.SerializeString(*Value);
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Name:
			{
				auto* NameProperty = static_cast<FNameProperty*>(Property);
				FName* Value = NameProperty->GetNameValuePtr(Container, ArrayIndex);
				std::string SerializedValue = Ar.IsSaving() ? Value->ToString() : std::string();
				Ar.SerializeString(SerializedValue);
				if (Ar.IsLoading() && !Ar.HasError()) *Value = FName(SerializedValue);
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Guid:
			{
				auto* GuidProperty = static_cast<FGuidProperty*>(Property);
				FGuid* Value = GuidProperty->GetGuidValuePtr(Container, ArrayIndex);
				FGuid SerializedValue = Ar.IsSaving() ? *Value : FGuid();
				Ar << SerializedValue.A << SerializedValue.B << SerializedValue.C << SerializedValue.D;
				if (Ar.IsLoading() && !Ar.HasError()) *Value = SerializedValue;
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Object:
			{
				auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
				if (!bIncludeRawObjectReferences && !ObjectProperty->IsObjectPtrWrapper())
				{
					break;
				}
				DObject* ReferencedObject = ObjectProperty->GetObjectPropertyValue(Container, ArrayIndex);
				Ar.SerializeObjectReference(ReferencedObject);
				if (Ar.IsLoading() && !Ar.HasError())
				{
					ObjectProperty->SetObjectPropertyValue(Container, ReferencedObject, ArrayIndex);
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
					Struct->ForEachProperty([&](FProperty* Field) {
						if (Ar.HasError() || !Field
							|| Field->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
						for (uint32 Index = 0; Index < Field->GetArrayDim() && !Ar.HasError(); ++Index)
							SerializePropertyValue(
								Ar, Field, StructValue, Index, bIncludeRawObjectReferences);
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
				FReflectedValueStorage Storage;
				if (!Storage.DefaultConstruct(Property, 0, &OperationError))
				{
					Ar.SetError(OperationError);
					break;
				}
				SerializeStructValue(Storage.GetValue());
				if (Ar.HasError()) break;
				if (Struct->HasPostDeserialize())
				{
					std::string PostDeserializeError;
					FDStructPostDeserializeContext Context{
						.Source = EDStructDeserializeSource::RuntimeArchive,
						.SourceVersion = 0,
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
					FArchiveMapVisitContext Context{Ar, MapProperty, bIncludeRawObjectReferences, Ar.RequiresCanonicalMapOrder()};
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
						SerializePropertyValue(Ar, MapProperty->GetKeyProp(), const_cast<void*>(Context.Entries[Index].Key), 0, bIncludeRawObjectReferences);
						SerializePropertyValue(Ar, MapProperty->GetValueProp(), const_cast<void*>(Context.Entries[Index].Value), 0, bIncludeRawObjectReferences);
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
						SerializePropertyValue(Ar, MapProperty->GetKeyProp(), KeyStorage.GetContainer(), 0, bIncludeRawObjectReferences);
						SerializePropertyValue(Ar, MapProperty->GetValueProp(), ValueStorage.GetContainer(), 0, bIncludeRawObjectReferences);
						if (Ar.HasError()) return;
						OpResult = Ops.InsertCopy(Detached.Get(), KeyStorage.GetValue(), ValueStorage.GetValue());
						if (OpResult != EContainerOpResult::Success)
						{
							Ar.SetError(OpResult == EContainerOpResult::DuplicateKey
								? std::format("MapDuplicateKey: decoded entry {} duplicates an earlier key.", Index)
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

			auto GetOrAssignId(DObject* Object) -> uint64
			{
				if (!Object)
				{
					return 0;
				}

				auto It = ObjectToId.find(Object);
				if (It != ObjectToId.end())
				{
					return It->second;
				}

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
				return Id;
			}

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
		};

		class FObjectGraphReferenceCollector : public FReferenceCollector
		{
		public:
			explicit FObjectGraphReferenceCollector(FObjectGraphContext& InContext)
				: Context(InContext)
			{
			}

			auto AddReferencedObject(DObject*& Object) -> void override
			{
				Gather(Object);
			}

			auto Gather(DObject* Object) -> void
			{
				if (!Object)
				{
					return;
				}

				const size_t PreviousCount = Context.Objects.size();
				Context.GetOrAssignId(Object);
				if (Context.Objects.size() == PreviousCount)
				{
					return;
				}

				Gather(Object->GetOuter());

				for (DObject* InnerObject : GDObjectArray.GetObjectsWithOuter(Object))
				{
					Gather(InnerObject);
				}

				Object->AddReferencedObjects(*this);
			}

		private:
			FObjectGraphContext& Context;
		};

		class FObjectGraphWriter : public FMemoryWriter
		{
		public:
			FObjectGraphWriter(std::vector<uint8>& InBytes, FObjectGraphContext& InContext)
				: FMemoryWriter(InBytes)
				, Context(InContext)
			{
			}

			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint64 Id = Context.GetOrAssignId(Object);
				*this << Id;
			}

		private:
			FObjectGraphContext& Context;
		};

		class FObjectGraphReader : public FMemoryReader
		{
		public:
			FObjectGraphReader(const std::vector<uint8>& InBytes, FObjectGraphContext& InContext)
				: FMemoryReader(InBytes)
				, Context(InContext)
			{
			}

			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint64 Id = 0;
				*this << Id;
				if (HasError()) return;
				if (Id > Context.IdToObject.size())
				{
					SetError("Invalid object graph reference identifier.");
					return;
				}
				Object = Context.ResolveId(Id);
			}

		private:
			FObjectGraphContext& Context;
		};
	}

	auto FArchive::SerializeString(std::string& Value) -> void
	{
		if (HasError()) return;
		uint64 Size = static_cast<uint64>(Value.size());
		*this << Size;
		if (HasError()) return;
		if (IsLoading())
		{
			if (Size > GetRemainingBytes() || Size > std::string().max_size())
			{
				SetError("Truncated or oversized string payload.");
				return;
			}
			std::string LoadedValue;
			LoadedValue.resize(static_cast<size_t>(Size));
			if (Size > 0) SerializeBytes(LoadedValue.data(), Size);
			if (!HasError()) Value = std::move(LoadedValue);
			return;
		}
		if (Size > 0)
		{
			SerializeBytes(Value.data(), Size);
		}
	}

	auto FArchive::SetError(std::string_view Message) -> void
	{
		if (!Error.empty()) return;
		Error = Message.starts_with("ArchiveFailure")
			? std::string(Message)
			: std::format("ArchiveFailure: {}", Message);
	}

	FMemoryWriter::FMemoryWriter(std::vector<uint8>& InBytes)
		: FArchive(EMode::Save)
		, Bytes(InBytes)
	{
	}

	auto FMemoryWriter::SerializeBytes(void* Data, uint64 Size) -> void
	{
		if (HasError()) return;
		if (!Data && Size > 0)
		{
			SetError("Cannot serialize bytes from a null source.");
			return;
		}
		if (Size == 0) return;
		const uint8* Source = static_cast<const uint8*>(Data);
		Bytes.insert(Bytes.end(), Source, Source + Size);
	}

	auto FMemoryWriter::SerializeObjectReference(DObject*& Object) -> void
	{
		uint64 AddressValue = reinterpret_cast<uint64>(Object);
		*this << AddressValue;
	}

	FMemoryReader::FMemoryReader(const std::vector<uint8>& InBytes)
		: FArchive(EMode::Load)
		, Bytes(InBytes)
	{
	}

	auto FMemoryReader::SerializeBytes(void* Data, uint64 Size) -> void
	{
		if (HasError()) return;
		if ((!Data && Size > 0) || Size > GetRemainingBytes())
		{
			SetError("Truncated byte payload.");
			return;
		}
		if (Size > 0)
			std::memcpy(Data, Bytes.data() + Offset, static_cast<size_t>(Size));
		Offset += Size;
	}

	auto FMemoryReader::SerializeObjectReference(DObject*& Object) -> void
	{
		uint64 AddressValue = 0;
		*this << AddressValue;
		if (!HasError()) Object = reinterpret_cast<DObject*>(AddressValue);
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

		class FSnapshotWriter final : public FMemoryWriter
		{
		public:
			FSnapshotWriter(std::vector<uint8>& InBytes, std::vector<DObject*>& InReferences)
				: FMemoryWriter(InBytes), References(InReferences) {}
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
			auto RequiresCanonicalMapOrder() const -> bool override { return true; }
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

		class FSnapshotReader final : public FMemoryReader
		{
		public:
			FSnapshotReader(const std::vector<uint8>& InBytes, const std::vector<DObject*>& InReferences)
				: FMemoryReader(InBytes), References(InReferences) {}
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
			Reader, const_cast<FProperty*>(Property), Container, ArrayIndex, true);
		if (!Reader.HasError() && Reader.GetRemainingBytes() != 0)
			Reader.SetError("Property snapshot has trailing bytes.");
		if (!Reader.HasError()) return true;
		if (OutError) *OutError = Reader.GetError();
		return false;
	}

	auto SerializeReflectedPropertyValue(
		FArchive& Ar,
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		bool bIncludeRawObjectReferences) -> void
	{
		SerializePropertyValue(
			Ar, Property, Container, ArrayIndex, bIncludeRawObjectReferences);
	}

	auto SerializeDObjectProperties(FArchive& Ar, DObject* Object) -> void
	{
		if (!Object || !Object->GetClass())
		{
			return;
		}

		Object->GetClass()->ForEachProperty(
			[&](FProperty* Property)
			{
				if (!Property || Property->HasAnyPropertyFlags(EPropertyFlags::Transient))
				{
					return;
				}

				for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
				{
					SerializeReflectedPropertyValue(Ar, Property, Object, Index);
				}
			},
			true
		);
	}

	auto SaveObjectGraphToMemory(DObject* RootObject, std::vector<uint8>& OutBytes) -> bool
	{
		if (!RootObject)
		{
			return false;
		}

		FObjectGraphContext Context;
		FObjectGraphReferenceCollector Collector(Context);
		Collector.Gather(RootObject);

		OutBytes.clear();
		FMemoryWriter HeaderWriter(OutBytes);
		uint32 Magic = ObjectGraphMagic;
		uint32 Version = ObjectGraphVersion;
		uint64 RootId = Context.ObjectToId[RootObject];
		uint64 ObjectCount = static_cast<uint64>(Context.Objects.size());
		HeaderWriter << Magic << Version << RootId << ObjectCount;

		for (DObject* Object : Context.Objects)
		{
			uint64 Id = Context.ObjectToId[Object];
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
			Object->Serialize(PropertyWriter);
			if (PropertyWriter.HasError()) return false;
			uint64 PropertySize = static_cast<uint64>(PropertyBytes.size());

			HeaderWriter << Id << OuterId;
			WriteString(HeaderWriter, ClassName);
			WriteString(HeaderWriter, ObjectName);
			HeaderWriter << PropertySize;
			if (PropertySize > 0)
			{
				HeaderWriter.SerializeBytes(PropertyBytes.data(), PropertySize);
			}
		}

		return !HeaderWriter.HasError();
	}

	auto LoadObjectGraphFromMemory(const std::vector<uint8>& Bytes) -> DObject*
	{
		FMemoryReader Reader(Bytes);
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
			Reader.SerializeString(Record.ClassName);
			Reader.SerializeString(Record.ObjectName);
			Reader << PropertySize;
			if (Reader.HasError() || PropertySize > Reader.GetRemainingBytes()
				|| PropertySize > std::vector<uint8>().max_size())
			{
				DiscardLoadedObjects();
				return nullptr;
			}
			Record.PropertyBytes.resize(static_cast<size_t>(PropertySize));
			if (PropertySize > 0)
			{
				Reader.SerializeBytes(Record.PropertyBytes.data(), PropertySize);
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
			DObject* Object = StaticConstructObject(Params);
			DObjectForceRegistration(Object);
			Context.IdToObject[static_cast<size_t>(Record.Id - 1)] = Object;
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
			Object->Serialize(PropertyReader);
			if (PropertyReader.HasError())
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

		std::vector<DObject*> Sources;
		std::unordered_set<DObject*> Visited;
		std::function<void(DObject*)> GatherInnerTree = [&](DObject* Object) {
			if (!Object || !Visited.insert(Object).second) return;
			Sources.push_back(Object);
			for (DObject* Inner : GDObjectArray.GetObjectsWithOuter(Object)) GatherInnerTree(Inner);
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
				for (DObject* Existing : GDObjectArray.GetObjectsWithOuter(DuplicateOuter))
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
				Duplicate = StaticConstructObject(Params);
				DObjectForceRegistration(Duplicate);
			}
			Duplicates.emplace(Source, Duplicate);
			if (Source == RootObject) DuplicateRoot = Duplicate;
		}

		class FDuplicateReader final : public FMemoryReader
		{
		public:
			FDuplicateReader(const std::vector<uint8>& Bytes, const std::unordered_map<DObject*, DObject*>& InDuplicates)
				: FMemoryReader(Bytes), DuplicateMap(InDuplicates) {}
			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint64 Address = 0;
				*this << Address;
				if (HasError()) return;
				DObject* SourceReference = reinterpret_cast<DObject*>(Address);
				const auto It = DuplicateMap.find(SourceReference);
				Object = It == DuplicateMap.end() ? SourceReference : It->second;
			}
		private:
			const std::unordered_map<DObject*, DObject*>& DuplicateMap;
		};

		for (DObject* Source : Sources)
		{
			std::vector<uint8> Bytes;
			FMemoryWriter Writer(Bytes);
			Source->Serialize(Writer);
			if (Writer.HasError())
			{
				if (OutError) *OutError = Writer.GetError();
				DiscardDuplicates();
				return nullptr;
			}
			FDuplicateReader Reader(Bytes, Duplicates);
			Duplicates[Source]->Serialize(Reader);
			if (Reader.HasError())
			{
				if (OutError) *OutError = Reader.GetError();
				DiscardDuplicates();
				return nullptr;
			}
		}

		std::string PostLoadError;
		if (!DuplicateRoot->PostLoad(PostLoadError))
		{
			if (OutError) *OutError = PostLoadError.empty() ? "Duplicated object graph failed PostLoad." : std::move(PostLoadError);
			DiscardDuplicates();
			return nullptr;
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

		class FRemappingReader final : public FMemoryReader
		{
		public:
			FRemappingReader(const std::vector<uint8>& Bytes, const std::unordered_map<DObject*, DObject*>& InReferenceMap)
				: FMemoryReader(Bytes), Map(InReferenceMap) {}
			auto SerializeObjectReference(DObject*& Object) -> void override
			{
				uint64 Address = 0;
				*this << Address;
				if (HasError()) return;
				DObject* SourceReference = reinterpret_cast<DObject*>(Address);
				const auto It = Map.find(SourceReference);
				Object = It == Map.end() ? SourceReference : It->second;
			}
		private:
			const std::unordered_map<DObject*, DObject*>& Map;
		};

		bool bSucceeded = true;
		Source->GetClass()->ForEachProperty([&](FProperty* Property) {
			if (!bSucceeded || !Property || !Property->HasAnyPropertyFlags(EPropertyFlags::Edit) || Property->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
			for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
			{
				std::vector<uint8> Bytes;
				FMemoryWriter Writer(Bytes);
				SerializePropertyValue(Writer, Property, Source, Index);
				if (Writer.HasError())
				{
					if (OutError) *OutError = Writer.GetError();
					bSucceeded = false;
					return;
				}
				FRemappingReader Reader(Bytes, ReferenceMap);
				SerializeReflectedPropertyValue(Reader, Property, Destination, Index);
				if (Reader.HasError())
				{
					if (OutError && OutError->empty()) *OutError = Reader.GetError();
					bSucceeded = false;
					return;
				}
			}
		}, true);
		if (!bSucceeded) return false;
		Destination->MarkPackageDirty();
		return true;
	}
}
