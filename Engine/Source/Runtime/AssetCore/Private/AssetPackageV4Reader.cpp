#include "AssetPackageV4Reader.h"

#include "AssetPackageArchive.h"
#include "AssetPackageValueCodec.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/Package.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace Durin::Asset::DastV4
{
	namespace
	{
		auto Fail(FReaderDiagnostic& Diagnostic, EReaderFailure Failure,
			std::string_view Message, uint64 Offset = 0, std::string Path = {}) -> bool
		{
			if (Diagnostic.Failure == EReaderFailure::None)
				Diagnostic = {Failure, std::move(Path), std::string(Message), Offset};
			return false;
		}

		auto ByteLess(std::string_view Left, std::string_view Right) -> bool
		{
			return std::lexicographical_compare(Left.begin(), Left.end(), Right.begin(), Right.end(),
				[](char A, char B) { return uint8(A) < uint8(B); });
		}

		auto IsValidUtf8(std::string_view Value) -> bool
		{
			for (size_t Index = 0; Index < Value.size();)
			{
				const uint8 Lead = uint8(Value[Index]);
				uint32 Codepoint = 0;
				size_t Length = 0;
				if (Lead <= 0x7f) { Codepoint = Lead; Length = 1; }
				else if (Lead >= 0xc2 && Lead <= 0xdf) { Codepoint = Lead & 0x1f; Length = 2; }
				else if (Lead >= 0xe0 && Lead <= 0xef) { Codepoint = Lead & 0x0f; Length = 3; }
				else if (Lead >= 0xf0 && Lead <= 0xf4) { Codepoint = Lead & 0x07; Length = 4; }
				else return false;
				if (Length > Value.size() - Index) return false;
				for (size_t Continuation = 1; Continuation < Length; ++Continuation)
				{
					const uint8 Byte = uint8(Value[Index + Continuation]);
					if ((Byte & 0xc0) != 0x80) return false;
					Codepoint = (Codepoint << 6) | (Byte & 0x3f);
				}
				if ((Length == 2 && Codepoint < 0x80) || (Length == 3 && Codepoint < 0x800)
					|| (Length == 4 && Codepoint < 0x10000) || Codepoint > 0x10ffff
					|| (Codepoint >= 0xd800 && Codepoint <= 0xdfff) || Codepoint == 0)
					return false;
				Index += Length;
			}
			return true;
		}

		class FWireReader
		{
		public:
			explicit FWireReader(std::span<const uint8> InBytes, uint64 InBaseOffset = 0)
				: Bytes(InBytes), BaseOffset(InBaseOffset) {}

			auto Position() const -> uint64 { return BaseOffset + Offset; }
			auto Remaining() const -> uint64 { return Bytes.size() - Offset; }
			auto IsAtEnd() const -> bool { return Offset == Bytes.size(); }

			auto U8(uint8& Out, FReaderDiagnostic& Diagnostic) -> bool
			{
				if (Offset == Bytes.size()) return Fail(Diagnostic, EReaderFailure::TruncatedInput,
					"Unexpected end of input.", Position());
				Out = Bytes[Offset++]; return true;
			}

			template<typename T>
			auto Fixed(T& Out, FReaderDiagnostic& Diagnostic) -> bool
			{
				static_assert(std::is_unsigned_v<T>);
				if (sizeof(T) > Remaining()) return Fail(Diagnostic, EReaderFailure::TruncatedInput,
					"Truncated fixed-width value.", Position());
				Out = 0;
				for (uint64 Index = 0; Index < sizeof(T); ++Index)
					Out |= T(Bytes[Offset++]) << (Index * 8);
				return true;
			}

			auto VarUInt(uint64& Out, FReaderDiagnostic& Diagnostic) -> bool
			{
				const uint64 Start = Position();
				uint64 Value = 0;
				for (uint32 Index = 0; Index < 10; ++Index)
				{
					uint8 Byte = 0;
					if (!U8(Byte, Diagnostic)) return false;
					const uint8 Payload = Byte & 0x7f;
					if (Index == 9 && Payload > 1)
						return Fail(Diagnostic, EReaderFailure::InvalidPrimitive,
							"VarUInt overflows uint64.", Start);
					Value |= uint64(Payload) << (Index * 7);
					if ((Byte & 0x80) == 0)
					{
						if (Index > 0 && Payload == 0)
							return Fail(Diagnostic, EReaderFailure::InvalidPrimitive,
								"VarUInt is not minimally encoded.", Start);
						Out = Value; return true;
					}
				}
				return Fail(Diagnostic, EReaderFailure::InvalidPrimitive,
					"VarUInt exceeds ten bytes.", Start);
			}

			auto VarInt(int64& Out, FReaderDiagnostic& Diagnostic) -> bool
			{
				uint64 Encoded = 0;
				if (!VarUInt(Encoded, Diagnostic)) return false;
				Out = static_cast<int64>((Encoded >> 1) ^ (uint64(0) - (Encoded & 1)));
				return true;
			}

			auto BytesSpan(uint64 Count, std::span<const uint8>& Out,
				FReaderDiagnostic& Diagnostic) -> bool
			{
				if (Count > Remaining()) return Fail(Diagnostic, EReaderFailure::TruncatedInput,
					"Length-delimited data exceeds its containing extent.", Position());
				Out = Bytes.subspan(Offset, static_cast<size_t>(Count));
				Offset += static_cast<size_t>(Count); return true;
			}

			auto Record(std::span<const uint8>& Out, FReaderDiagnostic& Diagnostic,
				uint64* OutOffset = nullptr) -> bool
			{
				uint64 Length = 0;
				if (!VarUInt(Length, Diagnostic)) return false;
				if (OutOffset) *OutOffset = Position();
				return BytesSpan(Length, Out, Diagnostic);
			}

			auto String(std::string& Out, const FReaderLimits& Limits,
				FReaderDiagnostic& Diagnostic, bool bAllowEmpty = true) -> bool
			{
				const uint64 Start = Position();
				uint64 Length = 0;
				std::span<const uint8> Data;
				if (!VarUInt(Length, Diagnostic)) return false;
				if (Length > Limits.StringBytes)
					return Fail(Diagnostic, EReaderFailure::LimitExceeded,
						"Wire string exceeds the configured bound.", Start);
				if (!BytesSpan(Length, Data, Diagnostic)) return false;
				std::string Result(reinterpret_cast<const char*>(Data.data()), Data.size());
				if ((!bAllowEmpty && Result.empty()) || !IsValidUtf8(Result))
					return Fail(Diagnostic, EReaderFailure::InvalidUtf8,
						"Wire string is empty where forbidden or is not canonical UTF-8.", Start);
				Out = std::move(Result); return true;
			}

			auto RequireEnd(FReaderDiagnostic& Diagnostic, EReaderFailure Failure,
				std::string_view Message) const -> bool
			{
				return IsAtEnd() || Fail(Diagnostic, Failure, Message, Position());
			}

		private:
			std::span<const uint8> Bytes;
			uint64 BaseOffset = 0;
			size_t Offset = 0;
		};

		auto ValidateLimits(const FReaderLimits& Limits, FReaderDiagnostic& Diagnostic) -> bool
		{
			if (Limits.PackageBytes == 0 || Limits.PackageBytes > MaximumPackageBytes
				|| Limits.StringBytes == 0 || Limits.StringBytes > MaximumStringBytes
				|| Limits.TableEntries == 0 || Limits.TableEntries > MaximumTableEntries
				|| Limits.SchemaFields == 0 || Limits.SchemaFields > MaximumSchemaFields
				|| Limits.CustomVersions > MaximumCustomVersions
				|| Limits.Dependencies > MaximumDependencies
				|| Limits.ContainerElements == 0 || Limits.ContainerElements > MaximumContainerElements
				|| Limits.ValueDepth > MaximumValueDepth)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded,
					"Reader limits must be nonzero and may only tighten frozen wire bounds.");
			return true;
		}

		auto DecodeHeaderInner(std::span<const uint8> Bytes, FValidatedHeader& Out,
			const FReaderLimits& Limits, FReaderDiagnostic& Diagnostic) -> bool
		{
			if (!ValidateLimits(Limits, Diagnostic)) return false;
			if (Bytes.size() > Limits.PackageBytes)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded,
					"Package exceeds the configured byte bound.");
			FWireReader Reader(Bytes);
			uint32 ReadMagic = 0, ReadVersion = 0, SummaryLength = 0;
			uint8 SectionCount = 0;
			if (!Reader.Fixed(ReadMagic, Diagnostic) || !Reader.Fixed(ReadVersion, Diagnostic)
				|| !Reader.Fixed(SummaryLength, Diagnostic) || !Reader.U8(SectionCount, Diagnostic)) return false;
			if (ReadMagic != Magic || ReadVersion != Version)
				return Fail(Diagnostic, EReaderFailure::InvalidHeader,
					"Package magic or explicit v4 version is invalid.");
			if (SummaryLength > MaximumSummaryBytes)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded,
					"Public summary exceeds the frozen bound.", 8);
			if (SectionCount != RequiredSectionCount)
				return Fail(Diagnostic, EReaderFailure::InvalidDirectory,
					"DAST v4 requires exactly five sections.", 12);
			std::span<const uint8> SummaryBytes;
			if (!Reader.BytesSpan(SummaryLength, SummaryBytes, Diagnostic)) return false;
			FWireReader Summary(SummaryBytes, 13);
			FValidatedHeader Result;
			if (!Summary.String(Result.AssetClass, Limits, Diagnostic, false)) return false;
			uint8 EntryKind = 0;
			if (!Summary.U8(EntryKind, Diagnostic) || EntryKind > 1)
				return Fail(Diagnostic, EReaderFailure::InvalidHeader,
					"Public summary entry kind is invalid.", Summary.Position());
			Result.EntryKind = static_cast<EAssetRegistryEntryKind>(EntryKind);
			if (!Summary.String(Result.RedirectDestination, Limits, Diagnostic)) return false;
			if ((EntryKind == 0) != Result.RedirectDestination.empty())
				return Fail(Diagnostic, EReaderFailure::InvalidHeader,
					"Redirect summary fields are inconsistent.", Summary.Position());
			uint64 DependencyCount = 0;
			if (!Summary.VarUInt(DependencyCount, Diagnostic)) return false;
			if (DependencyCount > Limits.Dependencies)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded,
					"Dependency count exceeds the configured bound.", Summary.Position());
			Result.Dependencies.reserve(static_cast<size_t>(DependencyCount));
			for (uint64 Index = 0; Index < DependencyCount; ++Index)
			{
				std::string Dependency;
				if (!Summary.String(Dependency, Limits, Diagnostic, false)) return false;
				if (!Result.Dependencies.empty() && !ByteLess(Result.Dependencies.back(), Dependency))
					return Fail(Diagnostic, EReaderFailure::NonCanonical,
						"Dependencies are duplicate or not bytewise sorted.", Summary.Position());
				Result.Dependencies.push_back(std::move(Dependency));
			}
			if (!Summary.VarUInt(Result.ObjectCount, Diagnostic)) return false;
			if (Result.ObjectCount > Limits.TableEntries)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded,
					"Object count exceeds the configured bound.", Summary.Position());
			if (!Summary.RequireEnd(Diagnostic, EReaderFailure::InvalidHeader,
				"Public summary has trailing bytes.")) return false;

			const uint64 FirstSection = 13ull + SummaryLength + RequiredSectionCount * 9ull;
			uint64 ExpectedOffset = FirstSection;
			for (uint8 Index = 0; Index < RequiredSectionCount; ++Index)
			{
				uint8 Kind = 0;
				uint32 Offset = 0, Length = 0;
				if (!Reader.U8(Kind, Diagnostic) || !Reader.Fixed(Offset, Diagnostic)
					|| !Reader.Fixed(Length, Diagnostic)) return false;
				if (Kind != Index + 1 || Offset != ExpectedOffset)
					return Fail(Diagnostic, EReaderFailure::InvalidDirectory,
						"Section kinds or extents are not canonical.", Reader.Position() - 9);
				const uint64 End = uint64(Offset) + Length;
				if (End < Offset || End > Bytes.size())
					return Fail(Diagnostic, EReaderFailure::InvalidDirectory,
						"Section extent exceeds the package.", Reader.Position() - 8);
				Result.Sections[Index] = {static_cast<ESectionKind>(Kind), Offset, Length};
				ExpectedOffset = End;
			}
			if (ExpectedOffset != Bytes.size())
				return Fail(Diagnostic, EReaderFailure::InvalidDirectory,
					"Directory leaves gaps or trailing bytes.", Reader.Position());
			Result.BytesRead = FirstSection;
			Out = std::move(Result);
			return true;
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

		auto SignedFits(ETypeOpcode Opcode, int64 Value) -> bool
		{
			switch (Opcode)
			{
			case ETypeOpcode::I8: return Value >= std::numeric_limits<int8>::min() && Value <= std::numeric_limits<int8>::max();
			case ETypeOpcode::I16: return Value >= std::numeric_limits<int16>::min() && Value <= std::numeric_limits<int16>::max();
			case ETypeOpcode::I32: return Value >= std::numeric_limits<int32>::min() && Value <= std::numeric_limits<int32>::max();
			case ETypeOpcode::I64: return true;
			default: return false;
			}
		}

		auto UnsignedFits(ETypeOpcode Opcode, uint64 Value) -> bool
		{
			switch (Opcode)
			{
			case ETypeOpcode::U8: return Value <= std::numeric_limits<uint8>::max();
			case ETypeOpcode::U16: return Value <= std::numeric_limits<uint16>::max();
			case ETypeOpcode::U32: return Value <= std::numeric_limits<uint32>::max();
			case ETypeOpcode::U64: return true;
			default: return false;
			}
		}

		auto DecodeValue(FWireReader& Reader, const FDecodedType& Type,
			const FDecodedPackage& Package, const FReaderLimits& Limits, uint32 Depth,
			FValue& OutValue, FReaderDiagnostic& Diagnostic, std::string Path) -> bool
		{
			if (Depth > Limits.ValueDepth)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded,
					"Value nesting depth exceeds the configured bound.", Reader.Position(), std::move(Path));
			FValue Value;
			switch (Type.Opcode)
			{
			case ETypeOpcode::Bool:
			{
				uint8 Encoded = 0;
				if (!Reader.U8(Encoded, Diagnostic) || Encoded > 1)
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Bool value is invalid.", Reader.Position(), std::move(Path));
				Value.Bool = Encoded != 0; break;
			}
			case ETypeOpcode::I8: case ETypeOpcode::I16: case ETypeOpcode::I32: case ETypeOpcode::I64:
				if (!Reader.VarInt(Value.Signed, Diagnostic)) return false;
				if (!SignedFits(Type.Opcode, Value.Signed))
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Signed value is out of range.", Reader.Position(), std::move(Path));
				break;
			case ETypeOpcode::U8: case ETypeOpcode::U16: case ETypeOpcode::U32: case ETypeOpcode::U64:
				if (!Reader.VarUInt(Value.Unsigned, Diagnostic)) return false;
				if (!UnsignedFits(Type.Opcode, Value.Unsigned))
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Unsigned value is out of range.", Reader.Position(), std::move(Path));
				break;
			case ETypeOpcode::F32:
			{
				uint32 Bits = 0; if (!Reader.Fixed(Bits, Diagnostic)) return false;
				if (std::isnan(std::bit_cast<float>(Bits)) && Bits != 0x7fc00000u)
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "F32 NaN is not canonical.", Reader.Position(), std::move(Path));
				Value.FloatingBits = Bits; break;
			}
			case ETypeOpcode::F64:
			{
				uint64 Bits = 0; if (!Reader.Fixed(Bits, Diagnostic)) return false;
				if (std::isnan(std::bit_cast<double>(Bits)) && Bits != 0x7ff8000000000000ull)
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "F64 NaN is not canonical.", Reader.Position(), std::move(Path));
				Value.FloatingBits = Bits; break;
			}
			case ETypeOpcode::String:
				if (!Reader.String(Value.Text, Limits, Diagnostic)) return false;
				break;
			case ETypeOpcode::Name:
			{
				uint64 Id = 0; if (!Reader.VarUInt(Id, Diagnostic) || Id == 0 || Id > Package.Names.size())
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Name value id is invalid.", Reader.Position(), std::move(Path));
				Value.Text = Package.Names[static_cast<size_t>(Id - 1)]; break;
			}
			case ETypeOpcode::Guid:
				if (!Reader.Fixed(Value.Guid.A, Diagnostic) || !Reader.Fixed(Value.Guid.B, Diagnostic)
					|| !Reader.Fixed(Value.Guid.C, Diagnostic) || !Reader.Fixed(Value.Guid.D, Diagnostic)) return false;
				break;
			case ETypeOpcode::Enum:
			{
				const auto Storage = static_cast<ETypeOpcode>(Type.Parameter);
				if (Storage >= ETypeOpcode::I8 && Storage <= ETypeOpcode::I64)
				{
					if (!Reader.VarInt(Value.Signed, Diagnostic)) return false;
					if (!SignedFits(Storage, Value.Signed)) return Fail(Diagnostic, EReaderFailure::InvalidValue,
						"Signed enum is out of range.", Reader.Position(), std::move(Path));
				}
				else
				{
					if (!Reader.VarUInt(Value.Unsigned, Diagnostic)) return false;
					if (!UnsignedFits(Storage, Value.Unsigned)) return Fail(Diagnostic, EReaderFailure::InvalidValue,
						"Unsigned enum is out of range.", Reader.Position(), std::move(Path));
				}
				break;
			}
			case ETypeOpcode::Intrinsic:
			{
				const uint64 Count = Type.Parameter == 1 ? 2 : Type.Parameter == 2 ? 3 : Type.Parameter == 5 ? 10 : 4;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					if (Type.Parameter == 6)
					{
						uint32 Bits = 0; if (!Reader.Fixed(Bits, Diagnostic)) return false;
						if (std::isnan(std::bit_cast<float>(Bits)) && Bits != 0x7fc00000u)
							return Fail(Diagnostic, EReaderFailure::InvalidValue, "Intrinsic F32 NaN is not canonical.", Reader.Position(), std::move(Path));
						Value.ComponentBits.push_back(Bits);
					}
					else
					{
						uint64 Bits = 0; if (!Reader.Fixed(Bits, Diagnostic)) return false;
						if (std::isnan(std::bit_cast<double>(Bits)) && Bits != 0x7ff8000000000000ull)
							return Fail(Diagnostic, EReaderFailure::InvalidValue, "Intrinsic F64 NaN is not canonical.", Reader.Position(), std::move(Path));
						Value.ComponentBits.push_back(Bits);
					}
				}
				break;
			}
			case ETypeOpcode::Struct:
			{
				uint64 SchemaId = 0;
				const FDecodedSchema* Schema = FindSchema(Package, Type.QualifiedName, SchemaId);
				uint64 Count = 0;
				if (!Schema || !Reader.VarUInt(Count, Diagnostic) || Count > Limits.SchemaFields)
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Struct schema or changed-field count is invalid.", Reader.Position(), std::move(Path));
				uint64 Previous = 0;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					uint64 FieldId = 0; uint8 Provenance = 0; std::span<const uint8> Encoded; uint64 Offset = 0;
					if (!Reader.VarUInt(FieldId, Diagnostic) || FieldId <= Previous || FieldId > Schema->Fields.size()
						|| !Reader.U8(Provenance, Diagnostic) || Provenance > 1 || !Reader.Record(Encoded, Diagnostic, &Offset))
						return Fail(Diagnostic, EReaderFailure::InvalidValue, "Struct field record is invalid.", Reader.Position(), std::move(Path));
					const FDecodedField& Field = Schema->Fields[static_cast<size_t>(FieldId - 1)];
					const FDecodedType* FieldType = TypeAt(Package, Field.TypeId);
					if (!FieldType) return Fail(Diagnostic, EReaderFailure::InvalidTable, "Struct field type id is invalid.", Offset, std::move(Path));
					FWireReader FieldReader(Encoded, Offset); FValue FieldValue;
					if (!DecodeValue(FieldReader, *FieldType, Package, Limits, Depth + 1, FieldValue, Diagnostic,
						std::format("{}::{}", Schema->QualifiedName, Field.Name))
						|| !FieldReader.RequireEnd(Diagnostic, EReaderFailure::InvalidValue, "Struct field payload has trailing bytes.")) return false;
					Value.FieldNames.push_back(Field.Name);
					Value.Provenances.push_back(Provenance == 0 ? EDefaultDeltaProvenance::Explicit : EDefaultDeltaProvenance::Forced);
					Value.Elements.push_back(std::move(FieldValue)); Previous = FieldId;
				}
				break;
			}
			case ETypeOpcode::FixedArray: case ETypeOpcode::Array:
			{
				if (Type.ChildTypeIds.size() != 1) return Fail(Diagnostic, EReaderFailure::InvalidTable,
					"Container type has invalid child count.", Reader.Position(), std::move(Path));
				const FDecodedType* ElementType = TypeAt(Package, Type.ChildTypeIds[0]);
				if (!ElementType) return Fail(Diagnostic, EReaderFailure::InvalidTable, "Container child type id is invalid.", Reader.Position(), std::move(Path));
				uint64 Count = Type.Parameter;
				if (Type.Opcode == ETypeOpcode::Array && !Reader.VarUInt(Count, Diagnostic)) return false;
				if (Count > Limits.ContainerElements) return Fail(Diagnostic, EReaderFailure::LimitExceeded,
					"Container count exceeds the configured bound.", Reader.Position(), std::move(Path));
				Value.Elements.reserve(static_cast<size_t>(Count));
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					FValue Element;
					if (!DecodeValue(Reader, *ElementType, Package, Limits, Depth + 1, Element, Diagnostic, Path)) return false;
					Value.Elements.push_back(std::move(Element));
				}
				break;
			}
			case ETypeOpcode::Map:
			{
				if (Type.ChildTypeIds.size() != 2) return Fail(Diagnostic, EReaderFailure::InvalidTable,
					"Map type has invalid child count.", Reader.Position(), std::move(Path));
				const FDecodedType* Key = TypeAt(Package, Type.ChildTypeIds[0]);
				const FDecodedType* Mapped = TypeAt(Package, Type.ChildTypeIds[1]);
				uint64 Count = 0;
				if (!Key || !Mapped || !Reader.VarUInt(Count, Diagnostic) || Count > Limits.ContainerElements)
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Map descriptor or count is invalid.", Reader.Position(), std::move(Path));
				Value.Elements.reserve(static_cast<size_t>(Count * 2));
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					FValue KeyValue, MappedValue;
					if (!DecodeValue(Reader, *Key, Package, Limits, Depth + 1, KeyValue, Diagnostic, Path)
						|| !DecodeValue(Reader, *Mapped, Package, Limits, Depth + 1, MappedValue, Diagnostic, Path)) return false;
					Value.Elements.push_back(std::move(KeyValue)); Value.Elements.push_back(std::move(MappedValue));
				}
				break;
			}
			case ETypeOpcode::HardRef:
				if (!Reader.U8(Value.ReferenceTag, Diagnostic) || Value.ReferenceTag > 2)
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Hard reference tag is invalid.", Reader.Position(), std::move(Path));
				if (Value.ReferenceTag != 0 && (!Reader.VarUInt(Value.ReferenceId, Diagnostic) || Value.ReferenceId == 0
					|| (Value.ReferenceTag == 1 && Value.ReferenceId > Package.Objects.size())
					|| (Value.ReferenceTag == 2 && Value.ReferenceId > Package.Header.Dependencies.size())))
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Hard reference id is invalid.", Reader.Position(), std::move(Path));
				break;
			case ETypeOpcode::SoftRef:
				if (!Reader.U8(Value.ReferenceTag, Diagnostic) || Value.ReferenceTag > 1)
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Soft reference tag is invalid.", Reader.Position(), std::move(Path));
				if (Value.ReferenceTag == 1)
				{
					if (!Reader.VarUInt(Value.ReferenceId, Diagnostic) || Value.ReferenceId == 0 || Value.ReferenceId > Package.Names.size())
						return Fail(Diagnostic, EReaderFailure::InvalidValue, "Soft reference name id is invalid.", Reader.Position(), std::move(Path));
					Value.Text = Package.Names[static_cast<size_t>(Value.ReferenceId - 1)];
				}
				break;
			case ETypeOpcode::Bytes:
			{
				uint64 Count = 0; std::span<const uint8> Data;
				if (!Reader.VarUInt(Count, Diagnostic) || Count > Limits.ContainerElements || !Reader.BytesSpan(Count, Data, Diagnostic))
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Byte value is invalid.", Reader.Position(), std::move(Path));
				Value.Bytes.assign(Data.begin(), Data.end()); break;
			}
			}
			OutValue = std::move(Value); return true;
		}

		auto DecodeTablesAndValues(std::span<const uint8> Bytes, FDecodedPackage& Package,
			const FReaderLimits& Limits, FReaderDiagnostic& Diagnostic) -> bool
		{
			auto Section = [&](size_t Index) {
				const auto& Entry = Package.Header.Sections[Index];
				return Bytes.subspan(Entry.Offset, Entry.Length);
			};
			FWireReader Names(Section(0), Package.Header.Sections[0].Offset);
			uint64 NameCount = 0;
			if (!Names.VarUInt(NameCount, Diagnostic) || NameCount > Limits.TableEntries)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded, "Name count exceeds the configured bound.", Names.Position());
			Package.Names.reserve(static_cast<size_t>(NameCount));
			for (uint64 Index = 0; Index < NameCount; ++Index)
			{
				std::string Name;
				if (!Names.String(Name, Limits, Diagnostic, false)) return false;
				if (!Package.Names.empty() && !ByteLess(Package.Names.back(), Name))
					return Fail(Diagnostic, EReaderFailure::NonCanonical, "Name table is not canonical.", Names.Position());
				Package.Names.push_back(std::move(Name));
			}
			if (!Names.RequireEnd(Diagnostic, EReaderFailure::InvalidTable, "Name section has trailing bytes.")) return false;

			FWireReader Types(Section(1), Package.Header.Sections[1].Offset);
			uint64 TypeCount = 0;
			if (!Types.VarUInt(TypeCount, Diagnostic) || TypeCount > Limits.TableEntries)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded, "Type count exceeds the configured bound.", Types.Position());
			Package.Types.reserve(static_cast<size_t>(TypeCount));
			for (uint64 Index = 0; Index < TypeCount; ++Index)
			{
				std::span<const uint8> RecordBytes; uint64 Offset = 0;
				if (!Types.Record(RecordBytes, Diagnostic, &Offset)) return false;
				FWireReader Record(RecordBytes, Offset); uint8 Opcode = 0;
				if (!Record.U8(Opcode, Diagnostic) || Opcode < 1 || Opcode > 0x17)
					return Fail(Diagnostic, EReaderFailure::InvalidTable, "Type opcode is unsupported.", Offset);
				FDecodedType Type{.Opcode = static_cast<ETypeOpcode>(Opcode)}; uint64 NameId = 0;
				switch (Type.Opcode)
				{
				case ETypeOpcode::Enum:
				{
					uint8 Storage = 0; if (!Record.VarUInt(NameId, Diagnostic) || !Record.U8(Storage, Diagnostic)) return false;
					Type.Parameter = Storage; break;
				}
				case ETypeOpcode::Intrinsic:
				{
					uint8 Layout = 0; if (!Record.U8(Layout, Diagnostic)) return false; Type.Parameter = Layout; break;
				}
				case ETypeOpcode::Struct: case ETypeOpcode::HardRef: case ETypeOpcode::SoftRef:
					if (!Record.VarUInt(NameId, Diagnostic)) return false; break;
				case ETypeOpcode::FixedArray:
				{
					uint64 Child = 0; if (!Record.VarUInt(Child, Diagnostic) || !Record.VarUInt(Type.Parameter, Diagnostic)) return false;
					Type.ChildTypeIds.push_back(Child); break;
				}
				case ETypeOpcode::Array:
				{
					uint64 Child = 0; if (!Record.VarUInt(Child, Diagnostic)) return false; Type.ChildTypeIds.push_back(Child); break;
				}
				case ETypeOpcode::Map:
				{
					uint64 Key = 0, Value = 0; if (!Record.VarUInt(Key, Diagnostic) || !Record.VarUInt(Value, Diagnostic)) return false;
					Type.ChildTypeIds = {Key, Value}; break;
				}
				default: break;
				}
				if (!Record.RequireEnd(Diagnostic, EReaderFailure::InvalidTable, "Type record has trailing bytes.")) return false;
				if (NameId > Package.Names.size() || ((Type.Opcode == ETypeOpcode::Struct || Type.Opcode == ETypeOpcode::Enum) && NameId == 0))
					return Fail(Diagnostic, EReaderFailure::InvalidTable, "Type name id is invalid.", Offset);
				if (NameId != 0) Type.QualifiedName = Package.Names[static_cast<size_t>(NameId - 1)];
				for (uint64 Child : Type.ChildTypeIds)
					if (Child == 0 || Child > TypeCount) return Fail(Diagnostic, EReaderFailure::InvalidTable, "Child type id is invalid.", Offset);
				Package.Types.push_back(std::move(Type));
			}
			if (!Types.RequireEnd(Diagnostic, EReaderFailure::InvalidTable, "Type section has trailing bytes.")) return false;

			FWireReader Schemas(Section(2), Package.Header.Sections[2].Offset);
			uint64 CustomCount = 0;
			if (!Schemas.VarUInt(CustomCount, Diagnostic) || CustomCount > Limits.CustomVersions)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded, "Custom-version count exceeds the configured bound.", Schemas.Position());
			for (uint64 Index = 0; Index < CustomCount; ++Index)
			{
				FCustomVersion Version; uint64 Value = 0;
				if (!Schemas.Fixed(Version.Guid.A, Diagnostic) || !Schemas.Fixed(Version.Guid.B, Diagnostic)
					|| !Schemas.Fixed(Version.Guid.C, Diagnostic) || !Schemas.Fixed(Version.Guid.D, Diagnostic)
					|| !Schemas.VarUInt(Value, Diagnostic) || Value > std::numeric_limits<uint32>::max())
					return Fail(Diagnostic, EReaderFailure::InvalidTable, "Custom-version entry is invalid.", Schemas.Position());
				Version.Value = static_cast<uint32>(Value); Package.CustomVersions.push_back(Version);
			}
			uint64 SchemaCount = 0;
			if (!Schemas.VarUInt(SchemaCount, Diagnostic) || SchemaCount > Limits.TableEntries)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded, "Schema count exceeds the configured bound.", Schemas.Position());
			for (uint64 Index = 0; Index < SchemaCount; ++Index)
			{
				std::span<const uint8> RecordBytes; uint64 Offset = 0;
				if (!Schemas.Record(RecordBytes, Diagnostic, &Offset)) return false;
				FWireReader Record(RecordBytes, Offset); uint64 NameId = 0, FieldCount = 0;
				if (!Record.VarUInt(NameId, Diagnostic) || NameId == 0 || NameId > Package.Names.size()
					|| !Record.VarUInt(FieldCount, Diagnostic) || FieldCount > Limits.SchemaFields)
					return Fail(Diagnostic, EReaderFailure::InvalidTable, "Schema header is invalid.", Offset);
				FDecodedSchema Schema{.QualifiedName = Package.Names[static_cast<size_t>(NameId - 1)]};
				for (uint64 FieldIndex = 0; FieldIndex < FieldCount; ++FieldIndex)
				{
					uint64 FieldName = 0, TypeId = 0, Flags = 0;
					if (!Record.VarUInt(FieldName, Diagnostic) || FieldName == 0 || FieldName > Package.Names.size()
						|| !Record.VarUInt(TypeId, Diagnostic) || TypeId == 0 || TypeId > Package.Types.size()
						|| !Record.VarUInt(Flags, Diagnostic))
						return Fail(Diagnostic, EReaderFailure::InvalidTable, "Schema field record is invalid.", Record.Position());
					Schema.Fields.push_back({Package.Names[static_cast<size_t>(FieldName - 1)], TypeId, Flags});
				}
				if (!Record.RequireEnd(Diagnostic, EReaderFailure::InvalidTable, "Schema record has trailing bytes.")) return false;
				Package.Schemas.push_back(std::move(Schema));
			}
			if (!Schemas.RequireEnd(Diagnostic, EReaderFailure::InvalidTable, "Schema section has trailing bytes.")) return false;

			FWireReader Objects(Section(3), Package.Header.Sections[3].Offset); uint64 ObjectCount = 0;
			if (!Objects.VarUInt(ObjectCount, Diagnostic) || ObjectCount != Package.Header.ObjectCount)
				return Fail(Diagnostic, EReaderFailure::InvalidTopology, "Object table count differs from the public summary.", Objects.Position());
			for (uint64 Index = 0; Index < ObjectCount; ++Index)
			{
				std::span<const uint8> RecordBytes; uint64 Offset = 0;
				if (!Objects.Record(RecordBytes, Diagnostic, &Offset)) return false;
				FWireReader Record(RecordBytes, Offset); uint64 OuterId = 0, ClassId = 0, ObjectNameId = 0;
				if (!Record.VarUInt(OuterId, Diagnostic) || !Record.VarUInt(ClassId, Diagnostic)
					|| !Record.VarUInt(ObjectNameId, Diagnostic) || ClassId == 0 || ClassId > Package.Names.size()
					|| ObjectNameId == 0 || ObjectNameId > Package.Names.size()
					|| !Record.RequireEnd(Diagnostic, EReaderFailure::InvalidTopology, "Object record has trailing bytes."))
					return Fail(Diagnostic, EReaderFailure::InvalidTopology, "Object record is invalid.", Offset);
				if ((Index == 0 && OuterId != 0) || (Index > 0 && (OuterId == 0 || OuterId > Index)))
					return Fail(Diagnostic, EReaderFailure::InvalidTopology, "Object outer id is not topological.", Offset);
				const std::string Name = Package.Names[static_cast<size_t>(ObjectNameId - 1)];
				const std::string OuterPath = OuterId == 0 ? std::string{} : Package.Objects[static_cast<size_t>(OuterId - 1)].Path;
				Package.Objects.push_back({Index + 1, OuterId,
					OuterPath.empty() ? Name : OuterPath + "/" + Name,
					Package.Names[static_cast<size_t>(ClassId - 1)], Name});
			}
			if (!Objects.RequireEnd(Diagnostic, EReaderFailure::InvalidTopology, "Object section has trailing bytes.")) return false;

			FWireReader Values(Section(4), Package.Header.Sections[4].Offset); uint64 ValueObjectCount = 0;
			if (!Values.VarUInt(ValueObjectCount, Diagnostic) || ValueObjectCount != ObjectCount)
				return Fail(Diagnostic, EReaderFailure::InvalidValue, "Value-section object count is invalid.", Values.Position());
			for (uint64 ObjectIndex = 0; ObjectIndex < ValueObjectCount; ++ObjectIndex)
			{
				std::span<const uint8> BlockBytes; uint64 BlockOffset = 0;
				if (!Values.Record(BlockBytes, Diagnostic, &BlockOffset)) return false;
				FWireReader Block(BlockBytes, BlockOffset); uint64 OverrideCount = 0;
				if (!Block.VarUInt(OverrideCount, Diagnostic) || OverrideCount > Limits.TableEntries)
					return Fail(Diagnostic, EReaderFailure::LimitExceeded, "Override count exceeds the configured bound.", Block.Position());
				FDecodedObjectValues ObjectValues; uint64 PreviousSchema = 0, PreviousField = 0;
				for (uint64 Index = 0; Index < OverrideCount; ++Index)
				{
					FDecodedOverride Override; std::span<const uint8> Payload;
					if (!Block.VarUInt(Override.SchemaId, Diagnostic) || !Block.VarUInt(Override.FieldId, Diagnostic)
						|| !Block.U8(Override.Provenance, Diagnostic) || Override.Provenance > 2
						|| !Block.Record(Payload, Diagnostic, &Override.PayloadOffset))
						return Fail(Diagnostic, EReaderFailure::InvalidValue, "Override record is invalid.", Block.Position());
					if (Override.SchemaId < PreviousSchema || (Override.SchemaId == PreviousSchema && Override.FieldId <= PreviousField))
						return Fail(Diagnostic, EReaderFailure::NonCanonical, "Overrides are duplicate or unordered.", Block.Position());
					const FDecodedSchema* Schema = SchemaAt(Package, Override.SchemaId);
					if (!Schema || Override.FieldId == 0 || Override.FieldId > Schema->Fields.size())
						return Fail(Diagnostic, EReaderFailure::InvalidValue, "Override schema or field id is invalid.", Block.Position());
					Override.PayloadSize = Payload.size();
					if (Override.Provenance == 2)
					{
						FWireReader Unknown(Payload, Override.PayloadOffset); std::span<const uint8> Closure, Retained;
						if (!Unknown.Record(Closure, Diagnostic) || !Unknown.Record(Retained, Diagnostic)
							|| !Unknown.RequireEnd(Diagnostic, EReaderFailure::InvalidRetainedClosure, "Unknown value body has trailing bytes.")) return false;
						Override.DescriptorClosure.assign(Closure.begin(), Closure.end());
						Override.RetainedPayload.assign(Retained.begin(), Retained.end());
					}
					else
					{
						const FDecodedField& Field = Schema->Fields[static_cast<size_t>(Override.FieldId - 1)];
						const FDecodedType* Type = TypeAt(Package, Field.TypeId);
						if (!Type) return Fail(Diagnostic, EReaderFailure::InvalidTable, "Override type id is invalid.", Override.PayloadOffset);
						FWireReader PayloadReader(Payload, Override.PayloadOffset);
						if (!DecodeValue(PayloadReader, *Type, Package, Limits, 0, Override.Value, Diagnostic,
							std::format("{}::{}", Schema->QualifiedName, Field.Name))
							|| !PayloadReader.RequireEnd(Diagnostic, EReaderFailure::InvalidValue, "Value payload has trailing bytes.")) return false;
					}
					PreviousSchema = Override.SchemaId; PreviousField = Override.FieldId;
					ObjectValues.Overrides.push_back(std::move(Override));
				}
				if (!Block.RequireEnd(Diagnostic, EReaderFailure::InvalidValue, "Object override block has trailing bytes.")) return false;
				Package.ObjectValues.push_back(std::move(ObjectValues));
			}
			return Values.RequireEnd(Diagnostic, EReaderFailure::InvalidValue, "Value section has trailing bytes.");
		}

		auto BuildWriterInput(const FDecodedPackage& Package, FPackageInput& Out,
			FReaderDiagnostic& Diagnostic) -> bool
		{
			FPackageInput Input;
			Input.AssetClass = Package.Header.AssetClass;
			Input.EntryKind = Package.Header.EntryKind;
			Input.RedirectDestination = Package.Header.RedirectDestination;
			Input.Dependencies = Package.Header.Dependencies;
			Input.AdditionalNames = Package.Names;
			std::vector<FTypePtr> Types; Types.reserve(Package.Types.size());
			for (const FDecodedType& Type : Package.Types)
				Types.push_back(MakeType(Type.Opcode, Type.QualifiedName, Type.Parameter));
			for (size_t Index = 0; Index < Package.Types.size(); ++Index)
				for (uint64 Child : Package.Types[Index].ChildTypeIds)
				{
					if (Child == 0 || Child > Types.size()) return Fail(Diagnostic, EReaderFailure::InvalidTable, "Decoded child type id is invalid.");
					Types[Index]->Children.push_back(Types[static_cast<size_t>(Child - 1)]);
				}
			Input.Types = Types;
			Input.CustomVersions = Package.CustomVersions;
			for (const FDecodedSchema& Schema : Package.Schemas)
			{
				FSchemaDescriptor Output{.QualifiedName = Schema.QualifiedName};
				for (const FDecodedField& Field : Schema.Fields)
				{
					if (Field.TypeId == 0 || Field.TypeId > Types.size()) return Fail(Diagnostic, EReaderFailure::InvalidTable, "Decoded schema type id is invalid.");
					Output.Fields.push_back({Field.Name, Types[static_cast<size_t>(Field.TypeId - 1)], Field.AuthoredFlags});
				}
				Input.Schemas.push_back(std::move(Output));
			}
			for (const FDecodedObject& Object : Package.Objects)
			{
				const std::string OuterPath = Object.OuterId == 0 ? std::string{} : Package.Objects[static_cast<size_t>(Object.OuterId - 1)].Path;
				Input.Objects.push_back({Object.Path, OuterPath, Object.ClassName, Object.ObjectName});
			}
			if (Package.ObjectValues.size() != Package.Objects.size())
				return Fail(Diagnostic, EReaderFailure::InvalidValue, "Decoded object/value counts differ.");
			for (size_t ObjectIndex = 0; ObjectIndex < Package.Objects.size(); ++ObjectIndex)
			{
				FObjectValueInput Values{.ObjectPath = Package.Objects[ObjectIndex].Path};
				for (const FDecodedOverride& Override : Package.ObjectValues[ObjectIndex].Overrides)
				{
					const FDecodedSchema* Schema = SchemaAt(Package, Override.SchemaId);
					if (!Schema || Override.FieldId == 0 || Override.FieldId > Schema->Fields.size())
						return Fail(Diagnostic, EReaderFailure::InvalidValue, "Decoded override id is invalid.");
					const FDecodedField& Field = Schema->Fields[static_cast<size_t>(Override.FieldId - 1)];
					if (Override.Provenance == 2)
						Values.RetainedUnknownOverrides.push_back({Schema->QualifiedName, Field.Name,
							Override.DescriptorClosure, Override.RetainedPayload});
					else Values.KnownOverrides.push_back({Schema->QualifiedName, Field.Name,
						Override.Provenance == 0 ? EDefaultDeltaProvenance::Explicit : EDefaultDeltaProvenance::Forced,
						Override.Value});
				}
				Input.ObjectValues.push_back(std::move(Values));
			}
			Out = std::move(Input); return true;
		}

		auto TranslateWriterFailure(EWriterFailure Failure) -> EReaderFailure
		{
			switch (Failure)
			{
			case EWriterFailure::DescriptorCycle: return EReaderFailure::DescriptorCycle;
			case EWriterFailure::InvalidTopology: return EReaderFailure::InvalidTopology;
			case EWriterFailure::InvalidRetainedClosure: return EReaderFailure::InvalidRetainedClosure;
			case EWriterFailure::LimitExceeded: case EWriterFailure::PackageTooLarge: return EReaderFailure::LimitExceeded;
			case EWriterFailure::InvalidValue: return EReaderFailure::InvalidValue;
			default: return EReaderFailure::InvalidTable;
			}
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
			case ETypeOpcode::Bytes: return K::UInt8;
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
			std::string Signature, std::vector<uint8> Payload) -> void
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
			case ETypeOpcode::Bytes: Writer.WriteBytes(Value.Bytes); return true;
			}
			return Fail(Diagnostic, EReaderFailure::InvalidValue, "Unsupported inspection value.", 0, std::move(Path));
		}

		auto BuildInspection(const FDecodedPackage& Package, std::span<const uint8> Bytes,
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
		auto AppendBigEndian(std::vector<uint8>& Token, T Value) -> void
		{
			for (size_t Index = sizeof(T); Index > 0; --Index)
				Token.push_back(static_cast<uint8>(Value >> ((Index - 1) * 8)));
		}

		template<std::integral T>
		auto AppendSortable(std::vector<uint8>& Token, T Value) -> void
		{
			using U = std::make_unsigned_t<T>;
			U Bits = std::bit_cast<U>(Value);
			if constexpr (std::is_signed_v<T>) Bits ^= U(1) << (sizeof(U) * 8 - 1);
			AppendBigEndian(Token, Bits);
		}

		template<std::floating_point T>
		auto AppendSortableFloat(std::vector<uint8>& Token, T Value) -> void
		{
			using U = std::conditional_t<sizeof(T) == 4, uint32, uint64>;
			U Bits = std::bit_cast<U>(Value); constexpr U Sign = U(1) << (sizeof(U) * 8 - 1);
			if ((Bits & ~Sign) == 0) Bits = 0;
			Bits = (Bits & Sign) ? ~Bits : (Bits ^ Sign); AppendBigEndian(Token, Bits);
		}

		auto BuildIntrinsicLedgerToken(uint64 Layout, std::span<const uint64> Components,
			std::vector<uint8>& Out, FReaderDiagnostic& Diagnostic) -> bool
		{
			Out.push_back(static_cast<uint8>(DurinCodeGen::EPropertyGenFlags::Struct));
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
					Out.push_back(static_cast<uint8>(DurinCodeGen::EPropertyGenFlags::Float));
					AppendSortableFloat(Out, std::bit_cast<float>(uint32(Components[Index])));
				}
				else
				{
					Out.push_back(static_cast<uint8>(DurinCodeGen::EPropertyGenFlags::Double));
					AppendSortableFloat(Out, std::bit_cast<double>(Components[Index]));
				}
			}
			return true;
		}

		auto BuildLedgerMapKeyToken(const FDecodedType& Type, const FValue& Value,
			std::vector<uint8>& Out, FReaderDiagnostic& Diagnostic) -> bool
		{
			Out.push_back(static_cast<uint8>(TypeKind(Type, FDecodedPackage{})));
			switch (Type.Opcode)
			{
			case ETypeOpcode::Bool: Out.back() = static_cast<uint8>(DurinCodeGen::EPropertyGenFlags::Bool); Out.push_back(Value.Bool ? 1 : 0); return true;
			case ETypeOpcode::I8: AppendSortable(Out, int8(Value.Signed)); return true;
			case ETypeOpcode::I16: AppendSortable(Out, int16(Value.Signed)); return true;
			case ETypeOpcode::I32: AppendSortable(Out, int32(Value.Signed)); return true;
			case ETypeOpcode::I64: AppendSortable(Out, Value.Signed); return true;
			case ETypeOpcode::U8: AppendSortable(Out, uint8(Value.Unsigned)); return true;
			case ETypeOpcode::U16: AppendSortable(Out, uint16(Value.Unsigned)); return true;
			case ETypeOpcode::U32: AppendSortable(Out, uint32(Value.Unsigned)); return true;
			case ETypeOpcode::U64: AppendSortable(Out, Value.Unsigned); return true;
			case ETypeOpcode::String:
				AppendBigEndian(Out, uint64(Value.Text.size())); Out.insert(Out.end(), Value.Text.begin(), Value.Text.end()); return true;
			case ETypeOpcode::Name:
				AppendBigEndian(Out, uint64(Value.Text.size())); Out.insert(Out.end(), Value.Text.begin(), Value.Text.end());
				AppendBigEndian(Out, uint32(0)); return true;
			case ETypeOpcode::Guid:
				AppendBigEndian(Out, Value.Guid.A); AppendBigEndian(Out, Value.Guid.B);
				AppendBigEndian(Out, Value.Guid.C); AppendBigEndian(Out, Value.Guid.D); return true;
			case ETypeOpcode::Enum:
			{
				const auto Storage = static_cast<ETypeOpcode>(Type.Parameter);
				Out.front() = static_cast<uint8>(DurinCodeGen::EPropertyGenFlags::Enum);
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

		auto RestoreNestedLedger(const FDecodedType& Type, const FValue& Value,
			const FDecodedPackage& Package, FAuthoredOverridePath& Path,
			std::vector<FAuthoredOverrideEntry>& Entries,
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
					Path.push_back(FAuthoredOverridePathToken::Field(FName(Schema->QualifiedName), FName(It->Name)));
					const auto Provenance = Value.Provenances[Index] == EDefaultDeltaProvenance::Forced
						? EAuthoredOverrideProvenance::Forced : EAuthoredOverrideProvenance::LoadedExplicit;
					Entries.push_back({Path, Provenance});
					if (!RestoreNestedLedger(*ChildType, Value.Elements[Index], Package, Path, Entries, Diagnostic)) return false;
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
					if (!RestoreNestedLedger(*ChildType, Value.Elements[Index], Package, Path, Entries, Diagnostic)) return false;
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
					std::vector<uint8> Token;
					if (!BuildLedgerMapKeyToken(*KeyType, Value.Elements[Index], Token, Diagnostic)) return false;
					Path.push_back(FAuthoredOverridePathToken::MapValue(std::move(Token)));
					if (!RestoreNestedLedger(*ValueType, Value.Elements[Index + 1], Package, Path, Entries, Diagnostic)) return false;
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
					std::vector<uint8> Token;
					if (!BuildLedgerMapKeyToken(*Key, Value.Elements[Index], Token, Diagnostic)) return false;
					Route.push_back({.Kind = EAssetReferenceRouteKind::MapValue, .MapKeyToken = std::move(Token)});
					if (!ExtractValueReferences(*Mapped, Value.Elements[Index + 1], Package, SourcePackage,
						Fingerprint, SourceObjectId, SourceClass, DeclaringType, FieldName, Route, Out, Diagnostic)) return false;
					Route.pop_back();
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

		// Converts recognized compatibility aliases at the bytes-to-runtime boundary.
		// The raw DecodePackage/ReencodePackage contract remains byte-preserving.
		auto CanonicalizeSerializedReflectionNames(FDecodedPackage& Package) -> void
		{
			CanonicalizeSerializedClassName(Package.Header.AssetClass);
			for (FDecodedObject& Object : Package.Objects)
				CanonicalizeSerializedClassName(Object.ClassName);
			for (FDecodedSchema& Schema : Package.Schemas)
				CanonicalizeSerializedSchemaName(Schema.QualifiedName);
			for (FDecodedType& Type : Package.Types)
				CanonicalizeSerializedTypeName(Type);
		}
	}

	auto ReadHeader(std::span<const uint8> Bytes, FValidatedHeader& OutHeader,
		const FReaderLimits& Limits, FReaderDiagnostic* OutDiagnostic) -> bool
	{
		FReaderDiagnostic Diagnostic; FValidatedHeader Result;
		const bool Success = DecodeHeaderInner(Bytes, Result, Limits, Diagnostic);
		if (Success)
		{
			CanonicalizeSerializedClassName(Result.AssetClass);
			OutHeader = std::move(Result);
		}
		if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic);
		return Success;
	}

	auto ReencodePackage(const FDecodedPackage& Package, std::vector<uint8>& OutBytes,
		FReaderDiagnostic* OutDiagnostic) -> bool
	{
		FReaderDiagnostic Diagnostic; FPackageInput Input;
		if (!BuildWriterInput(Package, Input, Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic); return false;
		}
		std::vector<uint8> Bytes; FWriterDiagnostic WriterDiagnostic;
		if (!WritePackage(Input, Bytes, &WriterDiagnostic))
		{
			Diagnostic = {TranslateWriterFailure(WriterDiagnostic.Failure), WriterDiagnostic.LogicalPath,
				WriterDiagnostic.Message, 0};
			if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic); return false;
		}
		OutBytes = std::move(Bytes);
		if (OutDiagnostic) OutDiagnostic->Reset();
		return true;
	}

	auto DecodePackage(std::span<const uint8> Bytes, FDecodedPackage& OutPackage,
		const FReaderLimits& Limits, FReaderDiagnostic* OutDiagnostic) -> bool
	{
		FReaderDiagnostic Diagnostic; FDecodedPackage Result;
		if (!DecodeHeaderInner(Bytes, Result.Header, Limits, Diagnostic)
			|| !DecodeTablesAndValues(Bytes, Result, Limits, Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic); return false;
		}
		std::vector<uint8> Canonical;
		if (!ReencodePackage(Result, Canonical, &Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic); return false;
		}
		if (!std::ranges::equal(Bytes, Canonical))
		{
			Fail(Diagnostic, EReaderFailure::NonCanonical,
				"Decoded package does not match canonical v4 re-emission.");
			if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic); return false;
		}
		OutPackage = std::move(Result);
		if (OutDiagnostic) OutDiagnostic->Reset();
		return true;
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
		RemoveFromRoot(Package);
		MarkObjectHierarchyAsGarbage(Package);
		Package = nullptr;
		CollectGarbage();
	}

	auto LoadAssetPackage(std::span<const uint8> Bytes, const FAssetPath& PackagePath,
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
			Fail(Diagnostic, EReaderFailure::PublicationFailure, "Live v4 load requires a validated package path.");
			return Finish({EAssetError::InvalidPath, Diagnostic.Message});
		}
		if (FindPackage(PackagePath.GetView()))
		{
			Fail(Diagnostic, EReaderFailure::PublicationFailure,
				"A package with the requested path is already live.");
			return Finish({EAssetError::AlreadyExists, Diagnostic.Message});
		}
		if (!DecodePackage(Bytes, Decoded, Limits, &Diagnostic))
			return Finish({EAssetError::CorruptFile, Diagnostic.Message});
		CanonicalizeSerializedReflectionNames(Decoded);
		if (Decoded.Objects.empty() || Decoded.Objects.front().ClassName != Decoded.Header.AssetClass)
		{
			Fail(Diagnostic, EReaderFailure::InvalidTopology, "Main object class differs from the public summary.");
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}

		DPackage* Package = NewObject<DPackage>(nullptr, FName(PackagePath.GetAssetName()));
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
				FStaticConstructObjectParameters Parameters{Class, Outer, FName(Descriptor.ObjectName), Class->PropertiesSize};
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
		for (const FCustomVersion& Version : Decoded.CustomVersions)
			CustomVersions.push_back({Version.Guid, static_cast<int32>(Version.Value)});
		FAssetLoadReport Report{.PackagePath = PackagePath};
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
				if (Override.Provenance == 2)
				{
					FAssetLegacyField Legacy{.DeclaringClass = Schema->QualifiedName, .Name = Field.Name,
						.TypeSignature = "DASTv4:RetainedClosure", .DescriptorClosure = Override.DescriptorClosure,
						.RetainedPayload = Override.RetainedPayload};
					Report.CompatibilityIssues.push_back({.ObjectPath = Decoded.Objects[ObjectIndex].Path,
						.DeclaringClass = Schema->QualifiedName, .LegacyFields = {std::move(Legacy)},
						.Classification = EAssetCompatibilityClassification::UnknownIncompatible,
						.MigrationSummary = "An exact DAST v4 unknown descriptor closure was retained.",
						.Risk = EAssetCompatibilityRisk::UnknownNewerSchema});
					continue;
				}
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
			std::vector<FAssetLegacyField> Legacy;
			FAssetResult Result = Private::LoadAuthoredObject(*Objects[ObjectIndex], Fields, Objects,
				Version, Legacy, CustomVersions);
			if (!Result)
			{
				Fail(Diagnostic, EReaderFailure::ArchiveFailure, Result.Message, 0, Decoded.Objects[ObjectIndex].Path); Rollback();
				return Finish(Result);
			}
			std::vector<std::pair<std::string, std::string>> LegacyIdentities;
			for (const FAssetLegacyField& Field : Legacy)
				LegacyIdentities.emplace_back(Field.DeclaringClass, Field.Name);
			for (FAssetLegacyField& Field : Legacy)
				Report.CompatibilityIssues.push_back({.ObjectPath = Decoded.Objects[ObjectIndex].Path,
					.DeclaringClass = Field.DeclaringClass, .LegacyFields = {std::move(Field)},
					.Classification = EAssetCompatibilityClassification::UnknownIncompatible,
					.MigrationSummary = "The DAST v4 field is incompatible with the live schema.",
					.Risk = EAssetCompatibilityRisk::UnknownNewerSchema});

			std::vector<FAuthoredOverrideEntry> LedgerEntries;
			for (const FDecodedOverride* Override : KnownOverrides)
			{
				const FDecodedSchema* Schema = SchemaAt(Decoded, Override->SchemaId);
				const FDecodedField& Field = Schema->Fields[static_cast<size_t>(Override->FieldId - 1)];
				const bool bLegacy = std::ranges::any_of(LegacyIdentities, [&](const auto& Item) {
					return Item.first == Schema->QualifiedName && Item.second == Field.Name; });
				if (bLegacy) continue;
				if (ShouldFail(Options, ELiveLoadPhase::RestoreLedger, Override->FieldId))
				{
					Fail(Diagnostic, EReaderFailure::ArchiveFailure, "Injected ledger restoration failure."); Rollback();
					return Finish({EAssetError::CorruptFile, Diagnostic.Message});
				}
				FAuthoredOverridePath Path{FAuthoredOverridePathToken::Field(FName(Schema->QualifiedName), FName(Field.Name))};
				const auto Provenance = Override->Provenance == 1 ? EAuthoredOverrideProvenance::Forced
					: EAuthoredOverrideProvenance::LoadedExplicit;
				LedgerEntries.push_back({Path, Provenance});
				const FDecodedType* Type = TypeAt(Decoded, Field.TypeId);
				if (!RestoreNestedLedger(*Type, Override->Value, Decoded, Path, LedgerEntries, Diagnostic))
				{
					Rollback(); return Finish({EAssetError::CorruptFile, Diagnostic.Message});
				}
			}
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
		}
		if (ShouldFail(Options, ELiveLoadPhase::Publish, 0))
		{
			Fail(Diagnostic, EReaderFailure::PublicationFailure, "Injected graph publication failure."); Rollback();
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}
		AddToRoot(Package);
		OutPackage = FLoadedAssetPackage(Package);
		if (OutReport) *OutReport = std::move(Report);
		Diagnostic.Reset(); return Finish({});
	}

	auto InspectPackage(std::span<const uint8> Bytes, FAssetPackageInspection& OutInspection,
		const FReaderLimits& Limits, FReaderDiagnostic* OutDiagnostic) -> FAssetResult
	{
		FReaderDiagnostic Diagnostic; FDecodedPackage Package;
		if (!DecodePackage(Bytes, Package, Limits, &Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = Diagnostic;
			return {EAssetError::CorruptFile, Diagnostic.Message};
		}
		CanonicalizeSerializedReflectionNames(Package);
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

	auto ExtractReferences(std::span<const uint8> Bytes, const FAssetPath& SourcePackage,
		std::vector<FAssetReferenceEdge>& OutReferences, const FReaderLimits& Limits,
		FReaderDiagnostic* OutDiagnostic) -> FAssetResult
	{
		FReaderDiagnostic Diagnostic; FDecodedPackage Package;
		if (!SourcePackage.IsValid())
		{
			Fail(Diagnostic, EReaderFailure::InvalidValue, "Reference extraction requires a validated source package path.");
			if (OutDiagnostic) *OutDiagnostic = Diagnostic;
			return {EAssetError::InvalidPath, Diagnostic.Message};
		}
		if (!DecodePackage(Bytes, Package, Limits, &Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = Diagnostic;
			return {EAssetError::CorruptFile, Diagnostic.Message};
		}
		CanonicalizeSerializedReflectionNames(Package);
		const FAssetPackageFingerprint Fingerprint{.FileSize = Bytes.size(),
			.ContentHash = FXxHash128::HashBuffer(Bytes), .ReaderVersion = Version};
		std::vector<FAssetReferenceEdge> References;
		if (Package.Header.EntryKind == EAssetRegistryEntryKind::Redirector)
		{
			FAssetPath Target; std::string Error;
			if (!FAssetPath::TryCreate(Package.Header.RedirectDestination, Target, &Error))
			{
				Fail(Diagnostic, EReaderFailure::InvalidHeader, Error);
				if (OutDiagnostic) *OutDiagnostic = Diagnostic;
				return {EAssetError::InvalidPath, Diagnostic.Message};
			}
			References.push_back({.SourcePackage = SourcePackage, .SourceFingerprint = Fingerprint,
				.SourceObjectId = 1, .SourceClass = Package.Header.AssetClass,
				.Kind = EAssetReferenceKind::Redirect, .TargetPath = std::move(Target),
				.DisplayRoute = "RedirectDestination"});
		}
		for (size_t ObjectIndex = 0; ObjectIndex < Package.Objects.size(); ++ObjectIndex)
			for (const FDecodedOverride& Override : Package.ObjectValues[ObjectIndex].Overrides)
			{
				if (Override.Provenance == 2) continue;
				const FDecodedSchema* Schema = SchemaAt(Package, Override.SchemaId);
				const FDecodedField& Field = Schema->Fields[static_cast<size_t>(Override.FieldId - 1)];
				const FDecodedType* Type = TypeAt(Package, Field.TypeId); if (!Type) continue;
				std::vector<FAssetReferenceRouteSegment> Route;
				if (!ExtractValueReferences(*Type, Override.Value, Package, SourcePackage, Fingerprint,
					ObjectIndex + 1, Package.Objects[ObjectIndex].ClassName, Schema->QualifiedName,
					Field.Name, Route, References, Diagnostic))
				{
					if (OutDiagnostic) *OutDiagnostic = Diagnostic;
					return {EAssetError::CorruptFile, Diagnostic.Message};
				}
			}
		OutReferences = std::move(References);
		if (OutDiagnostic) OutDiagnostic->Reset();
		return {};
	}

	auto ProbeCompatibility(std::span<const uint8> Bytes, const FAssetPath& PackagePath,
		const FReflectionCompatibilityCatalog& Catalog, FAssetPackageCompatibilityRecord& OutRecord,
		FAssetCompatibilityProbeStats* OutStats, const FReaderLimits& Limits,
		FReaderDiagnostic* OutDiagnostic) -> FAssetResult
	{
		FReaderDiagnostic Diagnostic; FDecodedPackage Package;
		if (!DecodePackage(Bytes, Package, Limits, &Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = Diagnostic;
			return {EAssetError::CorruptFile, Diagnostic.Message};
		}
		CanonicalizeSerializedReflectionNames(Package);
		FAssetPackageCompatibilityRecord Record{
			.PackagePath = PackagePath,
			.Fingerprint = {.FileSize = Bytes.size(), .ContentHash = FXxHash128::HashBuffer(Bytes),
				.ReaderVersion = Version},
			.FormatVersion = Version,
			.Inspection = EAssetCompatibilityInspection::Ready,
			.Compatibility = EAssetPackageCompatibility::Compatible,
			.Freshness = EAssetCompatibilityFreshness::Current};
		for (const std::string& Dependency : Package.Header.Dependencies)
		{
			FAssetPath Path; if (FAssetPath::TryCreate(Dependency, Path)) Record.Dependencies.push_back(std::move(Path));
		}
		for (size_t ObjectIndex = 0; ObjectIndex < Package.Objects.size(); ++ObjectIndex)
		{
			const FDecodedObject& Object = Package.Objects[ObjectIndex];
			const FReflectionCompatibilityClass* Class = Catalog.FindClass(Object.ClassName);
			if (!Class)
			{
				Record.Compatibility = EAssetPackageCompatibility::Unsupported;
				Record.Findings.push_back({.Code = EAssetCompatibilityFindingCode::UnavailableClass,
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
				const FReflectionCompatibilityField* Expected = Catalog.FindField(*Class, Schema->QualifiedName, Field.Name);
				if (!Expected)
				{
					if (Record.Compatibility == EAssetPackageCompatibility::Compatible)
						Record.Compatibility = EAssetPackageCompatibility::Incompatible;
					Record.Findings.push_back({.Code = EAssetCompatibilityFindingCode::UnknownField,
						.ObjectPath = Object.Path, .ClassIdentity = Object.ClassName,
						.DeclaringType = Schema->QualifiedName, .FieldName = Field.Name,
						.StoredKind = StoredKind, .StoredTypeSignature = StoredSignature,
						.PayloadSize = Override.PayloadSize, .PayloadOffset = Override.PayloadOffset,
						.Diagnostic = "Serialized field is not present in the current reflection catalog."});
				}
				else if (Expected->Kind != StoredKind || Expected->TypeSignature != StoredSignature)
				{
					if (Record.Compatibility == EAssetPackageCompatibility::Compatible)
						Record.Compatibility = EAssetPackageCompatibility::Incompatible;
					Record.Findings.push_back({.Code = EAssetCompatibilityFindingCode::IncompatibleFieldSignature,
						.ObjectPath = Object.Path, .ClassIdentity = Object.ClassName,
						.DeclaringType = Schema->QualifiedName, .FieldName = Field.Name,
						.StoredKind = StoredKind, .StoredTypeSignature = StoredSignature,
						.ExpectedKind = Expected->Kind, .ExpectedTypeSignature = Expected->TypeSignature,
						.PayloadSize = Override.PayloadSize, .PayloadOffset = Override.PayloadOffset,
						.Diagnostic = "Serialized field signature differs from the current reflection catalog."});
				}
			}
		}
		FAssetCompatibilityProbeStats Stats;
		Stats.PayloadBytesSkipped = 0;
		for (const auto& Object : Package.ObjectValues)
			for (const auto& Override : Object.Overrides) Stats.PayloadBytesSkipped += Override.PayloadSize;
		Stats.MetadataBytesRead = Package.Header.BytesRead
			+ Package.Header.Sections[0].Length + Package.Header.Sections[1].Length
			+ Package.Header.Sections[2].Length + Package.Header.Sections[3].Length;
		Stats.PeakMetadataBytes = Stats.MetadataBytesRead;
		OutRecord = std::move(Record); if (OutStats) *OutStats = Stats;
		if (OutDiagnostic) OutDiagnostic->Reset();
		return {};
	}

	auto RewriteReferences(
		std::span<const uint8> Bytes,
		std::span<const FAssetRedirectorFixupMapping> Mappings,
		uint64 ExpectedRewriteCount,
		std::vector<uint8>& OutBytes) -> FAssetResult
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
		std::span<const uint8> Bytes,
		const FAssetPath& DestinationPath,
		std::vector<uint8>& OutBytes) -> FAssetResult
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
