#include "DObject/PackageFormat.h"

#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::ObjectPackage
{
	namespace
	{
		struct FDirectoryEntry
		{
			EDastV8Section Kind{};
			uint64 Offset = 0;
			uint64 Size = 0;
			FXxHash128 Hash;
		};

		struct FParsedLayout
		{
			FBinaryEnvelopePreamble Preamble;
			bool bRedirect = false;
			std::array<FDirectoryEntry, DastV8SectionCount> Entries;
			std::span<const std::byte> AvailableBytes;
		};

		struct FRawType
		{
			EValueKind Kind{};
			uint32 NameId = 0;
			uint64 Parameter = 0;
			std::vector<uint32> Children;
		};

		struct FBulkEntry
		{
			uint32 ExportId = 0;
			uint32 SchemaId = 0;
			uint32 FieldId = 0;
			uint32 PathNameId = 0;
			uint64 LogicalSize = 0;
			FXxHash128 Hash;
			uint32 ElementSize = 0;
			uint32 Alignment = 0;
			EBulkStorageKind Storage = EBulkStorageKind::Unset;
			uint64 Offset = 0;
			uint64 Size = 0;
		};

		auto Fail(FPackageReaderDiagnostic* Diagnostic, EPackageReaderFailure Failure,
			std::string Message, std::string Path = {}) -> bool
		{
			if (Diagnostic) *Diagnostic = {Failure, std::move(Path), std::move(Message)};
			return false;
		}

		auto BytewiseLess(std::string_view Left, std::string_view Right) -> bool
		{
			return std::lexicographical_compare(Left.begin(), Left.end(), Right.begin(), Right.end(),
				[](char A, char B) { return static_cast<uint8>(A) < static_cast<uint8>(B); });
		}

		auto IsValidUtf8(std::string_view Value) -> bool
		{
			for (size_t Index = 0; Index < Value.size();)
			{
				const uint8 Lead = static_cast<uint8>(Value[Index++]);
				if (Lead < 0x80) continue;
				uint32 Code = 0;
				size_t Count = 0;
				if ((Lead & 0xe0) == 0xc0) { Code = Lead & 0x1f; Count = 1; }
				else if ((Lead & 0xf0) == 0xe0) { Code = Lead & 0x0f; Count = 2; }
				else if ((Lead & 0xf8) == 0xf0) { Code = Lead & 0x07; Count = 3; }
				else return false;
				if (Index + Count > Value.size()) return false;
				for (size_t Part = 0; Part < Count; ++Part)
				{
					const uint8 Next = static_cast<uint8>(Value[Index++]);
					if ((Next & 0xc0) != 0x80) return false;
					Code = (Code << 6) | (Next & 0x3f);
				}
				if ((Count == 1 && Code < 0x80) || (Count == 2 && Code < 0x800)
					|| (Count == 3 && Code < 0x10000) || Code > 0x10ffff
					|| (Code >= 0xd800 && Code <= 0xdfff)) return false;
			}
			return true;
		}

		template<std::unsigned_integral T>
		auto ReadAt(std::span<const std::byte> Bytes, uint64 Offset, T& Out) -> bool
		{
			return ReadLittleEndianAt(Bytes, Offset, Out);
		}

		auto ValidateLimits(const FPackageReaderLimits& Limits,
			FPackageReaderDiagnostic* Diagnostic) -> bool
		{
			if (Limits.MaximumHeaderBytes < BinaryEnvelopePreambleBytes
				|| Limits.MaximumHeaderBytes > Limits.MaximumPackageBytes
				|| Limits.MaximumPackageBytes > DastV8MaximumPackageBytes
				|| Limits.MaximumBulkBytes > DastV8MaximumBulkBytes
				|| Limits.MaximumTableEntries > DastV8MaximumTableEntries
				|| Limits.MaximumStringBytes > DastV8MaximumStringBytes
				|| Limits.MaximumContainerElements > DastV8MaximumContainerElements
				|| Limits.MaximumValueDepth > DastV8MaximumValueDepth)
				return Fail(Diagnostic, EPackageReaderFailure::LimitExceeded,
					"DAST v8 reader limits are internally inconsistent or exceed format limits.");
			return true;
		}

		auto ParseLayout(std::span<const std::byte> Available, uint64 PhysicalBytes,
			bool bComplete, const FPackageReaderLimits& Limits, FParsedLayout& Out,
			FPackageReaderDiagnostic* Diagnostic) -> bool
		{
			if (!ValidateLimits(Limits, Diagnostic)) return false;
			if (PhysicalBytes > Limits.MaximumPackageBytes || Available.size() < BinaryEnvelopePreambleBytes)
				return Fail(Diagnostic, EPackageReaderFailure::LimitExceeded,
					"DAST v8 input is truncated or exceeds the package limit.");
			FBinaryEnvelopePreamble Preamble;
			FBinaryEnvelopeDiagnostic EnvelopeDiagnostic;
			if (!ParseBinaryEnvelopePrefix(Available.first(BinaryEnvelopePreambleBytes), PhysicalBytes,
				{Limits.MaximumHeaderBytes, Limits.MaximumPackageBytes}, Preamble, &EnvelopeDiagnostic))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidEnvelope,
					std::string(EnvelopeDiagnostic.Message));
			if (Preamble.HeaderBytes > Available.size()
				|| (!bComplete && Preamble.HeaderBytes != Available.size())
				|| (bComplete && PhysicalBytes != Available.size()))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidEnvelope,
					"DAST v8 input does not match its declared header or file extent.");
			FBinaryFormatRegistry Registry;
			const FBinaryFormatDescriptor Descriptor{
				.FormatId = DastFormatId, .DebugName = std::string(DastFormatName),
				.MinimumFormatVersion = DastV8FormatVersion,
				.MaximumFormatVersion = DastV8FormatVersion,
				.SupportedRequiredFeatures = 0,
				.Limits = {Limits.MaximumHeaderBytes, Limits.MaximumPackageBytes}};
			if (!FBinaryFormatRegistry::Create(std::span(&Descriptor, 1), Registry, &EnvelopeDiagnostic))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidEnvelope,
					std::string(EnvelopeDiagnostic.Message));
			FValidatedBinaryEnvelope Validated;
			if (!ValidateBinaryEnvelopeHeader(Available.first(static_cast<size_t>(Preamble.HeaderBytes)),
				PhysicalBytes, {Limits.MaximumHeaderBytes, Limits.MaximumPackageBytes}, Registry,
				Validated, &EnvelopeDiagnostic))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidEnvelope,
					std::string(EnvelopeDiagnostic.Message));

			uint32 PackageKind = 0, Flags = 0, SectionCount = 0, EntryBytes = 0;
			uint64 Directory = 0, Reserved = 0;
			if (!ReadAt(Available, DastV8FormatHeaderOffset, PackageKind)
				|| !ReadAt(Available, DastV8FormatHeaderOffset + 4, Flags)
				|| !ReadAt(Available, DastV8FormatHeaderOffset + 8, Directory)
				|| !ReadAt(Available, DastV8FormatHeaderOffset + 16, SectionCount)
				|| !ReadAt(Available, DastV8FormatHeaderOffset + 20, EntryBytes)
				|| !ReadAt(Available, DastV8FormatHeaderOffset + 24, Reserved)
				|| PackageKind > 1 || Flags != 0 || Directory != DastV8DirectoryOffset
				|| SectionCount != DastV8SectionCount || EntryBytes != DastV8SectionEntryBytes
				|| Reserved != 0)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidFormatHeader,
					"DAST v8 format header is invalid.");

			FParsedLayout Parsed;
			Parsed.Preamble = Preamble;
			Parsed.bRedirect = PackageKind == 1;
			Parsed.AvailableBytes = Available;
			uint64 ExpectedOffset = DastV8FirstSectionOffset;
			for (uint32 Index = 0; Index < DastV8SectionCount; ++Index)
			{
				const uint64 Base = DastV8DirectoryOffset + uint64(Index) * DastV8SectionEntryBytes;
				uint32 Kind = 0, EntryFlags = 0;
				uint64 Offset = 0, Size = 0, HashLow = 0, HashHigh = 0, EntryReserved = 0;
				if (!ReadAt(Available, Base, Kind) || !ReadAt(Available, Base + 4, EntryFlags)
					|| !ReadAt(Available, Base + 8, Offset) || !ReadAt(Available, Base + 16, Size)
					|| !ReadAt(Available, Base + 24, HashLow) || !ReadAt(Available, Base + 32, HashHigh)
					|| !ReadAt(Available, Base + 40, EntryReserved)
					|| Kind != Index + 1 || EntryFlags != 1 || EntryReserved != 0
					|| Offset != ExpectedOffset || Size > PhysicalBytes - std::min(Offset, PhysicalBytes))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidDirectory,
						"DAST v8 section directory is invalid.", "Directory[" + std::to_string(Index) + "]");
				if (Offset > PhysicalBytes || Size > PhysicalBytes - Offset)
					return Fail(Diagnostic, EPackageReaderFailure::ArithmeticOverflow,
						"DAST v8 section extent overflows the file.", "Directory[" + std::to_string(Index) + "]");
				Parsed.Entries[Index] = {static_cast<EDastV8Section>(Kind), Offset, Size, {HashLow, HashHigh}};
				ExpectedOffset = Offset + Size;
				if ((bComplete || Index <= 2)
					&& FXxHash128::HashBuffer(Available.subspan(static_cast<size_t>(Offset), static_cast<size_t>(Size)))
						!= Parsed.Entries[Index].Hash)
					return Fail(Diagnostic, EPackageReaderFailure::HashMismatch,
						"A DAST v8 section digest does not match.", "Directory[" + std::to_string(Index) + "]");
			}
			if (ExpectedOffset != PhysicalBytes
				|| Preamble.HeaderBytes != Parsed.Entries[2].Offset + Parsed.Entries[2].Size)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidDirectory,
					"DAST v8 sections or header boundary do not cover their exact declared extents.");
			Out = Parsed;
			return true;
		}

		auto Section(const FParsedLayout& Layout, EDastV8Section Kind) -> std::span<const std::byte>
		{
			const FDirectoryEntry& Entry = Layout.Entries[static_cast<size_t>(Kind) - 1];
			return Layout.AvailableBytes.subspan(static_cast<size_t>(Entry.Offset), static_cast<size_t>(Entry.Size));
		}

		auto ReadNameId(FBinaryReader& Reader, const std::vector<std::string>& Names,
			uint32& Out, bool bNullable = false) -> bool
		{
			uint64 Id = 0;
			if (!Reader.ReadVarUInt(Id) || Id > Names.size() || (!bNullable && Id == 0)) return false;
			Out = static_cast<uint32>(Id);
			return true;
		}

		auto DecodeNames(const FParsedLayout& Layout, const FPackageReaderLimits& Limits,
			std::vector<std::string>& Out, FPackageReaderDiagnostic* Diagnostic) -> bool
		{
			FBinaryReader Reader(Section(Layout, EDastV8Section::Names),
				{Layout.Entries[1].Size, Limits.MaximumStringBytes});
			uint32 Version = 0;
			uint64 Count = 0;
			if (!Reader.ReadU32(Version) || Version != DastV8TableVersion
				|| !Reader.ReadVarUInt(Count) || Count > Limits.MaximumTableEntries)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
					"DAST v8 name table header is invalid.", "Names");
			std::vector<std::string> Names;
			Names.reserve(static_cast<size_t>(Count));
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				std::string Name;
				if (!Reader.ReadString(Name, Limits.MaximumStringBytes) || Name.empty() || !IsValidUtf8(Name)
					|| (!Names.empty() && !BytewiseLess(Names.back(), Name)))
					return Fail(Diagnostic, EPackageReaderFailure::NonCanonical,
						"DAST v8 names are invalid, duplicate, or out of canonical order.",
						"Names[" + std::to_string(Index) + "]");
				Names.push_back(std::move(Name));
			}
			if (!Reader.IsAtEnd()) return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
				"DAST v8 name table has trailing or malformed bytes.", "Names");
			Out = std::move(Names);
			return true;
		}

		auto DecodeImports(const FParsedLayout& Layout, const std::vector<std::string>& Names,
			const FPackageReaderLimits& Limits, std::vector<FPackageImport>& Out,
			FPackageReaderDiagnostic* Diagnostic) -> bool
		{
			FBinaryReader Reader(Section(Layout, EDastV8Section::Imports),
				{Layout.Entries[2].Size, Limits.MaximumStringBytes});
			uint32 Version = 0;
			uint64 Count = 0;
			if (!Reader.ReadU32(Version) || Version != DastV8TableVersion
				|| !Reader.ReadVarUInt(Count) || Count > Limits.MaximumTableEntries)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
					"DAST v8 import table header is invalid.", "Imports");
			std::vector<FPackageImport> Imports;
			Imports.reserve(static_cast<size_t>(Count));
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				uint32 PackageId = 0, ObjectId = 0, ClassId = 0;
				int64 Outer = 0;
				FPackageIndex OuterIndex;
				if (!ReadNameId(Reader, Names, PackageId) || !ReadNameId(Reader, Names, ObjectId, true)
					|| !ReadNameId(Reader, Names, ClassId, true) || !Reader.ReadVarInt(Outer)
					|| !FPackageIndex::TryFromRaw(Outer, OuterIndex))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
						"A DAST v8 import record is invalid.", "Imports[" + std::to_string(Index) + "]");
				Imports.push_back({Names[PackageId - 1], ObjectId ? Names[ObjectId - 1] : std::string{},
					ClassId ? Names[ClassId - 1] : std::string{}, OuterIndex});
			}
			if (!Reader.IsAtEnd()) return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
				"DAST v8 import table has trailing or malformed bytes.", "Imports");
			Out = std::move(Imports);
			return true;
		}

		auto DecodeRegistry(const FParsedLayout& Layout, const std::vector<std::string>& Names,
			std::string_view PackageName, uint64 PhysicalBulkBytes, FPackageV8RegistryData& Out,
			FPackageReaderDiagnostic* Diagnostic) -> bool
		{
			if (PackageName.empty() || !IsValidUtf8(PackageName)
				|| !std::ranges::binary_search(Names, PackageName, BytewiseLess))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidRegistry,
					"The caller-supplied package identity is absent from the canonical name table.",
					"Registry.PackageName");
			FBinaryReader Reader(Section(Layout, EDastV8Section::Registry));
			uint32 Version = 0, AssetClassId = 0, RedirectId = 0;
			uint64 MainExport = 0, ExportCount = 0;
			if (!Reader.ReadU32(Version) || Version != DastV8RegistryVersion
				|| !ReadNameId(Reader, Names, AssetClassId)
				|| !ReadNameId(Reader, Names, RedirectId, true)
				|| !Reader.ReadVarUInt(MainExport) || !Reader.ReadVarUInt(ExportCount)
				|| MainExport > std::numeric_limits<uint32>::max()
				|| ExportCount > DastV8MaximumTableEntries || MainExport > ExportCount
				|| (Layout.bRedirect != (RedirectId != 0)))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidRegistry,
					"DAST v8 Registry identity fields are invalid.", "Registry");
			FPackageV8RegistryData Registry{.PackageName = std::string(PackageName),
				.AssetClass = Names[AssetClassId - 1], .bRedirect = Layout.bRedirect,
				.RedirectDestination = RedirectId ? Names[RedirectId - 1] : std::string{},
				.MainExportId = static_cast<uint32>(MainExport),
				.ExportCount = static_cast<uint32>(ExportCount)};
			for (std::vector<std::string>* List : {&Registry.HardPackageReferences,
				&Registry.SoftPackageReferences, &Registry.SearchableNames})
			{
				uint64 Count = 0;
				if (!Reader.ReadVarUInt(Count) || Count > DastV8MaximumTableEntries)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidRegistry,
						"A DAST v8 Registry list count is invalid.", "Registry");
				uint32 Previous = 0;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					uint32 Id = 0;
					if (!ReadNameId(Reader, Names, Id) || Id <= Previous)
						return Fail(Diagnostic, EPackageReaderFailure::NonCanonical,
							"A DAST v8 Registry id list is duplicate or out of order.", "Registry");
					Previous = Id;
					List->push_back(Names[Id - 1]);
				}
			}
			if (!Reader.ReadU64(Registry.ExternalBulkBytes)
				|| !Reader.ReadHash128(Registry.ExternalBulkHash) || !Reader.IsAtEnd()
				|| Registry.ExternalBulkBytes != PhysicalBulkBytes
				|| (Registry.ExternalBulkBytes == 0 && !Registry.ExternalBulkHash.IsZero()))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidRegistry,
					"DAST v8 Registry bulk binding or extent is invalid.", "Registry.Bulk");
			Out = std::move(Registry);
			return true;
		}

		auto DecodeExports(const FParsedLayout& Layout, const std::vector<std::string>& Names,
			const FPackageReaderLimits& Limits, std::vector<FPackageExport>& Out,
			FPackageReaderDiagnostic* Diagnostic) -> bool
		{
			FBinaryReader Reader(Section(Layout, EDastV8Section::Exports));
			uint32 Version = 0;
			uint64 Count = 0;
			if (!Reader.ReadU32(Version) || Version != DastV8TableVersion
				|| !Reader.ReadVarUInt(Count) || Count > Limits.MaximumTableEntries)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
					"DAST v8 export table header is invalid.", "Exports");
			std::vector<FPackageExport> Exports;
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				uint32 ObjectId = 0, ClassId = 0;
				int64 Outer = 0;
				FPackageIndex OuterIndex;
				if (!ReadNameId(Reader, Names, ObjectId) || !ReadNameId(Reader, Names, ClassId)
					|| !Reader.ReadVarInt(Outer) || !FPackageIndex::TryFromRaw(Outer, OuterIndex))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
						"A DAST v8 export record is invalid.", "Exports[" + std::to_string(Index) + "]");
				Exports.push_back({.ObjectName = Names[ObjectId - 1], .ClassName = Names[ClassId - 1],
					.Outer = OuterIndex});
			}
			if (!Reader.IsAtEnd()) return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
				"DAST v8 export table has trailing or malformed bytes.", "Exports");
			Out = std::move(Exports);
			return true;
		}

		auto DecodeTypes(const FParsedLayout& Layout, const std::vector<std::string>& Names,
			const FPackageReaderLimits& Limits, std::vector<FSerializedType>& Out,
			FPackageReaderDiagnostic* Diagnostic) -> bool
		{
			FBinaryReader Reader(Section(Layout, EDastV8Section::Types));
			uint32 Version = 0;
			uint64 Count = 0;
			if (!Reader.ReadU32(Version) || Version != DastV8TableVersion
				|| !Reader.ReadVarUInt(Count) || Count > Limits.MaximumTableEntries)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
					"DAST v8 type table header is invalid.", "Types");
			std::vector<FRawType> Raw;
			Raw.reserve(static_cast<size_t>(Count));
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				uint64 RecordBytes = 0;
				if (!Reader.ReadVarUInt(RecordBytes) || RecordBytes > Reader.GetRemainingBytes())
					return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
						"A DAST v8 type record extent is invalid.", "Types[" + std::to_string(Index) + "]");
				std::span<const std::byte> RecordSpan;
				if (!Reader.ReadRegion(RecordSpan, RecordBytes, Layout.Entries[4].Size)) return false;
				FBinaryReader Record(RecordSpan);
				uint8 Tag = 0;
				uint32 NameId = 0;
				uint64 Parameter = 0, ChildCount = 0;
				if (!Record.ReadU8(Tag) || Tag == 0 || Tag > uint8(EValueKind::BulkData) + 1
					|| !ReadNameId(Record, Names, NameId, true) || !Record.ReadVarUInt(Parameter)
					|| !Record.ReadVarUInt(ChildCount) || ChildCount > Limits.MaximumTableEntries)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
						"A DAST v8 type record header is invalid.", "Types[" + std::to_string(Index) + "]");
				FRawType Type{static_cast<EValueKind>(Tag - 1), NameId, Parameter};
				for (uint64 Child = 0; Child < ChildCount; ++Child)
				{
					uint64 Id = 0;
					if (!Record.ReadVarUInt(Id) || Id == 0 || Id > Count)
						return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
							"A DAST v8 child type id is invalid.", "Types[" + std::to_string(Index) + "]");
					Type.Children.push_back(static_cast<uint32>(Id));
				}
				if (!Record.IsAtEnd()) return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
					"A DAST v8 type record has trailing bytes.", "Types[" + std::to_string(Index) + "]");
				Raw.push_back(std::move(Type));
			}
			if (!Reader.IsAtEnd()) return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
				"DAST v8 type table has trailing bytes.", "Types");

			std::vector<std::optional<FSerializedType>> Cache(Raw.size());
			std::vector<bool> Active(Raw.size());
			std::function<bool(uint32, FSerializedType&)> Build = [&](uint32 Id, FSerializedType& Result)
			{
				const size_t Index = Id - 1;
				if (Cache[Index]) { Result = *Cache[Index]; return true; }
				if (Active[Index]) return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
					"DAST v8 structural type graph contains a cycle.", "Types[" + std::to_string(Index) + "]");
				Active[Index] = true;
				const FRawType& Source = Raw[Index];
				FSerializedType Type{.Kind = Source.Kind,
					.QualifiedName = Source.NameId ? Names[Source.NameId - 1] : std::string{},
					.Parameter = Source.Parameter};
				for (uint32 ChildId : Source.Children)
				{
					FSerializedType Child;
					if (!Build(ChildId, Child)) return false;
					Type.Children.push_back(std::move(Child));
				}
				Active[Index] = false;
				Cache[Index] = Type;
				Result = std::move(Type);
				return true;
			};
			std::vector<FSerializedType> Types;
			for (uint32 Id = 1; Id <= Raw.size(); ++Id)
			{
				FSerializedType Type;
				if (!Build(Id, Type)) return false;
				Types.push_back(std::move(Type));
			}
			Out = std::move(Types);
			return true;
		}

		auto DecodeSchemas(const FParsedLayout& Layout, const std::vector<std::string>& Names,
			const std::vector<FSerializedType>& Types, const FPackageReaderLimits& Limits,
			std::vector<FCustomVersion>& OutVersions, std::vector<FSerializedSchema>& OutSchemas,
			FPackageReaderDiagnostic* Diagnostic) -> bool
		{
			FBinaryReader Reader(Section(Layout, EDastV8Section::Schemas));
			uint32 Version = 0;
			uint64 VersionCount = 0;
			if (!Reader.ReadU32(Version) || Version != DastV8TableVersion
				|| !Reader.ReadVarUInt(VersionCount) || VersionCount > Limits.MaximumTableEntries)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
					"DAST v8 schema section header is invalid.", "Schemas");
			std::vector<FCustomVersion> Versions;
			for (uint64 Index = 0; Index < VersionCount; ++Index)
			{
				FCustomVersion Custom;
				uint8 Flags = 0;
				if (!Reader.ReadGuid(Custom.Guid) || !Reader.ReadU32(Custom.Value)
					|| !Reader.ReadU8(Flags) || (Flags & 0xf0) != 0)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
						"A DAST v8 custom-version record is invalid.", "CustomVersions");
				uint32 Optional = 0;
				if ((Flags & 1) != 0) { if (!Reader.ReadU32(Optional)) return false; Custom.EmissionValue = Optional; }
				if ((Flags & 2) != 0) { if (!Reader.ReadU32(Optional)) return false; Custom.MaximumSupported = Optional; }
				Custom.bCodecKnown = (Flags & 4) != 0;
				Custom.bRequiredForInterpretation = (Flags & 8) != 0;
				Versions.push_back(Custom);
			}
			uint64 SchemaCount = 0;
			if (!Reader.ReadVarUInt(SchemaCount) || SchemaCount > Limits.MaximumTableEntries)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
					"DAST v8 schema count is invalid.", "Schemas");
			std::vector<FSerializedSchema> Schemas;
			for (uint64 SchemaIndex = 0; SchemaIndex < SchemaCount; ++SchemaIndex)
			{
				uint32 NameId = 0;
				uint64 FieldCount = 0;
				if (!ReadNameId(Reader, Names, NameId) || !Reader.ReadVarUInt(FieldCount)
					|| FieldCount > Limits.MaximumTableEntries)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
						"A DAST v8 schema header is invalid.", "Schemas");
				FSerializedSchema Schema{.QualifiedName = Names[NameId - 1]};
				for (uint64 FieldIndex = 0; FieldIndex < FieldCount; ++FieldIndex)
				{
					uint32 FieldNameId = 0;
					uint64 TypeId = 0, Flags = 0;
					if (!ReadNameId(Reader, Names, FieldNameId) || !Reader.ReadVarUInt(TypeId)
						|| TypeId == 0 || TypeId > Types.size() || !Reader.ReadVarUInt(Flags))
						return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
							"A DAST v8 schema field is invalid.", "Schemas");
					Schema.Fields.push_back({Names[FieldNameId - 1], Types[TypeId - 1], Flags});
				}
				Schemas.push_back(std::move(Schema));
			}
			if (!Reader.IsAtEnd()) return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
				"DAST v8 schema section has trailing bytes.", "Schemas");
			OutVersions = std::move(Versions);
			OutSchemas = std::move(Schemas);
			return true;
		}

		auto ReadValue(FBinaryReader& Reader, const std::vector<std::string>& Names,
			const std::vector<FSerializedType>& Types, const FSerializedType& Type,
			const FPackageReaderLimits& Limits, FSerializedValue& Out, uint32 Depth,
			FPackageReaderDiagnostic* Diagnostic, std::string Path) -> bool
		{
			if (Depth > Limits.MaximumValueDepth) return Fail(Diagnostic,
				EPackageReaderFailure::LimitExceeded, "A DAST v8 value exceeds the nesting limit.", Path);
			uint8 Tag = 0;
			if (!Reader.ReadU8(Tag) || Tag != static_cast<uint8>(Type.Kind) + 1)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidValue,
					"A DAST v8 value tag does not match its declared type.", Path);
			FSerializedValue Value;
			switch (Type.Kind)
			{
			case EValueKind::Bool:
			{ uint8 V = 0; if (!Reader.ReadU8(V) || V > 1) return false; Value.Bool = V != 0; break; }
			case EValueKind::I8: case EValueKind::I16: case EValueKind::I32: case EValueKind::I64:
				if (!Reader.ReadVarInt(Value.Signed)) return false; break;
			case EValueKind::U8: case EValueKind::U16: case EValueKind::U32: case EValueKind::U64:
				if (!Reader.ReadVarUInt(Value.Unsigned)) return false; break;
			case EValueKind::F32:
			{ uint32 V = 0; if (!Reader.ReadU32(V)) return false; Value.FloatingBits = V; break; }
			case EValueKind::F64: if (!Reader.ReadU64(Value.FloatingBits)) return false; break;
			case EValueKind::String:
				if (!Reader.ReadString(Value.Text, Limits.MaximumStringBytes) || !IsValidUtf8(Value.Text)) return false;
				break;
			case EValueKind::Name:
			{
				uint32 NameId = 0;
				uint64 Number = 0;
				if (!ReadNameId(Reader, Names, NameId) || !Reader.ReadVarUInt(Number)
					|| Number > std::numeric_limits<uint32>::max()) return false;
				Value.Text = Names[NameId - 1]; Value.NameNumber = static_cast<uint32>(Number); break;
			}
			case EValueKind::Guid: if (!Reader.ReadGuid(Value.Guid)) return false; break;
			case EValueKind::Enum:
				if (Type.Parameter >= uint64(EValueKind::I8) && Type.Parameter <= uint64(EValueKind::I64))
				{ if (!Reader.ReadVarInt(Value.Signed)) return false; }
				else if (!Reader.ReadVarUInt(Value.Unsigned)) return false;
				break;
			case EValueKind::Intrinsic:
			{
				uint64 Count = 0;
				if (!Reader.ReadVarUInt(Count) || Count > 10) return false;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					uint64 Bits = 0;
					if (Type.Parameter == 6) { uint32 Narrow = 0; if (!Reader.ReadU32(Narrow)) return false; Bits = Narrow; }
					else if (!Reader.ReadU64(Bits)) return false;
					Value.ComponentBits.push_back(Bits);
				}
				break;
			}
			case EValueKind::Struct:
			{
				uint64 Count = 0;
				if (!Reader.ReadVarUInt(Count) || Count != Type.Children.size()) return false;
				struct FField { std::string Name; FSerializedType Type; EPropertyProvenance Provenance{}; FSerializedValue Value; };
				std::vector<FField> Fields;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					uint32 NameId = 0;
					uint64 TypeId = 0;
					uint8 Provenance = 0;
					if (!ReadNameId(Reader, Names, NameId) || !Reader.ReadVarUInt(TypeId)
						|| TypeId == 0 || TypeId > Types.size() || !Reader.ReadU8(Provenance)
						|| Provenance > uint8(EPropertyProvenance::Forced)) return false;
					FSerializedValue Child;
					if (!ReadValue(Reader, Names, Types, Types[TypeId - 1], Limits, Child, Depth + 1,
						Diagnostic, Path + "." + Names[NameId - 1])) return false;
					Fields.push_back({Names[NameId - 1], Types[TypeId - 1],
						static_cast<EPropertyProvenance>(Provenance), std::move(Child)});
				}
				std::vector<bool> Used(Fields.size());
				for (const FSerializedType& ChildType : Type.Children)
				{
					const auto It = std::find_if(Fields.begin(), Fields.end(), [&](const FField& Field)
					{ return !Used[&Field - Fields.data()] && Field.Type == ChildType; });
					if (It == Fields.end()) return false;
					const size_t Index = static_cast<size_t>(It - Fields.begin()); Used[Index] = true;
					Value.FieldNames.push_back(It->Name); Value.Provenances.push_back(It->Provenance);
					Value.Elements.push_back(std::move(It->Value));
				}
				break;
			}
			case EValueKind::FixedArray:
			case EValueKind::Array:
			{
				uint64 Count = 0;
				if (!Reader.ReadVarUInt(Count) || Count > Limits.MaximumContainerElements) return false;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					FSerializedValue Child;
					if (!ReadValue(Reader, Names, Types, Type.Children.front(), Limits, Child, Depth + 1,
						Diagnostic, Path + "[" + std::to_string(Index) + "]")) return false;
					Value.Elements.push_back(std::move(Child));
				}
				break;
			}
			case EValueKind::Map:
			{
				uint64 Count = 0;
				if (!Reader.ReadVarUInt(Count) || Count > Limits.MaximumContainerElements) return false;
				for (uint64 Index = 0; Index < Count; ++Index)
					for (uint32 Part = 0; Part < 2; ++Part)
					{
						FSerializedValue Child;
						if (!ReadValue(Reader, Names, Types, Type.Children[Part], Limits, Child, Depth + 1,
							Diagnostic, Path + "[" + std::to_string(Index) + "]")) return false;
						Value.Elements.push_back(std::move(Child));
					}
				break;
			}
			case EValueKind::HardReference:
			{ int64 Raw = 0; if (!Reader.ReadVarInt(Raw) || !FPackageIndex::TryFromRaw(Raw, Value.Reference)) return false; break; }
			case EValueKind::SoftReference:
			{ uint32 NameId = 0; if (!ReadNameId(Reader, Names, NameId, true)) return false;
				if (NameId) Value.Text = Names[NameId - 1]; break; }
			case EValueKind::Byte:
			{ uint8 V = 0; if (!Reader.ReadU8(V)) return false; Value.Unsigned = V; break; }
			case EValueKind::Bytes:
			{
				uint64 Count = 0; std::span<const std::byte> Bytes;
				if (!Reader.ReadVarUInt(Count) || Count > Limits.MaximumPackageBytes
					|| !Reader.ReadRegion(Bytes, Count, Limits.MaximumPackageBytes)) return false;
				Value.Bytes.assign(Bytes.begin(), Bytes.end()); break;
			}
			case EValueKind::BulkData:
				if (!Reader.ReadVarUInt(Value.Unsigned) || Value.Unsigned == 0
					|| Value.Unsigned > Limits.MaximumTableEntries) return false;
				break;
			}
			Out = std::move(Value);
			return true;
		}

		auto DecodeValues(const FParsedLayout& Layout, const std::vector<std::string>& Names,
			const std::vector<FSerializedType>& Types, const std::vector<FSerializedSchema>& Schemas,
			const FPackageReaderLimits& Limits, std::vector<FPackageExport>& Exports,
			FPackageReaderDiagnostic* Diagnostic) -> bool
		{
			FBinaryReader Reader(Section(Layout, EDastV8Section::Values));
			uint32 Version = 0;
			uint64 ExportCount = 0;
			if (!Reader.ReadU32(Version) || Version != DastV8TableVersion
				|| !Reader.ReadVarUInt(ExportCount) || ExportCount != Exports.size()) return false;
			for (uint64 ExportIndex = 0; ExportIndex < ExportCount; ++ExportIndex)
			{
				uint64 ExportId = 0, PropertyCount = 0;
				if (!Reader.ReadVarUInt(ExportId) || ExportId != ExportIndex + 1
					|| !Reader.ReadVarUInt(PropertyCount) || PropertyCount > Limits.MaximumTableEntries) return false;
				for (uint64 PropertyIndex = 0; PropertyIndex < PropertyCount; ++PropertyIndex)
				{
					uint64 SchemaId = 0, FieldId = 0, TypeId = 0;
					uint8 Provenance = 0;
					if (!Reader.ReadVarUInt(SchemaId) || SchemaId == 0 || SchemaId > Schemas.size()
						|| !Reader.ReadVarUInt(FieldId) || FieldId == 0 || FieldId > Schemas[SchemaId - 1].Fields.size()
						|| !Reader.ReadVarUInt(TypeId) || TypeId == 0 || TypeId > Types.size()
						|| !Reader.ReadU8(Provenance) || Provenance > uint8(EPropertyProvenance::Forced)) return false;
					const FSerializedSchema& Schema = Schemas[SchemaId - 1];
					const FSerializedField& Field = Schema.Fields[FieldId - 1];
					if (Field.Type != Types[TypeId - 1]) return false;
					FPropertyTag Property{.DeclaringType = Schema.QualifiedName, .FieldName = Field.Name,
						.Type = Types[TypeId - 1], .Provenance = static_cast<EPropertyProvenance>(Provenance)};
					const std::string Path = Exports[ExportIndex].ObjectName + "." + Schema.QualifiedName + "." + Field.Name;
					if (!ReadValue(Reader, Names, Types, Property.Type, Limits, Property.Value, 0, Diagnostic, Path))
						return Fail(Diagnostic, EPackageReaderFailure::InvalidValue,
							"A DAST v8 property value is malformed.", Path);
					Exports[ExportIndex].Properties.push_back(std::move(Property));
				}
			}
			return Reader.IsAtEnd() || Fail(Diagnostic, EPackageReaderFailure::InvalidValue,
				"DAST v8 value section has trailing bytes.", "Values");
		}

		auto DecodeBulkDirectory(const FParsedLayout& Layout, const std::vector<std::string>& Names,
			const FPackageReaderLimits& Limits, std::vector<FBulkEntry>& Out,
			FPackageReaderDiagnostic* Diagnostic) -> bool
		{
			FBinaryReader Reader(Section(Layout, EDastV8Section::BulkDirectory));
			uint32 Version = 0;
			uint64 Count = 0;
			if (!Reader.ReadU32(Version) || Version != DastV8TableVersion
				|| !Reader.ReadVarUInt(Count) || Count > Limits.MaximumTableEntries) return false;
			std::vector<FBulkEntry> Entries;
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				FBulkEntry Entry;
				uint64 ExportId = 0, SchemaId = 0, FieldId = 0;
				uint8 Storage = 0;
				if (!Reader.ReadVarUInt(ExportId) || ExportId > std::numeric_limits<uint32>::max()
					|| !Reader.ReadVarUInt(SchemaId) || SchemaId > std::numeric_limits<uint32>::max()
					|| !Reader.ReadVarUInt(FieldId) || FieldId > std::numeric_limits<uint32>::max()
					|| !ReadNameId(Reader, Names, Entry.PathNameId) || !Reader.ReadU64(Entry.LogicalSize)
					|| !Reader.ReadHash128(Entry.Hash) || !Reader.ReadU32(Entry.ElementSize)
					|| !Reader.ReadU32(Entry.Alignment) || !Reader.ReadU8(Storage)
					|| Storage < uint8(EBulkStorageKind::Inline) || Storage > uint8(EBulkStorageKind::External)
					|| !Reader.ReadU64(Entry.Offset) || !Reader.ReadU64(Entry.Size)) return false;
				Entry.ExportId = static_cast<uint32>(ExportId); Entry.SchemaId = static_cast<uint32>(SchemaId);
				Entry.FieldId = static_cast<uint32>(FieldId); Entry.Storage = static_cast<EBulkStorageKind>(Storage);
				Entries.push_back(Entry);
			}
			if (!Reader.IsAtEnd()) return false;
			Out = std::move(Entries);
			return true;
		}

		auto BindBulkValue(FSerializedValue& Value, const FSerializedType& Type,
			std::string Path, uint32 ExportId, uint32 SchemaId, uint32 FieldId,
			const std::vector<std::string>& Names, const std::vector<FBulkEntry>& Entries,
			std::span<const std::byte> Inline, std::span<const std::byte> External,
			uint64 ExternalExtent, bool bExternalPayloadAvailable,
			std::array<uint64, 2>& Cursors, size_t& Used, FPackageReaderDiagnostic* Diagnostic) -> bool
		{
			if (Type.Kind == EValueKind::BulkData)
			{
				const uint64 Id = Value.Unsigned;
				if (Id != Used + 1 || Id > Entries.size()) return Fail(Diagnostic,
					EPackageReaderFailure::InvalidBulkData, "A BulkData handle is missing, repeated, or out of order.", Path);
				const FBulkEntry& Entry = Entries[Id - 1];
				if (Entry.ExportId != ExportId || Entry.SchemaId != SchemaId || Entry.FieldId != FieldId
					|| Names[Entry.PathNameId - 1] != Path || Entry.LogicalSize != Entry.Size
					|| Entry.ElementSize == 0 || Entry.Alignment == 0 || Entry.Alignment > 4096
					|| (Entry.Alignment & (Entry.Alignment - 1)) != 0 || Entry.Size % Entry.ElementSize != 0)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidBulkData,
						"A BulkData directory owner or shape is invalid.", Path);
				const size_t SegmentIndex = Entry.Storage == EBulkStorageKind::Inline ? 0 : 1;
				const bool bPayloadAvailable = SegmentIndex == 0 || bExternalPayloadAvailable;
				const std::span<const std::byte> Segment = SegmentIndex == 0 ? Inline : External;
				const uint64 SegmentSize = SegmentIndex == 0 ? Inline.size() : ExternalExtent;
				const uint64 Mask = Entry.Alignment - 1;
				if (Cursors[SegmentIndex] > std::numeric_limits<uint64>::max() - Mask) return false;
				const uint64 Expected = (Cursors[SegmentIndex] + Mask) & ~Mask;
				if (Entry.Offset != Expected || Entry.Offset > SegmentSize
					|| Entry.Size > SegmentSize - Entry.Offset
					|| (bPayloadAvailable && !std::ranges::all_of(
						Segment.subspan(static_cast<size_t>(Cursors[SegmentIndex]),
							static_cast<size_t>(Expected - Cursors[SegmentIndex])),
						[](std::byte Byte) { return Byte == std::byte{0}; })))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidBulkData,
						"A BulkData range, alignment, or padding is invalid.", Path);
				Value.Unsigned = 0;
				if (bPayloadAvailable)
				{
					const auto Payload = Segment.subspan(static_cast<size_t>(Entry.Offset),
						static_cast<size_t>(Entry.Size));
					if (FXxHash128::HashBuffer(Payload) != Entry.Hash)
						return Fail(Diagnostic, EPackageReaderFailure::HashMismatch,
							"A BulkData payload digest does not match.", Path);
					Value.Bytes.assign(Payload.begin(), Payload.end());
				}
				else
				{
					Value.Bytes.clear();
					Value.BulkStoredSize = Entry.Size;
					Value.BulkContentHash = Entry.Hash;
					Value.bBulkPayloadAvailable = false;
				}
				Value.BulkElementSize = Entry.ElementSize; Value.BulkAlignment = Entry.Alignment;
				Value.BulkOffset = Entry.Offset; Value.BulkStorage = Entry.Storage;
				Cursors[SegmentIndex] = Entry.Offset + Entry.Size; ++Used;
				return true;
			}
			if (Type.Kind == EValueKind::Struct)
			{
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
					if (!BindBulkValue(Value.Elements[Index], Type.Children[Index], Path + "." + Value.FieldNames[Index],
						ExportId, SchemaId, FieldId, Names, Entries, Inline, External,
						ExternalExtent, bExternalPayloadAvailable, Cursors, Used, Diagnostic)) return false;
			}
			else if (Type.Kind == EValueKind::Array || Type.Kind == EValueKind::FixedArray)
			{
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
					if (!BindBulkValue(Value.Elements[Index], Type.Children[0], Path + "[" + std::to_string(Index) + "]",
						ExportId, SchemaId, FieldId, Names, Entries, Inline, External,
						ExternalExtent, bExternalPayloadAvailable, Cursors, Used, Diagnostic)) return false;
			}
			else if (Type.Kind == EValueKind::Map)
			{
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
				{
					const std::string EntryPath = Path + "[" + std::to_string(Index / 2) + "]";
					if (!BindBulkValue(Value.Elements[Index], Type.Children[0], EntryPath + ".Key", ExportId,
						SchemaId, FieldId, Names, Entries, Inline, External,
						ExternalExtent, bExternalPayloadAvailable, Cursors, Used, Diagnostic)
						|| !BindBulkValue(Value.Elements[Index + 1], Type.Children[1], EntryPath + ".Value", ExportId,
						SchemaId, FieldId, Names, Entries, Inline, External,
						ExternalExtent, bExternalPayloadAvailable, Cursors, Used, Diagnostic)) return false;
				}
			}
			return true;
		}
	}

	auto ReadPackageV8Registry(std::span<const std::byte> FrontMatter, uint64 PhysicalPackageBytes,
		uint64 PhysicalBulkBytes, std::string_view PackageName, FPackageV8RegistryData& OutRegistry,
		FPackageReaderDiagnostic* OutDiagnostic, const FPackageReaderLimits& Limits) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		FParsedLayout Layout;
		if (!ParseLayout(FrontMatter, PhysicalPackageBytes, false, Limits, Layout, OutDiagnostic)) return false;
		std::vector<std::string> Names;
		std::vector<FPackageImport> Imports;
		FPackageV8RegistryData Registry;
		if (!DecodeNames(Layout, Limits, Names, OutDiagnostic)
			|| !DecodeImports(Layout, Names, Limits, Imports, OutDiagnostic)
			|| !DecodeRegistry(Layout, Names, PackageName, PhysicalBulkBytes, Registry, OutDiagnostic)) return false;
		OutRegistry = std::move(Registry);
		return true;
	}

	namespace
	{
		auto ReadPackageV8Impl(std::span<const std::byte> PackageBytes,
			std::span<const std::byte> BulkBytes, uint64 PhysicalBulkBytes,
			bool bExternalPayloadAvailable, std::string_view PackageName,
			FLinkerTables& OutLinker, FPackageReaderDiagnostic* OutDiagnostic,
			const FPackageReaderLimits& Limits) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		if (PhysicalBulkBytes > Limits.MaximumBulkBytes)
			return Fail(OutDiagnostic, EPackageReaderFailure::LimitExceeded,
				"The supplied DAST v8 bulk segment exceeds its limit.");
		FParsedLayout Layout;
		if (!ParseLayout(PackageBytes, PackageBytes.size(), true, Limits, Layout, OutDiagnostic)) return false;
		FLinkerTables Linker;
		FPackageV8RegistryData Registry;
		std::vector<FBulkEntry> BulkEntries;
		if (!DecodeNames(Layout, Limits, Linker.Names, OutDiagnostic)
			|| !DecodeImports(Layout, Linker.Names, Limits, Linker.Imports, OutDiagnostic)
			|| !DecodeRegistry(Layout, Linker.Names, PackageName, PhysicalBulkBytes, Registry, OutDiagnostic)
			|| !DecodeExports(Layout, Linker.Names, Limits, Linker.Exports, OutDiagnostic)
			|| Registry.ExportCount != Linker.Exports.size()
			|| !DecodeTypes(Layout, Linker.Names, Limits, Linker.Types, OutDiagnostic)
			|| !DecodeSchemas(Layout, Linker.Names, Linker.Types, Limits,
				Linker.CustomVersions, Linker.Schemas, OutDiagnostic)
			|| !DecodeValues(Layout, Linker.Names, Linker.Types, Linker.Schemas, Limits,
				Linker.Exports, OutDiagnostic)
			|| !DecodeBulkDirectory(Layout, Linker.Names, Limits, BulkEntries, OutDiagnostic))
			return OutDiagnostic && OutDiagnostic->Failure != EPackageReaderFailure::None ? false
				: Fail(OutDiagnostic, EPackageReaderFailure::InvalidTable,
					"A DAST v8 package table is malformed.");

		Linker.Summary.PackageName = Registry.PackageName;
		Linker.Summary.AssetClass = Registry.AssetClass;
		Linker.Summary.bRedirect = Registry.bRedirect;
		Linker.Summary.RedirectDestination = Registry.RedirectDestination;
		Linker.Summary.HardPackageReferences = Registry.HardPackageReferences;
		Linker.Summary.SoftPackageReferences = Registry.SoftPackageReferences;
		Linker.Summary.SearchableNames = Registry.SearchableNames;
		if (Registry.MainExportId != 0
			&& !FPackageIndex::TryExport(Registry.MainExportId - 1, Linker.Summary.MainExport))
			return Fail(OutDiagnostic, EPackageReaderFailure::InvalidIndex,
				"The DAST v8 main export id is invalid.", "Registry.MainExport");

		if (Registry.ExternalBulkBytes != PhysicalBulkBytes
			|| (bExternalPayloadAvailable
				&& (BulkBytes.empty() ? !Registry.ExternalBulkHash.IsZero()
					: FXxHash128::HashBuffer(BulkBytes) != Registry.ExternalBulkHash)))
			return Fail(OutDiagnostic, EPackageReaderFailure::HashMismatch,
				"The external DAST v8 bulk segment binding does not match.", "Registry.Bulk");
		const auto Inline = Section(Layout, EDastV8Section::InlineBulk);
		std::array<uint64, 2> Cursors{};
		size_t UsedBulk = 0;
		for (uint32 ExportIndex = 0; ExportIndex < Linker.Exports.size(); ++ExportIndex)
		{
			FPackageExport& Export = Linker.Exports[ExportIndex];
			std::string ExportPath;
			FPackageIndex Index;
			FPackageIndex::TryExport(ExportIndex, Index);
			if (!Linker.TryResolvePath(Index, ExportPath)) return Fail(OutDiagnostic,
				EPackageReaderFailure::InvalidTopology, "The DAST v8 export topology is invalid.", "Exports");
			for (FPropertyTag& Property : Export.Properties)
			{
				const auto SchemaIt = std::ranges::find(Linker.Schemas, Property.DeclaringType,
					&FSerializedSchema::QualifiedName);
				const uint32 SchemaId = static_cast<uint32>(SchemaIt - Linker.Schemas.begin() + 1);
				const auto FieldIt = std::ranges::find(SchemaIt->Fields, Property.FieldName, &FSerializedField::Name);
				const uint32 FieldId = static_cast<uint32>(FieldIt - SchemaIt->Fields.begin() + 1);
				const std::string Path = ExportPath + "." + Property.DeclaringType + "." + Property.FieldName;
				if (!BindBulkValue(Property.Value, Property.Type, Path, ExportIndex + 1, SchemaId, FieldId,
					Linker.Names, BulkEntries, Inline, BulkBytes, PhysicalBulkBytes,
					bExternalPayloadAvailable, Cursors, UsedBulk, OutDiagnostic)) return false;
			}
		}
		if (UsedBulk != BulkEntries.size() || Cursors[0] != Inline.size()
			|| Cursors[1] != PhysicalBulkBytes)
			return Fail(OutDiagnostic, EPackageReaderFailure::InvalidBulkData,
				"DAST v8 bulk entries do not consume their exact inline/external segments.", "BulkDirectory");

		std::vector<std::byte> CanonicalMain;
		FPackageWriterDiagnostic WriterDiagnostic;
		std::vector<std::byte> CanonicalBulk;
		const bool bCanonical = bExternalPayloadAvailable
			? WritePackageV8(Linker, CanonicalMain, CanonicalBulk, &WriterDiagnostic)
			: WritePackageV8Main(Linker, Registry.ExternalBulkBytes,
				Registry.ExternalBulkHash, CanonicalMain, &WriterDiagnostic);
		if (!bCanonical)
			return Fail(OutDiagnostic, EPackageReaderFailure::NonCanonical,
				"Decoded DAST v8 data violates the canonical linker contract: " + WriterDiagnostic.Message,
				WriterDiagnostic.LogicalPath);
		if (!std::ranges::equal(CanonicalMain, PackageBytes)
			|| (bExternalPayloadAvailable && !std::ranges::equal(CanonicalBulk, BulkBytes)))
			return Fail(OutDiagnostic, EPackageReaderFailure::NonCanonical,
				"DAST v8 bytes are logically valid but not in canonical writer form.");
		OutLinker = std::move(Linker);
		return true;
	}
	}

	auto ReadPackageV8(std::span<const std::byte> PackageBytes,
		std::span<const std::byte> BulkBytes, std::string_view PackageName,
		FLinkerTables& OutLinker, FPackageReaderDiagnostic* OutDiagnostic,
		const FPackageReaderLimits& Limits) -> bool
	{
		return ReadPackageV8Impl(PackageBytes, BulkBytes, BulkBytes.size(), true,
			PackageName, OutLinker, OutDiagnostic, Limits);
	}

	auto ReadPackageV8Metadata(std::span<const std::byte> PackageBytes,
		uint64 PhysicalBulkBytes, std::string_view PackageName,
		FLinkerTables& OutLinker, FPackageReaderDiagnostic* OutDiagnostic,
		const FPackageReaderLimits& Limits) -> bool
	{
		return ReadPackageV8Impl(PackageBytes, {}, PhysicalBulkBytes, false,
			PackageName, OutLinker, OutDiagnostic, Limits);
	}
}
