#include "AssetPackageLinker.h"
#include "AssetLiveLoadGuard.h"

#include "AssetPackageArchive.h"
#include "AssetPackageValueCodec.h"
#include "Asset/Load.h"
#include "AssetRuntimeStateInternal.h"
#include "AssetRegistry/Publication.h"

#include "DObject/Class.h"
#include "DObject/Archive.h"
#include "DObject/CanonicalMapKey.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Threading/RunnableThread.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::AssetPrivate
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

		auto LinkerApplyFail(FLinkerApplyDiagnostic& Diagnostic, EAssetError Error,
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

		// Only an absent field on a known declaring type is disposable. A current
		// property or any explicit historical route still owns type compatibility.
		auto IsRemovedField(std::string_view DeclaringType, std::string_view Name) -> bool
		{
			DStructBase* Owner = FindClassByQualifiedName(FName(DeclaringType));
			if (!Owner) Owner = FindStructByQualifiedName(FName(DeclaringType));
			if (!Owner || Owner->FindPropertyByName(FName(Name), false)) return false;
			bool bHistoricalName = false;
			Owner->ForEachProperty([&](FProperty* Property) {
				if (const auto* Route = Property->GetDeprecation())
					bHistoricalName |= Route->HistoricalName.ToString() == Name;
			}, false);
			return !bHistoricalName;
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
		auto WriteInteger(AssetPrivate::FByteWriter& Writer, uint64 Value) -> void
		{
			Writer.Write(static_cast<T>(Value));
		}

		auto WriteProjectedField(AssetPrivate::FByteWriter& Writer, std::string_view Owner,
			std::string_view Name, DurinCodeGen::EPropertyGenFlags Kind,
			std::string Signature, FByteBuffer Payload) -> void
		{
			Writer.WriteString(Owner); Writer.WriteString(Name); Writer.Write(uint8(Kind));
			Writer.WriteString(Signature); Writer.Write(uint64(Payload.size())); Writer.WriteBytes(Payload);
		}

		auto EncodeIntrinsicLoadValue(uint64 Layout, std::span<const uint64> Components,
			FByteWriter& Writer, FLinkerApplyDiagnostic& Diagnostic) -> bool
		{
			const std::string Owner(IntrinsicName(Layout));
			if (Owner.empty()) return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Intrinsic layout is invalid.");
			Writer.WriteString(Owner);
			if (Layout == 5)
			{
				if (Components.size() != 10) return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Transform component count is invalid.");
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
			if (Components.size() != Count) return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Intrinsic component count is invalid.");
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
			uint64 FieldIndex) -> FByteBuffer
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
			std::string Path, bool bDiscardRemovedFields) -> bool
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
				default: return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Enum storage width is invalid.", 0, std::move(Path));
				}
			}
			case K::Intrinsic:
				return EncodeIntrinsicLoadValue(Type.Parameter, Value.ComponentBits, Writer, Diagnostic);
			case K::Struct:
			{
				const auto* Schema = FindSchema(Linker, Type.QualifiedName);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size()
					|| Type.Children.size() != Value.Elements.size())
					return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Struct load projection is invalid.", 0, std::move(Path));
				uint64 FieldCount = 0;
				for (const auto& Name : Value.FieldNames)
					if (!bDiscardRemovedFields || !IsRemovedField(Schema->QualifiedName, Name)) ++FieldCount;
				Writer.WriteString(Type.QualifiedName); Writer.Write(FieldCount);
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto It = std::ranges::find(Schema->Fields, Value.FieldNames[Index],
						&ObjectPackage::FSerializedField::Name);
					if (It == Schema->Fields.end()) return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Struct field is absent from its schema.", 0, std::move(Path));
					if (bDiscardRemovedFields && IsRemovedField(Schema->QualifiedName, It->Name)) continue;
					const auto& ChildType = Type.Children[Index];
					FByteWriter Payload;
					if (!EncodeLoadArchiveValue(ChildType, Value.Elements[Index], Linker, Payload,
						BulkFieldIndex, Diagnostic,
						std::format("{}::{}", Schema->QualifiedName, It->Name), bDiscardRemovedFields)) return false;
					Writer.WriteString(Schema->QualifiedName); Writer.WriteString(It->Name);
					Writer.Write(uint8(TypeKind(ChildType))); Writer.WriteString(TypeSignature(ChildType));
					Writer.Write(uint64(Payload.Bytes.size())); Writer.WriteBytes(Payload.Bytes);
				}
				return true;
			}
			case K::FixedArray: case K::Array:
			{
				if (Type.Children.size() != 1) return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Array element type is invalid.", 0, std::move(Path));
				if (Type.Kind == K::Array) Writer.Write(uint64(Value.Elements.size()));
				for (const auto& Item : Value.Elements)
					if (!EncodeLoadArchiveValue(Type.Children[0], Item, Linker, Writer,
						BulkFieldIndex, Diagnostic, Path, bDiscardRemovedFields)) return false;
				return true;
			}
			case K::Map:
			{
				if (Type.Children.size() != 2 || Value.Elements.size() % 2 != 0)
					return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Map projection is invalid.", 0, std::move(Path));
				Writer.Write(uint64(Value.Elements.size() / 2));
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
					if (!EncodeLoadArchiveValue(Type.Children[0], Value.Elements[Index], Linker, Writer,
						BulkFieldIndex, Diagnostic, Path, bDiscardRemovedFields)
						|| !EncodeLoadArchiveValue(Type.Children[1], Value.Elements[Index + 1], Linker, Writer,
							BulkFieldIndex, Diagnostic, Path, bDiscardRemovedFields)) return false;
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
						return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Hard-reference import is invalid.", 0, std::move(Path));
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
			return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Unsupported load value.", 0, std::move(Path));
		}

		auto ShouldFail(const FLinkerLoadOptions& Options, ELinkerLoadPhase Phase, uint64 Index) -> bool
		{
			return Options.ShouldFail && Options.ShouldFail(Phase, Index);
		}

		auto FindExistingInner(DObject* Outer, std::string_view Name, DClass* Class,
			bool& bOutTypeMismatch) -> DObject*
		{
			bOutTypeMismatch = false;
			for (DObject* Object : GDObjectArray.GetObjectsWithOuter(Outer,
				Outer->GetPackage() && Outer->GetPackage()->IsGraphPrivate()
					? EObjectQueryScope::IncludeUnpublished : EObjectQueryScope::LiveOnly))
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

		auto RestoreNestedLedger(const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Value,
			const ObjectPackage::FLinkerTables& Linker, FAuthoredOverridePath& Path,
			std::vector<FAuthoredOverrideEntry>& Entries,
			FLinkerApplyDiagnostic& Diagnostic) -> bool
		{
			using K = ObjectPackage::EValueKind;
			if (Type.Kind == K::Struct)
			{
				const auto* Schema = FindSchema(Linker, Type.QualifiedName);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size()
					|| Value.Provenances.size() != Value.Elements.size()
					|| Type.Children.size() != Value.Elements.size())
					return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Struct ledger projection is invalid.");
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto It = std::ranges::find(Schema->Fields, Value.FieldNames[Index],
						&ObjectPackage::FSerializedField::Name);
					if (It == Schema->Fields.end()) return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Struct ledger field is missing.");
					if (IsRemovedField(Schema->QualifiedName, It->Name)) continue;
					const auto& ChildType = Type.Children[Index];
					const auto Provenance = Value.Provenances[Index] == ObjectPackage::EPropertyProvenance::Forced
						? EAuthoredOverrideProvenance::Forced : EAuthoredOverrideProvenance::LoadedExplicit;
					if (FindLinkerDeprecatedRoute(Linker, *Schema, *It, ChildType)) continue;
					Path.push_back(FAuthoredOverridePathToken::Field(FName(Schema->QualifiedName), FName(It->Name)));
					Entries.push_back({Path, Provenance});
					if (!RestoreNestedLedger(ChildType, Value.Elements[Index], Linker, Path,
						Entries, Diagnostic)) return false;
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
						Entries, Diagnostic)) return false;
					Path.pop_back();
				}
			}
			else if (Type.Kind == K::Map && Type.Children.size() == 2)
			{
				if (Value.Elements.size() % 2 != 0) return false;
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
				{
					FByteBuffer Token;
					std::string Error;
					if (!ObjectPackage::BuildCanonicalMapKeyToken(
						Type.Children[0], Value.Elements[Index], Token, &Error))
						return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, Error);
					Path.push_back(FAuthoredOverridePathToken::MapValue(std::move(Token)));
					if (!RestoreNestedLedger(Type.Children[1], Value.Elements[Index + 1], Linker, Path,
						Entries, Diagnostic)) return false;
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
			if (FProperty* Current = Owner->FindPropertyByName(FName(Field.Name), false);
				Current && !Current->IsDeprecated() && Current->GetKind() == Kind
				&& AssetPrivate::GetSerializedTypeSignature(Current) == Signature) return nullptr;
			FProperty* Match = nullptr;
			bool bAmbiguous = false;
			Owner->ForEachProperty([&](FProperty* Property) {
				if (bAmbiguous || !Property) return;
				const FPropertyDeprecation* Deprecation = Property->GetDeprecation();
				if (!Deprecation || Deprecation->HistoricalName.ToString() != Field.Name
					|| Property->GetKind() != Kind
					|| AssetPrivate::GetSerializedTypeSignature(Property) != Signature) return;
				if (Match) bAmbiguous = true;
				else Match = Property;
			}, false);
			return bAmbiguous ? nullptr : Match;
		}

		auto GatherLiveValueDependencies(const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Value, const ObjectPackage::FLinkerTables& Linker,
			std::vector<FPackagePath>& Dependencies, uint64& DiscardedFields,
			FLinkerApplyDiagnostic& Diagnostic) -> bool
		{
			using K = ObjectPackage::EValueKind;
			if (Type.Kind == K::Struct)
			{
				const auto* Schema = FindSchema(Linker, Type.QualifiedName);
				DStruct* Owner = FindStructByQualifiedName(FName(Type.QualifiedName));
				if (!Schema || !Owner || Value.FieldNames.size() != Value.Elements.size()
					|| Type.Children.size() != Value.Elements.size())
					return LinkerApplyFail(Diagnostic, EAssetError::UnsupportedProperty,
						"Serialized struct is unavailable or has an invalid schema.");
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto Field = std::ranges::find(Schema->Fields, Value.FieldNames[Index],
						&ObjectPackage::FSerializedField::Name);
					if (Field == Schema->Fields.end())
						return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Struct field has no schema.");
					if (IsRemovedField(Schema->QualifiedName, Field->Name)) { ++DiscardedFields; continue; }
					const auto& ChildType = Type.Children[Index];
					FProperty* Expected = Owner->FindPropertyByName(FName(Field->Name), false);
					if (!(Expected && !Expected->IsDeprecated() && Expected->GetKind() == TypeKind(ChildType)
						&& GetSerializedTypeSignature(Expected) == TypeSignature(ChildType))
						&& !FindLinkerDeprecatedRoute(Linker, *Schema, *Field, ChildType))
						return LinkerApplyFail(Diagnostic, EAssetError::UnsupportedProperty,
							std::format("Serialized struct field {}::{} is incompatible with the live schema.",
								Schema->QualifiedName, Field->Name));
					if (!GatherLiveValueDependencies(ChildType, Value.Elements[Index], Linker,
						Dependencies, DiscardedFields, Diagnostic)) return false;
				}
			}
			else if ((Type.Kind == K::Array || Type.Kind == K::FixedArray) && Type.Children.size() == 1)
			{
				for (const auto& Element : Value.Elements)
					if (!GatherLiveValueDependencies(Type.Children[0], Element, Linker,
						Dependencies, DiscardedFields, Diagnostic)) return false;
			}
			else if (Type.Kind == K::Map && Type.Children.size() == 2)
			{
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
					if (!GatherLiveValueDependencies(Type.Children[Index % 2], Value.Elements[Index], Linker,
						Dependencies, DiscardedFields, Diagnostic)) return false;
			}
			else if (Type.Kind == K::HardReference && Value.Reference.IsImport())
			{
				const ObjectPackage::FPackageImport* Import = nullptr;
				if (!Linker.TryGetImport(Value.Reference, Import) || !Import)
					return LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Hard-reference import is invalid.");
				Dependencies.push_back(Import->ObjectPath.GetPackagePath());
			}
			return true;
		}

		auto GatherNestedDeprecatedRouteEvidence(
			const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Value,
			const ObjectPackage::FLinkerTables& Linker,
			const FPackagePath& PackagePath,
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
					if (Field == Schema->Fields.end() || IsRemovedField(Schema->QualifiedName, Field->Name)) continue;
					const auto& ChildType = Type.Children[Index];
					FProperty* LiveRoute =
						FindLinkerDeprecatedRoute(Linker, *Schema, *Field, ChildType);
					if (LiveRoute)
					{
						Out.push_back({
							.PackagePath = PackagePath, .ObjectPath = std::string(ObjectPath),
							.DeclaringType = Schema->QualifiedName, .StoredFieldName = Field->Name,
							.DeprecatedPropertyName = LiveRoute->NamePrivate.ToString()});
					}
					else GatherNestedDeprecatedRouteEvidence(ChildType, Value.Elements[Index],
						Linker, PackagePath, ObjectPath, Out);
				}
			}
			else if ((Type.Kind == K::FixedArray || Type.Kind == K::Array)
				&& Type.Children.size() == 1)
			{
				for (const auto& Element : Value.Elements)
					GatherNestedDeprecatedRouteEvidence(Type.Children[0], Element, Linker,
						PackagePath, ObjectPath, Out);
			}
			else if (Type.Kind == K::Map && Type.Children.size() == 2)
			{
				for (size_t Index = 1; Index < Value.Elements.size(); Index += 2)
					GatherNestedDeprecatedRouteEvidence(Type.Children[1], Value.Elements[Index], Linker,
						PackagePath, ObjectPath, Out);
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
					return LinkerApplyFail(Diagnostic, EAssetError::InvalidObjectGraph,
						"An export path cannot be resolved.");
				auto& Export = Linker.Exports[Index];
				uint64 OuterId = 0;
				if (!Export.Outer.IsNull())
				{
					if (!Export.Outer.IsExport() || Export.Outer.GetTableIndex() >= Index)
						return LinkerApplyFail(Diagnostic, EAssetError::InvalidObjectGraph,
							"Export Outer topology is not constructible in table order.", 0, Path);
					OuterId = static_cast<uint64>(Export.Outer.GetTableIndex() + 1);
				}
				Views.push_back({&Export, std::move(Path), OuterId});
			}
			Out = std::move(Views);
			return true;
		}
		// Shared decoded-package phases; publication and PostLoad belong to callers.
		struct FLinkerApplication
		{
			ObjectPackage::FLinkerTables Linker;
			FPackagePath PackagePath;
			std::vector<FExportView> Exports;
			std::vector<FPackagePath> LiveDependencies;
			std::vector<DObject*> Objects;
			DPackage* Package = nullptr;
			FAssetLoadReport Report;
		};

		auto ValidateLinker(FLinkerApplication& Application, const FLinkerLoadOptions& Options,
			FLinkerApplyDiagnostic& Diagnostic) -> FAssetResult
		{
			const auto& PackagePath = Application.PackagePath;
			auto& Linker = Application.Linker;
			std::vector<FAssetCanonicalizationEvidence> CanonicalizationEvidence =
				GatherCanonicalizationEvidence(Linker, PackagePath);
			std::string CanonicalizationError;
			if (!CanonicalizeSerializedReflectionNames(Linker, &CanonicalizationError))
			{
				LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, CanonicalizationError);
				return {EAssetError::CorruptFile, Diagnostic.Message};
			}
			auto& Exports = Application.Exports;
			if (!BuildExportViews(Linker, Exports, Diagnostic))
				return {Diagnostic.Error, Diagnostic.Message};
			uint64 DiscardedFields = 0;
			auto& LiveDependencies = Application.LiveDependencies;
			for (const FExportView& Object : Exports)
			{
				DClass* Class = FindClassByQualifiedName(FName(Object.Export->ClassName));
				if (!Class)
				{
					LinkerApplyFail(Diagnostic, EAssetError::UnknownClass,
						"Serialized class is unavailable.", 0, Object.Path);
					return {EAssetError::UnknownClass, Diagnostic.Message};
				}
				for (const ObjectPackage::FPropertyTag& Property : Object.Export->Properties)
				{
					const auto* Schema = FindSchema(Linker, Property.DeclaringType);
					const auto Field = Schema ? std::ranges::find(Schema->Fields,
						Property.FieldName, &ObjectPackage::FSerializedField::Name)
						: std::vector<ObjectPackage::FSerializedField>::const_iterator{};
					if (!Schema || Field == Schema->Fields.end() || Field->Type != Property.Type)
					{
						LinkerApplyFail(Diagnostic, EAssetError::CorruptFile,
							"A linker property is absent from its declared schema.", 0, Object.Path);
						return {EAssetError::CorruptFile, Diagnostic.Message};
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
					if (!Options.bCooked && bDeclaringClassMatches
						&& IsRemovedField(Schema->QualifiedName, Field->Name))
					{
						++DiscardedFields;
						continue;
					}
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
						LinkerApplyFail(Diagnostic, EAssetError::UnsupportedProperty,
							std::format("Serialized field {}::{} is incompatible with the live schema.",
								Schema->QualifiedName, Field->Name), 0, Object.Path);
						return {EAssetError::UnsupportedProperty, Diagnostic.Message};
					}
					if (!Options.bCooked && !GatherLiveValueDependencies(Property.Type, Property.Value,
						Linker, LiveDependencies, DiscardedFields, Diagnostic))
						return {Diagnostic.Error, Diagnostic.Message};
				}
			}
			std::ranges::sort(LiveDependencies);
			LiveDependencies.erase(std::ranges::unique(LiveDependencies).begin(), LiveDependencies.end());
			if (Exports.empty())
			{
				LinkerApplyFail(Diagnostic, EAssetError::InvalidObjectGraph, "Package has no object exports.");
				return {EAssetError::InvalidObjectGraph, Diagnostic.Message};
			}

			Application.Report.PackagePath = PackagePath;
			Application.Report.CanonicalizationEvidence = std::move(CanonicalizationEvidence);
			Application.Report.DiscardedFieldCount = DiscardedFields;
			return {};
		}

		auto CreateLinkerSkeleton(FLinkerApplication& Application, const FLinkerLoadOptions& Options,
			FLinkerApplyDiagnostic& Diagnostic, std::vector<FStrongObjectPtr>* Pins = nullptr) -> FAssetResult
		{
			auto& Exports = Application.Exports;
			auto& Objects = Application.Objects;
			auto* Package = Application.Package;
			std::unordered_set<DObject*> Pinned;
			for (size_t Index = 0; Index < Exports.size(); ++Index)
			{
				if (ShouldFail(Options, ELinkerLoadPhase::CreateSkeleton, Index))
				{
					LinkerApplyFail(Diagnostic, EAssetError::InvalidObjectGraph, "Injected skeleton creation failure.");
					return {EAssetError::InvalidObjectGraph, Diagnostic.Message};
				}
				const FExportView& Descriptor = Exports[Index];
				DClass* Class = FindClassByQualifiedName(FName(Descriptor.Export->ClassName));
				if (!Class || !Class->ClassConstructor)
				{
					LinkerApplyFail(Diagnostic, EAssetError::UnknownClass, "Serialized class is unavailable.", 0, Descriptor.Path);
					return {EAssetError::UnknownClass, Diagnostic.Message};
				}
				DObject* Outer = Descriptor.OuterId == 0 ? static_cast<DObject*>(Package)
					: Objects[static_cast<size_t>(Descriptor.OuterId - 1)];
				bool bTypeMismatch = false;
				DObject* Object = FindExistingInner(Outer, Descriptor.Export->ObjectName, Class, bTypeMismatch);
				if (bTypeMismatch)
				{
					LinkerApplyFail(Diagnostic, EAssetError::TypeMismatch, "Existing default inner has a different class.", 0, Descriptor.Path);
					return {EAssetError::TypeMismatch, Diagnostic.Message};
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
					LinkerApplyFail(Diagnostic, EAssetError::InvalidObjectGraph, "Object construction failed.", 0, Descriptor.Path);
					return {EAssetError::InvalidObjectGraph, Diagnostic.Message};
				}
				Objects[Index] = Object;
				if (Pins && Pinned.insert(Object).second)
				{
					const size_t Begin = Pins->size();
					Pins->emplace_back(Object);
					for (size_t PinIndex = Begin; PinIndex < Pins->size(); ++PinIndex)
						for (DObject* Child : GDObjectArray.GetObjectsWithOuter((*Pins)[PinIndex].Get(), EObjectQueryScope::IncludeUnpublished))
							if (Pinned.insert(Child).second) Pins->emplace_back(Child);
				}
			}
			return {};
		}

		auto ApplyLinkerValues(FLinkerApplication& Application, const FLinkerLoadOptions& Options,
			FLinkerApplyDiagnostic& Diagnostic, const FPackageLoadBindings& Bindings) -> FAssetResult
		{
			const auto& PackagePath = Application.PackagePath;
			auto& Linker = Application.Linker;
			auto& Exports = Application.Exports;
			auto& Objects = Application.Objects;
			auto& Report = Application.Report;
			std::vector<FArchiveCustomVersion> CustomVersions;
			std::vector<std::pair<FGuid, int32>> LoadedCustomVersions;
			for (const ObjectPackage::FCustomVersion& Version : Linker.CustomVersions)
			{
				CustomVersions.push_back({Version.Guid, static_cast<int32>(Version.Value)});
				LoadedCustomVersions.emplace_back(Version.Guid, static_cast<int32>(Version.Value));
			}
			for (DObject* Object : Objects) Object->SetLoadedCustomVersions(LoadedCustomVersions);
			uint64 BulkFieldIndex = 0;
			for (size_t ObjectIndex = 0; ObjectIndex < Exports.size(); ++ObjectIndex)
				for (const auto& Property : Exports[ObjectIndex].Export->Properties)
				{
					if (!Options.bCooked && IsRemovedField(Property.DeclaringType, Property.FieldName)) continue;
					GatherNestedDeprecatedRouteEvidence(Property.Type, Property.Value, Linker,
						PackagePath, Exports[ObjectIndex].Path,
						Report.DeprecatedRouteEvidence);
				}
			for (size_t ObjectIndex = 0; ObjectIndex < Objects.size(); ++ObjectIndex)
			{
				if (ShouldFail(Options, ELinkerLoadPhase::ApplyValues, ObjectIndex))
				{
					LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Injected value application failure.");
					return {EAssetError::CorruptFile, Diagnostic.Message};
				}
				std::vector<FAuthoredPackageFieldRecord> Fields;
				std::vector<const ObjectPackage::FPropertyTag*> KnownProperties;
				for (const auto& Property : Exports[ObjectIndex].Export->Properties)
				{
					if (!Options.bCooked && IsRemovedField(Property.DeclaringType, Property.FieldName)) continue;
					FByteWriter Payload;
					if (!EncodeLoadArchiveValue(Property.Type, Property.Value, Linker, Payload,
						BulkFieldIndex, Diagnostic,
						std::format("{}::{}", Property.DeclaringType, Property.FieldName), !Options.bCooked))
					{
						return {EAssetError::CorruptFile, Diagnostic.Message};
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
					Bindings, Options.SourceFormatVersion, CustomVersions, LoadContext);
				if (!Result)
				{
					LinkerApplyFail(Diagnostic, EAssetError::UnsupportedProperty, Result.Message, 0, Exports[ObjectIndex].Path);
					return Result;
				}
				if (Options.bCooked) continue;
				std::vector<FAuthoredOverrideEntry> LedgerEntries;
				std::vector<FName> LoadedDeprecatedProperties;
				for (size_t PropertyIndex = 0; PropertyIndex < KnownProperties.size(); ++PropertyIndex)
				{
					const auto& Property = *KnownProperties[PropertyIndex];
					const auto* Schema = FindSchema(Linker, Property.DeclaringType);
					const auto Field = Schema ? std::ranges::find(Schema->Fields,
						Property.FieldName, &ObjectPackage::FSerializedField::Name)
						: std::vector<ObjectPackage::FSerializedField>::const_iterator{};
					if (!Schema || Field == Schema->Fields.end())
					{
						LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "A linker property lost its schema binding.");
						return {EAssetError::CorruptFile, Diagnostic.Message};
					}
					if (ShouldFail(Options, ELinkerLoadPhase::RestoreLedger, PropertyIndex + 1))
					{
						LinkerApplyFail(Diagnostic, EAssetError::CorruptFile, "Injected ledger restoration failure.");
						return {EAssetError::CorruptFile, Diagnostic.Message};
					}
					const auto Provenance = Property.Provenance == ObjectPackage::EPropertyProvenance::Forced
						? EAuthoredOverrideProvenance::Forced
						: EAuthoredOverrideProvenance::LoadedExplicit;
					FProperty* DeprecatedRoute =
						FindLinkerDeprecatedRoute(Linker, *Schema, *Field, Property.Type);
					if (DeprecatedRoute)
					{
						LoadedDeprecatedProperties.push_back(DeprecatedRoute->NamePrivate);
						FAssetDeprecatedRouteEvidence Evidence{
							.PackagePath = PackagePath,
							.ObjectPath = Exports[ObjectIndex].Path,
							.DeclaringType = Schema->QualifiedName,
							.StoredFieldName = Field->Name,
							.DeprecatedPropertyName = DeprecatedRoute->NamePrivate.ToString()};
						Report.DeprecatedRouteEvidence.push_back(std::move(Evidence));
						continue;
					}
					FAuthoredOverridePath Path{FAuthoredOverridePathToken::Field(
						FName(Schema->QualifiedName), FName(Field->Name))};
					LedgerEntries.push_back({Path, Provenance});
					if (!RestoreNestedLedger(Property.Type, Property.Value, Linker, Path,
						LedgerEntries, Diagnostic))
					{
						return {EAssetError::CorruptFile, Diagnostic.Message};
					}
				}
				Objects[ObjectIndex]->SetLoadedDeprecatedProperties(LoadedDeprecatedProperties);
				FAuthoredOverrideDiagnostic LedgerDiagnostic;
				if (!Objects[ObjectIndex]->ReplaceAuthoredOverrides(LedgerEntries, &LedgerDiagnostic))
				{
					LinkerApplyFail(Diagnostic, EAssetError::CorruptFile,
						"Could not restore authored intent.", 0, LedgerDiagnostic.LogicalPath);
					return {EAssetError::CorruptFile, Diagnostic.Message};
				}
			}
			return {};
		}

	}

	struct FPreparedPackageGraph::FState
	{
		DPackage* Package = nullptr;
		std::vector<FStrongObjectPtr> Pins;
		FPreparedPackageResource Storage;
		FAssetLoadReport Report;

		~FState()
		{
			if (Package && Package->IsGraphPrivate()) MarkObjectHierarchyAsGarbage(Package);
		}
	};

	FPreparedPackageGraph::FPreparedPackageGraph() = default;
	FPreparedPackageGraph::~FPreparedPackageGraph() = default;
	FPreparedPackageGraph::FPreparedPackageGraph(FPreparedPackageGraph&&) noexcept = default;
	auto FPreparedPackageGraph::operator=(FPreparedPackageGraph&&) noexcept -> FPreparedPackageGraph& = default;
	auto FPreparedPackageGraph::GetPackage() const -> DPackage* { return State ? State->Package : nullptr; }
	auto FPreparedPackageGraph::GetStorage() const -> const FPreparedPackageResource& { require(State); return State->Storage; }
	auto FPreparedPackageGraph::GetReport() const -> const FAssetLoadReport& { require(State); return State->Report; }

	auto PreparePackageGraphs(std::span<const FPackageGraphSource> Sources,
		const FPackageGraphPrepareOptions& Options, std::vector<FPreparedPackageGraph>& Out)
		-> FPackageGraphPrepareResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		using S = EPackageGraphPrepareStatus;
		static bool bPreparing = false;
		if (bPreparing) return {S::Busy, {}, "Package graph preparation cannot be reentered."};
		if (!FAssetRuntimeState::Get().GetLoadService().IsIdle())
			return {S::Busy, {}, "Package graph preparation requires an idle load service."};
		struct FExecutionScope
		{
			bool& Active;
			explicit FExecutionScope(bool& InActive) : Active(InActive) { Active = true; }
			~FExecutionScope() { Active = false; }
		} Execution(bPreparing);
		if (Sources.empty() || Sources.size() > Options.MaximumPackages)
			return {S::BudgetExceeded, {}, "Package count is outside the preparation budget."};
		bool bCancelled = false;
		auto Cancelled = [&]() {
			bCancelled = bCancelled || (Options.IsCancelled && Options.IsCancelled());
			return bCancelled;
		};
		FPackagePath CurrentPath;
		try
		{
			std::vector<FLinkerApplication> Applications(Sources.size());
			std::vector<FPreparedPackageGraph> Candidates(Sources.size());
			uint64 ObjectCount = 0;
			// Validate the entire batch before invoking any asset constructor.
			for (size_t Index = 0; Index < Sources.size(); ++Index)
			{
				const auto& Source = Sources[Index];
				CurrentPath = Source.PackagePath;
				if (Cancelled()) return {S::Cancelled, CurrentPath, "Package graph preparation cancelled."};
				if (!CurrentPath.IsValid() || Source.Storage.GetMainBytes().IsEmpty())
					return {S::InvalidClosure, CurrentPath, "A validated saved package closure is required."};
				if (IsAssetRegistryProjectionFenced(CurrentPath))
					return {S::Busy, CurrentPath, "Package projection is fenced; existing recovery must complete first."};
				// An ordinary external load may recurse into the replacement set.
				// Require its live skeletons so that recursion cannot publish a target.
				if (Options.DependencyLoadScope && !FindResidentPackage(CurrentPath))
					return {S::Unsupported, CurrentPath, "Scoped dependency loading requires resident replacement targets."};
				for (size_t Previous = 0; Previous < Index; ++Previous)
					if (Sources[Previous].PackagePath == CurrentPath)
						return {S::InvalidClosure, CurrentPath, "Duplicate package identity in preparation batch."};
				auto& Application = Applications[Index];
				Application.PackagePath = CurrentPath;
				ObjectPackage::FPackageReaderDiagnostic ReaderDiagnostic;
				const auto& Bulk = Source.Storage.GetBulkResource();
				if (!ObjectPackage::ReadPackageV9Metadata(Source.Storage.GetMainBytes(),
					Bulk ? Bulk->GetSegmentExtent() : 0, CurrentPath, Application.Linker, &ReaderDiagnostic))
					return {S::InvalidClosure, CurrentPath, ReaderDiagnostic.Message};
				FLinkerApplyDiagnostic Diagnostic;
				if (auto Result = ValidateLinker(Application, {}, Diagnostic); !Result)
					return {S::InvalidClosure, CurrentPath, Result.Message};
				if (ObjectCount >= Options.MaximumObjects
					|| Application.Exports.size() > Options.MaximumObjects - ObjectCount - 1)
					return {S::BudgetExceeded, CurrentPath, "Package skeletons exceed the object budget."};
				ObjectCount += Application.Exports.size() + 1;
				for (const auto& Export : Application.Exports)
				{
					DClass* Class = FindClassByQualifiedName(FName(Export.Export->ClassName));
					if (std::ranges::find(Options.AdmittedClasses, Class) == Options.AdmittedClasses.end())
						return {S::Unsupported, CurrentPath,
							std::format("Class {} has no isolated deserialization admission.", Export.Export->ClassName)};
				}
			}
			// Admit external residency before constructing any candidate. Only the
			// supplied scope may own new loads; callers retain it even on failure.
			std::vector<std::pair<FPackagePath, DPackage*>> ExternalPackages;
			std::vector<FStrongObjectPtr> ExternalPins;
			for (size_t PackageIndex = 0; PackageIndex < Applications.size(); ++PackageIndex)
			{
				const auto& Application = Applications[PackageIndex];
				for (size_t DependencyIndex = 0; DependencyIndex < Application.LiveDependencies.size(); ++DependencyIndex)
				{
					const auto& Path = Application.LiveDependencies[DependencyIndex];
					CurrentPath = Application.PackagePath;
					if (Cancelled()) return {S::Cancelled, Application.PackagePath, "Package graph preparation cancelled."};
					if (Options.ShouldFail && Options.ShouldFail(PackageIndex, ELinkerLoadPhase::ResolveDependency, DependencyIndex))
						return {S::MissingDependency, Application.PackagePath, "Injected dependency binding failure."};
					if (IsAssetRegistryProjectionFenced(Path))
						return {S::Busy, Path, "Dependency projection is fenced; existing recovery must complete first."};
					if (std::ranges::find(Sources, Path, &FPackageGraphSource::PackagePath) != Sources.end()) continue;
					if (std::ranges::find(ExternalPackages, Path, &std::pair<FPackagePath, DPackage*>::first)
						!= ExternalPackages.end()) continue;
					DPackage* Package = FindPackage(Path.GetView());
					if (!Package && Options.DependencyLoadScope)
					{
						const auto Result = Options.DependencyLoadScope->LoadPackage(Path, Package);
						if (!Result) return {Result.Error == EAssetError::InUse ? S::Busy : S::MissingDependency,
							Path, Result.Message};
					}
					if (!Package) return {S::MissingDependency, Application.PackagePath,
						std::format("External dependency {} must be admitted and resident before graph preparation.", Path.ToString())};
					ExternalPackages.emplace_back(Path, Package);
					const size_t Begin = ExternalPins.size();
					ExternalPins.emplace_back(Package);
					for (size_t PinIndex = Begin; PinIndex < ExternalPins.size(); ++PinIndex)
						for (DObject* Child : GDObjectArray.GetObjectsWithOuter(ExternalPins[PinIndex].Get(), EObjectQueryScope::LiveOnly))
							ExternalPins.emplace_back(Child);
				}
			}
			auto LoadOptions = [&](size_t PackageIndex) {
				FLinkerLoadOptions Result;
				Result.ShouldFail = [&, PackageIndex](ELinkerLoadPhase Phase, uint64 ObjectIndex) {
					return Cancelled() || (Options.ShouldFail && Options.ShouldFail(PackageIndex, Phase, ObjectIndex));
				};
				return Result;
			};
			ObjectCount = 0;
			for (size_t Index = 0; Index < Applications.size(); ++Index)
			{
				auto& Application = Applications[Index];
				CurrentPath = Application.PackagePath;
				if (Cancelled()) return {S::Cancelled, CurrentPath, "Package graph preparation cancelled."};
				auto& State = Candidates[Index].State;
				State = std::make_unique<FPreparedPackageGraph::FState>();
				State->Storage = Sources[Index].Storage;
				State->Package = NewObject<DPackage>(nullptr, FName(CurrentPath.GetAssetName()));
				if (!State->Package || !State->Package->InitializePreparedAssetPackage(CurrentPath))
					return {S::InvalidClosure, CurrentPath, "Could not create private package skeleton."};
				State->Pins.emplace_back(State->Package);
				Application.Package = State->Package;
				Application.Objects.resize(Application.Exports.size());
				FLinkerApplyDiagnostic Diagnostic;
				if (auto Result = CreateLinkerSkeleton(Application, LoadOptions(Index), Diagnostic, &State->Pins); !Result)
					return {Cancelled() ? S::Cancelled : S::InvalidClosure, CurrentPath, Result.Message};
				if (State->Pins.size() > Options.MaximumObjects - ObjectCount)
					return {S::BudgetExceeded, CurrentPath, "Default inners exceed the object budget."};
				ObjectCount += State->Pins.size();
				State->Pins.insert(State->Pins.end(), ExternalPins.begin(), ExternalPins.end());
			}
			auto Resolve = [&](const FObjectPath& Path, DObject*& Object) -> FAssetResult {
				Object = nullptr;
				DPackage* Package = nullptr;
				for (const auto& Application : Applications)
					if (Application.PackagePath == Path.GetPackagePath()) Package = Application.Package;
				if (!Package)
					for (const auto& External : ExternalPackages)
						if (External.first == Path.GetPackagePath()) Package = External.second;
				if (!Package) return {EAssetError::MissingDependency, "Object reference has no admitted package binding."};
				DObject* Current = Package;
				auto Descend = [&](std::string_view Name) {
					if (!Current) return;
					const auto Children = GDObjectArray.GetObjectsWithOuter(Current, EObjectQueryScope::IncludeUnpublished);
					const auto It = std::ranges::find(Children, FName(Name), &DObject::GetFName);
					Current = It == Children.end() ? nullptr : *It;
				};
				Descend(Path.GetAssetPath().GetAssetName());
				for (std::string_view Name : Path.GetSubobjectNames()) Descend(Name);
				Object = Current;
				return Object ? FAssetResult{} : FAssetResult{EAssetError::MissingDependency, "Admitted package has no matching object."};
			};
			for (size_t Index = 0; Index < Applications.size(); ++Index)
			{
				auto& Application = Applications[Index];
				CurrentPath = Application.PackagePath;
				const FPackageLoadBindings Bindings{Sources[Index].Storage.GetBulkResource(), Resolve};
				FLinkerApplyDiagnostic Diagnostic;
				if (auto Result = ApplyLinkerValues(Application, LoadOptions(Index), Diagnostic, Bindings); !Result)
					return {Cancelled() ? S::Cancelled : S::InvalidClosure, CurrentPath, Result.Message};
				Application.Package->ClearDirty();
				Application.Package->SetCanonicalResaveRecommended(!Application.Report.CanonicalizationEvidence.empty()
					|| !Application.Report.DeprecatedRouteEvidence.empty() || Application.Report.DiscardedFieldCount != 0);
				Candidates[Index].State->Report = std::move(Application.Report);
			}
			for (const auto& Source : Sources)
			{
				CurrentPath = Source.PackagePath;
				if (auto Result = Source.Storage.Revalidate(Cancelled); !Result)
					return {Result.Status == EPreparedPackageResourceStatus::Cancelled ? S::Cancelled : S::Stale,
						CurrentPath, Result.Message};
			}
			for (const auto& Source : Sources)
				if (IsAssetRegistryProjectionFenced(Source.PackagePath))
					return {S::Busy, Source.PackagePath, "Package projection became fenced during preparation."};
			for (const auto& [Path, Package] : ExternalPackages)
			{
				if (IsAssetRegistryProjectionFenced(Path))
					return {S::Busy, Path, "Dependency projection became fenced during preparation."};
				if (FindPackage(Path.GetView()) != Package)
					return {S::Stale, Path, "Admitted dependency identity changed during preparation."};
			}
			Out = std::move(Candidates);
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return {S::BudgetExceeded, CurrentPath, "Allocation failed during package graph preparation."};
		}
	}

	auto ApplyLivePackageLinker(ObjectPackage::FLinkerTables Linker,
		const FPackagePath& PackagePath, DPackage*& OutPackage,
		FAssetLoadReport* OutReport, const FLinkerLoadOptions& Options,
		std::string* OutError) -> FAssetResult
	{
		OutPackage = nullptr;
		FLinkerApplyDiagnostic Diagnostic;
		FAssetLiveLoadGuard LiveLoadGuard(Options.DependencyLoadPolicy
			&& Options.DependencyLoadPolicy->bRejectImplicitLiveLoads);
		auto Finish = [&](FAssetResult Result) {
			if (OutError) *OutError = Result ? std::string{} : Diagnostic.Message;
			return Result;
		};
		if (!PackagePath.IsValid())
		{
			LinkerApplyFail(Diagnostic, EAssetError::InvalidPath, "Live linker application requires a validated package path.");
			return Finish({EAssetError::InvalidPath, Diagnostic.Message});
		}
		if (Options.bPrivateGraph && (!Options.DependencyLoadPolicy
			|| !Options.DependencyLoadPolicy->bRejectImplicitLiveLoads))
		{
			LinkerApplyFail(Diagnostic, EAssetError::InvalidObjectGraph,
				"Private package loading requires an explicit closed dependency policy.");
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}
		if (Options.DependencyLoadPolicy
			&& (!Options.DependencyLoadPolicy->ResolvePackage
				|| !Options.DependencyLoadPolicy->ResolveObject
				|| !Options.DependencyLoadPolicy->Rollback))
		{
			LinkerApplyFail(Diagnostic, EAssetError::InvalidObjectGraph,
				"An explicit dependency load policy requires package, object, and rollback callbacks.");
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}
		if (Linker.Summary.PackagePath != PackagePath)
		{
			LinkerApplyFail(Diagnostic, EAssetError::InvalidPath,
				"Linker package identity does not match the requested package path.");
			return Finish({EAssetError::InvalidPath, Diagnostic.Message});
		}
		if (FindPackage(PackagePath.GetView()))
		{
			LinkerApplyFail(Diagnostic, EAssetError::AlreadyExists,
				"A package with the requested path is already live.");
			return Finish({EAssetError::AlreadyExists, Diagnostic.Message});
		}
		FLinkerApplication Application;
		Application.Linker = std::move(Linker);
		Application.PackagePath = PackagePath;
		if (FAssetResult Result = ValidateLinker(Application, Options, Diagnostic); !Result)
			return Finish(Result);
		auto& Exports = Application.Exports;
		auto& Objects = Application.Objects;
		auto& Report = Application.Report;

		DPackage* Package = NewObject<DPackage>(
			nullptr,
			FName(PackagePath.GetAssetName()),
			EObjectFlags::Standalone);
		if (!Package)
		{
			LinkerApplyFail(Diagnostic, EAssetError::InvalidObjectGraph, "Could not allocate the package skeleton.");
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}
		if (Options.bPrivateGraph)
		{
			if (!Package->InitializePreparedAssetPackage(PackagePath))
			{
				MarkObjectHierarchyAsGarbage(Package); CollectGarbage();
				LinkerApplyFail(Diagnostic, EAssetError::InvalidObjectGraph,
					"Could not initialize a private capture package.");
				return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
			}
		}
		else Package->InitializeAssetPackage(PackagePath);
		Application.Package = Package;
		Objects.resize(Exports.size(), nullptr);
		// Nested production loads belong to the enclosing load transaction. Direct
		// linker applications own only the dependencies admitted by their explicit calls.
		FAssetPackageLoadScope DependencyScope;
		const bool bOwnDependencies = !Options.DependencyLoadPolicy
			&& FAssetRuntimeState::Get().GetLoadService().IsIdle();
		bool bSkeletonPublished = false;
		bool bFinalized = false;
		auto Rollback = [&]() {
			if (std::exchange(bFinalized, true)) return;
			std::exception_ptr Failure;
			auto Cleanup = [&](auto&& Work) {
				try { Work(); }
				catch (...) { if (!Failure) Failure = std::current_exception(); }
			};
			Cleanup([&] {
				if (bSkeletonPublished && Options.OnSkeletonRollback)
					Options.OnSkeletonRollback(Package);
			});
			Cleanup([&] { MarkObjectHierarchyAsGarbage(Package); CollectGarbage(); });
			Cleanup([&] {
				if (Options.DependencyLoadPolicy) Options.DependencyLoadPolicy->Rollback();
				else if (bOwnDependencies) (void)DependencyScope.Release();
			});
			if (Failure) std::rethrow_exception(Failure);
		};

		struct FScopedRollback
		{
			decltype(Rollback)& Execute;
			~FScopedRollback()
			{
				try { Execute(); }
				catch (...) { /* Preserve an in-flight callback exception during cleanup. */ }
			}
		} Scope{Rollback};

		if (FAssetResult Result = CreateLinkerSkeleton(Application, Options, Diagnostic); !Result)
		{
			Rollback(); return Finish(Result);
		}

		if (!LiveLoadGuard.GetFailure())
		{
			const FAssetResult Result = LiveLoadGuard.GetFailure();
			LinkerApplyFail(Diagnostic, Result.Error, Result.Message); Rollback();
			return Finish(Result);
		}
		if (Options.OnSkeletonReady)
		{
			FAssetResult PublishResult = Options.OnSkeletonReady(Package);
			if (!PublishResult)
			{
				LinkerApplyFail(Diagnostic, EAssetError::InvalidObjectGraph,
					PublishResult.Message.empty() ? "Could not publish the package skeleton."
						: PublishResult.Message);
				Rollback();
				return Finish(PublishResult);
			}
			bSkeletonPublished = true;
		}

		const auto& Dependencies = Options.bCooked ? Application.Linker.Summary.HardPackageDependencies : Application.LiveDependencies;
		for (size_t Index = 0; Index < Dependencies.size(); ++Index)
		{
			if (ShouldFail(Options, ELinkerLoadPhase::ResolveDependency, Index))
			{
				LinkerApplyFail(Diagnostic, EAssetError::MissingDependency, "Injected dependency failure."); Rollback();
				return Finish({EAssetError::MissingDependency, Diagnostic.Message});
			}
			const FPackagePath& Path = Dependencies[Index];
			DPackage* Dependency = nullptr;
			FAssetResult Result = Options.DependencyLoadPolicy
				? Options.DependencyLoadPolicy->ResolvePackage(Path, Dependency)
				: bOwnDependencies ? DependencyScope.LoadPackage(Path, Dependency)
					: LoadPackage(Path, Dependency);
			if (Result && !Dependency)
				Result = {EAssetError::MissingDependency, "Dependency resolver returned no package."};
			if (!Result)
			{
				const EAssetError Error = Options.DependencyLoadPolicy ? Result.Error : EAssetError::MissingDependency;
				LinkerApplyFail(Diagnostic, Error, Result.Message); Rollback();
				return Finish({Error, Diagnostic.Message});
			}
		}

		// Preserve report mutations emitted while admitting ordinary dependencies.
		auto CanonicalizationEvidence = std::move(Report.CanonicalizationEvidence);
		const uint64 DiscardedFields = Report.DiscardedFieldCount;
		Report = OutReport ? *OutReport : FAssetLoadReport{};
		Report.PackagePath = PackagePath;
		Report.CanonicalizationEvidence = std::move(CanonicalizationEvidence);
		Report.DiscardedFieldCount = DiscardedFields;
		// Choose sources once for this package; object archives only consume them.
		const FPackageLoadBindings Bindings{
			.BulkResource = Options.BulkResource,
			.ResolveExternalObject = [Policy = Options.DependencyLoadPolicy, bOwnDependencies, &DependencyScope](const FObjectPath& Path, DObject*& Object) {
				return Policy ? Policy->ResolveObject(Path, Object)
					: bOwnDependencies ? DependencyScope.LoadObject(Path, nullptr, Object)
						: LoadObject(Path, nullptr, Object);
			}};
		if (FAssetResult Result = ApplyLinkerValues(Application, Options, Diagnostic, Bindings); !Result)
		{
			Rollback(); return Finish(Result);
		}

		Package->ClearDirty();
		for (size_t Reverse = Objects.size(); Reverse > 0; --Reverse)
		{
			const size_t Index = Reverse - 1;
			if (ShouldFail(Options, ELinkerLoadPhase::PostLoad, Index))
			{
				LinkerApplyFail(Diagnostic, EAssetError::InvalidObjectGraph, "Injected PostLoad failure."); Rollback();
				return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
			}
			std::string Error;
			if (!Objects[Index]->PostLoad(Error))
			{
				LinkerApplyFail(Diagnostic, EAssetError::InvalidObjectGraph,
					Error.empty() ? "Object PostLoad failed." : Error, 0, Exports[Index].Path); Rollback();
				return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
			}
			Objects[Index]->ClearLoadedCustomVersions();
			Objects[Index]->ClearLoadedDeprecatedProperties();
		}
		if (ShouldFail(Options, ELinkerLoadPhase::Publish, 0))
		{
			LinkerApplyFail(Diagnostic, EAssetError::InvalidObjectGraph, "Injected graph publication failure."); Rollback();
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}
		if (!LiveLoadGuard.GetFailure())
		{
			const FAssetResult Result = LiveLoadGuard.GetFailure();
			LinkerApplyFail(Diagnostic, Result.Error, Result.Message); Rollback();
			return Finish(Result);
		}
		OutPackage = Package;
		Package->SetCanonicalResaveRecommended(!Report.CanonicalizationEvidence.empty()
			|| !Report.DeprecatedRouteEvidence.empty() || Report.DiscardedFieldCount != 0);
		if (OutReport) *OutReport = std::move(Report);
		bFinalized = true;
		Diagnostic.Reset(); return Finish({});
	}

}
