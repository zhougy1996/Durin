#include "DObject/Archive.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"

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

		auto SerializePropertyValue(FArchive& Ar, FProperty* Property, void* Container, uint32 ArrayIndex) -> void
		{
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
			case DurinCodeGen::EPropertyGenFlags::Object:
			{
				auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
				if (!ObjectProperty->IsObjectPtrWrapper())
				{
					break;
				}
				DObject* ReferencedObject = ObjectProperty->GetObjectPropertyValue(Container, ArrayIndex);
				Ar.SerializeObjectReference(ReferencedObject);
				if (Ar.IsLoading())
				{
					ObjectProperty->SetObjectPropertyValue(Container, ReferencedObject, ArrayIndex);
				}
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				auto* StructProperty = static_cast<FStructProperty*>(Property);
				DStruct* Struct = StructProperty->GetStruct();
				if (!Struct) break;
				void* StructValue = Property->GetValuePtr(Container, ArrayIndex);
				Struct->ForEachProperty([&](FProperty* Field) {
					if (!Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
					for (uint32 Index = 0; Index < Field->GetArrayDim(); ++Index) SerializePropertyValue(Ar, Field, StructValue, Index);
				}, false);
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				auto* ArrayProperty = static_cast<FArrayProperty*>(Property);
				FProperty* Inner = ArrayProperty->GetInner();
				if (!Inner || !ArrayProperty->HasArrayHelper())
				{
					break;
				}

				uint64 Num = Ar.IsSaving() ? ArrayProperty->Num(Container, ArrayIndex) : 0;
				Ar << Num;
				if (Ar.IsLoading())
				{
					ArrayProperty->Resize(Container, Num, ArrayIndex);
				}
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					void* Element = ArrayProperty->GetMutableElementPtr(Container, Index, ArrayIndex);
					SerializePropertyValue(Ar, Inner, Element, 0);
				}
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				auto* MapProperty = static_cast<FMapProperty*>(Property);
				if (!MapProperty->HasMapHelper()) break;
				uint64 Num = Ar.IsSaving() ? MapProperty->Num(Container, ArrayIndex) : 0;
				Ar << Num;
				if (Ar.IsSaving())
				{
					for (uint64 Index = 0; Index < Num; ++Index)
					{
						SerializePropertyValue(Ar, MapProperty->GetKeyProp(), const_cast<void*>(MapProperty->GetKeyPtr(Container, Index, ArrayIndex)), 0);
						SerializePropertyValue(Ar, MapProperty->GetValueProp(), const_cast<void*>(MapProperty->GetMappedValuePtr(Container, Index, ArrayIndex)), 0);
					}
				}
				else
				{
					MapProperty->Clear(Container, ArrayIndex);
					for (uint64 Index = 0; Index < Num; ++Index)
					{
						void* Key = MapProperty->CreateKey();
						void* Value = MapProperty->CreateValue();
						SerializePropertyValue(Ar, MapProperty->GetKeyProp(), Key, 0);
						SerializePropertyValue(Ar, MapProperty->GetValueProp(), Value, 0);
						MapProperty->Insert(Container, Key, Value, ArrayIndex);
						MapProperty->DestroyKey(Key);
						MapProperty->DestroyValue(Value);
					}
				}
				break;
			}
			default:
				break;
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

				for (DObject* InnerObject : Object->GetInnerObjects())
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
				Object = Context.ResolveId(Id);
			}

		private:
			FObjectGraphContext& Context;
		};
	}

	auto FArchive::SerializeString(std::string& Value) -> void
	{
		uint64 Size = static_cast<uint64>(Value.size());
		*this << Size;
		if (IsLoading())
		{
			Value.resize(static_cast<size_t>(Size));
		}
		if (Size > 0)
		{
			SerializeBytes(Value.data(), Size);
		}
	}

	FMemoryWriter::FMemoryWriter(std::vector<uint8>& InBytes)
		: FArchive(EMode::Save)
		, Bytes(InBytes)
	{
	}

	auto FMemoryWriter::SerializeBytes(void* Data, uint64 Size) -> void
	{
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
		check(Offset + Size <= Bytes.size());
		std::memcpy(Data, Bytes.data() + Offset, static_cast<size_t>(Size));
		Offset += Size;
	}

	auto FMemoryReader::SerializeObjectReference(DObject*& Object) -> void
	{
		uint64 AddressValue = 0;
		*this << AddressValue;
		Object = reinterpret_cast<DObject*>(AddressValue);
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
					SerializePropertyValue(Ar, Property, Object, Index);
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

		return true;
	}

	auto LoadObjectGraphFromMemory(const std::vector<uint8>& Bytes) -> DObject*
	{
		FMemoryReader Reader(Bytes);
		uint32 Magic = 0;
		uint32 Version = 0;
		uint64 RootId = 0;
		uint64 ObjectCount = 0;
		Reader << Magic << Version << RootId << ObjectCount;
		if (Magic != ObjectGraphMagic || Version != ObjectGraphVersion)
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
		FObjectGraphContext Context;
		Context.IdToObject.resize(static_cast<size_t>(ObjectCount));

		for (FLoadedObjectRecord& Record : Records)
		{
			uint64 PropertySize = 0;
			Reader << Record.Id << Record.OuterId;
			Reader.SerializeString(Record.ClassName);
			Reader.SerializeString(Record.ObjectName);
			Reader << PropertySize;
			Record.PropertyBytes.resize(static_cast<size_t>(PropertySize));
			if (PropertySize > 0)
			{
				Reader.SerializeBytes(Record.PropertyBytes.data(), PropertySize);
			}

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

		for (const FLoadedObjectRecord& Record : Records)
		{
			DObject* Object = Context.ResolveId(Record.Id);
			if (!Object)
			{
				continue;
			}
			Object->SetOuterPrivate(Context.ResolveId(Record.OuterId));
			FObjectGraphReader PropertyReader(Record.PropertyBytes, Context);
			Object->Serialize(PropertyReader);
		}

		return Context.ResolveId(RootId);
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
			for (DObject* Inner : Object->GetInnerObjects()) GatherInnerTree(Inner);
		};
		GatherInnerTree(RootObject);

		std::unordered_map<DObject*, DObject*> Duplicates;
		std::unordered_set<DObject*> ClaimedConstructedInners;
		DObject* DuplicateRoot = nullptr;
		for (DObject* Source : Sources)
		{
			DObject* DuplicateOuter = Source == RootObject ? NewOuter : Duplicates[Source->GetOuter()];
			if (Source != RootObject && !DuplicateOuter)
			{
				if (OutError) *OutError = "Object graph contains an inner object whose outer was not duplicated.";
				if (DuplicateRoot) DestroyObject(DuplicateRoot);
				return nullptr;
			}

			DObject* Duplicate = nullptr;
			if (Source != RootObject)
			{
				// Actor constructors create their default components. Reuse those matching inners
				// instead of constructing a second component with the same identity.
				for (DObject* Existing : DuplicateOuter->GetInnerObjects())
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
					if (DuplicateRoot) DestroyObject(DuplicateRoot);
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
			FDuplicateReader Reader(Bytes, Duplicates);
			Duplicates[Source]->Serialize(Reader);
		}

		std::string PostLoadError;
		if (!DuplicateRoot->PostLoad(PostLoadError))
		{
			if (OutError) *OutError = PostLoadError.empty() ? "Duplicated object graph failed PostLoad." : std::move(PostLoadError);
			DestroyObject(DuplicateRoot);
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
				DObject* SourceReference = reinterpret_cast<DObject*>(Address);
				const auto It = Map.find(SourceReference);
				Object = It == Map.end() ? SourceReference : It->second;
			}
		private:
			const std::unordered_map<DObject*, DObject*>& Map;
		};

		Source->GetClass()->ForEachProperty([&](FProperty* Property) {
			if (!Property || !Property->HasAnyPropertyFlags(EPropertyFlags::Edit) || Property->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
			for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
			{
				std::vector<uint8> Bytes;
				FMemoryWriter Writer(Bytes);
				SerializePropertyValue(Writer, Property, Source, Index);
				FRemappingReader Reader(Bytes, ReferenceMap);
				SerializePropertyValue(Reader, Property, Destination, Index);
			}
		}, true);
		Destination->MarkPackageDirty();
		return true;
	}
}
