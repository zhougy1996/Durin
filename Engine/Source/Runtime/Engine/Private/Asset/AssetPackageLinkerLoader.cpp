#include "AssetPackageLinker.h"

#include "AssetPackageArchive.h"
#include "AssetPackageValueCodec.h"
#include "Asset/Load.h"

#include "DObject/Class.h"
#include "DObject/Archive.h"
#include "DObject/CanonicalMapKey.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::Asset::Private
{
	namespace
	{
		struct FLinkerApplyDiagnostic
		{
			EAssetError Error = EAssetError::None;
			std::string LogicalPath;
			std::string Message;

			auto Reset() -> void { *this = {}; }
		};

		auto Fail(FLinkerApplyDiagnostic& Diagnostic, EAssetError Error,
			std::string_view Message, uint64 Offset = 0, std::string Path = {}) -> bool
		{
			(void)Offset;
			if (Diagnostic.Error == EAssetError::None)
				Diagnostic = {Error, std::move(Path), std::string(Message)};
			return false;
		}

		auto FindSchema(const ObjectPackage::FLinkerTables& Linker,
			std::string_view Name) -> const ObjectPackage::FSerializedSchema*
		{
			const auto It = std::ranges::find(Linker.Schemas, Name,
				&ObjectPackage::FSerializedSchema::QualifiedName);
			return It == Linker.Schemas.end() ? nullptr : &*It;
		}

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

		template<typename T>
		auto WriteInteger(Private::FByteWriter& Writer, uint64 Value) -> void
		{
			Writer.Write(static_cast<T>(Value));
		}

		auto WriteProjectedField(Private::FByteWriter& Writer, std::string_view Owner,
			std::string_view Name, DurinCodeGen::EPropertyGenFlags Kind,
			std::string Signature, FByteArray Payload) -> void
		{
			Writer.WriteString(Owner); Writer.WriteString(Name); Writer.Write(uint8(Kind));
			Writer.WriteString(Signature); Writer.Write(uint64(Payload.size())); Writer.WriteBytes(Payload);
		}

		auto EncodeIntrinsicLoadValue(uint64 Layout, std::span<const uint64> Components,
			FByteWriter& Writer, FLinkerApplyDiagnostic& Diagnostic) -> bool
		{
			const std::string Owner(IntrinsicName(Layout));
			if (Owner.empty()) return Fail(Diagnostic, EAssetError::CorruptFile, "Intrinsic layout is invalid.");
			Writer.WriteString(Owner);
			if (Layout == 5)
			{
				if (Components.size() != 10) return Fail(Diagnostic, EAssetError::CorruptFile, "Transform component count is invalid.");
				Writer.Write(uint64(3));
				for (const auto [Name, ChildLayout, Offset, Count] : {
					std::tuple<std::string_view, uint64, size_t, size_t>{"Rotation", 4, 0, 4},
					{"Translation", 2, 4, 3}, {"Scale3D", 2, 7, 3}})
				{
					FByteWriter Payload;
					if (!EncodeIntrinsicLoadValue(ChildLayout, Components.subspan(Offset, Count), Payload, Diagnostic)) return false;
					WriteProjectedField(Writer, Owner, Name, DurinCodeGen::EPropertyGenFlags::Struct,
						std::format("Struct<{}>", IntrinsicName(ChildLayout)), std::move(Payload.Bytes));
				}
				return true;
			}
			const std::array<std::string_view, 4> Lower = {"x", "y", "z", "w"};
			const std::array<std::string_view, 4> Color = {"R", "G", "B", "A"};
			const uint64 Count = Layout == 1 ? 2 : Layout == 2 ? 3 : 4;
			if (Components.size() != Count) return Fail(Diagnostic, EAssetError::CorruptFile, "Intrinsic component count is invalid.");
			Writer.Write(Count);
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				FByteWriter Payload;
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

		auto MakeBulkDescriptor(const ObjectPackage::FSerializedValue& Value,
			uint64 FieldIndex) -> FByteArray
		{
			const uint64 StoredSize = Value.bBulkPayloadAvailable
				? Value.Bytes.size() : Value.BulkStoredSize;
			const FXxHash128 Hash = Value.bBulkPayloadAvailable
				? FXxHash128::HashBuffer(Value.Bytes) : Value.BulkContentHash;
			FGuid PayloadId{
				static_cast<uint32>(Hash.HashLow),
				static_cast<uint32>(Hash.HashLow >> 32),
				static_cast<uint32>(Hash.HashHigh),
				static_cast<uint32>(Hash.HashHigh >> 32)};
			if (!PayloadId.IsValid()) PayloadId.A = 1;
			FBinaryWriter Writer;
			Writer.WriteU64(FieldIndex);
			const bool bExternal = Value.BulkStorage == ObjectPackage::EBulkStorageKind::External;
			Writer.WriteU8(bExternal ? 1 : 0);
			Writer.WriteU8(0);
			Writer.WriteU16(static_cast<uint16>(Value.BulkAlignment));
			Writer.WriteU32(1);
			Writer.WriteGuid(PayloadId);
			Writer.WriteHash128(Hash);
			Writer.WriteU64(StoredSize);
			Writer.WriteU64(StoredSize);
			Writer.WriteU64(bExternal ? Value.BulkOffset : 0);
			if (!bExternal) Writer.WriteBytes(Value.Bytes);
			return Writer.TakeBytes();
		}

		auto EncodeLoadArchiveValue(const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Value,
			const ObjectPackage::FLinkerTables& Linker, FByteWriter& Writer,
			uint64& BulkFieldIndex, FLinkerApplyDiagnostic& Diagnostic,
			std::string Path) -> bool
		{
			using K = ObjectPackage::EValueKind;
			switch (Type.Kind)
			{
			case K::Bool: Writer.Write(uint8(Value.Bool)); return true;
			case K::I8: Writer.Write(int8(Value.Signed)); return true;
			case K::I16: Writer.Write(int16(Value.Signed)); return true;
			case K::I32: Writer.Write(int32(Value.Signed)); return true;
			case K::I64: Writer.Write(Value.Signed); return true;
			case K::U8: case K::Byte: Writer.Write(uint8(Value.Unsigned)); return true;
			case K::U16: Writer.Write(uint16(Value.Unsigned)); return true;
			case K::U32: Writer.Write(uint32(Value.Unsigned)); return true;
			case K::U64: Writer.Write(Value.Unsigned); return true;
			case K::F32: Writer.Write(uint32(Value.FloatingBits)); return true;
			case K::F64: Writer.Write(Value.FloatingBits); return true;
			case K::String: case K::Name: Writer.WriteString(Value.Text); return true;
			case K::Guid:
				Writer.Write(Value.Guid.A); Writer.Write(Value.Guid.B); Writer.Write(Value.Guid.C); Writer.Write(Value.Guid.D); return true;
			case K::Enum:
			{
				const auto Storage = static_cast<K>(Type.Parameter);
				const uint64 Bits = Storage >= K::I8 && Storage <= K::I64
					? static_cast<uint64>(Value.Signed) : Value.Unsigned;
				switch (TypeWidth(Storage))
				{
				case 1: WriteInteger<uint8>(Writer, Bits); return true;
				case 2: WriteInteger<uint16>(Writer, Bits); return true;
				case 4: WriteInteger<uint32>(Writer, Bits); return true;
				case 8: Writer.Write(Bits); return true;
				default: return Fail(Diagnostic, EAssetError::CorruptFile, "Enum storage width is invalid.", 0, std::move(Path));
				}
			}
			case K::Intrinsic:
				return EncodeIntrinsicLoadValue(Type.Parameter, Value.ComponentBits, Writer, Diagnostic);
			case K::Struct:
			{
				const auto* Schema = FindSchema(Linker, Type.QualifiedName);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size()
					|| Type.Children.size() != Value.Elements.size())
					return Fail(Diagnostic, EAssetError::CorruptFile, "Struct load projection is invalid.", 0, std::move(Path));
				Writer.WriteString(Type.QualifiedName); Writer.Write(uint64(Value.Elements.size()));
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto It = std::ranges::find(Schema->Fields, Value.FieldNames[Index],
						&ObjectPackage::FSerializedField::Name);
					if (It == Schema->Fields.end()) return Fail(Diagnostic, EAssetError::CorruptFile, "Struct field is absent from its schema.", 0, std::move(Path));
					const auto& ChildType = Type.Children[Index];
					FByteWriter Payload;
					if (!EncodeLoadArchiveValue(ChildType, Value.Elements[Index], Linker, Payload,
						BulkFieldIndex, Diagnostic,
						std::format("{}::{}", Schema->QualifiedName, It->Name))) return false;
					Writer.WriteString(Schema->QualifiedName); Writer.WriteString(It->Name);
					Writer.Write(uint8(TypeKind(ChildType))); Writer.WriteString(TypeSignature(ChildType));
					Writer.Write(uint64(Payload.Bytes.size())); Writer.WriteBytes(Payload.Bytes);
				}
				return true;
			}
			case K::FixedArray: case K::Array:
			{
				if (Type.Children.size() != 1) return Fail(Diagnostic, EAssetError::CorruptFile, "Array element type is invalid.", 0, std::move(Path));
				if (Type.Kind == K::Array) Writer.Write(uint64(Value.Elements.size()));
				for (const auto& Item : Value.Elements)
					if (!EncodeLoadArchiveValue(Type.Children[0], Item, Linker, Writer,
						BulkFieldIndex, Diagnostic, Path)) return false;
				return true;
			}
			case K::Map:
			{
				if (Type.Children.size() != 2 || Value.Elements.size() % 2 != 0)
					return Fail(Diagnostic, EAssetError::CorruptFile, "Map projection is invalid.", 0, std::move(Path));
				Writer.Write(uint64(Value.Elements.size() / 2));
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
					if (!EncodeLoadArchiveValue(Type.Children[0], Value.Elements[Index], Linker, Writer,
						BulkFieldIndex, Diagnostic, Path)
						|| !EncodeLoadArchiveValue(Type.Children[1], Value.Elements[Index + 1], Linker, Writer,
							BulkFieldIndex, Diagnostic, Path)) return false;
				return true;
			}
			case K::HardReference:
				if (Value.Reference.IsNull()) Writer.Write(uint8{0});
				else if (Value.Reference.IsExport())
				{
					Writer.Write(uint8{1}); Writer.Write(uint64(Value.Reference.GetTableIndex() + 1));
				}
				else
				{
					const ObjectPackage::FPackageImport* Import = nullptr;
					if (!Linker.TryGetImport(Value.Reference, Import) || !Import)
						return Fail(Diagnostic, EAssetError::CorruptFile, "Hard-reference import is invalid.", 0, std::move(Path));
					Writer.Write(uint8{2}); Writer.WriteString(Import->ObjectPath.ToString());
				}
				return true;
			case K::SoftReference:
				Writer.Write(Value.Text.empty() ? uint8{0} : uint8{1});
				if (!Value.Text.empty()) Writer.WriteString(Value.Text);
				return true;
			case K::Bytes:
				Writer.WriteBytes(Value.Bytes); return true;
			case K::BulkData:
				Writer.WriteBytes(MakeBulkDescriptor(Value, ++BulkFieldIndex)); return true;
			}
			return Fail(Diagnostic, EAssetError::CorruptFile, "Unsupported load value.", 0, std::move(Path));
		}

		auto ShouldFail(const FLinkerLoadOptions& Options, ELinkerLoadPhase Phase, uint64 Index) -> bool
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

		auto FindLinkerDeprecatedRoute(const ObjectPackage::FLinkerTables& Linker,
			const ObjectPackage::FSerializedSchema& Schema,
			const ObjectPackage::FSerializedField& Field,
			const ObjectPackage::FSerializedType& Type) -> FProperty*;

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

		auto RestoreNestedLedger(const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Value,
			const ObjectPackage::FLinkerTables& Linker, FAuthoredOverridePath& Path,
			std::vector<FAuthoredOverrideEntry>& Entries,
			bool& bUsedDeprecatedRoute,
			FLinkerApplyDiagnostic& Diagnostic) -> bool
		{
			using K = ObjectPackage::EValueKind;
			if (Type.Kind == K::Struct)
			{
				const auto* Schema = FindSchema(Linker, Type.QualifiedName);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size()
					|| Value.Provenances.size() != Value.Elements.size()
					|| Type.Children.size() != Value.Elements.size())
					return Fail(Diagnostic, EAssetError::CorruptFile, "Struct ledger projection is invalid.");
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto It = std::ranges::find(Schema->Fields, Value.FieldNames[Index],
						&ObjectPackage::FSerializedField::Name);
					if (It == Schema->Fields.end()) return Fail(Diagnostic, EAssetError::CorruptFile, "Struct ledger field is missing.");
					const auto& ChildType = Type.Children[Index];
					const auto Provenance = Value.Provenances[Index] == ObjectPackage::EPropertyProvenance::Forced
						? EAuthoredOverrideProvenance::Forced : EAuthoredOverrideProvenance::LoadedExplicit;
					if (FProperty* Route = FindLinkerDeprecatedRoute(Linker, *Schema, *It, ChildType))
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
					if (!RestoreNestedLedger(ChildType, Value.Elements[Index], Linker, Path,
						Entries, bUsedDeprecatedRoute, Diagnostic)) return false;
					Path.pop_back();
				}
			}
			else if ((Type.Kind == K::FixedArray || Type.Kind == K::Array)
				&& Type.Children.size() == 1)
			{
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					Path.push_back(Type.Kind == K::FixedArray
						? FAuthoredOverridePathToken::FixedArrayElement(Index)
						: FAuthoredOverridePathToken::ArrayElement(Index));
					if (!RestoreNestedLedger(Type.Children[0], Value.Elements[Index], Linker, Path,
						Entries, bUsedDeprecatedRoute, Diagnostic)) return false;
					Path.pop_back();
				}
			}
			else if (Type.Kind == K::Map && Type.Children.size() == 2)
			{
				if (Value.Elements.size() % 2 != 0) return false;
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
				{
					FByteArray Token;
					std::string Error;
					if (!ObjectPackage::BuildCanonicalMapKeyToken(
						Type.Children[0], Value.Elements[Index], Token, &Error))
						return Fail(Diagnostic, EAssetError::CorruptFile, Error);
					Path.push_back(FAuthoredOverridePathToken::MapValue(std::move(Token)));
					if (!RestoreNestedLedger(Type.Children[1], Value.Elements[Index + 1], Linker, Path,
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

		auto CanonicalizeSerializedTypeName(ObjectPackage::FSerializedType& Type) -> void
		{
			for (auto& Child : Type.Children) CanonicalizeSerializedTypeName(Child);
			if (Type.QualifiedName.empty()) return;
			if (Type.Kind == ObjectPackage::EValueKind::Enum)
			{
				if (DEnum* Enum = FindEnumBySerializedName(FName(Type.QualifiedName)))
					Type.QualifiedName = Enum->GetQualifiedName().ToString();
			}
			else if (Type.Kind == ObjectPackage::EValueKind::Struct)
			{
				if (DStruct* Struct = FindStructBySerializedName(FName(Type.QualifiedName)))
					Type.QualifiedName = Struct->GetQualifiedName().ToString();
			}
			else if (Type.Kind == ObjectPackage::EValueKind::HardReference
				|| Type.Kind == ObjectPackage::EValueKind::SoftReference)
			{
				CanonicalizeSerializedClassName(Type.QualifiedName);
			}
		}

		auto GatherCanonicalizationEvidence(
			const ObjectPackage::FLinkerTables& Linker,
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

			for (size_t Index = 0; Index < Linker.Exports.size(); ++Index)
				AddClass(Linker.Exports[Index].ClassName, EAssetSerializedIdentityLocation::ObjectRecord,
					std::format("objects[{}].class", Index));
			for (size_t Index = 0; Index < Linker.Schemas.size(); ++Index)
			{
				const std::string& Stored = Linker.Schemas[Index].QualifiedName;
				const size_t Before = Result.size();
				AddClass(Stored, EAssetSerializedIdentityLocation::Schema,
					std::format("schemas[{}].identity", Index));
				if (Result.size() == Before)
					AddStruct(Stored, EAssetSerializedIdentityLocation::Schema,
						std::format("schemas[{}].identity", Index));
				DStructBase* Owner = FindClassBySerializedName(FName(Stored));
				if (!Owner) Owner = FindStructBySerializedName(FName(Stored));
				if (Owner)
					for (size_t FieldIndex = 0; FieldIndex < Linker.Schemas[Index].Fields.size(); ++FieldIndex)
					{
						const std::string& FieldName = Linker.Schemas[Index].Fields[FieldIndex].Name;
						if (FProperty* Property = Owner->FindPropertyBySerializedName(FName(FieldName), false);
							Property && Property->NamePrivate.ToString() != FieldName)
							Result.push_back({PackagePath, FieldName, Property->NamePrivate.ToString(),
								EAssetReflectedIdentityKind::Property,
								EAssetSerializedIdentityLocation::Schema,
								std::format("schemas[{}].fields[{}].name", Index, FieldIndex)});
					}
			}
			for (size_t Index = 0; Index < Linker.Types.size(); ++Index)
			{
				const auto& Type = Linker.Types[Index];
				const std::string Path = std::format("types[{}].identity", Index);
				switch (Type.Kind)
				{
				case ObjectPackage::EValueKind::Enum: AddEnum(Type.QualifiedName, EAssetSerializedIdentityLocation::TypeDescriptor, Path); break;
				case ObjectPackage::EValueKind::Struct: AddStruct(Type.QualifiedName, EAssetSerializedIdentityLocation::TypeDescriptor, Path); break;
				case ObjectPackage::EValueKind::HardReference:
				case ObjectPackage::EValueKind::SoftReference: AddClass(Type.QualifiedName, EAssetSerializedIdentityLocation::TypeDescriptor, Path); break;
				default: break;
				}
			}
			std::ranges::sort(Result, [](const auto& Left, const auto& Right) {
				return std::tie(Left.Location, Left.LogicalPath, Left.Kind, Left.StoredIdentity, Left.CurrentIdentity)
					< std::tie(Right.Location, Right.LogicalPath, Right.Kind, Right.StoredIdentity, Right.CurrentIdentity);
			});
			return Result;
		}

		// Converts recognized reflection aliases on the Engine-owned linker copy.
		auto CanonicalizeSerializedReflectionNames(
			ObjectPackage::FLinkerTables& Linker,
			std::string* OutError = nullptr) -> bool
		{
			for (auto& Export : Linker.Exports)
				CanonicalizeSerializedClassName(Export.ClassName);
			for (auto& Asset : Linker.Summary.TopLevelAssets)
				CanonicalizeSerializedClassName(Asset.ClassName);
			for (auto& Import : Linker.Imports)
				CanonicalizeSerializedClassName(Import.ClassName);
			for (auto& Schema : Linker.Schemas)
			{
				CanonicalizeSerializedSchemaName(Schema.QualifiedName);
				std::unordered_set<std::string> CurrentFieldNames;
				for (auto& Field : Schema.Fields)
				{
					CanonicalizeSerializedPropertyName(Schema.QualifiedName, Field.Name);
					CanonicalizeSerializedTypeName(Field.Type);
					if (!CurrentFieldNames.emplace(Field.Name).second)
					{
						if (OutError) *OutError = std::format(
							"Serialized schema {} contains fields that canonicalize to duplicate name {}.",
							Schema.QualifiedName, Field.Name);
						return false;
					}
				}
			}
			for (auto& Type : Linker.Types)
				CanonicalizeSerializedTypeName(Type);
			for (auto& Export : Linker.Exports)
				for (auto& Property : Export.Properties)
				{
					CanonicalizeSerializedSchemaName(Property.DeclaringType);
					CanonicalizeSerializedPropertyName(Property.DeclaringType, Property.FieldName);
					CanonicalizeSerializedTypeName(Property.Type);
				}
			return true;
		}

		auto LinkerCustomVersion(const ObjectPackage::FLinkerTables& Linker,
			const FGuid& Guid) -> int32
		{
			const auto It = std::ranges::find(Linker.CustomVersions, Guid,
				&ObjectPackage::FCustomVersion::Guid);
			return It == Linker.CustomVersions.end() ? -1 : static_cast<int32>(It->Value);
		}

		auto FindLinkerDeprecatedRoute(const ObjectPackage::FLinkerTables& Linker,
			const ObjectPackage::FSerializedSchema& Schema,
			const ObjectPackage::FSerializedField& Field,
			const ObjectPackage::FSerializedType& Type) -> FProperty*
		{
			DStructBase* Owner = FindClassByQualifiedName(FName(Schema.QualifiedName));
			if (!Owner) Owner = FindStructByQualifiedName(FName(Schema.QualifiedName));
			if (!Owner) return nullptr;
			const DurinCodeGen::EPropertyGenFlags Kind = TypeKind(Type);
			const std::string Signature = TypeSignature(Type);
			FProperty* Match = nullptr;
			bool bAmbiguous = false;
			Owner->ForEachProperty([&](FProperty* Property) {
				if (bAmbiguous || !Property) return;
				const FPropertyDeprecation* Deprecation = Property->GetDeprecation();
				if (!Deprecation || Deprecation->HistoricalName.ToString() != Field.Name
					|| LinkerCustomVersion(Linker, Deprecation->CustomVersionGuid)
						>= Deprecation->DeprecatedBefore
					|| Property->GetKind() != Kind
					|| Private::GetSerializedTypeSignature(Property) != Signature) return;
				if (Match) bAmbiguous = true;
				else Match = Property;
			}, false);
			return bAmbiguous ? nullptr : Match;
		}

		auto GatherNestedDeprecatedRouteEvidence(
			const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Value,
			const ObjectPackage::FLinkerTables& Linker,
			std::span<const std::pair<FGuid, int32>> Versions, const FPackagePath& PackagePath,
			std::string_view ObjectPath, std::vector<FAssetDeprecatedRouteEvidence>& Out) -> void
		{
			using K = ObjectPackage::EValueKind;
			if (Type.Kind == K::Struct)
			{
				const auto* Schema = FindSchema(Linker, Type.QualifiedName);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size()
					|| Type.Children.size() != Value.Elements.size()) return;
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto Field = std::ranges::find(
						Schema->Fields, Value.FieldNames[Index], &ObjectPackage::FSerializedField::Name);
					if (Field == Schema->Fields.end()) continue;
					const auto& ChildType = Type.Children[Index];
					FProperty* LiveRoute =
						FindLinkerDeprecatedRoute(Linker, *Schema, *Field, ChildType);
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
					else GatherNestedDeprecatedRouteEvidence(ChildType, Value.Elements[Index],
						Linker, Versions, PackagePath, ObjectPath, Out);
				}
			}
			else if ((Type.Kind == K::FixedArray || Type.Kind == K::Array)
				&& Type.Children.size() == 1)
			{
				for (const auto& Element : Value.Elements)
					GatherNestedDeprecatedRouteEvidence(Type.Children[0], Element, Linker,
						Versions, PackagePath, ObjectPath, Out);
			}
			else if (Type.Kind == K::Map && Type.Children.size() == 2)
			{
				for (size_t Index = 1; Index < Value.Elements.size(); Index += 2)
					GatherNestedDeprecatedRouteEvidence(Type.Children[1], Value.Elements[Index], Linker,
						Versions, PackagePath, ObjectPath, Out);
			}
		}

		struct FExportView
		{
			ObjectPackage::FPackageExport* Export = nullptr;
			std::string Path;
			uint64 OuterId = 0;
		};

		auto BuildExportViews(ObjectPackage::FLinkerTables& Linker,
			std::vector<FExportView>& Out, FLinkerApplyDiagnostic& Diagnostic) -> bool
		{
			std::vector<FExportView> Views;
			Views.reserve(Linker.Exports.size());
			for (size_t Index = 0; Index < Linker.Exports.size(); ++Index)
			{
				ObjectPackage::FPackageIndex PackageIndex;
				std::string Path;
				if (!ObjectPackage::FPackageIndex::TryExport(Index, PackageIndex)
					|| !Linker.TryResolvePath(PackageIndex, Path))
					return Fail(Diagnostic, EAssetError::InvalidObjectGraph,
						"An export path cannot be resolved.");
				auto& Export = Linker.Exports[Index];
				uint64 OuterId = 0;
				if (!Export.Outer.IsNull())
				{
					if (!Export.Outer.IsExport() || Export.Outer.GetTableIndex() >= Index)
						return Fail(Diagnostic, EAssetError::InvalidObjectGraph,
							"Export Outer topology is not constructible in table order.", 0, Path);
					OuterId = static_cast<uint64>(Export.Outer.GetTableIndex() + 1);
				}
				Views.push_back({&Export, std::move(Path), OuterId});
			}
			Out = std::move(Views);
			return true;
		}
	}

	auto ApplyLivePackageLinker(ObjectPackage::FLinkerTables Linker,
		const FPackagePath& PackagePath, DPackage*& OutPackage,
		FAssetLoadReport* OutReport, const FLinkerLoadOptions& Options,
		std::string* OutError) -> FAssetResult
	{
		OutPackage = nullptr;
		FLinkerApplyDiagnostic Diagnostic;
		auto Finish = [&](FAssetResult Result) {
			if (OutError) *OutError = Result ? std::string{} : Diagnostic.Message;
			return Result;
		};
		if (!PackagePath.IsValid())
		{
			Fail(Diagnostic, EAssetError::InvalidPath, "Live linker application requires a validated package path.");
			return Finish({EAssetError::InvalidPath, Diagnostic.Message});
		}
		if (Linker.Summary.PackagePath != PackagePath)
		{
			Fail(Diagnostic, EAssetError::InvalidPath,
				"Linker package identity does not match the requested package path.");
			return Finish({EAssetError::InvalidPath, Diagnostic.Message});
		}
		if (FindPackage(PackagePath.GetView()))
		{
			Fail(Diagnostic, EAssetError::AlreadyExists,
				"A package with the requested path is already live.");
			return Finish({EAssetError::AlreadyExists, Diagnostic.Message});
		}
		std::vector<FAssetCanonicalizationEvidence> CanonicalizationEvidence =
			GatherCanonicalizationEvidence(Linker, PackagePath);
		std::string CanonicalizationError;
		if (!CanonicalizeSerializedReflectionNames(Linker, &CanonicalizationError))
		{
			Fail(Diagnostic, EAssetError::CorruptFile, CanonicalizationError);
			return Finish({EAssetError::CorruptFile, Diagnostic.Message});
		}
		std::vector<FExportView> Exports;
		if (!BuildExportViews(Linker, Exports, Diagnostic))
			return Finish({Diagnostic.Error, Diagnostic.Message});
		for (const FExportView& Object : Exports)
		{
			DClass* Class = FindClassByQualifiedName(FName(Object.Export->ClassName));
			if (!Class)
			{
				Fail(Diagnostic, EAssetError::UnknownClass,
					"Serialized class is unavailable.", 0, Object.Path);
				return Finish({EAssetError::UnknownClass, Diagnostic.Message});
			}
			for (const ObjectPackage::FPropertyTag& Property : Object.Export->Properties)
			{
				const auto* Schema = FindSchema(Linker, Property.DeclaringType);
				const auto Field = Schema ? std::ranges::find(Schema->Fields,
					Property.FieldName, &ObjectPackage::FSerializedField::Name)
					: std::vector<ObjectPackage::FSerializedField>::const_iterator{};
				if (!Schema || Field == Schema->Fields.end() || Field->Type != Property.Type)
				{
					Fail(Diagnostic, EAssetError::CorruptFile,
						"A linker property is absent from its declared schema.", 0, Object.Path);
					return Finish({EAssetError::CorruptFile, Diagnostic.Message});
				}
				DClass* DeclaringClass = FindClassByQualifiedName(FName(Schema->QualifiedName));
				bool bDeclaringClassMatches = false;
				for (DClass* Ancestor = Class; Ancestor; Ancestor = Ancestor->GetSuperClass())
					if (Ancestor == DeclaringClass)
					{
						bDeclaringClassMatches = true;
						break;
					}
				FProperty* Expected = bDeclaringClassMatches
					? DeclaringClass->FindPropertyByName(FName(Field->Name), false) : nullptr;
				const bool bCurrentCompatible = Expected
					&& !Expected->GetDeprecation()
					&& Expected->GetKind() == TypeKind(Property.Type)
					&& GetSerializedTypeSignature(Expected) == TypeSignature(Property.Type);
				const bool bDeprecatedCompatible =
					FindLinkerDeprecatedRoute(Linker, *Schema, *Field, Property.Type);
				// Cooked native projection fields are validated against the exact
				// SerializeCooked manifest and must be consumed by the load Archive.
				const bool bCookedNativeCandidate = Options.bCooked && !Expected;
				const bool bCompatible = bCurrentCompatible || bDeprecatedCompatible
					|| bCookedNativeCandidate;
				if (!bCompatible)
				{
					Fail(Diagnostic, EAssetError::UnsupportedProperty,
						std::format("Serialized field {}::{} is incompatible with the live schema.",
							Schema->QualifiedName, Field->Name), 0, Object.Path);
					return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
				}
			}
		}
		if (Exports.empty())
		{
			Fail(Diagnostic, EAssetError::InvalidObjectGraph, "Package has no object exports.");
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}

		DPackage* Package = NewObject<DPackage>(
			nullptr,
			FName(PackagePath.GetAssetName()),
			EObjectFlags::Standalone);
		if (!Package)
		{
			Fail(Diagnostic, EAssetError::InvalidObjectGraph, "Could not allocate the package skeleton.");
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}
		Package->InitializeAssetPackage(PackagePath);
		std::vector<DObject*> Objects(Exports.size(), nullptr);
		const FAssetPackageLoadSnapshot DependencySnapshot = CapturePackageLoadSnapshot();
		bool bSkeletonPublished = false;
		auto Rollback = [&]() {
			if (bSkeletonPublished && Options.OnSkeletonRollback)
				Options.OnSkeletonRollback(Package);
			MarkObjectHierarchyAsGarbage(Package); CollectGarbage();
			ReleasePackagesLoadedSince(DependencySnapshot);
		};

		for (size_t Index = 0; Index < Exports.size(); ++Index)
		{
			if (ShouldFail(Options, ELinkerLoadPhase::CreateSkeleton, Index))
			{
				Fail(Diagnostic, EAssetError::InvalidObjectGraph, "Injected skeleton creation failure."); Rollback();
				return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
			}
			const FExportView& Descriptor = Exports[Index];
			DClass* Class = FindClassByQualifiedName(FName(Descriptor.Export->ClassName));
			if (!Class || !Class->ClassConstructor)
			{
				Fail(Diagnostic, EAssetError::UnknownClass, "Serialized class is unavailable.", 0, Descriptor.Path); Rollback();
				return Finish({EAssetError::UnknownClass, Diagnostic.Message});
			}
			DObject* Outer = Descriptor.OuterId == 0 ? static_cast<DObject*>(Package)
				: Objects[static_cast<size_t>(Descriptor.OuterId - 1)];
			bool bTypeMismatch = false;
			DObject* Object = FindExistingInner(Outer, Descriptor.Export->ObjectName, Class, bTypeMismatch);
			if (bTypeMismatch)
			{
				Fail(Diagnostic, EAssetError::TypeMismatch, "Existing default inner has a different class.", 0, Descriptor.Path); Rollback();
				return Finish({EAssetError::TypeMismatch, Diagnostic.Message});
			}
			if (!Object)
			{
				FStaticConstructObjectParameters Parameters{
					Class, Outer, FName(Descriptor.Export->ObjectName), Class->PropertiesSize,
					Descriptor.OuterId == 0 ? EObjectFlags::Public : EObjectFlags::NoFlags};
				Parameters.Purpose = EObjectConstructionPurpose::AssetLoad;
				Object = StaticConstructObject(Parameters);
				if (Object) DObjectForceRegistration(Object);
			}
			if (!Object)
			{
				Fail(Diagnostic, EAssetError::InvalidObjectGraph, "Object construction failed.", 0, Descriptor.Path); Rollback();
				return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
			}
			Objects[Index] = Object;
		}
		if (Options.OnSkeletonReady)
		{
			FAssetResult PublishResult = Options.OnSkeletonReady(Package);
			if (!PublishResult)
			{
				Fail(Diagnostic, EAssetError::InvalidObjectGraph,
					PublishResult.Message.empty() ? "Could not publish the package skeleton."
						: PublishResult.Message);
				Rollback();
				return Finish(PublishResult);
			}
			bSkeletonPublished = true;
		}

		for (size_t Index = 0; Index < Linker.Summary.HardPackageDependencies.size(); ++Index)
		{
			if (ShouldFail(Options, ELinkerLoadPhase::ResolveDependency, Index))
			{
				Fail(Diagnostic, EAssetError::MissingDependency, "Injected dependency failure."); Rollback();
				return Finish({EAssetError::MissingDependency, Diagnostic.Message});
			}
			const FPackagePath& Path = Linker.Summary.HardPackageDependencies[Index];
			DPackage* Dependency = nullptr; FAssetResult Result = LoadPackage(Path, Dependency);
			if (!Result)
			{
				Fail(Diagnostic, EAssetError::MissingDependency, Result.Message); Rollback();
				return Finish({EAssetError::MissingDependency, Diagnostic.Message});
			}
		}

		std::vector<FArchiveCustomVersion> CustomVersions;
		std::vector<std::pair<FGuid, int32>> LoadedCustomVersions;
		for (const ObjectPackage::FCustomVersion& Version : Linker.CustomVersions)
		{
			CustomVersions.push_back({Version.Guid, static_cast<int32>(Version.Value)});
			LoadedCustomVersions.emplace_back(Version.Guid, static_cast<int32>(Version.Value));
		}
		for (DObject* Object : Objects) Object->SetLoadedCustomVersions(LoadedCustomVersions);
		FAssetLoadReport Report = OutReport ? *OutReport : FAssetLoadReport{};
		Report.PackagePath = PackagePath;
		Report.CanonicalizationEvidence = std::move(CanonicalizationEvidence);
		uint64 BulkFieldIndex = 0;
		for (size_t ObjectIndex = 0; ObjectIndex < Exports.size(); ++ObjectIndex)
			for (const auto& Property : Exports[ObjectIndex].Export->Properties)
				GatherNestedDeprecatedRouteEvidence(Property.Type, Property.Value, Linker,
					LoadedCustomVersions, PackagePath, Exports[ObjectIndex].Path,
					Report.DeprecatedRouteEvidence);
		for (size_t ObjectIndex = 0; ObjectIndex < Objects.size(); ++ObjectIndex)
		{
			if (ShouldFail(Options, ELinkerLoadPhase::ApplyValues, ObjectIndex))
			{
				Fail(Diagnostic, EAssetError::CorruptFile, "Injected value application failure."); Rollback();
				return Finish({EAssetError::CorruptFile, Diagnostic.Message});
			}
			std::vector<FAuthoredPackageFieldRecord> Fields;
			std::vector<const ObjectPackage::FPropertyTag*> KnownProperties;
			for (const auto& Property : Exports[ObjectIndex].Export->Properties)
			{
				FByteWriter Payload;
				if (!EncodeLoadArchiveValue(Property.Type, Property.Value, Linker, Payload,
					BulkFieldIndex, Diagnostic,
					std::format("{}::{}", Property.DeclaringType, Property.FieldName)))
				{
					Rollback(); return Finish({EAssetError::CorruptFile, Diagnostic.Message});
				}
				Fields.push_back({Property.DeclaringType, Property.FieldName, TypeKind(Property.Type),
					TypeSignature(Property.Type), std::move(Payload.Bytes)});
				KnownProperties.push_back(&Property);
			}
			FArchiveState LoadContext;
			LoadContext.bCooking = Options.bCooked;
			LoadContext.bFilterEditorOnly = Options.bCooked;
			LoadContext.Target = Options.Target;
			FAssetResult Result = LoadAuthoredObject(*Objects[ObjectIndex], Fields, Objects,
				PackagePath, Options.SourceFormatVersion, CustomVersions, LoadContext);
			if (!Result)
			{
				Fail(Diagnostic, EAssetError::UnsupportedProperty, Result.Message, 0, Exports[ObjectIndex].Path); Rollback();
				return Finish(Result);
			}
			if (Options.bCooked) continue;
			std::vector<FAuthoredOverrideEntry> LedgerEntries;
			bool bUsedDeprecatedRoute = false;
			for (size_t PropertyIndex = 0; PropertyIndex < KnownProperties.size(); ++PropertyIndex)
			{
				const auto& Property = *KnownProperties[PropertyIndex];
				const auto* Schema = FindSchema(Linker, Property.DeclaringType);
				const auto Field = Schema ? std::ranges::find(Schema->Fields,
					Property.FieldName, &ObjectPackage::FSerializedField::Name)
					: std::vector<ObjectPackage::FSerializedField>::const_iterator{};
				if (!Schema || Field == Schema->Fields.end())
				{
					Fail(Diagnostic, EAssetError::CorruptFile, "A linker property lost its schema binding."); Rollback();
					return Finish({EAssetError::CorruptFile, Diagnostic.Message});
				}
				if (ShouldFail(Options, ELinkerLoadPhase::RestoreLedger, PropertyIndex + 1))
				{
					Fail(Diagnostic, EAssetError::CorruptFile, "Injected ledger restoration failure."); Rollback();
					return Finish({EAssetError::CorruptFile, Diagnostic.Message});
				}
				const auto Provenance = Property.Provenance == ObjectPackage::EPropertyProvenance::Forced
					? EAuthoredOverrideProvenance::Forced
					: EAuthoredOverrideProvenance::LoadedExplicit;
				FProperty* DeprecatedRoute =
					FindLinkerDeprecatedRoute(Linker, *Schema, *Field, Property.Type);
				if (DeprecatedRoute)
				{
					bUsedDeprecatedRoute = true;
					const FPropertyDeprecation* Deprecation = DeprecatedRoute->GetDeprecation();
					FAssetDeprecatedRouteEvidence Evidence{
						.PackagePath = PackagePath,
						.ObjectPath = Exports[ObjectIndex].Path,
						.DeclaringType = Schema->QualifiedName,
						.StoredFieldName = Field->Name,
						.DeprecatedPropertyName = DeprecatedRoute->NamePrivate.ToString(),
						.CustomVersionGuid = Deprecation->CustomVersionGuid,
						.SourceVersion = LinkerCustomVersion(Linker, Deprecation->CustomVersionGuid),
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
					FName(Schema->QualifiedName), FName(Field->Name))};
				LedgerEntries.push_back({Path, Provenance});
				if (!RestoreNestedLedger(Property.Type, Property.Value, Linker, Path,
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
				Fail(Diagnostic, EAssetError::CorruptFile,
					"Could not restore authored intent.", 0, LedgerDiagnostic.LogicalPath); Rollback();
				return Finish({EAssetError::CorruptFile, Diagnostic.Message});
			}
		}

		Package->ClearDirty();
		for (size_t Reverse = Objects.size(); Reverse > 0; --Reverse)
		{
			const size_t Index = Reverse - 1;
			if (ShouldFail(Options, ELinkerLoadPhase::PostLoad, Index))
			{
				Fail(Diagnostic, EAssetError::InvalidObjectGraph, "Injected PostLoad failure."); Rollback();
				return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
			}
			std::string Error;
			if (!Objects[Index]->PostLoad(Error))
			{
				Fail(Diagnostic, EAssetError::InvalidObjectGraph,
					Error.empty() ? "Object PostLoad failed." : Error, 0, Exports[Index].Path); Rollback();
				return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
			}
			Objects[Index]->ClearLoadedCustomVersions();
		}
		if (ShouldFail(Options, ELinkerLoadPhase::Publish, 0))
		{
			Fail(Diagnostic, EAssetError::InvalidObjectGraph, "Injected graph publication failure."); Rollback();
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}
		OutPackage = Package;
		Package->SetCanonicalResaveRecommended(!Report.CanonicalizationEvidence.empty()
			|| !Report.DeprecatedRouteEvidence.empty());
		if (OutReport) *OutReport = std::move(Report);
		Diagnostic.Reset(); return Finish({});
	}

}
