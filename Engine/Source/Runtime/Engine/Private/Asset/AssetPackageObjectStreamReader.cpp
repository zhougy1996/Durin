#include "Asset/PackageObjectStreamReader.h"

#include "AssetPackageArchive.h"
#include "AssetPackageValueCodec.h"
#include "Asset/Load.h"
#include "Asset/Testing.h"

#include "DObject/Class.h"
#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/Package.h"

namespace Durin::Asset::PackageObjectStream
{
	namespace
	{
		thread_local uint64 GReencodeCountForTesting = 0;
		auto Fail(FReaderDiagnostic& Diagnostic, EReaderFailure Failure,
			std::string_view Message, uint64 Offset = 0, std::string Path = {}) -> bool
		{
			if (Diagnostic.Failure == EReaderFailure::None)
				Diagnostic = {Failure, std::move(Path), std::string(Message), Offset};
			return false;
		}

		auto TypeAt(const FDecodedPackage& Package, uint64 Id) -> const FDecodedType*
		{
			return Id == 0 || Id > Package.Types.size() ? nullptr : &Package.Types[static_cast<size_t>(Id - 1)];
		}

		auto SchemaAt(const FDecodedPackage& Package, uint64 Id) -> const FDecodedSchema*
		{
			return Id == 0 || Id > Package.Schemas.size() ? nullptr : &Package.Schemas[static_cast<size_t>(Id - 1)];
		}

		auto FindSchema(const FDecodedPackage& Package, std::string_view Name,
			uint64& OutId) -> const FDecodedSchema*
		{
			for (size_t Index = 0; Index < Package.Schemas.size(); ++Index)
				if (Package.Schemas[Index].QualifiedName == Name)
				{
					OutId = Index + 1; return &Package.Schemas[Index];
				}
			OutId = 0; return nullptr;
		}

		auto TypeKind(const FDecodedType& Input, const FDecodedPackage& Package)
			-> DurinCodeGen::EPropertyGenFlags
		{
			using K = DurinCodeGen::EPropertyGenFlags;
			const FDecodedType* Type = &Input;
			if (Type->Opcode == ETypeOpcode::FixedArray && Type->ChildTypeIds.size() == 1)
				Type = TypeAt(Package, Type->ChildTypeIds[0]);
			if (!Type) return K::None;
			switch (Type->Opcode)
			{
			case ETypeOpcode::Bool: return K::Bool;
			case ETypeOpcode::I8: return K::Int8; case ETypeOpcode::I16: return K::Int16;
			case ETypeOpcode::I32: return K::Int32; case ETypeOpcode::I64: return K::Int64;
			case ETypeOpcode::U8: return K::UInt8; case ETypeOpcode::U16: return K::UInt16;
			case ETypeOpcode::U32: return K::UInt32; case ETypeOpcode::U64: return K::UInt64;
			case ETypeOpcode::F32: return K::Float; case ETypeOpcode::F64: return K::Double;
			case ETypeOpcode::String: return K::String; case ETypeOpcode::Name: return K::Name;
			case ETypeOpcode::Guid: return K::Guid; case ETypeOpcode::Enum: return K::Enum;
			case ETypeOpcode::Intrinsic: case ETypeOpcode::Struct: return K::Struct;
			case ETypeOpcode::Array: return K::Array; case ETypeOpcode::Map: return K::Map;
			case ETypeOpcode::HardRef: return K::Object; case ETypeOpcode::SoftRef: return K::SoftObject;
			case ETypeOpcode::Bytes: return K::Blob;
			case ETypeOpcode::BulkData: return K::BulkData;
			case ETypeOpcode::FixedArray: break;
			}
			return K::None;
		}

		auto TypeWidth(ETypeOpcode Opcode) -> uint64
		{
			switch (Opcode)
			{
			case ETypeOpcode::Bool: case ETypeOpcode::I8: case ETypeOpcode::U8: return 1;
			case ETypeOpcode::I16: case ETypeOpcode::U16: return 2;
			case ETypeOpcode::I32: case ETypeOpcode::U32: case ETypeOpcode::F32: return 4;
			case ETypeOpcode::I64: case ETypeOpcode::U64: case ETypeOpcode::F64: return 8;
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

		auto TypeSignature(const FDecodedType& Input, const FDecodedPackage& Package) -> std::string
		{
			const FDecodedType* Type = &Input;
			if (Type->Opcode == ETypeOpcode::FixedArray && Type->ChildTypeIds.size() == 1)
				Type = TypeAt(Package, Type->ChildTypeIds[0]);
			if (!Type) return "Invalid";
			using K = DurinCodeGen::EPropertyGenFlags;
			switch (Type->Opcode)
			{
			case ETypeOpcode::Array:
				return std::format("Array<{}>", Type->ChildTypeIds.size() == 1 && TypeAt(Package, Type->ChildTypeIds[0])
					? TypeSignature(*TypeAt(Package, Type->ChildTypeIds[0]), Package) : "Invalid");
			case ETypeOpcode::Map:
				return std::format("Map<{},{}>", Type->ChildTypeIds.size() == 2 && TypeAt(Package, Type->ChildTypeIds[0])
					? TypeSignature(*TypeAt(Package, Type->ChildTypeIds[0]), Package) : "Invalid",
					Type->ChildTypeIds.size() == 2 && TypeAt(Package, Type->ChildTypeIds[1])
					? TypeSignature(*TypeAt(Package, Type->ChildTypeIds[1]), Package) : "Invalid");
			case ETypeOpcode::HardRef: return std::format("Object:{}:true", Type->QualifiedName.empty() ? "DObject" : Type->QualifiedName);
			case ETypeOpcode::SoftRef: return std::format("SoftObject:{}:v1", Type->QualifiedName.empty() ? "DObject" : Type->QualifiedName);
			case ETypeOpcode::Enum: return std::format("Enum:{}:{}", Type->QualifiedName, TypeWidth(static_cast<ETypeOpcode>(Type->Parameter)));
			case ETypeOpcode::Struct: return std::format("Struct<{}>", Type->QualifiedName);
			case ETypeOpcode::Intrinsic: return std::format("Struct<{}>", IntrinsicName(Type->Parameter));
			case ETypeOpcode::String: return std::format("{}:v1", uint32(K::String));
			case ETypeOpcode::Name: return std::format("{}:v1", uint32(K::Name));
			case ETypeOpcode::Guid: return std::format("{}:v1", uint32(K::Guid));
			case ETypeOpcode::Bytes: return std::format("{}:v1", uint32(K::Blob));
			case ETypeOpcode::BulkData: return std::format("{}:v1", uint32(K::BulkData));
			default: return std::format("{}:{}", uint32(TypeKind(*Type, Package)), TypeWidth(Type->Opcode));
			}
		}

		template<typename T>
		auto WriteInteger(Private::FByteWriter& Writer, uint64 Value) -> void
		{
			Writer.Write(static_cast<T>(Value));
		}

		auto WriteProjectedField(Private::FByteWriter& Writer, std::string_view Owner,
			std::string_view Name, DurinCodeGen::EPropertyGenFlags Kind,
			std::string Signature, std::vector<std::byte> Payload) -> void
		{
			Writer.WriteString(Owner); Writer.WriteString(Name); Writer.Write(uint8(Kind));
			Writer.WriteString(Signature); Writer.Write(uint64(Payload.size())); Writer.WriteBytes(Payload);
		}

		auto EncodeIntrinsicLoadValue(uint64 Layout, std::span<const uint64> Components,
			Private::FByteWriter& Writer, FReaderDiagnostic& Diagnostic) -> bool
		{
			const std::string Owner(IntrinsicName(Layout));
			if (Owner.empty()) return Fail(Diagnostic, EReaderFailure::InvalidValue, "Intrinsic layout is invalid.");
			Writer.WriteString(Owner);
			if (Layout == 5)
			{
				if (Components.size() != 10) return Fail(Diagnostic, EReaderFailure::InvalidValue, "Transform component count is invalid.");
				Writer.Write(uint64(3));
				for (const auto [Name, ChildLayout, Offset, Count] : {
					std::tuple<std::string_view, uint64, size_t, size_t>{"Rotation", 4, 0, 4},
					{"Translation", 2, 4, 3}, {"Scale3D", 2, 7, 3}})
				{
					Private::FByteWriter Payload;
					if (!EncodeIntrinsicLoadValue(ChildLayout, Components.subspan(Offset, Count), Payload, Diagnostic)) return false;
					WriteProjectedField(Writer, Owner, Name, DurinCodeGen::EPropertyGenFlags::Struct,
						std::format("Struct<{}>", IntrinsicName(ChildLayout)), std::move(Payload.Bytes));
				}
				return true;
			}
			const std::array<std::string_view, 4> Lower = {"x", "y", "z", "w"};
			const std::array<std::string_view, 4> Color = {"R", "G", "B", "A"};
			const uint64 Count = Layout == 1 ? 2 : Layout == 2 ? 3 : 4;
			if (Components.size() != Count) return Fail(Diagnostic, EReaderFailure::InvalidValue, "Intrinsic component count is invalid.");
			Writer.Write(Count);
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				Private::FByteWriter Payload;
				if (Layout == 6) Payload.Write(uint32(Components[Index])); else Payload.Write(Components[Index]);
				const std::string_view Name = Layout == 6 ? Color[Index]
					: Layout == 4 ? Lower[(Index + 3) % 4] : Lower[Index];
				const auto Kind = Layout == 6 ? DurinCodeGen::EPropertyGenFlags::Float
					: DurinCodeGen::EPropertyGenFlags::Double;
				WriteProjectedField(Writer, Owner, Name, Kind,
					std::format("{}:{}", uint32(Kind), Layout == 6 ? 4 : 8), std::move(Payload.Bytes));
			}
			return true;
		}

		auto EncodeLoadArchiveValue(const FDecodedType& Type, const FValue& Value,
			const FDecodedPackage& Package, Private::FByteWriter& Writer,
			FReaderDiagnostic& Diagnostic, std::string Path) -> bool
		{
			switch (Type.Opcode)
			{
			case ETypeOpcode::Bool: Writer.Write(uint8(Value.Bool)); return true;
			case ETypeOpcode::I8: Writer.Write(int8(Value.Signed)); return true;
			case ETypeOpcode::I16: Writer.Write(int16(Value.Signed)); return true;
			case ETypeOpcode::I32: Writer.Write(int32(Value.Signed)); return true;
			case ETypeOpcode::I64: Writer.Write(Value.Signed); return true;
			case ETypeOpcode::U8: Writer.Write(uint8(Value.Unsigned)); return true;
			case ETypeOpcode::U16: Writer.Write(uint16(Value.Unsigned)); return true;
			case ETypeOpcode::U32: Writer.Write(uint32(Value.Unsigned)); return true;
			case ETypeOpcode::U64: Writer.Write(Value.Unsigned); return true;
			case ETypeOpcode::F32: Writer.Write(uint32(Value.FloatingBits)); return true;
			case ETypeOpcode::F64: Writer.Write(Value.FloatingBits); return true;
			case ETypeOpcode::String: case ETypeOpcode::Name: Writer.WriteString(Value.Text); return true;
			case ETypeOpcode::Guid:
				Writer.Write(Value.Guid.A); Writer.Write(Value.Guid.B); Writer.Write(Value.Guid.C); Writer.Write(Value.Guid.D); return true;
			case ETypeOpcode::Enum:
			{
				const auto Storage = static_cast<ETypeOpcode>(Type.Parameter);
				const uint64 Bits = Storage >= ETypeOpcode::I8 && Storage <= ETypeOpcode::I64
					? static_cast<uint64>(Value.Signed) : Value.Unsigned;
				switch (TypeWidth(Storage))
				{
				case 1: WriteInteger<uint8>(Writer, Bits); return true;
				case 2: WriteInteger<uint16>(Writer, Bits); return true;
				case 4: WriteInteger<uint32>(Writer, Bits); return true;
				case 8: Writer.Write(Bits); return true;
				default: return Fail(Diagnostic, EReaderFailure::InvalidTable, "Enum storage width is invalid.", 0, std::move(Path));
				}
			}
			case ETypeOpcode::Intrinsic:
				return EncodeIntrinsicLoadValue(Type.Parameter, Value.ComponentBits, Writer, Diagnostic);
			case ETypeOpcode::Struct:
			{
				uint64 SchemaId = 0; const FDecodedSchema* Schema = FindSchema(Package, Type.QualifiedName, SchemaId);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size())
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Struct load projection is invalid.", 0, std::move(Path));
				Writer.WriteString(Type.QualifiedName); Writer.Write(uint64(Value.Elements.size()));
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto It = std::ranges::find(Schema->Fields, Value.FieldNames[Index], &FDecodedField::Name);
					if (It == Schema->Fields.end()) return Fail(Diagnostic, EReaderFailure::InvalidValue, "Struct field is absent from its schema.", 0, std::move(Path));
					const FDecodedType* ChildType = TypeAt(Package, It->TypeId);
					if (!ChildType) return Fail(Diagnostic, EReaderFailure::InvalidTable, "Struct field type is invalid.", 0, std::move(Path));
					Private::FByteWriter Payload;
					if (!EncodeLoadArchiveValue(*ChildType, Value.Elements[Index], Package, Payload, Diagnostic,
						std::format("{}::{}", Schema->QualifiedName, It->Name))) return false;
					Writer.WriteString(Schema->QualifiedName); Writer.WriteString(It->Name);
					Writer.Write(uint8(TypeKind(*ChildType, Package))); Writer.WriteString(TypeSignature(*ChildType, Package));
					Writer.Write(uint64(Payload.Bytes.size())); Writer.WriteBytes(Payload.Bytes);
				}
				return true;
			}
			case ETypeOpcode::FixedArray: case ETypeOpcode::Array:
			{
				const FDecodedType* Element = Type.ChildTypeIds.size() == 1 ? TypeAt(Package, Type.ChildTypeIds[0]) : nullptr;
				if (!Element) return Fail(Diagnostic, EReaderFailure::InvalidTable, "Array element type is invalid.", 0, std::move(Path));
				if (Type.Opcode == ETypeOpcode::Array) Writer.Write(uint64(Value.Elements.size()));
				for (const FValue& Item : Value.Elements)
					if (!EncodeLoadArchiveValue(*Element, Item, Package, Writer, Diagnostic, Path)) return false;
				return true;
			}
			case ETypeOpcode::Map:
			{
				const FDecodedType* Key = Type.ChildTypeIds.size() == 2 ? TypeAt(Package, Type.ChildTypeIds[0]) : nullptr;
				const FDecodedType* Mapped = Type.ChildTypeIds.size() == 2 ? TypeAt(Package, Type.ChildTypeIds[1]) : nullptr;
				if (!Key || !Mapped || Value.Elements.size() % 2 != 0) return Fail(Diagnostic, EReaderFailure::InvalidValue, "Map projection is invalid.", 0, std::move(Path));
				Writer.Write(uint64(Value.Elements.size() / 2));
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
					if (!EncodeLoadArchiveValue(*Key, Value.Elements[Index], Package, Writer, Diagnostic, Path)
						|| !EncodeLoadArchiveValue(*Mapped, Value.Elements[Index + 1], Package, Writer, Diagnostic, Path)) return false;
				return true;
			}
			case ETypeOpcode::HardRef:
				Writer.Write(Value.ReferenceTag);
				if (Value.ReferenceTag == 1) Writer.Write(Value.ReferenceId);
				else if (Value.ReferenceTag == 2)
				{
					const auto& Targets = Package.Header.HardReferenceTargets.empty()
						? Package.Header.Dependencies
						: Package.Header.HardReferenceTargets;
					Writer.WriteString(Targets[static_cast<size_t>(Value.ReferenceId - 1)]);
				}
				return true;
			case ETypeOpcode::SoftRef:
				Writer.Write(Value.ReferenceTag); if (Value.ReferenceTag == 1) Writer.WriteString(Value.Text); return true;
			case ETypeOpcode::Bytes: case ETypeOpcode::BulkData:
				Writer.WriteBytes(Value.Bytes); return true;
			}
			return Fail(Diagnostic, EReaderFailure::InvalidValue, "Unsupported load value.", 0, std::move(Path));
		}

		auto ShouldFail(const FLiveLoadOptions& Options, ELiveLoadPhase Phase, uint64 Index) -> bool
		{
			return Options.ShouldFail && Options.ShouldFail(Phase, Index);
		}

		auto FindExistingInner(DObject* Outer, std::string_view Name, DClass* Class,
			bool& bOutTypeMismatch) -> DObject*
		{
			bOutTypeMismatch = false;
			for (DObject* Object : GDObjectArray.GetObjectsWithOuter(Outer, EObjectQueryScope::LiveOnly))
			{
				if (!Object || Object->GetName() != Name) continue;
				if (Object->GetClass() != Class) { bOutTypeMismatch = true; return nullptr; }
				return Object;
			}
			return nullptr;
		}

		auto FindDecodedDeprecatedRoute(const FDecodedPackage& Package,
			const FDecodedSchema& Schema, const FDecodedField& Field,
			const FDecodedType& Type) -> FProperty*;

		auto MergeDuplicateAuthoredOverrideEntries(
			std::vector<FAuthoredOverrideEntry>& Entries) -> void
		{
			std::ranges::sort(Entries, [](const FAuthoredOverrideEntry& Left,
				const FAuthoredOverrideEntry& Right) {
				return CompareAuthoredOverridePaths(Left.Path, Right.Path) < 0;
			});
			size_t OutputCount = 0;
			for (size_t Index = 0; Index < Entries.size(); ++Index)
			{
				if (OutputCount != 0 && CompareAuthoredOverridePaths(
						Entries[OutputCount - 1].Path, Entries[Index].Path) == 0)
				{
					if (Entries[Index].Provenance == EAuthoredOverrideProvenance::Forced)
						Entries[OutputCount - 1].Provenance = EAuthoredOverrideProvenance::Forced;
					continue;
				}
				if (OutputCount != Index) Entries[OutputCount] = std::move(Entries[Index]);
				++OutputCount;
			}
			Entries.resize(OutputCount);
		}

		auto RestoreNestedLedger(const FDecodedType& Type, const FValue& Value,
			const FDecodedPackage& Package, FAuthoredOverridePath& Path,
			std::vector<FAuthoredOverrideEntry>& Entries,
			bool& bUsedDeprecatedRoute,
			FReaderDiagnostic& Diagnostic) -> bool
		{
			if (Type.Opcode == ETypeOpcode::Struct)
			{
				uint64 SchemaId = 0; const FDecodedSchema* Schema = FindSchema(Package, Type.QualifiedName, SchemaId);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size()
					|| Value.Provenances.size() != Value.Elements.size())
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Struct ledger projection is invalid.");
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto It = std::ranges::find(Schema->Fields, Value.FieldNames[Index], &FDecodedField::Name);
					if (It == Schema->Fields.end()) return Fail(Diagnostic, EReaderFailure::InvalidValue, "Struct ledger field is missing.");
					const FDecodedType* ChildType = TypeAt(Package, It->TypeId); if (!ChildType) return false;
					const auto Provenance = Value.Provenances[Index] == EDefaultDeltaProvenance::Forced
						? EAuthoredOverrideProvenance::Forced : EAuthoredOverrideProvenance::LoadedExplicit;
					if (FProperty* Route = FindDecodedDeprecatedRoute(Package, *Schema, *It, *ChildType))
					{
						bUsedDeprecatedRoute = true;
						for (FName Target : Route->GetDeprecation()->MigrationTargets)
						{
							Path.push_back(FAuthoredOverridePathToken::Field(
								FName(Schema->QualifiedName), Target));
							Entries.push_back({Path, Provenance});
							Path.pop_back();
						}
						continue;
					}
					Path.push_back(FAuthoredOverridePathToken::Field(FName(Schema->QualifiedName), FName(It->Name)));
					Entries.push_back({Path, Provenance});
					if (!RestoreNestedLedger(*ChildType, Value.Elements[Index], Package, Path,
						Entries, bUsedDeprecatedRoute, Diagnostic)) return false;
					Path.pop_back();
				}
			}
			else if ((Type.Opcode == ETypeOpcode::FixedArray || Type.Opcode == ETypeOpcode::Array)
				&& Type.ChildTypeIds.size() == 1)
			{
				const FDecodedType* ChildType = TypeAt(Package, Type.ChildTypeIds[0]); if (!ChildType) return false;
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					Path.push_back(Type.Opcode == ETypeOpcode::FixedArray
						? FAuthoredOverridePathToken::FixedArrayElement(Index)
						: FAuthoredOverridePathToken::ArrayElement(Index));
					if (!RestoreNestedLedger(*ChildType, Value.Elements[Index], Package, Path,
						Entries, bUsedDeprecatedRoute, Diagnostic)) return false;
					Path.pop_back();
				}
			}
			else if (Type.Opcode == ETypeOpcode::Map && Type.ChildTypeIds.size() == 2)
			{
				const FDecodedType* KeyType = TypeAt(Package, Type.ChildTypeIds[0]);
				const FDecodedType* ValueType = TypeAt(Package, Type.ChildTypeIds[1]);
				if (!KeyType || !ValueType || Value.Elements.size() % 2 != 0) return false;
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
				{
					std::vector<std::byte> Token;
					if (!BuildCanonicalMapKeyToken(Package, *KeyType, Value.Elements[Index], Token, &Diagnostic)) return false;
					Path.push_back(FAuthoredOverridePathToken::MapValue(std::move(Token)));
					if (!RestoreNestedLedger(*ValueType, Value.Elements[Index + 1], Package, Path,
						Entries, bUsedDeprecatedRoute, Diagnostic)) return false;
					Path.pop_back();
				}
			}
			return true;
		}

		auto CanonicalizeSerializedClassName(std::string& Name) -> void
		{
			if (DClass* Class = FindClassBySerializedName(FName(Name)))
				Name = Class->GetQualifiedName().ToString();
		}

		auto CanonicalizeSerializedSchemaName(std::string& Name) -> void
		{
			if (DClass* Class = FindClassBySerializedName(FName(Name)))
			{
				Name = Class->GetQualifiedName().ToString();
				return;
			}
			if (DStruct* Struct = FindStructBySerializedName(FName(Name)))
				Name = Struct->GetQualifiedName().ToString();
		}

		auto CanonicalizeSerializedPropertyName(
			std::string_view DeclaringType,
			std::string& Name) -> void
		{
			DStructBase* Owner = FindClassBySerializedName(FName(DeclaringType));
			if (!Owner) Owner = FindStructBySerializedName(FName(DeclaringType));
			if (Owner)
				if (FProperty* Property = Owner->FindPropertyBySerializedName(FName(Name), false))
					Name = Property->NamePrivate.ToString();
		}

		auto CanonicalizeSerializedTypeName(FDecodedType& Type) -> void
		{
			if (Type.QualifiedName.empty()) return;
			if (Type.Opcode == ETypeOpcode::Enum)
			{
				if (DEnum* Enum = FindEnumBySerializedName(FName(Type.QualifiedName)))
					Type.QualifiedName = Enum->GetQualifiedName().ToString();
			}
			else if (Type.Opcode == ETypeOpcode::Struct)
			{
				if (DStruct* Struct = FindStructBySerializedName(FName(Type.QualifiedName)))
					Type.QualifiedName = Struct->GetQualifiedName().ToString();
			}
			else if (Type.Opcode == ETypeOpcode::HardRef || Type.Opcode == ETypeOpcode::SoftRef)
			{
				CanonicalizeSerializedClassName(Type.QualifiedName);
			}
		}

		auto GatherCanonicalizationEvidence(
			const FDecodedPackage& Package,
			const FPackagePath& PackagePath)
			-> std::vector<FAssetCanonicalizationEvidence>
		{
			std::vector<FAssetCanonicalizationEvidence> Result;
			auto AddClass = [&](std::string_view Stored, EAssetSerializedIdentityLocation Location,
				std::string LogicalPath) {
				if (DClass* Class = FindClassBySerializedName(FName(Stored));
					Class && Class->GetQualifiedName().ToString() != Stored)
					Result.push_back({PackagePath, std::string(Stored),
						Class->GetQualifiedName().ToString(), EAssetReflectedIdentityKind::Class,
						Location, std::move(LogicalPath)});
			};
			auto AddStruct = [&](std::string_view Stored, EAssetSerializedIdentityLocation Location,
				std::string LogicalPath) {
				if (DStruct* Struct = FindStructBySerializedName(FName(Stored));
					Struct && Struct->GetQualifiedName().ToString() != Stored)
					Result.push_back({PackagePath, std::string(Stored),
						Struct->GetQualifiedName().ToString(), EAssetReflectedIdentityKind::Struct,
						Location, std::move(LogicalPath)});
			};
			auto AddEnum = [&](std::string_view Stored, EAssetSerializedIdentityLocation Location,
				std::string LogicalPath) {
				if (DEnum* Enum = FindEnumBySerializedName(FName(Stored));
					Enum && Enum->GetQualifiedName().ToString() != Stored)
					Result.push_back({PackagePath, std::string(Stored),
						Enum->GetQualifiedName().ToString(), EAssetReflectedIdentityKind::Enum,
						Location, std::move(LogicalPath)});
			};

			AddClass(Package.Header.AssetClass, EAssetSerializedIdentityLocation::PackageHeader,
				"header.assetClass");
			for (size_t Index = 0; Index < Package.Objects.size(); ++Index)
				AddClass(Package.Objects[Index].ClassName, EAssetSerializedIdentityLocation::ObjectRecord,
					std::format("objects[{}].class", Index));
			for (size_t Index = 0; Index < Package.Schemas.size(); ++Index)
			{
				const std::string& Stored = Package.Schemas[Index].QualifiedName;
				const size_t Before = Result.size();
				AddClass(Stored, EAssetSerializedIdentityLocation::Schema,
					std::format("schemas[{}].identity", Index));
				if (Result.size() == Before)
					AddStruct(Stored, EAssetSerializedIdentityLocation::Schema,
						std::format("schemas[{}].identity", Index));
				DStructBase* Owner = FindClassBySerializedName(FName(Stored));
				if (!Owner) Owner = FindStructBySerializedName(FName(Stored));
				if (Owner)
					for (size_t FieldIndex = 0; FieldIndex < Package.Schemas[Index].Fields.size(); ++FieldIndex)
					{
						const std::string& FieldName = Package.Schemas[Index].Fields[FieldIndex].Name;
						if (FProperty* Property = Owner->FindPropertyBySerializedName(FName(FieldName), false);
							Property && Property->NamePrivate.ToString() != FieldName)
							Result.push_back({PackagePath, FieldName, Property->NamePrivate.ToString(),
								EAssetReflectedIdentityKind::Property,
								EAssetSerializedIdentityLocation::Schema,
								std::format("schemas[{}].fields[{}].name", Index, FieldIndex)});
					}
			}
			for (size_t Index = 0; Index < Package.Types.size(); ++Index)
			{
				const FDecodedType& Type = Package.Types[Index];
				const std::string Path = std::format("types[{}].identity", Index);
				switch (Type.Opcode)
				{
				case ETypeOpcode::Enum: AddEnum(Type.QualifiedName, EAssetSerializedIdentityLocation::TypeDescriptor, Path); break;
				case ETypeOpcode::Struct: AddStruct(Type.QualifiedName, EAssetSerializedIdentityLocation::TypeDescriptor, Path); break;
				case ETypeOpcode::HardRef:
				case ETypeOpcode::SoftRef: AddClass(Type.QualifiedName, EAssetSerializedIdentityLocation::TypeDescriptor, Path); break;
				default: break;
				}
			}
			std::ranges::sort(Result, [](const auto& Left, const auto& Right) {
				return std::tie(Left.Location, Left.LogicalPath, Left.Kind, Left.StoredIdentity, Left.CurrentIdentity)
					< std::tie(Right.Location, Right.LogicalPath, Right.Kind, Right.StoredIdentity, Right.CurrentIdentity);
			});
			return Result;
		}

		// Converts recognized reflection aliases at the bytes-to-runtime boundary.
		// The raw DecodePackage/ReencodePackage contract remains byte-preserving.
		auto CanonicalizeSerializedReflectionNames(
			FDecodedPackage& Package,
			std::string* OutError = nullptr) -> bool
		{
			CanonicalizeSerializedClassName(Package.Header.AssetClass);
			for (FDecodedObject& Object : Package.Objects)
				CanonicalizeSerializedClassName(Object.ClassName);
			for (FDecodedSchema& Schema : Package.Schemas)
			{
				CanonicalizeSerializedSchemaName(Schema.QualifiedName);
				std::unordered_set<std::string> CurrentFieldNames;
				for (FDecodedField& Field : Schema.Fields)
				{
					CanonicalizeSerializedPropertyName(Schema.QualifiedName, Field.Name);
					if (!CurrentFieldNames.emplace(Field.Name).second)
					{
						if (OutError) *OutError = std::format(
							"Serialized schema {} contains fields that canonicalize to duplicate name {}.",
							Schema.QualifiedName, Field.Name);
						return false;
					}
				}
			}
			for (FDecodedType& Type : Package.Types)
				CanonicalizeSerializedTypeName(Type);
			return true;
		}

		auto DecodedCustomVersion(const FDecodedPackage& Package,
			const FGuid& Guid) -> int32
		{
			const auto It = std::ranges::find(Package.CustomVersions, Guid, &FCustomVersion::Guid);
			return It == Package.CustomVersions.end() ? -1 : static_cast<int32>(It->Value);
		}

		auto FindDecodedDeprecatedRoute(const FDecodedPackage& Package,
			const FDecodedSchema& Schema, const FDecodedField& Field,
			const FDecodedType& Type) -> FProperty*
		{
			DStructBase* Owner = FindClassByQualifiedName(FName(Schema.QualifiedName));
			if (!Owner) Owner = FindStructByQualifiedName(FName(Schema.QualifiedName));
			if (!Owner) return nullptr;
			const DurinCodeGen::EPropertyGenFlags Kind = TypeKind(Type, Package);
			const std::string Signature = TypeSignature(Type, Package);
			FProperty* Match = nullptr;
			bool bAmbiguous = false;
			Owner->ForEachProperty([&](FProperty* Property) {
				if (bAmbiguous || !Property) return;
				const FPropertyDeprecation* Deprecation = Property->GetDeprecation();
				if (!Deprecation || Deprecation->HistoricalName.ToString() != Field.Name
					|| DecodedCustomVersion(Package, Deprecation->CustomVersionGuid)
						>= Deprecation->DeprecatedBefore
					|| Property->GetKind() != Kind
					|| Private::GetSerializedTypeSignature(Property) != Signature) return;
				if (Match) bAmbiguous = true;
				else Match = Property;
			}, false);
			return bAmbiguous ? nullptr : Match;
		}

		auto GatherNestedDeprecatedRouteEvidence(const FDecodedType& Type, const FValue& Value,
			const FDecodedPackage& Package,
			std::span<const std::pair<FGuid, int32>> Versions, const FPackagePath& PackagePath,
			std::string_view ObjectPath, std::vector<FAssetDeprecatedRouteEvidence>& Out) -> void
		{
			if (Type.Opcode == ETypeOpcode::Struct)
			{
				uint64 SchemaId = 0;
				const FDecodedSchema* Schema = FindSchema(Package, Type.QualifiedName, SchemaId);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size()) return;
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto Field = std::ranges::find(
						Schema->Fields, Value.FieldNames[Index], &FDecodedField::Name);
					if (Field == Schema->Fields.end()) continue;
					const FDecodedType* ChildType = TypeAt(Package, Field->TypeId);
					if (!ChildType) continue;
					FProperty* LiveRoute =
						FindDecodedDeprecatedRoute(Package, *Schema, *Field, *ChildType);
					if (LiveRoute)
					{
						const FPropertyDeprecation* Deprecation = LiveRoute->GetDeprecation();
						const FGuid VersionGuid = Deprecation->CustomVersionGuid;
						const auto Version = std::ranges::find_if(Versions,
							[&](const auto& Pair) { return Pair.first == VersionGuid; });
						std::vector<std::string> MigrationTargets;
						for (FName Target : Deprecation->MigrationTargets)
							MigrationTargets.push_back(Target.ToString());
						Out.push_back({
							.PackagePath = PackagePath, .ObjectPath = std::string(ObjectPath),
							.DeclaringType = Schema->QualifiedName, .StoredFieldName = Field->Name,
							.DeprecatedPropertyName = LiveRoute->NamePrivate.ToString(),
							.MigrationTargets = std::move(MigrationTargets),
							.CustomVersionGuid = VersionGuid,
							.SourceVersion = Version == Versions.end() ? -1 : Version->second,
							.DeprecatedBefore = Deprecation->DeprecatedBefore});
					}
					else GatherNestedDeprecatedRouteEvidence(*ChildType, Value.Elements[Index],
						Package, Versions, PackagePath, ObjectPath, Out);
				}
			}
			else if ((Type.Opcode == ETypeOpcode::FixedArray || Type.Opcode == ETypeOpcode::Array)
				&& Type.ChildTypeIds.size() == 1)
			{
				if (const FDecodedType* Child = TypeAt(Package, Type.ChildTypeIds[0]))
					for (const FValue& Element : Value.Elements)
						GatherNestedDeprecatedRouteEvidence(*Child, Element, Package,
							Versions, PackagePath, ObjectPath, Out);
			}
			else if (Type.Opcode == ETypeOpcode::Map && Type.ChildTypeIds.size() == 2)
			{
				if (const FDecodedType* Child = TypeAt(Package, Type.ChildTypeIds[1]))
					for (size_t Index = 1; Index < Value.Elements.size(); Index += 2)
						GatherNestedDeprecatedRouteEvidence(*Child, Value.Elements[Index], Package,
							Versions, PackagePath, ObjectPath, Out);
			}
		}
	}

	auto ResetAssetPackageReencodeCountForTesting() -> void
	{
		ResetReencodeCountForTesting();
	}

	auto GetAssetPackageReencodeCountForTesting() -> uint64
	{
		return GetReencodeCountForTesting();
	}

	FLoadedAssetPackage::~FLoadedAssetPackage() { Reset(); }

	FLoadedAssetPackage::FLoadedAssetPackage(FLoadedAssetPackage&& Other) noexcept
		: Package(std::exchange(Other.Package, nullptr)) {}

	auto FLoadedAssetPackage::operator=(FLoadedAssetPackage&& Other) noexcept
		-> FLoadedAssetPackage&
	{
		if (this != &Other) { Reset(); Package = std::exchange(Other.Package, nullptr); }
		return *this;
	}

	auto FLoadedAssetPackage::Reset() -> void
	{
		if (!Package) return;
		MarkObjectHierarchyAsGarbage(Package);
		Package = nullptr;
		CollectGarbage();
	}

	auto LoadDecodedAssetPackage(FDecodedPackage Decoded, const FPackagePath& PackagePath,
		FLoadedAssetPackage& OutPackage, FAssetLoadReport* OutReport,
		const FLiveLoadOptions& Options, FReaderDiagnostic* OutDiagnostic) -> FAssetResult
	{
		FReaderDiagnostic Diagnostic;
		auto Finish = [&](FAssetResult Result) {
			if (OutDiagnostic) *OutDiagnostic = Diagnostic; return Result;
		};
		if (!PackagePath.IsValid())
		{
			Fail(Diagnostic, EReaderFailure::PublicationFailure, "Live object-stream load requires a validated package path.");
			return Finish({EAssetError::InvalidPath, Diagnostic.Message});
		}
		if (FindPackage(PackagePath.GetView()))
		{
			Fail(Diagnostic, EReaderFailure::PublicationFailure,
				"A package with the requested path is already live.");
			return Finish({EAssetError::AlreadyExists, Diagnostic.Message});
		}
		std::vector<FAssetCanonicalizationEvidence> CanonicalizationEvidence =
			GatherCanonicalizationEvidence(Decoded, PackagePath);
		std::string CanonicalizationError;
		if (!CanonicalizeSerializedReflectionNames(Decoded, &CanonicalizationError))
		{
			Fail(Diagnostic, EReaderFailure::InvalidTable, CanonicalizationError);
			return Finish({EAssetError::CorruptFile, Diagnostic.Message});
		}
		for (size_t ObjectIndex = 0; ObjectIndex < Decoded.Objects.size(); ++ObjectIndex)
		{
			const FDecodedObject& Object = Decoded.Objects[ObjectIndex];
			DClass* Class = FindClassByQualifiedName(FName(Object.ClassName));
			if (!Class)
			{
				Fail(Diagnostic, EReaderFailure::UnknownClass,
					"Serialized class is unavailable.", 0, Object.Path);
				return Finish({EAssetError::UnknownClass, Diagnostic.Message});
			}
			for (const FDecodedOverride& Override :
				Decoded.ObjectValues[ObjectIndex].Overrides)
			{
				const FDecodedSchema* Schema = SchemaAt(Decoded, Override.SchemaId);
				if (!Schema || Override.FieldId == 0
					|| Override.FieldId > Schema->Fields.size())
					continue;
				const FDecodedField& Field =
					Schema->Fields[static_cast<size_t>(Override.FieldId - 1)];
				const FDecodedType* Type = TypeAt(Decoded, Field.TypeId);
				DClass* DeclaringClass = FindClassByQualifiedName(FName(Schema->QualifiedName));
				bool bDeclaringClassMatches = false;
				for (DClass* Ancestor = Class; Ancestor; Ancestor = Ancestor->GetSuperClass())
					if (Ancestor == DeclaringClass)
					{
						bDeclaringClassMatches = true;
						break;
					}
				FProperty* Expected = bDeclaringClassMatches
					? DeclaringClass->FindPropertyByName(FName(Field.Name), false) : nullptr;
				const bool bCurrentCompatible = Override.Provenance != 2 && Type && Expected
					&& !Expected->GetDeprecation()
					&& Expected->GetKind() == TypeKind(*Type, Decoded)
					&& Private::GetSerializedTypeSignature(Expected) == TypeSignature(*Type, Decoded);
				const bool bDeprecatedCompatible = Override.Provenance != 2 && Type
					&& FindDecodedDeprecatedRoute(Decoded, *Schema, Field, *Type);
				// Cooked native projection fields are validated against the exact
				// SerializeCooked manifest and must be consumed by the load Archive.
				const bool bCookedNativeCandidate = Options.bCooked
					&& Override.Provenance != 2 && Type && !Expected;
				const bool bCompatible = bCurrentCompatible || bDeprecatedCompatible
					|| bCookedNativeCandidate;
				if (!bCompatible)
				{
					Fail(Diagnostic, EReaderFailure::ArchiveFailure,
						std::format("Serialized field {}::{} is incompatible with the live schema.",
							Schema->QualifiedName, Field.Name), 0, Object.Path);
					return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
				}
			}
		}
		if (Decoded.Objects.empty())
		{
			Fail(Diagnostic, EReaderFailure::InvalidTopology, "Package has no object exports.");
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}

		DPackage* Package = NewObject<DPackage>(
			nullptr,
			FName(PackagePath.GetAssetName()),
			EObjectFlags::Standalone);
		if (!Package)
		{
			Fail(Diagnostic, EReaderFailure::PublicationFailure, "Could not allocate the package skeleton.");
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}
		Package->InitializeAssetPackage(PackagePath);
		std::vector<DObject*> Objects(Decoded.Objects.size(), nullptr);
		const FAssetPackageLoadSnapshot DependencySnapshot = CapturePackageLoadSnapshot();
		bool bSkeletonPublished = false;
		auto Rollback = [&]() {
			if (bSkeletonPublished && Options.OnSkeletonRollback)
				Options.OnSkeletonRollback(Package);
			MarkObjectHierarchyAsGarbage(Package); CollectGarbage();
			ReleasePackagesLoadedSince(DependencySnapshot);
		};

		for (size_t Index = 0; Index < Decoded.Objects.size(); ++Index)
		{
			if (ShouldFail(Options, ELiveLoadPhase::CreateSkeleton, Index))
			{
				Fail(Diagnostic, EReaderFailure::InvalidTopology, "Injected skeleton creation failure."); Rollback();
				return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
			}
			const FDecodedObject& Descriptor = Decoded.Objects[Index];
			DClass* Class = FindClassByQualifiedName(FName(Descriptor.ClassName));
			if (!Class || !Class->ClassConstructor)
			{
				Fail(Diagnostic, EReaderFailure::UnknownClass, "Serialized class is unavailable.", 0, Descriptor.Path); Rollback();
				return Finish({EAssetError::UnknownClass, Diagnostic.Message});
			}
			DObject* Outer = Descriptor.OuterId == 0 ? static_cast<DObject*>(Package)
				: Objects[static_cast<size_t>(Descriptor.OuterId - 1)];
			bool bTypeMismatch = false;
			DObject* Object = FindExistingInner(Outer, Descriptor.ObjectName, Class, bTypeMismatch);
			if (bTypeMismatch)
			{
				Fail(Diagnostic, EReaderFailure::TypeMismatch, "Existing default inner has a different class.", 0, Descriptor.Path); Rollback();
				return Finish({EAssetError::TypeMismatch, Diagnostic.Message});
			}
			if (!Object)
			{
				FStaticConstructObjectParameters Parameters{
					Class, Outer, FName(Descriptor.ObjectName), Class->PropertiesSize,
					Descriptor.OuterId == 0 ? EObjectFlags::Public : EObjectFlags::NoFlags};
				Parameters.Purpose = EObjectConstructionPurpose::AssetLoad;
				Object = StaticConstructObject(Parameters);
				if (Object) DObjectForceRegistration(Object);
			}
			if (!Object)
			{
				Fail(Diagnostic, EReaderFailure::InvalidTopology, "Object construction failed.", 0, Descriptor.Path); Rollback();
				return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
			}
			Objects[Index] = Object;
		}
		if (Options.OnSkeletonReady)
		{
			FAssetResult PublishResult = Options.OnSkeletonReady(Package);
			if (!PublishResult)
			{
				Fail(Diagnostic, EReaderFailure::PublicationFailure,
					PublishResult.Message.empty() ? "Could not publish the package skeleton."
						: PublishResult.Message);
				Rollback();
				return Finish(PublishResult);
			}
			bSkeletonPublished = true;
		}

		for (size_t Index = 0; Index < Decoded.Header.Dependencies.size(); ++Index)
		{
			if (ShouldFail(Options, ELiveLoadPhase::ResolveDependency, Index))
			{
				Fail(Diagnostic, EReaderFailure::MissingDependency, "Injected dependency failure."); Rollback();
				return Finish({EAssetError::MissingDependency, Diagnostic.Message});
			}
			FPackagePath Path; std::string PathError;
			if (!FPackagePath::TryCreate(Decoded.Header.Dependencies[Index], Path, &PathError))
			{
				Fail(Diagnostic, EReaderFailure::MissingDependency, PathError); Rollback();
				return Finish({EAssetError::InvalidPath, Diagnostic.Message});
			}
			DPackage* Dependency = nullptr; FAssetResult Result = LoadPackage(Path, Dependency);
			if (!Result)
			{
				Fail(Diagnostic, EReaderFailure::MissingDependency, Result.Message); Rollback();
				return Finish({EAssetError::MissingDependency, Diagnostic.Message});
			}
		}

		std::vector<FArchiveCustomVersion> CustomVersions;
		std::vector<std::pair<FGuid, int32>> LoadedCustomVersions;
		for (const FCustomVersion& Version : Decoded.CustomVersions)
		{
			CustomVersions.push_back({Version.Guid, static_cast<int32>(Version.Value)});
			LoadedCustomVersions.emplace_back(Version.Guid, static_cast<int32>(Version.Value));
		}
		for (DObject* Object : Objects) Object->SetLoadedCustomVersions(LoadedCustomVersions);
		FAssetLoadReport Report = OutReport ? *OutReport : FAssetLoadReport{};
		Report.PackagePath = PackagePath;
		Report.CanonicalizationEvidence = std::move(CanonicalizationEvidence);
		for (size_t ObjectIndex = 0; ObjectIndex < Decoded.Objects.size(); ++ObjectIndex)
			for (const FDecodedOverride& Override : Decoded.ObjectValues[ObjectIndex].Overrides)
			{
				const FDecodedSchema* Schema = SchemaAt(Decoded, Override.SchemaId);
				if (!Schema || Override.FieldId == 0 || Override.FieldId > Schema->Fields.size()) continue;
				const FDecodedField& Field = Schema->Fields[Override.FieldId - 1];
				if (const FDecodedType* Type = TypeAt(Decoded, Field.TypeId))
					GatherNestedDeprecatedRouteEvidence(*Type, Override.Value, Decoded,
						LoadedCustomVersions, PackagePath,
						Decoded.Objects[ObjectIndex].Path, Report.DeprecatedRouteEvidence);
			}
		for (size_t ObjectIndex = 0; ObjectIndex < Objects.size(); ++ObjectIndex)
		{
			if (ShouldFail(Options, ELiveLoadPhase::ApplyValues, ObjectIndex))
			{
				Fail(Diagnostic, EReaderFailure::ArchiveFailure, "Injected value application failure."); Rollback();
				return Finish({EAssetError::CorruptFile, Diagnostic.Message});
			}
			std::vector<Private::FAuthoredPackageFieldRecord> Fields;
			std::vector<const FDecodedOverride*> KnownOverrides;
			for (const FDecodedOverride& Override : Decoded.ObjectValues[ObjectIndex].Overrides)
			{
				const FDecodedSchema* Schema = SchemaAt(Decoded, Override.SchemaId);
				const FDecodedField& Field = Schema->Fields[static_cast<size_t>(Override.FieldId - 1)];
				const FDecodedType* Type = TypeAt(Decoded, Field.TypeId);
				Private::FByteWriter Payload;
				if (!Type || !EncodeLoadArchiveValue(*Type, Override.Value, Decoded, Payload, Diagnostic,
					std::format("{}::{}", Schema->QualifiedName, Field.Name)))
				{
					Rollback(); return Finish({EAssetError::CorruptFile, Diagnostic.Message});
				}
				Fields.push_back({Schema->QualifiedName, Field.Name, TypeKind(*Type, Decoded),
					TypeSignature(*Type, Decoded), std::move(Payload.Bytes)});
				KnownOverrides.push_back(&Override);
			}
			FArchiveState LoadContext;
			LoadContext.bCooking = Options.bCooked;
			LoadContext.bFilterEditorOnly = Options.bCooked;
			LoadContext.Target = Options.Target;
			FAssetResult Result = Private::LoadAuthoredObject(*Objects[ObjectIndex], Fields, Objects,
				PackagePath, Options.SourceFormatVersion, CustomVersions, LoadContext);
			if (!Result)
			{
				Fail(Diagnostic, EReaderFailure::ArchiveFailure, Result.Message, 0, Decoded.Objects[ObjectIndex].Path); Rollback();
				return Finish(Result);
			}
			if (Options.bCooked) continue;
			std::vector<FAuthoredOverrideEntry> LedgerEntries;
			bool bUsedDeprecatedRoute = false;
			for (const FDecodedOverride* Override : KnownOverrides)
			{
				const FDecodedSchema* Schema = SchemaAt(Decoded, Override->SchemaId);
				const FDecodedField& Field = Schema->Fields[static_cast<size_t>(Override->FieldId - 1)];
				if (ShouldFail(Options, ELiveLoadPhase::RestoreLedger, Override->FieldId))
				{
					Fail(Diagnostic, EReaderFailure::ArchiveFailure, "Injected ledger restoration failure."); Rollback();
					return Finish({EAssetError::CorruptFile, Diagnostic.Message});
				}
				const auto Provenance = Override->Provenance == 1 ? EAuthoredOverrideProvenance::Forced
					: EAuthoredOverrideProvenance::LoadedExplicit;
				const FDecodedType* Type = TypeAt(Decoded, Field.TypeId);
				FProperty* DeprecatedRoute = Type
					? FindDecodedDeprecatedRoute(Decoded, *Schema, Field, *Type) : nullptr;
				if (DeprecatedRoute)
				{
					bUsedDeprecatedRoute = true;
					const FPropertyDeprecation* Deprecation = DeprecatedRoute->GetDeprecation();
					FAssetDeprecatedRouteEvidence Evidence{
						.PackagePath = PackagePath,
						.ObjectPath = Decoded.Objects[ObjectIndex].Path,
						.DeclaringType = Schema->QualifiedName,
						.StoredFieldName = Field.Name,
						.DeprecatedPropertyName = DeprecatedRoute->NamePrivate.ToString(),
						.CustomVersionGuid = Deprecation->CustomVersionGuid,
						.SourceVersion = DecodedCustomVersion(Decoded, Deprecation->CustomVersionGuid),
						.DeprecatedBefore = Deprecation->DeprecatedBefore};
					for (FName Target : Deprecation->MigrationTargets)
					{
						LedgerEntries.push_back({FAuthoredOverridePath{FAuthoredOverridePathToken::Field(
							FName(Schema->QualifiedName), Target)}, Provenance});
						Evidence.MigrationTargets.push_back(Target.ToString());
					}
					Report.DeprecatedRouteEvidence.push_back(std::move(Evidence));
					continue;
				}
				FAuthoredOverridePath Path{FAuthoredOverridePathToken::Field(
					FName(Schema->QualifiedName), FName(Field.Name))};
				LedgerEntries.push_back({Path, Provenance});
				if (!RestoreNestedLedger(*Type, Override->Value, Decoded, Path,
					LedgerEntries, bUsedDeprecatedRoute, Diagnostic))
				{
					Rollback(); return Finish({EAssetError::CorruptFile, Diagnostic.Message});
				}
			}
			// Deprecated routes may converge on the same current field. Ordinary
			// packages retain the linear append path and skip this normalization.
			if (bUsedDeprecatedRoute)
				MergeDuplicateAuthoredOverrideEntries(LedgerEntries);
			FAuthoredOverrideDiagnostic LedgerDiagnostic;
			if (!Objects[ObjectIndex]->ReplaceAuthoredOverrides(LedgerEntries, &LedgerDiagnostic))
			{
				Fail(Diagnostic, EReaderFailure::ArchiveFailure,
					"Could not restore authored intent.", 0, LedgerDiagnostic.LogicalPath); Rollback();
				return Finish({EAssetError::CorruptFile, Diagnostic.Message});
			}
		}

		Package->ClearDirty();
		for (size_t Reverse = Objects.size(); Reverse > 0; --Reverse)
		{
			const size_t Index = Reverse - 1;
			if (ShouldFail(Options, ELiveLoadPhase::PostLoad, Index))
			{
				Fail(Diagnostic, EReaderFailure::PostLoadFailure, "Injected PostLoad failure."); Rollback();
				return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
			}
			std::string Error;
			if (!Objects[Index]->PostLoad(Error))
			{
				Fail(Diagnostic, EReaderFailure::PostLoadFailure,
					Error.empty() ? "Object PostLoad failed." : Error, 0, Decoded.Objects[Index].Path); Rollback();
				return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
			}
			Objects[Index]->ClearLoadedCustomVersions();
		}
		if (ShouldFail(Options, ELiveLoadPhase::Publish, 0))
		{
			Fail(Diagnostic, EReaderFailure::PublicationFailure, "Injected graph publication failure."); Rollback();
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}
		OutPackage = FLoadedAssetPackage(Package);
		Package->SetCanonicalResaveRecommended(!Report.CanonicalizationEvidence.empty()
			|| !Report.DeprecatedRouteEvidence.empty());
		if (OutReport) *OutReport = std::move(Report);
		Diagnostic.Reset(); return Finish({});
	}

}
