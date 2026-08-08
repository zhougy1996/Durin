#include "PackageV4Feasibility.h"

#include "DObject/DurinPropertyTypes.h"
#include "Hash/XxHash.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstring>
#include <format>
#include <map>
#include <unordered_map>

namespace Durin::Testing::DastV4
{
	namespace
	{
		constexpr uint32 DastV3 = 3;
		constexpr uint64 MaximumV3FieldCount = 100000;

		struct FParsedValue
		{
			FValue Value;
			std::vector<std::pair<std::string, FParsedValue>> StructFields;
			std::vector<FParsedValue> Elements;
		};

		struct FParsedField
		{
			std::string DeclaringType;
			std::string Name;
			FTypePtr Type;
			FParsedValue Value;
		};

		struct FParsedObject
		{
			uint64 Id = 0;
			uint64 OuterId = 0;
			std::string ClassName;
			std::string ObjectName;
			std::vector<FParsedField> Fields;
		};

		struct FParsedPackage
		{
			FPublicSummary Summary;
			std::vector<FParsedObject> Objects;
			std::map<std::string, std::map<std::string, FTypePtr>> Schemas;
			std::vector<std::string> DiscoveredNames;
			uint32 MaximumNesting = 0;
		};

		struct FReader
		{
			std::span<const uint8> Bytes;
			size_t Offset = 0;

			template<typename T>
			auto Read(T& Out, size_t End) -> bool
			{
				if (Offset > End || sizeof(T) > End - Offset) return false;
				std::memcpy(&Out, Bytes.data() + Offset, sizeof(T));
				Offset += sizeof(T);
				return true;
			}

			auto ReadBytes(size_t Count, size_t End, std::span<const uint8>& Out) -> bool
			{
				if (Offset > End || Count > End - Offset) return false;
				Out = Bytes.subspan(Offset, Count);
				Offset += Count;
				return true;
			}
		};

		auto Fail(std::string& OutError, std::string_view Message) -> bool
		{
			OutError.assign(Message);
			return false;
		}

		auto ReadString(FReader& Reader, size_t End, std::string& Out, std::string& Error) -> bool
		{
			uint64 Size = 0;
			std::span<const uint8> Data;
			if (!Reader.Read(Size, End) || Size > MaximumStringBytes
				|| !Reader.ReadBytes(static_cast<size_t>(Size), End, Data))
				return Fail(Error, "invalid v3 string extent");
			Out.assign(reinterpret_cast<const char*>(Data.data()), Data.size());
			return IsValidUtf8(Out) || Fail(Error, "invalid v3 UTF-8 string");
		}

		auto ParseUnsigned(std::string_view Text, uint64& Out) -> bool
		{
			const auto Result = std::from_chars(Text.data(), Text.data() + Text.size(), Out);
			return Result.ec == std::errc{} && Result.ptr == Text.data() + Text.size();
		}

		auto FindTopLevelComma(std::string_view Text) -> size_t
		{
			uint32 Depth = 0;
			for (size_t Index = 0; Index < Text.size(); ++Index)
			{
				if (Text[Index] == '<') ++Depth;
				else if (Text[Index] == '>') { if (Depth == 0) return std::string_view::npos; --Depth; }
				else if (Text[Index] == ',' && Depth == 0) return Index;
			}
			return std::string_view::npos;
		}

		auto ScalarOpcode(uint64 Kind) -> ETypeOpcode
		{
			using E = DurinCodeGen::EPropertyGenFlags;
			switch (E(Kind))
			{
			case E::Bool: return ETypeOpcode::Bool;
			case E::Int8: return ETypeOpcode::I8;
			case E::Int16: return ETypeOpcode::I16;
			case E::Int32: return ETypeOpcode::I32;
			case E::Int64: return ETypeOpcode::I64;
			case E::UInt8: return ETypeOpcode::U8;
			case E::UInt16: return ETypeOpcode::U16;
			case E::UInt32: return ETypeOpcode::U32;
			case E::UInt64: return ETypeOpcode::U64;
			case E::Float: return ETypeOpcode::F32;
			case E::Double: return ETypeOpcode::F64;
			default: return ETypeOpcode::Bytes;
			}
		}

		auto ParseType(std::string_view Signature, FTypePtr& Out, std::string& Error) -> bool
		{
			if (Signature.starts_with("Native<"))
			{
				const size_t Version = Signature.rfind(">:v");
				uint64 Ignored = 0;
				if (Version == std::string_view::npos || Version < 7
					|| !ParseUnsigned(Signature.substr(Version + 3), Ignored))
					return Fail(Error, "invalid native logical type signature");
				return ParseType(Signature.substr(7, Version - 7), Out, Error);
			}
			if (Signature.starts_with("Array<") && Signature.ends_with('>'))
			{
				FTypePtr Element;
				if (!ParseType(Signature.substr(6, Signature.size() - 7), Element, Error)) return false;
				Out = MakeType(ETypeOpcode::Array, {}, 0, {Element});
				return true;
			}
			if (Signature.starts_with("Map<") && Signature.ends_with('>'))
			{
				const std::string_view Body = Signature.substr(4, Signature.size() - 5);
				const size_t Comma = FindTopLevelComma(Body);
				FTypePtr Key, Value;
				if (Comma == std::string_view::npos || !ParseType(Body.substr(0, Comma), Key, Error)
					|| !ParseType(Body.substr(Comma + 1), Value, Error))
					return Fail(Error, "invalid map logical type signature");
				Out = MakeType(ETypeOpcode::Map, {}, 0, {Key, Value});
				return true;
			}
			if (Signature.starts_with("Struct<") && Signature.ends_with('>'))
			{
				Out = MakeType(ETypeOpcode::Struct,
					std::string(Signature.substr(7, Signature.size() - 8)));
				return true;
			}
			if (Signature.starts_with("Object:"))
			{
				const size_t Last = Signature.rfind(':');
				Out = MakeType(ETypeOpcode::HardRef, Last > 7
					? std::string(Signature.substr(7, Last - 7)) : std::string{});
				return true;
			}
			if (Signature.starts_with("SoftObject:"))
			{
				const size_t Last = Signature.rfind(':');
				Out = MakeType(ETypeOpcode::SoftRef, Last > 11
					? std::string(Signature.substr(11, Last - 11)) : std::string{});
				return true;
			}
			if (Signature.starts_with("Enum:"))
			{
				const size_t Last = Signature.rfind(':');
				uint64 Bytes = 0;
				if (Last <= 5 || !ParseUnsigned(Signature.substr(Last + 1), Bytes)
					|| (Bytes != 1 && Bytes != 2 && Bytes != 4 && Bytes != 8))
					return Fail(Error, "invalid enum logical type signature");
				const ETypeOpcode Storage = Bytes == 1 ? ETypeOpcode::U8 : Bytes == 2
					? ETypeOpcode::U16 : Bytes == 4 ? ETypeOpcode::U32 : ETypeOpcode::U64;
				Out = MakeType(ETypeOpcode::Enum,
					std::string(Signature.substr(5, Last - 5)), uint8(Storage));
				return true;
			}
			const size_t Colon = Signature.rfind(':');
			uint64 Kind = 0;
			if (Colon == std::string_view::npos || !ParseUnsigned(Signature.substr(0, Colon), Kind))
				return Fail(Error, "unknown v3 logical type signature");
			using E = DurinCodeGen::EPropertyGenFlags;
			if (E(Kind) == E::String) Out = MakeType(ETypeOpcode::String);
			else if (E(Kind) == E::Name) Out = MakeType(ETypeOpcode::Name);
			else if (E(Kind) == E::Guid) Out = MakeType(ETypeOpcode::Guid);
			else
			{
				const ETypeOpcode Opcode = ScalarOpcode(Kind);
				if (Opcode == ETypeOpcode::Bytes) return Fail(Error, "unsupported v3 scalar kind");
				Out = MakeType(Opcode);
			}
			return true;
		}

		auto IsSigned(ETypeOpcode Opcode) -> bool
		{
			return Opcode >= ETypeOpcode::I8 && Opcode <= ETypeOpcode::I64;
		}

		auto ScalarBytes(ETypeOpcode Opcode) -> size_t
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

		auto ParseOneValue(FReader& Reader, size_t End, const FTypePtr& Type,
			FParsedPackage& Package, FParsedValue& Out, uint32 Depth, std::string& Error) -> bool;

		auto ParseField(FReader& Reader, size_t End, FParsedPackage& Package,
			FParsedField& Out, uint32 Depth, std::string& Error) -> bool
		{
			uint8 IgnoredKind = 0;
			std::string Signature;
			uint64 PayloadSize = 0;
			if (!ReadString(Reader, End, Out.DeclaringType, Error)
				|| !ReadString(Reader, End, Out.Name, Error) || !Reader.Read(IgnoredKind, End)
				|| !ReadString(Reader, End, Signature, Error) || !Reader.Read(PayloadSize, End)
				|| PayloadSize > End - Reader.Offset || !ParseType(Signature, Out.Type, Error))
				return Fail(Error, "invalid v3 field record");
			const size_t PayloadEnd = Reader.Offset + static_cast<size_t>(PayloadSize);
			std::vector<FParsedValue> Repeated;
			while (Reader.Offset < PayloadEnd)
			{
				FParsedValue Value;
				const size_t Before = Reader.Offset;
				if (!ParseOneValue(Reader, PayloadEnd, Out.Type, Package, Value, Depth, Error)) return false;
				if (Reader.Offset <= Before) return Fail(Error, "v3 field value made no progress");
				Repeated.push_back(std::move(Value));
			}
			if (Repeated.empty()) return Fail(Error, "empty v3 field payload");
			if (Repeated.size() == 1) Out.Value = std::move(Repeated.front());
			else
			{
				Out.Type = MakeType(ETypeOpcode::FixedArray, {}, Repeated.size(), {Out.Type});
				Out.Value.Elements = std::move(Repeated);
			}
			Package.Schemas[Out.DeclaringType].try_emplace(Out.Name, Out.Type);
			return true;
		}

		auto ParseOneValue(FReader& Reader, size_t End, const FTypePtr& Type,
			FParsedPackage& Package, FParsedValue& Out, uint32 Depth, std::string& Error) -> bool
		{
			if (Depth > MaximumValueDepth) return Fail(Error, "v3 nesting exceeds v4 bound");
			Package.MaximumNesting = std::max(Package.MaximumNesting, Depth);
			const size_t Width = ScalarBytes(Type->Opcode);
			if (Width != 0)
			{
				std::span<const uint8> Data;
				if (!Reader.ReadBytes(Width, End, Data)) return Fail(Error, "truncated v3 scalar");
				uint64 Bits = 0;
				std::memcpy(&Bits, Data.data(), Width);
				if (Type->Opcode == ETypeOpcode::Bool) Out.Value.Bool = Bits != 0;
				else if (Type->Opcode == ETypeOpcode::F32)
					Out.Value.Number = std::bit_cast<float>(uint32(Bits));
				else if (Type->Opcode == ETypeOpcode::F64) Out.Value.Number = std::bit_cast<double>(Bits);
				else if (IsSigned(Type->Opcode))
				{
					if (Width < 8 && (Bits & (uint64(1) << (Width * 8 - 1)))) Bits |= ~uint64(0) << (Width * 8);
					Out.Value.Signed = static_cast<int64>(Bits);
				}
				else Out.Value.Unsigned = Bits;
				return true;
			}
			switch (Type->Opcode)
			{
			case ETypeOpcode::String:
			case ETypeOpcode::Name:
				if (!ReadString(Reader, End, Out.Value.Text, Error)) return false;
				if (Type->Opcode == ETypeOpcode::Name) Package.DiscoveredNames.push_back(Out.Value.Text);
				return true;
			case ETypeOpcode::Guid:
				return Reader.Read(Out.Value.Guid.A, End) && Reader.Read(Out.Value.Guid.B, End)
					&& Reader.Read(Out.Value.Guid.C, End) && Reader.Read(Out.Value.Guid.D, End)
					|| Fail(Error, "truncated v3 guid");
			case ETypeOpcode::Enum:
			{
				const size_t EnumBytes = Type->Parameter == uint8(ETypeOpcode::U8) ? 1
					: Type->Parameter == uint8(ETypeOpcode::U16) ? 2
					: Type->Parameter == uint8(ETypeOpcode::U32) ? 4 : 8;
				std::span<const uint8> Data;
				if (!Reader.ReadBytes(EnumBytes, End, Data)) return Fail(Error, "truncated v3 enum");
				std::memcpy(&Out.Value.Unsigned, Data.data(), EnumBytes);
				return true;
			}
			case ETypeOpcode::HardRef:
			{
				uint8 Tag = 0;
				if (!Reader.Read(Tag, End) || Tag > 2) return Fail(Error, "invalid v3 hard reference");
				Out.Value.ReferenceTag = Tag;
				if (Tag == 1) return Reader.Read(Out.Value.ReferenceId, End)
					|| Fail(Error, "truncated v3 internal reference");
				if (Tag == 2)
				{
					std::string Path;
					if (!ReadString(Reader, End, Path, Error)) return false;
					auto It = std::find(Package.Summary.Dependencies.begin(),
						Package.Summary.Dependencies.end(), Path);
					if (It == Package.Summary.Dependencies.end())
						return Fail(Error, "v3 external reference missing from dependencies");
					Out.Value.ReferenceId = std::distance(Package.Summary.Dependencies.begin(), It) + 1;
				}
				return true;
			}
			case ETypeOpcode::SoftRef:
			{
				uint8 Tag = 0;
				if (!Reader.Read(Tag, End) || Tag > 1) return Fail(Error, "invalid v3 soft reference");
				Out.Value.ReferenceTag = Tag;
				if (Tag == 1)
				{
					if (!ReadString(Reader, End, Out.Value.Text, Error)) return false;
					Package.DiscoveredNames.push_back(Out.Value.Text);
				}
				return true;
			}
			case ETypeOpcode::Struct:
			{
				std::string StructName;
				uint64 Count = 0;
				if (!ReadString(Reader, End, StructName, Error) || StructName != Type->QualifiedName
					|| !Reader.Read(Count, End) || Count > MaximumV3FieldCount)
					return Fail(Error, "invalid v3 struct header");
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					FParsedField Field;
					if (!ParseField(Reader, End, Package, Field, Depth + 1, Error)) return false;
					Out.StructFields.emplace_back(Field.Name, std::move(Field.Value));
				}
				return true;
			}
			case ETypeOpcode::Array:
			case ETypeOpcode::Map:
			{
				uint64 Count = 0;
				if (!Reader.Read(Count, End) || Count > MaximumContainerElements)
					return Fail(Error, "invalid v3 container count");
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					const uint64 Values = Type->Opcode == ETypeOpcode::Map ? 2 : 1;
					for (uint64 Part = 0; Part < Values; ++Part)
					{
						FParsedValue Element;
						const FTypePtr& ElementType = Type->Opcode == ETypeOpcode::Array
							? Type->Children[0] : Type->Children[Part];
						if (!ParseOneValue(Reader, End, ElementType, Package, Element, Depth + 1, Error)) return false;
						Out.Elements.push_back(std::move(Element));
					}
				}
				return true;
			}
			default: return Fail(Error, "unsupported v3 value kind");
			}
		}

		auto ParsePackage(std::span<const uint8> Bytes, FParsedPackage& Out, std::string& Error) -> bool
		{
			FReader Reader{Bytes};
			uint32 MagicValue = 0, VersionValue = 0;
			if (!Reader.Read(MagicValue, Bytes.size()) || !Reader.Read(VersionValue, Bytes.size())
				|| MagicValue != Magic || VersionValue != DastV3
				|| !ReadString(Reader, Bytes.size(), Out.Summary.AssetClass, Error)
				|| !Reader.Read(Out.Summary.EntryKind, Bytes.size())
				|| !ReadString(Reader, Bytes.size(), Out.Summary.RedirectDestination, Error))
				return Fail(Error, "invalid DAST v3 public summary");
			uint64 DependencyCount = 0;
			if (!Reader.Read(DependencyCount, Bytes.size()) || DependencyCount > MaximumDependencies)
				return Fail(Error, "invalid v3 dependency count");
			for (uint64 Index = 0; Index < DependencyCount; ++Index)
			{
				std::string Dependency;
				if (!ReadString(Reader, Bytes.size(), Dependency, Error)) return false;
				Out.Summary.Dependencies.push_back(std::move(Dependency));
			}
			uint64 ObjectCount = 0;
			if (!Reader.Read(ObjectCount, Bytes.size()) || ObjectCount == 0 || ObjectCount > MaximumObjects)
				return Fail(Error, "invalid v3 object count");
			Out.Summary.ObjectCount = ObjectCount;
			for (uint64 ObjectIndex = 0; ObjectIndex < ObjectCount; ++ObjectIndex)
			{
				FParsedObject Object;
				uint64 FieldCount = 0;
				if (!Reader.Read(Object.Id, Bytes.size()) || !Reader.Read(Object.OuterId, Bytes.size())
					|| !ReadString(Reader, Bytes.size(), Object.ClassName, Error)
					|| !ReadString(Reader, Bytes.size(), Object.ObjectName, Error)
					|| !Reader.Read(FieldCount, Bytes.size()) || FieldCount > MaximumV3FieldCount)
					return Fail(Error, "invalid v3 object record");
				for (uint64 FieldIndex = 0; FieldIndex < FieldCount; ++FieldIndex)
				{
					FParsedField Field;
					if (!ParseField(Reader, Bytes.size(), Out, Field, 1, Error)) return false;
					Object.Fields.push_back(std::move(Field));
				}
				Out.Objects.push_back(std::move(Object));
			}
			return Reader.Offset == Bytes.size() || Fail(Error, "trailing v3 package bytes");
		}

		auto MaterializeValue(const FParsedValue& Parsed, const FTypePtr& Type,
			const FFrozenTables& Tables, FValue& Out, uint64& Omitted, std::string& Error,
			const FDefaultDeltaNode* DeltaNode = nullptr) -> bool
		{
			Out = Parsed.Value;
			if (Type->Opcode == ETypeOpcode::FixedArray || Type->Opcode == ETypeOpcode::Array
				|| Type->Opcode == ETypeOpcode::Map)
			{
				Out.Elements.clear();
				for (size_t Index = 0; Index < Parsed.Elements.size(); ++Index)
				{
					const FTypePtr& ElementType = Type->Opcode == ETypeOpcode::Map
						? Type->Children[Index % 2] : Type->Children[0];
					FValue Element;
					const FDefaultDeltaNode* ElementPlan = DeltaNode && Index < DeltaNode->Elements.size()
						? DeltaNode->Elements[Index].get() : nullptr;
					if (!MaterializeValue(Parsed.Elements[Index], ElementType, Tables, Element, Omitted, Error, ElementPlan)) return false;
					Out.Elements.push_back(std::move(Element));
				}
				return true;
			}
			if (Type->Opcode != ETypeOpcode::Struct) return true;
			Out = {};
			const uint64 SchemaId = Tables.SchemaId(Type->QualifiedName);
			if (SchemaId == 0) return Fail(Error, "parsed struct schema was not discovered");
			struct FChanged { uint64 Id; uint8 Provenance; FValue Value; };
			std::vector<FChanged> Changed;
			for (const auto& [Name, Child] : Parsed.StructFields)
			{
				const FDefaultDeltaFieldPlan* DeltaField = nullptr;
				if (DeltaNode)
				{
					auto It = std::ranges::find_if(DeltaNode->Fields,
						[&](const FDefaultDeltaFieldPlan& Candidate) {
							return Candidate.Descriptor.Name.ToString() == Name;
						});
					if (It == DeltaNode->Fields.end()) return Fail(Error, "delta plan is missing a parsed struct field");
					DeltaField = &*It;
					if (DeltaField->Disposition == EDefaultDeltaDisposition::Omitted)
					{
						++Omitted;
						continue;
					}
				}
				const uint64 FieldId = Tables.FieldId(SchemaId, Name);
				if (FieldId == 0) return Fail(Error, "parsed struct field was not discovered");
				const FTypePtr& FieldType = Tables.Schemas[SchemaId - 1].Fields[FieldId - 1].Type;
				FValue Value;
				if (!MaterializeValue(Child, FieldType, Tables, Value, Omitted, Error,
					DeltaField && DeltaField->Value ? DeltaField->Value.get() : nullptr)) return false;
				const uint8 Provenance = DeltaField
					&& DeltaField->Provenance == EDefaultDeltaProvenance::Forced ? 1 : 0;
				Changed.push_back({FieldId, Provenance, std::move(Value)});
			}
			std::sort(Changed.begin(), Changed.end(), [](const FChanged& A, const FChanged& B) { return A.Id < B.Id; });
			for (FChanged& Field : Changed)
			{
				Out.FieldIds.push_back(Field.Id);
				Out.Provenances.push_back(Field.Provenance);
				Out.Elements.push_back(std::move(Field.Value));
			}
			return true;
		}
	}

	auto AdaptArchiveLogicalType(const FArchiveLogicalTypeDescriptor& Input,
		FTypePtr& OutType, std::string& OutError) -> bool
	{
		using K = FArchiveLogicalTypeDescriptor::EKind;
		switch (Input.Kind)
		{
		case K::Scalar:
			if (Input.bFloating) OutType = MakeType(Input.BitWidth == 32 ? ETypeOpcode::F32 : ETypeOpcode::F64);
			else if (Input.bSigned) OutType = MakeType(Input.BitWidth == 8 ? ETypeOpcode::I8 : Input.BitWidth == 16
				? ETypeOpcode::I16 : Input.BitWidth == 32 ? ETypeOpcode::I32 : ETypeOpcode::I64);
			else OutType = MakeType(Input.BitWidth == 8 ? ETypeOpcode::U8 : Input.BitWidth == 16
				? ETypeOpcode::U16 : Input.BitWidth == 32 ? ETypeOpcode::U32 : ETypeOpcode::U64);
			return true;
		case K::Enum:
			OutType = MakeType(ETypeOpcode::Enum, Input.QualifiedType.ToString(), uint8(Input.bSigned
				? (Input.BitWidth == 8 ? ETypeOpcode::I8 : Input.BitWidth == 16 ? ETypeOpcode::I16
					: Input.BitWidth == 32 ? ETypeOpcode::I32 : ETypeOpcode::I64)
				: (Input.BitWidth == 8 ? ETypeOpcode::U8 : Input.BitWidth == 16 ? ETypeOpcode::U16
					: Input.BitWidth == 32 ? ETypeOpcode::U32 : ETypeOpcode::U64)));
			return true;
		case K::String: OutType = MakeType(ETypeOpcode::String); return true;
		case K::Name: OutType = MakeType(ETypeOpcode::Name); return true;
		case K::Guid: OutType = MakeType(ETypeOpcode::Guid); return true;
		case K::Bytes: OutType = MakeType(ETypeOpcode::Bytes); return true;
		case K::Object: OutType = MakeType(ETypeOpcode::HardRef, Input.QualifiedType.ToString()); return true;
		case K::SoftObject: OutType = MakeType(ETypeOpcode::SoftRef, Input.QualifiedType.ToString()); return true;
		case K::Struct: OutType = MakeType(ETypeOpcode::Struct, Input.QualifiedType.ToString()); return true;
		case K::Array: case K::FixedArray:
		{
			FTypePtr Element;
			if (!Input.ElementType || !AdaptArchiveLogicalType(*Input.ElementType, Element, OutError))
				return Fail(OutError, "archive array element type is missing");
			OutType = MakeType(Input.Kind == K::Array ? ETypeOpcode::Array : ETypeOpcode::FixedArray,
				{}, Input.Kind == K::FixedArray ? Input.FixedArrayDimension : 0, {Element});
			return true;
		}
		case K::Map:
		{
			FTypePtr Key, Value;
			if (!Input.KeyType || !Input.ValueType
				|| !AdaptArchiveLogicalType(*Input.KeyType, Key, OutError)
				|| !AdaptArchiveLogicalType(*Input.ValueType, Value, OutError))
				return Fail(OutError, "archive map child type is missing");
			OutType = MakeType(ETypeOpcode::Map, {}, 0, {Key, Value});
			return true;
		}
		}
		return Fail(OutError, "unsupported archive logical type");
	}

	auto AddArchiveDiscoveredField(const FArchiveFieldDescriptor& Field,
		FTableInput& InOutTables, std::string& OutError) -> bool
	{
		if (Field.DeclaringType.IsNone() || Field.Name.IsNone())
			return Fail(OutError, "archive discovered field identity is incomplete");
		FTypePtr Type;
		if (!AdaptArchiveLogicalType(Field.LogicalType, Type, OutError)) return false;
		auto Schema = std::find_if(InOutTables.Schemas.begin(), InOutTables.Schemas.end(),
			[&](const FSchemaDescriptor& Candidate)
			{ return Candidate.QualifiedName == Field.DeclaringType.ToString(); });
		if (Schema == InOutTables.Schemas.end())
		{
			InOutTables.Schemas.push_back({Field.DeclaringType.ToString(), {}});
			Schema = std::prev(InOutTables.Schemas.end());
		}
		if (std::ranges::any_of(Schema->Fields, [&](const FFieldDescriptor& Candidate)
			{ return Candidate.Name == Field.Name.ToString(); }))
			return Fail(OutError, "duplicate archive discovered field");
		Schema->Fields.push_back({Field.Name.ToString(), Type, 0});
		InOutTables.Types.push_back(std::move(Type));
		return true;
	}

	auto BuildFeasibilityPackageFromV3(std::span<const uint8> V3Bytes, bool ReverseDiscovery,
		FFeasibilityPackage& OutPackage, std::string& OutError, const FDefaultDeltaPlan* DeltaPlan) -> bool
	{
		FParsedPackage Parsed;
		if (!ParsePackage(V3Bytes, Parsed, OutError)) return false;
		FTableInput Input;
		Input.PublicDependencyCount = Parsed.Summary.Dependencies.size();
		Input.AdditionalNames = Parsed.DiscoveredNames;
		for (const auto& [SchemaName, Fields] : Parsed.Schemas)
		{
			FSchemaDescriptor Schema{.QualifiedName = SchemaName};
			for (const auto& [FieldName, Type] : Fields)
			{
				Schema.Fields.push_back({FieldName, Type, 0});
				Input.Types.push_back(Type);
			}
			Input.Schemas.push_back(std::move(Schema));
		}
		std::unordered_map<uint64, std::string> ObjectPaths;
		for (const FParsedObject& Object : Parsed.Objects)
		{
			const std::string OuterPath = Object.OuterId == 0 ? std::string{} : ObjectPaths[Object.OuterId];
			if (Object.OuterId != 0 && OuterPath.empty()) return Fail(OutError, "v3 object outer precedes no parent");
			const std::string Path = OuterPath.empty() ? Object.ObjectName : OuterPath + "/" + Object.ObjectName;
			ObjectPaths.emplace(Object.Id, Path);
			Input.Objects.push_back({Path, OuterPath, Object.ClassName, Object.ObjectName});
		}
		if (ReverseDiscovery)
		{
			std::reverse(Input.AdditionalNames.begin(), Input.AdditionalNames.end());
			std::reverse(Input.Types.begin(), Input.Types.end());
			std::reverse(Input.Schemas.begin(), Input.Schemas.end());
			for (FSchemaDescriptor& Schema : Input.Schemas) std::reverse(Schema.Fields.begin(), Schema.Fields.end());
			std::reverse(Input.Objects.begin(), Input.Objects.end());
		}
		FFrozenTables Tables;
		if (!FreezeTables(Input, Tables, OutError)) return false;
		std::vector<FObjectValueInput> ObjectValues;
		uint64 Omitted = 0;
		uint64 OverrideCount = 0;
		for (size_t ObjectIndex = 0; ObjectIndex < Parsed.Objects.size(); ++ObjectIndex)
		{
			const FParsedObject& Object = Parsed.Objects[ObjectIndex];
			const FDefaultDeltaObjectPlan* DeltaObject = DeltaPlan && ObjectIndex < DeltaPlan->Objects.size()
				? &DeltaPlan->Objects[ObjectIndex] : nullptr;
			FObjectValueInput ObjectValue{.ObjectPath = ObjectPaths.at(Object.Id)};
			for (const FParsedField& Field : Object.Fields)
			{
				const FDefaultDeltaFieldPlan* DeltaField = nullptr;
				if (DeltaObject)
				{
					auto It = std::ranges::find_if(DeltaObject->Fields,
						[&](const FDefaultDeltaFieldPlan& Candidate) {
							return Candidate.Descriptor.DeclaringType.ToString() == Field.DeclaringType
								&& Candidate.Descriptor.Name.ToString() == Field.Name;
						});
					if (It == DeltaObject->Fields.end()) return Fail(OutError, "delta plan is missing a parsed object field");
					DeltaField = &*It;
					if (DeltaField->Disposition == EDefaultDeltaDisposition::Omitted)
					{
						++Omitted;
						continue;
					}
				}
				FValue Value;
				if (!MaterializeValue(Field.Value, Field.Type, Tables, Value, Omitted, OutError,
					DeltaField && DeltaField->Value ? DeltaField->Value.get() : nullptr)) return false;
				FOverrideCandidate Override{
					.SchemaName = Field.DeclaringType,
					.FieldName = Field.Name,
					.Value = std::move(Value),
					.LoadedExplicit = DeltaField == nullptr
						|| DeltaField->Provenance == EDefaultDeltaProvenance::Explicit,
					.Forced = DeltaField
						&& DeltaField->Provenance == EDefaultDeltaProvenance::Forced,
				};
				ObjectValue.Overrides.push_back(std::move(Override));
				++OverrideCount;
			}
			ObjectValues.push_back(std::move(ObjectValue));
		}
		std::array<std::vector<uint8>, 4> TableSections;
		std::vector<uint8> ValueSection;
		if (!EncodeTableSections(Tables, TableSections, OutError)
			|| !EncodeValueSection(ObjectValues, Tables, ValueSection, OutError)) return false;
		std::array<std::vector<uint8>, SectionCount> Sections{
			TableSections[0], TableSections[1], TableSections[2], TableSections[3], ValueSection};
		std::vector<uint8> Bytes;
		if (!EncodeEnvelope(Parsed.Summary, Sections, Bytes, OutError)) return false;
		FFeasibilityReport Report;
		for (size_t Index = 0; Index < Sections.size(); ++Index) Report.SectionBytes[Index] = Sections[Index].size();
		Report.TotalBytes = Bytes.size();
		Report.EnvelopeAndDirectoryBytes = Bytes.size();
		for (uint64 SectionSize : Report.SectionBytes) Report.EnvelopeAndDirectoryBytes -= SectionSize;
		Report.NameCount = Tables.Names.size();
		Report.TypeCount = Tables.Types.size();
		Report.SchemaCount = Tables.Schemas.size();
		Report.ObjectCount = Tables.Objects.size();
		Report.OverrideCount = OverrideCount;
		Report.OmittedDefaultCount = Omitted;
		Report.MaximumNesting = Parsed.MaximumNesting;
		Report.ParseOperations = Report.NameCount + Report.TypeCount + Report.SchemaCount
			+ Report.ObjectCount + OverrideCount;
		Report.AllocationInputs = Report.NameCount + Report.TypeCount + Report.SchemaCount
			+ Report.ObjectCount + Parsed.Summary.Dependencies.size();
		Report.Digest = FXxHash64::HashBuffer(Bytes).HashValue;
		OutPackage = {std::move(Bytes), Report};
		return true;
	}
}
