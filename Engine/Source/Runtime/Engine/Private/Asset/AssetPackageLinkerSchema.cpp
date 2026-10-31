#include "AssetPackageLinkerSchema.h"
#include "AssetObjectLoadTiming.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/PackageValueCodec.h"

namespace Durin::AssetPrivate::LinkerSchema
{
	FLoadSchemaBindings::~FLoadSchemaBindings()
	{
		FObjectLoadTimingSession::RecordBindingWork(Linker.Summary.PackagePath, Builds, Reuses);
	}

	auto FLoadSchemaBindings::FindProperty(const ObjectPackage::FSerializedSchema& Schema, std::string_view Name)
		-> const FPropertyLoadInfo*
	{
		// Reject a foreign schema rather than caching an unowned descriptor pointer.
		if (FindSchema(Linker, Schema.QualifiedName) != &Schema) return nullptr;
		const auto Field = std::ranges::find(Schema.Fields, Name, &ObjectPackage::FSerializedField::Name);
		if (Field == Schema.Fields.end()) return nullptr;
		DStructBase* Owner = FindClassByQualifiedName(FName(Schema.QualifiedName));
		if (!Owner) Owner = FindStructByQualifiedName(FName(Schema.QualifiedName));
		const auto Existing = Entries.find(&*Field);
		if (Existing != Entries.end())
		{
			auto& Entry = Existing->second;
			if (Owner != Entry.OwnerPointer) return nullptr;
			// Hand-built metadata can be registered by name without managed identity.
			// Re-resolve that unproven lifetime rather than treating a null key as live.
			if (Owner && Entry.Owner.IsNull())
			{
				auto Info = FindPropertyByName(Schema, Name);
				if (!Info) return nullptr;
				++Builds;
				Entry.Info = std::move(*Info);
			}
			else
			{
				if (Owner && ResolveObjectKey(Entry.Owner) != Owner) return nullptr;
				++Reuses;
			}
			return &Entry.Info;
		}
		auto Info = FindPropertyByName(Schema, Name);
		if (!Info) return nullptr;
		++Builds;
		return &Entries.emplace(&*Field, FEntry{std::move(*Info), FObjectKey(Owner), Owner}).first->second.Info;
	}

	namespace
	{
		auto TypeKind(const ObjectPackage::FSerializedType& Input)
			-> DurinCodeGen::EPropertyGenFlags
		{
			using K = DurinCodeGen::EPropertyGenFlags;
			const ObjectPackage::FSerializedType* Type = &Input;
			if (Type->Kind == ObjectPackage::EValueKind::FixedArray && Type->Children.size() == 1)
				Type = &Type->Children[0];
			switch (Type->Kind)
			{
			case ObjectPackage::EValueKind::Bool: return K::Bool;
			case ObjectPackage::EValueKind::I8: return K::Int8;
			case ObjectPackage::EValueKind::I16: return K::Int16;
			case ObjectPackage::EValueKind::I32: return K::Int32;
			case ObjectPackage::EValueKind::I64: return K::Int64;
			case ObjectPackage::EValueKind::U8: return K::UInt8;
			case ObjectPackage::EValueKind::U16: return K::UInt16;
			case ObjectPackage::EValueKind::U32: return K::UInt32;
			case ObjectPackage::EValueKind::U64: return K::UInt64;
			case ObjectPackage::EValueKind::F32: return K::Float;
			case ObjectPackage::EValueKind::F64: return K::Double;
			case ObjectPackage::EValueKind::String: return K::String;
			case ObjectPackage::EValueKind::Name: return K::Name;
			case ObjectPackage::EValueKind::Guid: return K::Guid;
			case ObjectPackage::EValueKind::Enum: return K::Enum;
			case ObjectPackage::EValueKind::Intrinsic:
			case ObjectPackage::EValueKind::Struct: return K::Struct;
			case ObjectPackage::EValueKind::Array: return K::Array;
			case ObjectPackage::EValueKind::Map: return K::Map;
			case ObjectPackage::EValueKind::HardReference: return K::Object;
			case ObjectPackage::EValueKind::SoftReference: return K::SoftObject;
			case ObjectPackage::EValueKind::Byte:
			case ObjectPackage::EValueKind::Bytes: return K::Blob;
			case ObjectPackage::EValueKind::BulkData: return K::BulkData;
			case ObjectPackage::EValueKind::FixedArray: break;
			}
			return K::None;
		}

	}

	auto TypeWidth(ObjectPackage::EValueKind Kind) -> uint64
	{
		switch (Kind)
		{
		case ObjectPackage::EValueKind::Bool:
		case ObjectPackage::EValueKind::I8:
		case ObjectPackage::EValueKind::U8: return 1;
		case ObjectPackage::EValueKind::I16:
		case ObjectPackage::EValueKind::U16: return 2;
		case ObjectPackage::EValueKind::I32:
		case ObjectPackage::EValueKind::U32:
		case ObjectPackage::EValueKind::F32: return 4;
		case ObjectPackage::EValueKind::I64:
		case ObjectPackage::EValueKind::U64:
		case ObjectPackage::EValueKind::F64: return 8;
		default: return 0;
		}
	}

	auto IntrinsicName(uint64 Layout) -> std::string_view
	{
		switch (Layout)
		{
		case 1: return "Durin::FVector2"; case 2: return "Durin::FVector3";
		case 3: return "Durin::FVector4"; case 4: return "Durin::FQuat";
		case 5: return "Durin::FTransform"; case 6: return "Durin::FLinearColor";
		default: return {};
		}
	}

	namespace
	{
		auto TypeSignature(const ObjectPackage::FSerializedType& Input) -> std::string
		{
			const ObjectPackage::FSerializedType* Type = &Input;
			if (Type->Kind == ObjectPackage::EValueKind::FixedArray && Type->Children.size() == 1)
				Type = &Type->Children[0];
			using K = DurinCodeGen::EPropertyGenFlags;
			switch (Type->Kind)
			{
			case ObjectPackage::EValueKind::Array:
				return std::format("Array<{}>", Type->Children.size() == 1
					? TypeSignature(Type->Children[0]) : "Invalid");
			case ObjectPackage::EValueKind::Map:
				return std::format("Map<{},{}>", Type->Children.size() == 2
					? TypeSignature(Type->Children[0]) : "Invalid", Type->Children.size() == 2
					? TypeSignature(Type->Children[1]) : "Invalid");
			case ObjectPackage::EValueKind::HardReference:
				return std::format("Object:{}:true", Type->QualifiedName.empty() ? "DObject" : Type->QualifiedName);
			case ObjectPackage::EValueKind::SoftReference:
				return std::format("SoftObject:{}:v1", Type->QualifiedName.empty() ? "DObject" : Type->QualifiedName);
			case ObjectPackage::EValueKind::Enum:
				return std::format("Enum:{}:{}", Type->QualifiedName,
					TypeWidth(static_cast<ObjectPackage::EValueKind>(Type->Parameter)));
			case ObjectPackage::EValueKind::Struct: return std::format("Struct<{}>", Type->QualifiedName);
			case ObjectPackage::EValueKind::Intrinsic: return std::format("Struct<{}>", IntrinsicName(Type->Parameter));
			case ObjectPackage::EValueKind::String: return std::format("{}:v1", uint32(K::String));
			case ObjectPackage::EValueKind::Name: return std::format("{}:v1", uint32(K::Name));
			case ObjectPackage::EValueKind::Guid: return std::format("{}:v1", uint32(K::Guid));
			case ObjectPackage::EValueKind::Byte:
			case ObjectPackage::EValueKind::Bytes: return std::format("{}:v1", uint32(K::Blob));
			case ObjectPackage::EValueKind::BulkData: return std::format("{}:v1", uint32(K::BulkData));
			default: return std::format("{}:{}", uint32(TypeKind(*Type)), TypeWidth(Type->Kind));
			}
		}

	}

	auto ResolvePropertyForLoad(DStructBase* Owner, std::string_view StoredName,
		DurinCodeGen::EPropertyGenFlags Kind, std::string_view Signature) -> FPropertyLoadBinding
	{
		if (!Owner) return {};
		FProperty* Current = Owner->FindPropertyByName(FName(StoredName), false);
		if (Current && !Current->IsDeprecated() && Current->GetKind() == Kind
			&& PackagePrivate::GetSerializedTypeSignature(Current) == Signature)
			return {EPropertyLoadRoute::Current, Current, true};
		FProperty* Match = nullptr;
		bool bAmbiguous = false;
		bool bHistoricalIdentity = false;
		Owner->ForEachProperty([&](FProperty* Property) {
			const auto* Route = Property ? Property->GetDeprecation() : nullptr;
			if (!Route || Route->HistoricalName.ToString() != StoredName) return;
			bHistoricalIdentity = true;
			if (Property->GetKind() != Kind
				|| PackagePrivate::GetSerializedTypeSignature(Property) != Signature) return;
			if (Match) bAmbiguous = true;
			else Match = Property;
		}, false);
		if (Match && !bAmbiguous) return {EPropertyLoadRoute::Deprecated, Match, Current != nullptr};
		if (!Current && !bHistoricalIdentity) return {EPropertyLoadRoute::Removed};
		return {EPropertyLoadRoute::Incompatible, nullptr, Current != nullptr};
	}

	auto FindSchema(const ObjectPackage::FLinkerTables& Linker, std::string_view Name)
		-> const ObjectPackage::FSerializedSchema*
	{
		const auto It = std::ranges::find(Linker.Schemas, Name, &ObjectPackage::FSerializedSchema::QualifiedName);
		return It == Linker.Schemas.end() ? nullptr : &*It;
	}

	auto FindPropertyByName(const ObjectPackage::FSerializedSchema& Schema, std::string_view Name)
		-> std::optional<FPropertyLoadInfo>
	{
		const auto It = std::ranges::find(Schema.Fields, Name, &ObjectPackage::FSerializedField::Name);
		if (It == Schema.Fields.end()) return std::nullopt;
		DStructBase* Owner = FindClassByQualifiedName(FName(Schema.QualifiedName));
		if (!Owner) Owner = FindStructByQualifiedName(FName(Schema.QualifiedName));
		const auto Kind = TypeKind(It->Type);
		auto Signature = TypeSignature(It->Type);
		auto Match = ResolvePropertyForLoad(Owner, It->Name, Kind, Signature);
		return FPropertyLoadInfo{&*It, Match, Kind, std::move(Signature)};
	}

	namespace
	{
		// Registry lookups are local to this preprocessing pass. Evidence and the
		// normalized tables use the same resolved identity, rather than two walks.
		class FIdentityNormalizer
		{
		public:
			auto Class(std::string& Name) -> DClass*
			{
				const auto [It, Added] = Classes.try_emplace(Name);
				if (Added) It->second = FindClassBySerializedName(FName(Name));
				if (It->second) Name = It->second->GetQualifiedName().ToString();
				return It->second;
			}
			auto Struct(std::string& Name) -> DStruct*
			{
				const auto [It, Added] = Structs.try_emplace(Name);
				if (Added) It->second = FindStructBySerializedName(FName(Name));
				if (It->second) Name = It->second->GetQualifiedName().ToString();
				return It->second;
			}
			auto Enum(std::string& Name) -> void
			{
				const auto [It, Added] = Enums.try_emplace(Name);
				if (Added) It->second = FindEnumBySerializedName(FName(Name));
				if (It->second) Name = It->second->GetQualifiedName().ToString();
			}
			auto Owner(std::string& Name) -> DStructBase*
			{
				if (auto* Resolved = Class(Name)) return Resolved;
				return Struct(Name);
			}
			auto Field(DStructBase* Owner, std::string& Name) -> void
			{
				if (!Owner) return;
				auto& Names = FieldNames[Owner];
				const auto [It, Added] = Names.try_emplace(Name);
				if (Added) It->second = Owner->FindPropertyBySerializedName(FName(Name), false);
				if (It->second) Name = It->second->NamePrivate.ToString();
			}
			auto Type(ObjectPackage::FSerializedType& Type) -> void
			{
				for (auto& Child : Type.Children) this->Type(Child);
				if (Type.QualifiedName.empty()) return;
				using K = ObjectPackage::EValueKind;
				switch (Type.Kind)
				{
				case K::Enum: Enum(Type.QualifiedName); break;
				case K::Struct: Struct(Type.QualifiedName); break;
				case K::HardReference: case K::SoftReference: Class(Type.QualifiedName); break;
				default: break;
				}
			}
			auto Value(const ObjectPackage::FSerializedType& Type, ObjectPackage::FSerializedValue& Value)
				-> FAssetReadResult
			{
				using K = ObjectPackage::EValueKind;
				if (Type.Kind == K::Struct)
				{
					std::string Name = Type.QualifiedName;
					DStructBase* Owner = Struct(Name);
					if (Value.FieldTypes) for (auto& FieldType : *Value.FieldTypes) this->Type(FieldType);
					const auto& Types = ObjectPackage::StructFieldTypes(Type, Value);
					if (Value.FieldNames.size() != Value.Elements.size() || Types.size() != Value.Elements.size())
						return {EAssetReadError::CorruptFile, "Struct identity projection is invalid."};
					std::unordered_set<std::string_view> Names;
					for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
					{
						Field(Owner, Value.FieldNames[Index]);
						if (!Names.emplace(Value.FieldNames[Index]).second)
							return {EAssetReadError::CorruptFile, "Struct fields canonicalize to a duplicate name."};
						if (auto Result = this->Value(Types[Index], Value.Elements[Index]); !Result) return Result;
					}
				}
				else if ((Type.Kind == K::Array || Type.Kind == K::FixedArray) && Type.Children.size() == 1)
				{
					for (auto& Element : Value.Elements)
						if (auto Result = this->Value(Type.Children[0], Element); !Result) return Result;
				}
				else if (Type.Kind == K::Map && Type.Children.size() == 2)
				{
					for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
						if (auto Result = this->Value(Type.Children[Index % 2], Value.Elements[Index]); !Result) return Result;
				}
				return {};
			}
		private:
			std::unordered_map<std::string, DClass*> Classes;
			std::unordered_map<std::string, DStruct*> Structs;
			std::unordered_map<std::string, DEnum*> Enums;
			std::map<DStructBase*, std::unordered_map<std::string, FProperty*>> FieldNames;
		};
	}

	auto FixupSerializedNames(ObjectPackage::FLinkerTables& Linker,
		std::vector<FAssetCanonicalizationEvidence>& OutEvidence) -> FAssetReadResult
	{
		const auto& PackagePath = Linker.Summary.PackagePath;
		FIdentityNormalizer Normalize;
		std::vector<FAssetCanonicalizationEvidence> Evidence;
		auto Record = [&](std::string Stored, const std::string& Current, EAssetReflectedIdentityKind Kind,
			EAssetSerializedIdentityLocation Location, std::string Path) {
			if (Stored != Current) Evidence.push_back({PackagePath, std::move(Stored), Current, Kind, Location, std::move(Path)});
		};
		for (size_t Index = 0; Index < Linker.Exports.size(); ++Index)
		{
			auto& Name = Linker.Exports[Index].ClassName;
			std::string Stored = Name;
			Normalize.Class(Name);
			Record(std::move(Stored), Name, EAssetReflectedIdentityKind::Class,
				EAssetSerializedIdentityLocation::ObjectRecord, std::format("objects[{}].class", Index));
		}
		for (auto& Asset : Linker.Summary.TopLevelAssets) Normalize.Class(Asset.ClassName);
		for (auto& Import : Linker.Imports) Normalize.Class(Import.ClassName);
		std::unordered_set<std::string_view> SchemaNames;
		for (size_t Index = 0; Index < Linker.Schemas.size(); ++Index)
		{
			auto& Schema = Linker.Schemas[Index];
			std::string Stored = Schema.QualifiedName;
			DClass* Class = Normalize.Class(Schema.QualifiedName);
			DStructBase* Owner = Class ? static_cast<DStructBase*>(Class) : Normalize.Struct(Schema.QualifiedName);
			if (!SchemaNames.emplace(Schema.QualifiedName).second)
				return {EAssetReadError::CorruptFile,
					std::format("Serialized schemas canonicalize to duplicate identity {}.", Schema.QualifiedName)};
			Record(std::move(Stored), Schema.QualifiedName,
				Class ? EAssetReflectedIdentityKind::Class : EAssetReflectedIdentityKind::Struct,
				EAssetSerializedIdentityLocation::Schema, std::format("schemas[{}].identity", Index));
			std::unordered_set<std::string_view> Names;
			for (size_t FieldIndex = 0; FieldIndex < Schema.Fields.size(); ++FieldIndex)
			{
				auto& Field = Schema.Fields[FieldIndex];
				Stored = Field.Name;
				Normalize.Field(Owner, Field.Name);
				Record(std::move(Stored), Field.Name, EAssetReflectedIdentityKind::Property,
					EAssetSerializedIdentityLocation::Schema, std::format("schemas[{}].fields[{}].name", Index, FieldIndex));
				Normalize.Type(Field.Type);
				if (!Names.emplace(Field.Name).second)
					return {EAssetReadError::CorruptFile, std::format(
						"Serialized schema {} contains fields that canonicalize to duplicate name {}.", Schema.QualifiedName, Field.Name)};
			}
		}
		for (size_t Index = 0; Index < Linker.Types.size(); ++Index)
		{
			auto& Type = Linker.Types[Index];
			std::string Stored = Type.QualifiedName;
			Normalize.Type(Type);
			using K = ObjectPackage::EValueKind;
			const auto Kind = Type.Kind == K::Enum ? EAssetReflectedIdentityKind::Enum
				: Type.Kind == K::Struct ? EAssetReflectedIdentityKind::Struct : EAssetReflectedIdentityKind::Class;
			Record(std::move(Stored), Type.QualifiedName, Kind,
				EAssetSerializedIdentityLocation::TypeDescriptor, std::format("types[{}].identity", Index));
		}
		for (auto& Export : Linker.Exports)
			for (auto& Property : Export.Properties)
			{
				DStructBase* Owner = Normalize.Owner(Property.DeclaringType);
				Normalize.Field(Owner, Property.FieldName);
				Normalize.Type(Property.Type);
				if (auto Result = Normalize.Value(Property.Type, Property.Value); !Result) return Result;
			}
		std::ranges::sort(Evidence, [](const auto& Left, const auto& Right) {
			return std::tie(Left.Location, Left.LogicalPath, Left.Kind, Left.StoredIdentity, Left.CurrentIdentity)
				< std::tie(Right.Location, Right.LogicalPath, Right.Kind, Right.StoredIdentity, Right.CurrentIdentity);
		});
		OutEvidence = std::move(Evidence);
		return {};
	}

}
