#include "AssetRegistry/ObjectStream.h"

#include "DObject/Class.h"
#include "Serialization/BinaryFormat.h"

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
			explicit FWireReader(std::span<const std::byte> InBytes, uint64 InBaseOffset = 0)
				: Bytes(InBytes), BaseOffset(InBaseOffset), Reader(InBytes) {}

			auto Position() const -> uint64 { return BaseOffset + Reader.Tell(); }
			auto Remaining() const -> uint64 { return Reader.GetRemainingBytes(); }
			auto IsAtEnd() const -> bool { return Reader.IsAtEnd(); }

			auto U8(uint8& Out, FReaderDiagnostic& Diagnostic) -> bool
			{
				if (Remaining() == 0) return Fail(Diagnostic, EReaderFailure::TruncatedInput,
					"Unexpected end of input.", Position());
				return Reader.ReadU8(Out);
			}

			template<typename T>
			auto Fixed(T& Out, FReaderDiagnostic& Diagnostic) -> bool
			{
				static_assert(std::is_unsigned_v<T>);
				if (sizeof(T) > Remaining()) return Fail(Diagnostic, EReaderFailure::TruncatedInput,
					"Truncated fixed-width value.", Position());
				return Reader.ReadInteger(Out);
			}

			auto VarUInt(uint64& Out, FReaderDiagnostic& Diagnostic) -> bool
			{
				const uint64 LocalStart = Reader.Tell();
				const uint64 Start = BaseOffset + LocalStart;
				if (Reader.ReadVarUInt(Out)) return true;
				const uint64 Consumed = Reader.Tell() - LocalStart;
				if (Consumed == 0 || (Consumed < 10
					&& (std::to_integer<uint8>(Bytes[static_cast<size_t>(LocalStart + Consumed - 1)]) & 0x80) != 0))
					return Fail(Diagnostic, EReaderFailure::TruncatedInput,
						"Unexpected end of input.", BaseOffset + Reader.Tell());
				const uint8 Last = std::to_integer<uint8>(
					Bytes[static_cast<size_t>(LocalStart + Consumed - 1)]);
				if ((Last & 0x80) == 0 && Consumed > 1 && (Last & 0x7f) == 0)
					return Fail(Diagnostic, EReaderFailure::InvalidPrimitive,
						"VarUInt is not minimally encoded.", Start);
				if (Consumed == 10 && (Last & 0x7f) > 1)
					return Fail(Diagnostic, EReaderFailure::InvalidPrimitive,
						"VarUInt overflows uint64.", Start);
				return Fail(Diagnostic, EReaderFailure::InvalidPrimitive,
					"VarUInt exceeds ten bytes.", Start);
			}

			auto VarInt(int64& Out, FReaderDiagnostic& Diagnostic) -> bool
			{
				uint64 Encoded = 0;
				if (!VarUInt(Encoded, Diagnostic)) return false;
				Out = std::bit_cast<int64>((Encoded >> 1) ^ (uint64(0) - (Encoded & 1)));
				return true;
			}

			auto BytesSpan(uint64 Count, std::span<const std::byte>& Out,
				FReaderDiagnostic& Diagnostic) -> bool
			{
				if (Count > Remaining()) return Fail(Diagnostic, EReaderFailure::TruncatedInput,
					"Length-delimited data exceeds its containing extent.", Position());
				return Reader.ReadRegion(Out, Count, std::numeric_limits<uint64>::max());
			}

			auto Record(std::span<const std::byte>& Out, FReaderDiagnostic& Diagnostic,
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
				std::span<const std::byte> Data;
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
			std::span<const std::byte> Bytes;
			uint64 BaseOffset = 0;
			FBinaryReader Reader;
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
				|| Limits.ByteValueBytes == 0 || Limits.ByteValueBytes > MaximumByteValueBytes
				|| Limits.ValueDepth > MaximumValueDepth)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded,
					"Reader limits must be nonzero and may only tighten frozen wire bounds.");
			return true;
		}

		auto DecodeHeaderInner(std::span<const std::byte> Bytes, uint64 PackageSize,
			FValidatedHeader& Out, const FReaderLimits& Limits,
			FReaderDiagnostic& Diagnostic) -> bool
		{
			if (!ValidateLimits(Limits, Diagnostic)) return false;
			if (PackageSize == 0) PackageSize = Bytes.size();
			if (PackageSize < Bytes.size() || PackageSize > Limits.PackageBytes)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded,
					"Package exceeds the configured byte bound.");
			FWireReader Reader(Bytes);
			uint32 ReadMagic = 0, ReadVersion = 0, SummaryLength = 0;
			uint8 SectionCount = 0;
			if (!Reader.Fixed(ReadMagic, Diagnostic) || !Reader.Fixed(ReadVersion, Diagnostic)
				|| !Reader.Fixed(SummaryLength, Diagnostic) || !Reader.U8(SectionCount, Diagnostic)) return false;
			if (ReadMagic != Magic || ReadVersion != Version)
				return Fail(Diagnostic, EReaderFailure::InvalidHeader,
					"Package magic or object-stream version is invalid.");
			if (SummaryLength > MaximumSummaryBytes)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded,
					"Public summary exceeds the frozen bound.", 8);
			if (SectionCount != RequiredSectionCount)
				return Fail(Diagnostic, EReaderFailure::InvalidDirectory,
					"the package object stream requires exactly five sections.", 12);
			std::span<const std::byte> SummaryBytes;
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
				if (End < Offset || End > PackageSize)
					return Fail(Diagnostic, EReaderFailure::InvalidDirectory,
						"Section extent exceeds the package.", Reader.Position() - 8);
				Result.Sections[Index] = {static_cast<ESectionKind>(Kind), Offset, Length};
				ExpectedOffset = End;
			}
			if (ExpectedOffset != PackageSize)
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
					uint64 FieldId = 0; uint8 Provenance = 0; std::span<const std::byte> Encoded; uint64 Offset = 0;
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
			case ETypeOpcode::Bytes: case ETypeOpcode::BulkData:
			{
				uint64 Count = 0; std::span<const std::byte> Data;
				if (!Reader.VarUInt(Count, Diagnostic) || Count > Limits.ByteValueBytes || !Reader.BytesSpan(Count, Data, Diagnostic))
					return Fail(Diagnostic, EReaderFailure::InvalidValue, "Byte value is invalid.", Reader.Position(), std::move(Path));
				Value.Bytes.assign(Data.begin(), Data.end()); break;
			}
			}
			OutValue = std::move(Value); return true;
		}

		auto DecodeTablesAndValues(
			const std::array<std::span<const std::byte>, RequiredSectionCount>& SectionBytes,
			const std::array<uint64, RequiredSectionCount>& SectionOffsets,
			FDecodedPackage& Package,
			const FReaderLimits& Limits, FReaderDiagnostic& Diagnostic,
			bool bDecodePayloads = true) -> bool
		{
			auto Section = [&](size_t Index) { return SectionBytes[Index]; };
			FWireReader Names(Section(0), SectionOffsets[0]);
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

			FWireReader Types(Section(1), SectionOffsets[1]);
			uint64 TypeCount = 0;
			if (!Types.VarUInt(TypeCount, Diagnostic) || TypeCount > Limits.TableEntries)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded, "Type count exceeds the configured bound.", Types.Position());
			Package.Types.reserve(static_cast<size_t>(TypeCount));
			for (uint64 Index = 0; Index < TypeCount; ++Index)
			{
				std::span<const std::byte> RecordBytes; uint64 Offset = 0;
				if (!Types.Record(RecordBytes, Diagnostic, &Offset)) return false;
				FWireReader Record(RecordBytes, Offset); uint8 Opcode = 0;
				if (!Record.U8(Opcode, Diagnostic) || Opcode < 1 || Opcode > 0x18)
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

			FWireReader Schemas(Section(2), SectionOffsets[2]);
			uint64 CustomCount = 0;
			if (!Schemas.VarUInt(CustomCount, Diagnostic) || CustomCount > Limits.CustomVersions)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded, "Custom-version count exceeds the configured bound.", Schemas.Position());
			for (uint64 Index = 0; Index < CustomCount; ++Index)
			{
				FCustomVersion Version; uint64 Value = 0;
				if (!Schemas.Fixed(Version.Guid.A, Diagnostic) || !Schemas.Fixed(Version.Guid.B, Diagnostic)
					|| !Schemas.Fixed(Version.Guid.C, Diagnostic) || !Schemas.Fixed(Version.Guid.D, Diagnostic)
					|| !Schemas.VarUInt(Value, Diagnostic) || Value > std::numeric_limits<int32>::max())
					return Fail(Diagnostic, EReaderFailure::InvalidTable, "Custom-version entry is invalid.", Schemas.Position());
				Version.Value = static_cast<uint32>(Value); Package.CustomVersions.push_back(Version);
			}
			uint64 SchemaCount = 0;
			if (!Schemas.VarUInt(SchemaCount, Diagnostic) || SchemaCount > Limits.TableEntries)
				return Fail(Diagnostic, EReaderFailure::LimitExceeded, "Schema count exceeds the configured bound.", Schemas.Position());
			for (uint64 Index = 0; Index < SchemaCount; ++Index)
			{
				std::span<const std::byte> RecordBytes; uint64 Offset = 0;
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

			FWireReader Objects(Section(3), SectionOffsets[3]); uint64 ObjectCount = 0;
			if (!Objects.VarUInt(ObjectCount, Diagnostic) || ObjectCount != Package.Header.ObjectCount)
				return Fail(Diagnostic, EReaderFailure::InvalidTopology, "Object table count differs from the public summary.", Objects.Position());
			for (uint64 Index = 0; Index < ObjectCount; ++Index)
			{
				std::span<const std::byte> RecordBytes; uint64 Offset = 0;
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

			FWireReader Values(Section(4), SectionOffsets[4]); uint64 ValueObjectCount = 0;
			if (!Values.VarUInt(ValueObjectCount, Diagnostic) || ValueObjectCount != ObjectCount)
				return Fail(Diagnostic, EReaderFailure::InvalidValue, "Value-section object count is invalid.", Values.Position());
			for (uint64 ObjectIndex = 0; ObjectIndex < ValueObjectCount; ++ObjectIndex)
			{
				std::span<const std::byte> BlockBytes; uint64 BlockOffset = 0;
				if (!Values.Record(BlockBytes, Diagnostic, &BlockOffset)) return false;
				FWireReader Block(BlockBytes, BlockOffset); uint64 OverrideCount = 0;
				if (!Block.VarUInt(OverrideCount, Diagnostic) || OverrideCount > Limits.TableEntries)
					return Fail(Diagnostic, EReaderFailure::LimitExceeded, "Override count exceeds the configured bound.", Block.Position());
				FDecodedObjectValues ObjectValues; uint64 PreviousSchema = 0, PreviousField = 0;
				for (uint64 Index = 0; Index < OverrideCount; ++Index)
				{
					FDecodedOverride Override; std::span<const std::byte> Payload;
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
					if (!bDecodePayloads)
					{
						// Record() already validated the complete extent. Compatibility
						// inspection intentionally leaves the payload untouched.
					}
					else if (Override.Provenance == 2)
					{
						FWireReader Unknown(Payload, Override.PayloadOffset); std::span<const std::byte> Closure, Retained;
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

		auto DecodeTablesAndValues(std::span<const std::byte> Bytes, FDecodedPackage& Package,
			const FReaderLimits& Limits, FReaderDiagnostic& Diagnostic,
			bool bDecodePayloads = true) -> bool
		{
			std::array<std::span<const std::byte>, RequiredSectionCount> Sections;
			std::array<uint64, RequiredSectionCount> Offsets;
			for (size_t Index = 0; Index < RequiredSectionCount; ++Index)
			{
				const auto& Entry = Package.Header.Sections[Index];
				Sections[Index] = Bytes.subspan(Entry.Offset, Entry.Length);
				Offsets[Index] = Entry.Offset;
			}
			return DecodeTablesAndValues(
				Sections, Offsets, Package, Limits, Diagnostic, bDecodePayloads);
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
			std::string_view DeclaringType, std::string& Name) -> void
		{
			DStructBase* Owner = FindClassBySerializedName(FName(DeclaringType));
			if (!Owner) Owner = FindStructBySerializedName(FName(DeclaringType));
			if (Owner)
				if (FProperty* Property =
					Owner->FindPropertyBySerializedName(FName(Name), false))
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
			else if (Type.Opcode == ETypeOpcode::HardRef
				|| Type.Opcode == ETypeOpcode::SoftRef)
				CanonicalizeSerializedClassName(Type.QualifiedName);
		}

		auto CanonicalizeSerializedReflectionNames(
			FDecodedPackage& Package, std::string* OutError = nullptr) -> bool
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

	}

	auto ReadHeader(std::span<const std::byte> Bytes, FValidatedHeader& OutHeader,
		const FReaderLimits& Limits, FReaderDiagnostic* OutDiagnostic,
		uint64 PackageSize) -> bool
	{
		FReaderDiagnostic Diagnostic; FValidatedHeader Result;
		const bool Success = DecodeHeaderInner(
			Bytes, PackageSize, Result, Limits, Diagnostic);
		if (Success)
		{
			CanonicalizeSerializedClassName(Result.AssetClass);
			OutHeader = std::move(Result);
		}
		if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic);
		return Success;
	}

	auto ReencodePackage(const FDecodedPackage& Package, FByteArray& OutBytes,
		FReaderDiagnostic* OutDiagnostic) -> bool
	{
		++GReencodeCountForTesting;
		FReaderDiagnostic Diagnostic; FPackageInput Input;
		if (!BuildWriterInput(Package, Input, Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic); return false;
		}
		FByteArray Bytes; FWriterDiagnostic WriterDiagnostic;
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

	auto DecodePackageStructure(std::span<const std::byte> Bytes, FDecodedPackage& OutPackage,
		const FReaderLimits& Limits, FReaderDiagnostic* OutDiagnostic) -> bool
	{
		FReaderDiagnostic Diagnostic; FDecodedPackage Result;
		if (!DecodeHeaderInner(
			Bytes, Bytes.size(), Result.Header, Limits, Diagnostic)
			|| !DecodeTablesAndValues(Bytes, Result, Limits, Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic); return false;
		}
		OutPackage = std::move(Result);
		if (OutDiagnostic) OutDiagnostic->Reset();
		return true;
	}

	auto DecodePackageDescriptors(std::span<const std::byte> Bytes,
		FDecodedPackage& OutPackage, const FReaderLimits& Limits,
		FReaderDiagnostic* OutDiagnostic) -> bool
	{
		FReaderDiagnostic Diagnostic;
		FDecodedPackage Result;
		if (!DecodeHeaderInner(Bytes, Bytes.size(), Result.Header, Limits, Diagnostic)
			|| !DecodeTablesAndValues(Bytes, Result, Limits, Diagnostic, false))
		{
			if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic);
			return false;
		}
		OutPackage = std::move(Result);
		if (OutDiagnostic) OutDiagnostic->Reset();
		return true;
	}

	auto DecodePackageDescriptorSections(FValidatedHeader Header,
		const std::array<std::span<const std::byte>, RequiredSectionCount>& Sections,
		const std::array<uint64, RequiredSectionCount>& SectionOffsets,
		FDecodedPackage& OutPackage, const FReaderLimits& Limits,
		FReaderDiagnostic* OutDiagnostic) -> bool
	{
		FReaderDiagnostic Diagnostic;
		FDecodedPackage Result{.Header = std::move(Header)};
		if (!DecodeTablesAndValues(
			Sections, SectionOffsets, Result, Limits, Diagnostic, false))
		{
			if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic);
			return false;
		}
		OutPackage = std::move(Result);
		if (OutDiagnostic) OutDiagnostic->Reset();
		return true;
	}

	auto DecodePackage(std::span<const std::byte> Bytes, FDecodedPackage& OutPackage,
		const FReaderLimits& Limits, FReaderDiagnostic* OutDiagnostic) -> bool
	{
		FReaderDiagnostic Diagnostic;
		FDecodedPackage Result;
		if (!DecodePackageStructure(Bytes, Result, Limits, &Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic); return false;
		}
		FByteArray Canonical;
		if (!ReencodePackage(Result, Canonical, &Diagnostic))
		{
			if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic); return false;
		}
		if (!std::ranges::equal(Bytes, Canonical))
		{
			Fail(Diagnostic, EReaderFailure::NonCanonical,
				"Decoded package does not match canonical object-stream re-emission.");
			if (OutDiagnostic) *OutDiagnostic = std::move(Diagnostic); return false;
		}
		OutPackage = std::move(Result);
		if (OutDiagnostic) OutDiagnostic->Reset();
		return true;
	}

	auto ResetReencodeCountForTesting() -> void
	{
		GReencodeCountForTesting = 0;
	}

	auto GetReencodeCountForTesting() -> uint64
	{
		return GReencodeCountForTesting;
	}

}
