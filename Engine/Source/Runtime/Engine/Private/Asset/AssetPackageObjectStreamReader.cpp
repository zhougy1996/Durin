#include "Asset/PackageObjectStreamReader.h"

#include "AssetPackageArchive.h"
#include "AssetPackageValueCodec.h"
#include "Asset/PackageSchema.h"
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

		auto EncodeIntrinsicValue(uint64 Layout, std::span<const uint64> Components,
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
					if (!EncodeIntrinsicValue(ChildLayout, Components.subspan(Offset, Count), Payload, Diagnostic)) return false;
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

		auto EncodeInspectionValue(const FDecodedType& Type, const FValue& Value,
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
				return EncodeIntrinsicValue(Type.Parameter, Value.ComponentBits, Writer, Diagnostic);
			case ETypeOpcode::Struct:
			{
				uint64 SchemaId = 0; const FDecodedSchema* Schema = FindSchema(Package, Type.QualifiedName, SchemaId);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size())
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Struct inspection projection is invalid.", 0, std::move(Path));
				Writer.WriteString(Type.QualifiedName); Writer.Write(uint64(Value.Elements.size()));
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto It = std::ranges::find(Schema->Fields, Value.FieldNames[Index], &FDecodedField::Name);
					if (It == Schema->Fields.end()) return Fail(Diagnostic, EReaderFailure::InvalidValue, "Struct field is absent from its schema.", 0, std::move(Path));
					const FDecodedType* ChildType = TypeAt(Package, It->TypeId);
					if (!ChildType) return Fail(Diagnostic, EReaderFailure::InvalidTable, "Struct field type is invalid.", 0, std::move(Path));
					Private::FByteWriter Payload;
					if (!EncodeInspectionValue(*ChildType, Value.Elements[Index], Package, Payload, Diagnostic,
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
					if (!EncodeInspectionValue(*Element, Item, Package, Writer, Diagnostic, Path)) return false;
				return true;
			}
			case ETypeOpcode::Map:
			{
				const FDecodedType* Key = Type.ChildTypeIds.size() == 2 ? TypeAt(Package, Type.ChildTypeIds[0]) : nullptr;
				const FDecodedType* Mapped = Type.ChildTypeIds.size() == 2 ? TypeAt(Package, Type.ChildTypeIds[1]) : nullptr;
				if (!Key || !Mapped || Value.Elements.size() % 2 != 0) return Fail(Diagnostic, EReaderFailure::InvalidValue, "Map projection is invalid.", 0, std::move(Path));
				Writer.Write(uint64(Value.Elements.size() / 2));
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
					if (!EncodeInspectionValue(*Key, Value.Elements[Index], Package, Writer, Diagnostic, Path)
						|| !EncodeInspectionValue(*Mapped, Value.Elements[Index + 1], Package, Writer, Diagnostic, Path)) return false;
				return true;
			}
			case ETypeOpcode::HardRef:
				Writer.Write(Value.ReferenceTag);
				if (Value.ReferenceTag == 1) Writer.Write(Value.ReferenceId);
				else if (Value.ReferenceTag == 2) Writer.WriteString(Package.Header.Dependencies[static_cast<size_t>(Value.ReferenceId - 1)]);
				return true;
			case ETypeOpcode::SoftRef:
				Writer.Write(Value.ReferenceTag); if (Value.ReferenceTag == 1) Writer.WriteString(Value.Text); return true;
			case ETypeOpcode::Bytes: case ETypeOpcode::BulkData:
				Writer.WriteBytes(Value.Bytes); return true;
			}
			return Fail(Diagnostic, EReaderFailure::InvalidValue, "Unsupported inspection value.", 0, std::move(Path));
		}

		auto BuildInspection(const FDecodedPackage& Package, std::span<const std::byte> Bytes,
			FAssetPackageInspection& Out, FReaderDiagnostic& Diagnostic) -> bool
		{
			FAssetPackageInspection Inspection;
			Inspection.Header = {
				.AssetClassName = Package.Header.AssetClass,
				.EntryKind = Package.Header.EntryKind,
				.FormatVersion = Version,
				.ObjectCount = Package.Header.ObjectCount,
				.BytesRead = Package.Header.BytesRead};
			if (!Package.Header.RedirectDestination.empty())
				FAssetPath::TryCreate(Package.Header.RedirectDestination, Inspection.Header.RedirectDestination);
			for (const std::string& Dependency : Package.Header.Dependencies)
			{
				FAssetPath Path; if (!FAssetPath::TryCreate(Dependency, Path))
					return Fail(Diagnostic, EReaderFailure::InvalidHeader, "Dependency path is invalid.");
				Inspection.Header.Dependencies.push_back(std::move(Path));
			}
			Inspection.Fingerprint = {.FileSize = Bytes.size(),
				.ContentHash = FXxHash128::HashBuffer(Bytes), .ReaderVersion = Version};
			for (size_t ObjectIndex = 0; ObjectIndex < Package.Objects.size(); ++ObjectIndex)
			{
				const FDecodedObject& Object = Package.Objects[ObjectIndex];
				FAssetPackageObjectInspection Output{Object.Id, Object.OuterId, Object.ClassName, Object.ObjectName, Object.Path, {}};
				for (const FDecodedOverride& Override : Package.ObjectValues[ObjectIndex].Overrides)
				{
					const FDecodedSchema* Schema = SchemaAt(Package, Override.SchemaId);
					if (!Schema || Override.FieldId == 0 || Override.FieldId > Schema->Fields.size()) return false;
					const FDecodedField& Field = Schema->Fields[static_cast<size_t>(Override.FieldId - 1)];
					FAssetPackageField OutputField{.DeclaringClass = Schema->QualifiedName, .Name = Field.Name,
						.SourceFormatVersion = Version};
					if (Override.Provenance == 2)
					{
						OutputField.TypeSignature = "DASTv4:RetainedClosure";
						Private::FByteWriter Body; Body.Write(uint64(Override.DescriptorClosure.size()));
						Body.WriteBytes(Override.DescriptorClosure); Body.Write(uint64(Override.RetainedPayload.size()));
						Body.WriteBytes(Override.RetainedPayload); OutputField.Payload = std::move(Body.Bytes);
					}
					else
					{
						const FDecodedType* Type = TypeAt(Package, Field.TypeId); if (!Type) return false;
						OutputField.Kind = TypeKind(*Type, Package); OutputField.TypeSignature = TypeSignature(*Type, Package);
						Private::FByteWriter Payload;
						if (!EncodeInspectionValue(*Type, Override.Value, Package, Payload, Diagnostic,
							std::format("{}::{}", Schema->QualifiedName, Field.Name))) return false;
						OutputField.Payload = std::move(Payload.Bytes);
					}
					Output.Fields.push_back(std::move(OutputField));
				}
				Inspection.Objects.push_back(std::move(Output));
			}
			Out = std::move(Inspection); return true;
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

		template<std::unsigned_integral T>
		auto AppendBigEndian(std::vector<std::byte>& Token, T Value) -> void
		{
			for (size_t Index = sizeof(T); Index > 0; --Index)
				Token.push_back(static_cast<std::byte>(Value >> ((Index - 1) * 8)));
		}

		template<std::integral T>
		auto AppendSortable(std::vector<std::byte>& Token, T Value) -> void
		{
			using U = std::make_unsigned_t<T>;
			U Bits = std::bit_cast<U>(Value);
			if constexpr (std::is_signed_v<T>) Bits ^= U(1) << (sizeof(U) * 8 - 1);
			AppendBigEndian(Token, Bits);
		}

		template<std::floating_point T>
		auto AppendSortableFloat(std::vector<std::byte>& Token, T Value) -> void
		{
			using U = std::conditional_t<sizeof(T) == 4, uint32, uint64>;
			U Bits = std::bit_cast<U>(Value); constexpr U Sign = U(1) << (sizeof(U) * 8 - 1);
			if ((Bits & ~Sign) == 0) Bits = 0;
			Bits = (Bits & Sign) ? ~Bits : (Bits ^ Sign); AppendBigEndian(Token, Bits);
		}

		auto BuildIntrinsicLedgerToken(uint64 Layout, std::span<const uint64> Components,
			std::vector<std::byte>& Out, FReaderDiagnostic& Diagnostic) -> bool
		{
			Out.push_back(static_cast<std::byte>(DurinCodeGen::EPropertyGenFlags::Struct));
			if (Layout == 5)
			{
				if (Components.size() != 10) return false;
				for (const auto [Ordinal, ChildLayout, Offset, Count] : {
					std::tuple<uint32, uint64, size_t, size_t>{0, 4, 0, 4}, {1, 2, 4, 3}, {2, 2, 7, 3}})
				{
					AppendBigEndian(Out, Ordinal); AppendBigEndian(Out, uint32(0));
					if (!BuildIntrinsicLedgerToken(ChildLayout, Components.subspan(Offset, Count), Out, Diagnostic)) return false;
				}
				return true;
			}
			const uint64 Count = Layout == 1 ? 2 : Layout == 2 ? 3 : 4;
			if (Components.size() != Count) return false;
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				AppendBigEndian(Out, Index); AppendBigEndian(Out, uint32(0));
				if (Layout == 6)
				{
					Out.push_back(static_cast<std::byte>(DurinCodeGen::EPropertyGenFlags::Float));
					AppendSortableFloat(Out, std::bit_cast<float>(uint32(Components[Index])));
				}
				else
				{
					Out.push_back(static_cast<std::byte>(DurinCodeGen::EPropertyGenFlags::Double));
					AppendSortableFloat(Out, std::bit_cast<double>(Components[Index]));
				}
			}
			return true;
		}

		auto BuildLedgerMapKeyToken(const FDecodedType& Type, const FValue& Value,
			std::vector<std::byte>& Out, FReaderDiagnostic& Diagnostic) -> bool
		{
			Out.push_back(static_cast<std::byte>(TypeKind(Type, FDecodedPackage{})));
			switch (Type.Opcode)
			{
			case ETypeOpcode::Bool: Out.back() = static_cast<std::byte>(DurinCodeGen::EPropertyGenFlags::Bool); Out.push_back(Value.Bool ? std::byte{1} : std::byte{0}); return true;
			case ETypeOpcode::I8: AppendSortable(Out, int8(Value.Signed)); return true;
			case ETypeOpcode::I16: AppendSortable(Out, int16(Value.Signed)); return true;
			case ETypeOpcode::I32: AppendSortable(Out, int32(Value.Signed)); return true;
			case ETypeOpcode::I64: AppendSortable(Out, Value.Signed); return true;
			case ETypeOpcode::U8: AppendSortable(Out, uint8(Value.Unsigned)); return true;
			case ETypeOpcode::U16: AppendSortable(Out, uint16(Value.Unsigned)); return true;
			case ETypeOpcode::U32: AppendSortable(Out, uint32(Value.Unsigned)); return true;
			case ETypeOpcode::U64: AppendSortable(Out, Value.Unsigned); return true;
			case ETypeOpcode::String:
				AppendBigEndian(Out, uint64(Value.Text.size())); {
					const auto Bytes = std::as_bytes(std::span(Value.Text)); Out.insert(Out.end(), Bytes.begin(), Bytes.end()); } return true;
			case ETypeOpcode::Name:
				AppendBigEndian(Out, uint64(Value.Text.size())); {
					const auto Bytes = std::as_bytes(std::span(Value.Text)); Out.insert(Out.end(), Bytes.begin(), Bytes.end()); }
				AppendBigEndian(Out, uint32(0)); return true;
			case ETypeOpcode::Guid:
				AppendBigEndian(Out, Value.Guid.A); AppendBigEndian(Out, Value.Guid.B);
				AppendBigEndian(Out, Value.Guid.C); AppendBigEndian(Out, Value.Guid.D); return true;
			case ETypeOpcode::Enum:
			{
				const auto Storage = static_cast<ETypeOpcode>(Type.Parameter);
				Out.front() = static_cast<std::byte>(DurinCodeGen::EPropertyGenFlags::Enum);
				if (Storage == ETypeOpcode::I8) AppendSortable(Out, int8(Value.Signed));
				else if (Storage == ETypeOpcode::I16) AppendSortable(Out, int16(Value.Signed));
				else if (Storage == ETypeOpcode::I32) AppendSortable(Out, int32(Value.Signed));
				else if (Storage == ETypeOpcode::I64) AppendSortable(Out, Value.Signed);
				else if (Storage == ETypeOpcode::U8) AppendSortable(Out, uint8(Value.Unsigned));
				else if (Storage == ETypeOpcode::U16) AppendSortable(Out, uint16(Value.Unsigned));
				else if (Storage == ETypeOpcode::U32) AppendSortable(Out, uint32(Value.Unsigned));
				else if (Storage == ETypeOpcode::U64) AppendSortable(Out, Value.Unsigned);
				else return Fail(Diagnostic, EReaderFailure::InvalidValue, "Map enum key storage is invalid.");
				return true;
			}
			case ETypeOpcode::Intrinsic:
				Out.clear(); return BuildIntrinsicLedgerToken(Type.Parameter, Value.ComponentBits, Out, Diagnostic);
			default: return Fail(Diagnostic, EReaderFailure::ArchiveFailure,
				"Live nested authored intent does not support this map key type.");
			}
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
					if (!BuildLedgerMapKeyToken(*KeyType, Value.Elements[Index], Token, Diagnostic)) return false;
					Path.push_back(FAuthoredOverridePathToken::MapValue(std::move(Token)));
					if (!RestoreNestedLedger(*ValueType, Value.Elements[Index + 1], Package, Path,
						Entries, bUsedDeprecatedRoute, Diagnostic)) return false;
					Path.pop_back();
				}
			}
			return true;
		}

		auto ExtractValueReferences(const FDecodedType& Type, const FValue& Value,
			const FDecodedPackage& Package, const FAssetPath& SourcePackage,
			const FAssetPackageFingerprint& Fingerprint, uint64 SourceObjectId,
			std::string_view SourceClass, std::string_view DeclaringType,
			std::string_view FieldName, std::vector<FAssetReferenceRouteSegment>& Route,
			std::vector<FAssetReferenceEdge>& Out, FReaderDiagnostic& Diagnostic) -> bool
		{
			auto Add = [&](EAssetReferenceKind Kind, std::string_view Target,
				std::string ExpectedClass) -> bool {
				FAssetPath TargetPath; std::string Error;
				if (!FAssetPath::TryCreate(Target, TargetPath, &Error))
					return Fail(Diagnostic, EReaderFailure::InvalidValue, Error);
				std::string Display = std::format("{}::{}", DeclaringType, FieldName);
				for (const auto& Segment : Route)
				{
					if (Segment.Kind == EAssetReferenceRouteKind::FixedArray || Segment.Kind == EAssetReferenceRouteKind::ArrayElement)
						Display += std::format("[{}]", Segment.Index);
					else if (Segment.Kind == EAssetReferenceRouteKind::MapValue) Display += "{value}";
					else Display += std::format(".{}", Segment.FieldName);
				}
				Out.push_back({.SourcePackage = SourcePackage, .SourceFingerprint = Fingerprint,
					.SourceObjectId = SourceObjectId, .SourceClass = std::string(SourceClass),
					.DeclaringType = std::string(DeclaringType), .FieldName = std::string(FieldName),
					.Kind = Kind, .ExpectedClass = std::move(ExpectedClass),
					.TargetPath = std::move(TargetPath), .Route = Route, .DisplayRoute = std::move(Display)});
				return true;
			};
			if (Type.Opcode == ETypeOpcode::HardRef && Value.ReferenceTag != 0)
			{
				const std::string_view Target = Value.ReferenceTag == 1 ? SourcePackage.GetView()
					: std::string_view(Package.Header.Dependencies[static_cast<size_t>(Value.ReferenceId - 1)]);
				return Add(EAssetReferenceKind::HardObject, Target,
					Type.QualifiedName.empty() ? "DObject" : Type.QualifiedName);
			}
			if (Type.Opcode == ETypeOpcode::SoftRef && Value.ReferenceTag == 1)
				return Add(EAssetReferenceKind::SoftObject, Value.Text,
					Type.QualifiedName.empty() ? "DObject" : Type.QualifiedName);
			if (Type.Opcode == ETypeOpcode::Struct)
			{
				uint64 SchemaId = 0; const FDecodedSchema* Schema = FindSchema(Package, Type.QualifiedName, SchemaId);
				if (!Schema) return false;
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto It = std::ranges::find(Schema->Fields, Value.FieldNames[Index], &FDecodedField::Name);
					if (It == Schema->Fields.end()) return false;
					const FDecodedType* Child = TypeAt(Package, It->TypeId); if (!Child) return false;
					Route.push_back({.Kind = EAssetReferenceRouteKind::StructField,
						.DeclaringType = Schema->QualifiedName, .FieldName = It->Name});
					if (!ExtractValueReferences(*Child, Value.Elements[Index], Package, SourcePackage,
						Fingerprint, SourceObjectId, SourceClass, DeclaringType, FieldName, Route, Out, Diagnostic)) return false;
					Route.pop_back();
				}
			}
			else if ((Type.Opcode == ETypeOpcode::FixedArray || Type.Opcode == ETypeOpcode::Array)
				&& Type.ChildTypeIds.size() == 1)
			{
				const FDecodedType* Child = TypeAt(Package, Type.ChildTypeIds[0]); if (!Child) return false;
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					Route.push_back({.Kind = Type.Opcode == ETypeOpcode::FixedArray
						? EAssetReferenceRouteKind::FixedArray : EAssetReferenceRouteKind::ArrayElement,
						.Index = Index});
					if (!ExtractValueReferences(*Child, Value.Elements[Index], Package, SourcePackage,
						Fingerprint, SourceObjectId, SourceClass, DeclaringType, FieldName, Route, Out, Diagnostic)) return false;
					Route.pop_back();
				}
			}
			else if (Type.Opcode == ETypeOpcode::Map && Type.ChildTypeIds.size() == 2)
			{
				const FDecodedType* Key = TypeAt(Package, Type.ChildTypeIds[0]);
				const FDecodedType* Mapped = TypeAt(Package, Type.ChildTypeIds[1]);
				if (!Key || !Mapped) return false;
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
				{
					std::vector<std::byte> Token;
					if (!BuildLedgerMapKeyToken(*Key, Value.Elements[Index], Token, Diagnostic)) return false;
					Route.push_back({.Kind = EAssetReferenceRouteKind::MapValue, .MapKeyToken = std::move(Token)});
					if (!ExtractValueReferences(*Mapped, Value.Elements[Index + 1], Package, SourcePackage,
						Fingerprint, SourceObjectId, SourceClass, DeclaringType, FieldName, Route, Out, Diagnostic)) return false;
					Route.pop_back();
				}
			}
			return true;
		}

		auto CanonicalizeSerializedClassName(
			std::string& Name, const FReflectionSchemaCatalog* Catalog = nullptr) -> void
		{
			if (Catalog)
			{
				const FReflectionSerializedAlias* Alias = Catalog->FindSerializedAlias(Name);
				if (Alias && Alias->Kind == EAssetReflectedIdentityKind::Class)
					Name = Alias->CurrentIdentity;
				return;
			}
			if (DClass* Class = FindClassBySerializedName(FName(Name)))
				Name = Class->GetQualifiedName().ToString();
		}

		auto CanonicalizeSerializedSchemaName(
			std::string& Name, const FReflectionSchemaCatalog* Catalog = nullptr) -> void
		{
			if (Catalog)
			{
				const FReflectionSerializedAlias* Alias = Catalog->FindSerializedAlias(Name);
				if (Alias && (Alias->Kind == EAssetReflectedIdentityKind::Class
					|| Alias->Kind == EAssetReflectedIdentityKind::Struct))
					Name = Alias->CurrentIdentity;
				return;
			}
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
			std::string& Name,
			const FReflectionSchemaCatalog* Catalog = nullptr) -> void
		{
			if (Catalog)
			{
				if (const FReflectionSerializedPropertyAlias* Alias =
					Catalog->FindSerializedPropertyAlias(DeclaringType, Name))
					Name = Alias->CurrentName;
				return;
			}
			DStructBase* Owner = FindClassBySerializedName(FName(DeclaringType));
			if (!Owner) Owner = FindStructBySerializedName(FName(DeclaringType));
			if (Owner)
				if (FProperty* Property = Owner->FindPropertyBySerializedName(FName(Name), false))
					Name = Property->NamePrivate.ToString();
		}

		auto CanonicalizeSerializedTypeName(
			FDecodedType& Type, const FReflectionSchemaCatalog* Catalog = nullptr) -> void
		{
			if (Type.QualifiedName.empty()) return;
			if (Catalog)
			{
				const FReflectionSerializedAlias* Alias = Catalog->FindSerializedAlias(Type.QualifiedName);
				const bool bKindMatches = Alias && (
					(Type.Opcode == ETypeOpcode::Enum && Alias->Kind == EAssetReflectedIdentityKind::Enum)
					|| (Type.Opcode == ETypeOpcode::Struct && Alias->Kind == EAssetReflectedIdentityKind::Struct)
					|| ((Type.Opcode == ETypeOpcode::HardRef || Type.Opcode == ETypeOpcode::SoftRef)
						&& Alias->Kind == EAssetReflectedIdentityKind::Class));
				if (bKindMatches) Type.QualifiedName = Alias->CurrentIdentity;
				return;
			}
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
			const FAssetPath& PackagePath,
			const FReflectionSchemaCatalog* Catalog = nullptr)
			-> std::vector<FAssetCanonicalizationEvidence>
		{
			std::vector<FAssetCanonicalizationEvidence> Result;
			auto AddClass = [&](std::string_view Stored, EAssetSerializedIdentityLocation Location,
				std::string LogicalPath) {
				if (Catalog)
				{
					const FReflectionSerializedAlias* Alias = Catalog->FindSerializedAlias(Stored);
					if (Alias && Alias->Kind == EAssetReflectedIdentityKind::Class)
						Result.push_back({PackagePath, std::string(Stored), Alias->CurrentIdentity,
							Alias->Kind, Location, std::move(LogicalPath)});
					return;
				}
				if (DClass* Class = FindClassBySerializedName(FName(Stored));
					Class && Class->GetQualifiedName().ToString() != Stored)
					Result.push_back({PackagePath, std::string(Stored),
						Class->GetQualifiedName().ToString(), EAssetReflectedIdentityKind::Class,
						Location, std::move(LogicalPath)});
			};
			auto AddStruct = [&](std::string_view Stored, EAssetSerializedIdentityLocation Location,
				std::string LogicalPath) {
				if (Catalog)
				{
					const FReflectionSerializedAlias* Alias = Catalog->FindSerializedAlias(Stored);
					if (Alias && Alias->Kind == EAssetReflectedIdentityKind::Struct)
						Result.push_back({PackagePath, std::string(Stored), Alias->CurrentIdentity,
							Alias->Kind, Location, std::move(LogicalPath)});
					return;
				}
				if (DStruct* Struct = FindStructBySerializedName(FName(Stored));
					Struct && Struct->GetQualifiedName().ToString() != Stored)
					Result.push_back({PackagePath, std::string(Stored),
						Struct->GetQualifiedName().ToString(), EAssetReflectedIdentityKind::Struct,
						Location, std::move(LogicalPath)});
			};
			auto AddEnum = [&](std::string_view Stored, EAssetSerializedIdentityLocation Location,
				std::string LogicalPath) {
				if (Catalog)
				{
					const FReflectionSerializedAlias* Alias = Catalog->FindSerializedAlias(Stored);
					if (Alias && Alias->Kind == EAssetReflectedIdentityKind::Enum)
						Result.push_back({PackagePath, std::string(Stored), Alias->CurrentIdentity,
							Alias->Kind, Location, std::move(LogicalPath)});
					return;
				}
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
				std::string CurrentDeclaringType = Stored;
				if (Catalog)
				{
					if (const FReflectionSerializedAlias* Alias = Catalog->FindSerializedAlias(Stored);
						Alias && (Alias->Kind == EAssetReflectedIdentityKind::Class
							|| Alias->Kind == EAssetReflectedIdentityKind::Struct))
						CurrentDeclaringType = Alias->CurrentIdentity;
					for (size_t FieldIndex = 0; FieldIndex < Package.Schemas[Index].Fields.size(); ++FieldIndex)
					{
						const std::string& FieldName = Package.Schemas[Index].Fields[FieldIndex].Name;
						if (const FReflectionSerializedPropertyAlias* Alias =
							Catalog->FindSerializedPropertyAlias(CurrentDeclaringType, FieldName))
							Result.push_back({PackagePath, FieldName, Alias->CurrentName,
								EAssetReflectedIdentityKind::Property,
								EAssetSerializedIdentityLocation::Schema,
								std::format("schemas[{}].fields[{}].name", Index, FieldIndex)});
					}
				}
				else
				{
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
			std::string* OutError = nullptr,
			const FReflectionSchemaCatalog* Catalog = nullptr) -> bool
		{
			CanonicalizeSerializedClassName(Package.Header.AssetClass, Catalog);
			for (FDecodedObject& Object : Package.Objects)
				CanonicalizeSerializedClassName(Object.ClassName, Catalog);
			for (FDecodedSchema& Schema : Package.Schemas)
			{
				CanonicalizeSerializedSchemaName(Schema.QualifiedName, Catalog);
				std::unordered_set<std::string> CurrentFieldNames;
				for (FDecodedField& Field : Schema.Fields)
				{
					CanonicalizeSerializedPropertyName(Schema.QualifiedName, Field.Name, Catalog);
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
				CanonicalizeSerializedTypeName(Type, Catalog);
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
			const FDecodedPackage& Package, const FReflectionSchemaCatalog* Catalog,
			std::span<const std::pair<FGuid, int32>> Versions, const FAssetPath& PackagePath,
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
					const FReflectionDeprecatedPropertyRoute* SnapshotRoute = Catalog
						? Catalog->FindDeprecatedPropertyRoute(Schema->QualifiedName, Field->Name,
							TypeKind(*ChildType, Package), TypeSignature(*ChildType, Package), Versions)
						: nullptr;
					FProperty* LiveRoute = Catalog ? nullptr
						: FindDecodedDeprecatedRoute(Package, *Schema, *Field, *ChildType);
					if (SnapshotRoute || LiveRoute)
					{
						const FPropertyDeprecation* Deprecation = LiveRoute
							? LiveRoute->GetDeprecation() : nullptr;
						const FGuid VersionGuid = SnapshotRoute
							? SnapshotRoute->CustomVersionGuid : Deprecation->CustomVersionGuid;
						const auto Version = std::ranges::find_if(Versions,
							[&](const auto& Pair) { return Pair.first == VersionGuid; });
						std::vector<std::string> MigrationTargets;
						if (SnapshotRoute) MigrationTargets = SnapshotRoute->MigrationTargets;
						else for (FName Target : Deprecation->MigrationTargets)
							MigrationTargets.push_back(Target.ToString());
						Out.push_back({
							.PackagePath = PackagePath, .ObjectPath = std::string(ObjectPath),
							.DeclaringType = Schema->QualifiedName, .StoredFieldName = Field->Name,
							.DeprecatedPropertyName = SnapshotRoute
								? SnapshotRoute->DeprecatedPropertyName : LiveRoute->NamePrivate.ToString(),
							.MigrationTargets = std::move(MigrationTargets),
							.CustomVersionGuid = VersionGuid,
							.SourceVersion = Version == Versions.end() ? -1 : Version->second,
							.DeprecatedBefore = SnapshotRoute
								? SnapshotRoute->DeprecatedBefore : Deprecation->DeprecatedBefore});
					}
					else GatherNestedDeprecatedRouteEvidence(*ChildType, Value.Elements[Index],
						Package, Catalog, Versions, PackagePath, ObjectPath, Out);
				}
			}
			else if ((Type.Opcode == ETypeOpcode::FixedArray || Type.Opcode == ETypeOpcode::Array)
				&& Type.ChildTypeIds.size() == 1)
			{
				if (const FDecodedType* Child = TypeAt(Package, Type.ChildTypeIds[0]))
					for (const FValue& Element : Value.Elements)
						GatherNestedDeprecatedRouteEvidence(*Child, Element, Package, Catalog,
							Versions, PackagePath, ObjectPath, Out);
			}
			else if (Type.Opcode == ETypeOpcode::Map && Type.ChildTypeIds.size() == 2)
			{
				if (const FDecodedType* Child = TypeAt(Package, Type.ChildTypeIds[1]))
					for (size_t Index = 1; Index < Value.Elements.size(); Index += 2)
						GatherNestedDeprecatedRouteEvidence(*Child, Value.Elements[Index], Package,
							Catalog, Versions, PackagePath, ObjectPath, Out);
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

	auto LoadAssetPackage(std::span<const std::byte> Bytes, const FAssetPath& PackagePath,
		FLoadedAssetPackage& OutPackage, FAssetLoadReport* OutReport,
		const FLiveLoadOptions& Options, const FReaderLimits& Limits,
		FReaderDiagnostic* OutDiagnostic) -> FAssetResult
	{
		FReaderDiagnostic Diagnostic; FDecodedPackage Decoded;
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
		if (!DecodePackageStructure(Bytes, Decoded, Limits, &Diagnostic))
			return Finish({EAssetError::CorruptFile, Diagnostic.Message});
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
		if (Decoded.Objects.empty() || Decoded.Objects.front().ClassName != Decoded.Header.AssetClass)
		{
			Fail(Diagnostic, EReaderFailure::InvalidTopology, "Main object class differs from the public summary.");
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
					Index == 0 ? EObjectFlags::Public : EObjectFlags::NoFlags};
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
			if (Index == 0 && !Package->SetAsset(Object))
			{
				Fail(Diagnostic, EReaderFailure::InvalidTopology, "Could not assign the package main asset."); Rollback();
				return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
			}
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
			FAssetPath Path; std::string PathError;
			if (!FAssetPath::TryCreate(Decoded.Header.Dependencies[Index], Path, &PathError))
			{
				Fail(Diagnostic, EReaderFailure::MissingDependency, PathError); Rollback();
				return Finish({EAssetError::InvalidPath, Diagnostic.Message});
			}
			DObject* Dependency = nullptr; FAssetResult Result = LoadAsset(Path, Dependency);
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
						nullptr, LoadedCustomVersions, PackagePath,
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
				if (!Type || !EncodeInspectionValue(*Type, Override.Value, Decoded, Payload, Diagnostic,
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

	auto InspectPackage(std::span<const std::byte> Bytes, FAssetPackageInspection& OutInspection,
		const FReaderLimits& Limits, FReaderDiagnostic* OutDiagnostic) -> FAssetResult
	{
		FReaderDiagnostic Diagnostic; FDecodedPackage Package;
		if (!DecodePackage(Bytes, Package, Limits, &Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = Diagnostic;
			return {EAssetError::CorruptFile, Diagnostic.Message};
		}
		std::string CanonicalizationError;
		if (!CanonicalizeSerializedReflectionNames(Package, &CanonicalizationError))
		{
			Fail(Diagnostic, EReaderFailure::InvalidTable, CanonicalizationError);
			if (OutDiagnostic) *OutDiagnostic = Diagnostic;
			return {EAssetError::CorruptFile, Diagnostic.Message};
		}
		FAssetPackageInspection Inspection;
		if (!BuildInspection(Package, Bytes, Inspection, Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = Diagnostic;
			return {EAssetError::CorruptFile, Diagnostic.Message};
		}
		OutInspection = std::move(Inspection);
		if (OutDiagnostic) OutDiagnostic->Reset();
		return {};
	}

	auto RequiresDecodedSchemaPayloadValues(
		const FDecodedPackage& Package,
		const FReflectionSchemaCatalog& Catalog) -> bool
	{
		std::vector<std::pair<FGuid, int32>> Versions;
		for (const FCustomVersion& Version : Package.CustomVersions)
			Versions.emplace_back(Version.Guid, static_cast<int32>(Version.Value));
		std::unordered_set<uint64> Visiting;
		std::function<bool(uint64)> ContainsNestedRoute = [&](uint64 TypeId) {
			if (TypeId == 0 || TypeId > Package.Types.size()) return false;
			if (!Visiting.insert(TypeId).second) return false;
			const FDecodedType& Type = Package.Types[static_cast<size_t>(TypeId - 1)];
			bool bFound = false;
			if (Type.Opcode == ETypeOpcode::Struct)
			{
				const auto Schema = std::ranges::find(
					Package.Schemas, Type.QualifiedName, &FDecodedSchema::QualifiedName);
				if (Schema != Package.Schemas.end())
				{
					for (const FDecodedField& Field : Schema->Fields)
					{
						const FDecodedType* FieldType = TypeAt(Package, Field.TypeId);
						if (!FieldType) continue;
						if (Catalog.FindDeprecatedPropertyRoute(Schema->QualifiedName, Field.Name,
							TypeKind(*FieldType, Package), TypeSignature(*FieldType, Package), Versions)
							|| ContainsNestedRoute(Field.TypeId))
						{
							bFound = true;
							break;
						}
					}
				}
			}
			else if ((Type.Opcode == ETypeOpcode::FixedArray || Type.Opcode == ETypeOpcode::Array)
				&& Type.ChildTypeIds.size() == 1)
				bFound = ContainsNestedRoute(Type.ChildTypeIds[0]);
			else if (Type.Opcode == ETypeOpcode::Map && Type.ChildTypeIds.size() == 2)
				bFound = ContainsNestedRoute(Type.ChildTypeIds[1]);
			Visiting.erase(TypeId);
			return bFound;
		};
		for (const FDecodedObjectValues& Values : Package.ObjectValues)
			for (const FDecodedOverride& Override : Values.Overrides)
			{
				const FDecodedSchema* Schema = SchemaAt(Package, Override.SchemaId);
				if (!Schema || Override.FieldId == 0 || Override.FieldId > Schema->Fields.size())
					continue;
				if (ContainsNestedRoute(Schema->Fields[static_cast<size_t>(Override.FieldId - 1)].TypeId))
					return true;
			}
		return false;
	}

	auto InspectDecodedPackageSchema(FDecodedPackage Package, uint64,
		bool bPayloadValuesDecoded, const FAssetPath& PackagePath,
		const FReflectionSchemaCatalog& Catalog,
		FPackageSchemaInspection& OutRecord,
		FReaderDiagnostic* OutDiagnostic) -> FAssetResult
	{
		FReaderDiagnostic Diagnostic;
		std::vector<FAssetCanonicalizationEvidence> CanonicalizationEvidence =
			GatherCanonicalizationEvidence(Package, PackagePath, &Catalog);
		std::string CanonicalizationError;
		if (!CanonicalizeSerializedReflectionNames(Package, &CanonicalizationError, &Catalog))
		{
			if (OutDiagnostic)
				Fail(*OutDiagnostic, EReaderFailure::InvalidTable, CanonicalizationError);
			return {EAssetError::CorruptFile, CanonicalizationError};
		}
		FPackageSchemaInspection Record{
			.FormatVersion = Version,
			.EntryKind = Package.Header.EntryKind,
			.Status = EPackageSchemaStatus::Compatible,
			.CanonicalizationEvidence = std::move(CanonicalizationEvidence)};
		for (const std::string& Dependency : Package.Header.Dependencies)
		{
			FAssetPath Path; if (FAssetPath::TryCreate(Dependency, Path)) Record.Dependencies.push_back(std::move(Path));
		}
		std::vector<std::pair<FGuid, int32>> SourceVersions;
		for (const FCustomVersion& CustomVersion : Package.CustomVersions)
			SourceVersions.emplace_back(
				CustomVersion.Guid, static_cast<int32>(CustomVersion.Value));
		for (size_t ObjectIndex = 0; ObjectIndex < Package.Objects.size(); ++ObjectIndex)
		{
			const FDecodedObject& Object = Package.Objects[ObjectIndex];
			const FReflectionSchemaClass* Class = Catalog.FindClass(Object.ClassName);
			if (!Class)
			{
				Record.Status = EPackageSchemaStatus::Unsupported;
				Record.Issues.push_back({.Code = EPackageSchemaIssueCode::UnavailableClass,
					.ObjectPath = Object.Path, .ClassIdentity = Object.ClassName,
					.Diagnostic = "Serialized class is unavailable."});
				continue;
			}
			for (const FDecodedOverride& Override : Package.ObjectValues[ObjectIndex].Overrides)
			{
				const FDecodedSchema* Schema = SchemaAt(Package, Override.SchemaId);
				if (!Schema || Override.FieldId == 0 || Override.FieldId > Schema->Fields.size()) continue;
				const FDecodedField& Field = Schema->Fields[static_cast<size_t>(Override.FieldId - 1)];
				const FDecodedType* Type = TypeAt(Package, Field.TypeId);
				const auto StoredKind = Override.Provenance == 2 || !Type
					? DurinCodeGen::EPropertyGenFlags::None : TypeKind(*Type, Package);
				const std::string StoredSignature = Override.Provenance == 2 || !Type
					? "DASTv4:RetainedClosure" : TypeSignature(*Type, Package);
				const size_t NestedEvidenceBegin = Record.DeprecatedRouteEvidence.size();
				if (bPayloadValuesDecoded && Type) GatherNestedDeprecatedRouteEvidence(*Type, Override.Value, Package,
					&Catalog, SourceVersions, PackagePath, Object.Path,
					Record.DeprecatedRouteEvidence);
				for (size_t EvidenceIndex = NestedEvidenceBegin;
					EvidenceIndex < Record.DeprecatedRouteEvidence.size(); ++EvidenceIndex)
				{
					const auto& Evidence = Record.DeprecatedRouteEvidence[EvidenceIndex];
					Record.Issues.push_back({
						.Code = EPackageSchemaIssueCode::DeprecatedRouteUsed,
						.ObjectPath = Object.Path, .ClassIdentity = Object.ClassName,
						.DeclaringType = Evidence.DeclaringType,
						.FieldName = Evidence.StoredFieldName,
						.Diagnostic = "Nested serialized field is consumed by a versioned deprecated route."});
				}
				const FReflectionDeprecatedPropertyRoute* DeprecatedRoute =
					Override.Provenance == 2 ? nullptr : Catalog.FindDeprecatedPropertyRoute(
						Schema->QualifiedName, Field.Name, StoredKind, StoredSignature,
						SourceVersions);
				if (DeprecatedRoute)
				{
					const auto Version = std::ranges::find_if(SourceVersions,
						[&](const auto& Pair) { return Pair.first == DeprecatedRoute->CustomVersionGuid; });
					const int32 SourceVersion = Version == SourceVersions.end()
						? -1 : Version->second;
					Record.Issues.push_back({
						.Code = EPackageSchemaIssueCode::DeprecatedRouteUsed,
						.ObjectPath = Object.Path, .ClassIdentity = Object.ClassName,
						.DeclaringType = Schema->QualifiedName, .FieldName = Field.Name,
						.StoredKind = StoredKind, .StoredTypeSignature = StoredSignature,
						.ExpectedKind = DeprecatedRoute->Kind,
						.ExpectedTypeSignature = DeprecatedRoute->TypeSignature,
						.PayloadSize = Override.PayloadSize, .PayloadOffset = Override.PayloadOffset,
						.Diagnostic = "Serialized field is consumed by a versioned deprecated route."});
					Record.DeprecatedRouteEvidence.push_back({
						.PackagePath = PackagePath, .ObjectPath = Object.Path,
						.DeclaringType = Schema->QualifiedName, .StoredFieldName = Field.Name,
						.DeprecatedPropertyName = DeprecatedRoute->DeprecatedPropertyName,
						.MigrationTargets = DeprecatedRoute->MigrationTargets,
						.CustomVersionGuid = DeprecatedRoute->CustomVersionGuid,
						.SourceVersion = SourceVersion,
						.DeprecatedBefore = DeprecatedRoute->DeprecatedBefore});
					continue;
				}
				const FReflectionSchemaField* Expected = Catalog.FindField(*Class, Schema->QualifiedName, Field.Name);
				if (!Expected)
				{
					if (Record.Status == EPackageSchemaStatus::Compatible)
						Record.Status = EPackageSchemaStatus::Incompatible;
					Record.Issues.push_back({.Code = EPackageSchemaIssueCode::UnknownField,
						.ObjectPath = Object.Path, .ClassIdentity = Object.ClassName,
						.DeclaringType = Schema->QualifiedName, .FieldName = Field.Name,
						.StoredKind = StoredKind, .StoredTypeSignature = StoredSignature,
						.PayloadSize = Override.PayloadSize, .PayloadOffset = Override.PayloadOffset,
						.Diagnostic = "Serialized field is not present in the current reflection catalog."});
				}
				else if (Expected->Kind != StoredKind || Expected->TypeSignature != StoredSignature)
				{
					if (Record.Status == EPackageSchemaStatus::Compatible)
						Record.Status = EPackageSchemaStatus::Incompatible;
					Record.Issues.push_back({.Code = EPackageSchemaIssueCode::IncompatibleFieldSignature,
						.ObjectPath = Object.Path, .ClassIdentity = Object.ClassName,
						.DeclaringType = Schema->QualifiedName, .FieldName = Field.Name,
						.StoredKind = StoredKind, .StoredTypeSignature = StoredSignature,
						.ExpectedKind = Expected->Kind, .ExpectedTypeSignature = Expected->TypeSignature,
						.PayloadSize = Override.PayloadSize, .PayloadOffset = Override.PayloadOffset,
						.Diagnostic = "Serialized field signature differs from the current reflection catalog."});
				}
			}
		}
		OutRecord = std::move(Record);
		if (OutDiagnostic) OutDiagnostic->Reset();
		return {};
	}

	auto InspectSchema(std::span<const std::byte> Bytes, const FAssetPath& PackagePath,
		const FReflectionSchemaCatalog& Catalog, FPackageSchemaInspection& OutRecord,
		FPackageSchemaReadStats* OutStats, const FReaderLimits& Limits,
		FReaderDiagnostic* OutDiagnostic) -> FAssetResult
	{
		FReaderDiagnostic Diagnostic;
		FDecodedPackage Package;
		if (!DecodePackageDescriptors(Bytes, Package, Limits, &Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = Diagnostic;
			return {EAssetError::CorruptFile, Diagnostic.Message};
		}
		const bool bNeedsPayloadValues =
			RequiresDecodedSchemaPayloadValues(Package, Catalog);
		if (bNeedsPayloadValues && !DecodePackage(Bytes, Package, Limits, &Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = Diagnostic;
			return {EAssetError::CorruptFile, Diagnostic.Message};
		}
		FAssetResult Result = InspectDecodedPackageSchema(Package, Bytes.size(),
			bNeedsPayloadValues, PackagePath, Catalog, OutRecord, &Diagnostic);
		if (!Result)
		{
			if (OutDiagnostic) *OutDiagnostic = Diagnostic;
			return Result;
		}
		if (OutStats)
		{
			OutStats->PayloadBytesSkipped = 0;
			if (!bNeedsPayloadValues)
				for (const auto& Object : Package.ObjectValues)
					for (const auto& Override : Object.Overrides)
						OutStats->PayloadBytesSkipped += Override.PayloadSize;
			OutStats->MetadataBytesRead = bNeedsPayloadValues ? Bytes.size()
				: Package.Header.BytesRead + Package.Header.Sections[0].Length
					+ Package.Header.Sections[1].Length + Package.Header.Sections[2].Length
					+ Package.Header.Sections[3].Length;
			OutStats->PeakMetadataBytes = OutStats->MetadataBytesRead;
		}
		if (OutDiagnostic) OutDiagnostic->Reset();
		return {};
	}

	auto RewriteReferences(
		std::span<const std::byte> Bytes,
		std::span<const FAssetRedirectorFixupMapping> Mappings,
		uint64 ExpectedRewriteCount,
		std::vector<std::byte>& OutBytes) -> FAssetResult
	{
		auto FindDestination = [&](const FAssetPath& Source) -> const FAssetPath* {
			const auto It = std::ranges::find(
				Mappings, Source, &FAssetRedirectorFixupMapping::RedirectorPath);
			return It == Mappings.end() ? nullptr : &It->FinalPath;
		};
		FDecodedPackage Package;
		FReaderDiagnostic Diagnostic;
		if (!DecodePackage(Bytes, Package, {}, &Diagnostic))
			return {EAssetError::CorruptFile, Diagnostic.Message};

		std::vector<std::string> RewrittenDependencies = Package.Header.Dependencies;
		for (std::string& Dependency : RewrittenDependencies)
		{
			FAssetPath Path;
			if (!FAssetPath::TryCreate(Dependency, Path))
				return {EAssetError::CorruptFile, "Invalid dependency path."};
			if (const FAssetPath* Destination = FindDestination(Path))
				Dependency = Destination->ToString();
		}
		std::vector<std::string> CanonicalDependencies = RewrittenDependencies;
		std::ranges::sort(CanonicalDependencies);
		CanonicalDependencies.erase(
			std::unique(CanonicalDependencies.begin(), CanonicalDependencies.end()),
			CanonicalDependencies.end());
		std::vector<uint64> DependencyIds(RewrittenDependencies.size());
		for (size_t Index = 0; Index < RewrittenDependencies.size(); ++Index)
			DependencyIds[Index] = static_cast<uint64>(
				std::ranges::lower_bound(CanonicalDependencies, RewrittenDependencies[Index])
				- CanonicalDependencies.begin() + 1);

		uint64 RewriteCount = 0;
		std::function<FAssetResult(uint64, FValue&)> RewriteValue;
		RewriteValue = [&](uint64 TypeId, FValue& Value) -> FAssetResult {
			if (TypeId == 0 || TypeId > Package.Types.size())
				return {EAssetError::CorruptFile, "Asset reference type id is invalid."};
			const FDecodedType& Type = Package.Types[static_cast<size_t>(TypeId - 1)];
			if (Type.Opcode == ETypeOpcode::HardRef && Value.ReferenceTag == 2)
			{
				if (Value.ReferenceId == 0 || Value.ReferenceId > DependencyIds.size())
					return {EAssetError::CorruptFile, "Hard reference dependency id is invalid."};
				const size_t ReferenceIndex = static_cast<size_t>(Value.ReferenceId - 1);
				if (DependencyIds[ReferenceIndex] != Value.ReferenceId
					|| RewrittenDependencies[ReferenceIndex]
						!= Package.Header.Dependencies[ReferenceIndex])
					++RewriteCount;
				Value.ReferenceId = DependencyIds[ReferenceIndex];
				return {};
			}
			if (Type.Opcode == ETypeOpcode::SoftRef && Value.ReferenceTag == 1)
			{
				FAssetPath Path;
				if (!FAssetPath::TryCreate(Value.Text, Path))
					return {EAssetError::CorruptFile, "Soft reference path is invalid."};
				if (const FAssetPath* Destination = FindDestination(Path))
				{
					Value.Text = Destination->ToString();
					Package.Names.push_back(Value.Text);
					++RewriteCount;
				}
				return {};
			}
			if (Type.Opcode == ETypeOpcode::Struct)
			{
				const auto Schema = std::ranges::find(
					Package.Schemas, Type.QualifiedName, &FDecodedSchema::QualifiedName);
				if (Schema == Package.Schemas.end()
					|| Value.Elements.size() != Schema->Fields.size())
					return {EAssetError::CorruptFile, "Struct reference schema is invalid."};
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
					if (FAssetResult Result = RewriteValue(
						Schema->Fields[Index].TypeId, Value.Elements[Index]); !Result)
						return Result;
				return {};
			}
			if (Type.ChildTypeIds.empty()) return {};
			for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
			{
				const size_t ChildIndex = Type.ChildTypeIds.size() == 1
					? 0 : Index % Type.ChildTypeIds.size();
				if (FAssetResult Result = RewriteValue(
					Type.ChildTypeIds[ChildIndex], Value.Elements[Index]); !Result)
					return Result;
			}
			return {};
		};
		for (FDecodedObjectValues& Object : Package.ObjectValues)
			for (FDecodedOverride& Override : Object.Overrides)
			{
				if (Override.SchemaId == 0 || Override.SchemaId > Package.Schemas.size())
					return {EAssetError::CorruptFile, "Override schema id is invalid."};
				const auto& Schema = Package.Schemas[static_cast<size_t>(Override.SchemaId - 1)];
				if (Override.FieldId == 0 || Override.FieldId > Schema.Fields.size())
					return {EAssetError::CorruptFile, "Override field id is invalid."};
				if (FAssetResult Result = RewriteValue(
					Schema.Fields[static_cast<size_t>(Override.FieldId - 1)].TypeId,
					Override.Value); !Result)
					return Result;
			}
		Package.Header.Dependencies = std::move(CanonicalDependencies);
		if (Package.Header.EntryKind == EAssetRegistryEntryKind::Redirector)
		{
			FAssetPath Redirect;
			if (!FAssetPath::TryCreate(Package.Header.RedirectDestination, Redirect))
				return {EAssetError::CorruptFile, "Redirect destination is invalid."};
			if (const FAssetPath* Destination = FindDestination(Redirect))
				Package.Header.RedirectDestination = Destination->ToString();
		}
		if (ExpectedRewriteCount != std::numeric_limits<uint64>::max()
			&& RewriteCount != ExpectedRewriteCount)
			return {EAssetError::InUse, std::format(
				"AssetReferenceFixupStaleIndex: expected {} occurrence(s), parsed {}.",
				ExpectedRewriteCount, RewriteCount)};
		if (!ReencodePackage(Package, OutBytes, &Diagnostic))
			return {EAssetError::CorruptFile, Diagnostic.Message};
		return {};
	}

	auto RelocatePackage(
		std::span<const std::byte> Bytes,
		const FAssetPath& DestinationPath,
		std::vector<std::byte>& OutBytes) -> FAssetResult
	{
		FDecodedPackage Package;
		FReaderDiagnostic Diagnostic;
		if (!DecodePackage(Bytes, Package, {}, &Diagnostic))
			return {EAssetError::CorruptFile, Diagnostic.Message};
		if (Package.Header.EntryKind != EAssetRegistryEntryKind::Asset)
			return {EAssetError::InvalidPackageType,
				"Only a real asset package can be relocated."};
		const auto Root = std::ranges::find(Package.Objects, uint64{0}, &FDecodedObject::OuterId);
		if (Root == Package.Objects.end())
			return {EAssetError::InvalidObjectGraph,
				"The relocation source has no valid main object."};
		const std::string OldRootPath = Root->Path;
		const std::string NewRootPath(DestinationPath.GetAssetName());
		for (FDecodedObject& Object : Package.Objects)
			if (Object.Path == OldRootPath || Object.Path.starts_with(OldRootPath + "/"))
				Object.Path.replace(0, OldRootPath.size(), NewRootPath);
		Root->ObjectName = NewRootPath;
		if (!ReencodePackage(Package, OutBytes, &Diagnostic))
			return {EAssetError::CorruptFile, Diagnostic.Message};
		return {};
	}
}
