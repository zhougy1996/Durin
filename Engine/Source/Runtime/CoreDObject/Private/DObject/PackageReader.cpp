#include "PackageDiagnosticInternal.h"
#include "PackageFormatInternal.h"
#include "DObject/PackageFormat.h"

#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::ObjectPackage
{
	using namespace Private;
	namespace
	{
		struct FDirectoryEntry
		{
			EDastSection Kind{};
			uint64 Offset = 0;
			uint64 Size = 0;
			FXxHash128 Hash;
		};

		struct FParsedLayout
		{
			uint32 FormatVersion = 0;
			std::array<FDirectoryEntry, DastSectionCount> Entries;
			FByteView AvailableBytes;
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

		auto Fail(FPackageReaderResult* Diagnostic, EPackageReaderFailure Failure,
			EPackageReaderReason Reason, std::string Path = {},
			std::string Message = {}, std::string Subject = {}) -> bool
		{
			if (Diagnostic) *Diagnostic = {Failure, std::move(Path), Reason, std::move(Message), std::move(Subject)};
			return false;
		}

		template<std::unsigned_integral T>
		auto ReadAt(FByteView Bytes, uint64 Offset, T& Out) -> bool
		{
			return ReadLittleEndianAt(Bytes, Offset, Out);
		}

		auto ValidateLimits(const FPackageReaderLimits& Limits,
			FPackageReaderResult* Diagnostic) -> bool
		{
			if (Limits.MaximumHeaderBytes < BinaryEnvelopePreambleBytes
				|| Limits.MaximumHeaderBytes > Limits.MaximumPackageBytes
				|| Limits.MaximumPackageBytes > DastMaximumPackageBytes
				|| Limits.MaximumBulkBytes > DastMaximumBulkBytes
				|| Limits.MaximumTableEntries > DastMaximumTableEntries
				|| Limits.MaximumStringBytes > DastMaximumStringBytes
				|| Limits.MaximumContainerElements > DastMaximumContainerElements
				|| Limits.MaximumValueDepth > DastMaximumValueDepth)
				return Fail(Diagnostic, EPackageReaderFailure::LimitExceeded,
					EPackageReaderReason::InvalidLimits);
			return true;
		}

		auto ParseLayout(FByteView Available, uint64 PhysicalBytes,
			bool bComplete, const FPackageReaderLimits& Limits, FParsedLayout& Out,
			FPackageReaderResult* Diagnostic) -> bool
		{
			if (!ValidateLimits(Limits, Diagnostic)) return false;
			if (PhysicalBytes > Limits.MaximumPackageBytes || Available.size() < BinaryEnvelopePreambleBytes)
				return Fail(Diagnostic, EPackageReaderFailure::LimitExceeded,
					EPackageReaderReason::InputExtent);
			FBinaryEnvelopePreamble Preamble;
			std::expected<void, EBinaryEnvelopeError> EnvelopeDiagnostic;
			if (!(EnvelopeDiagnostic = ParseBinaryEnvelopePrefix(Available.first(BinaryEnvelopePreambleBytes), PhysicalBytes, {Limits.MaximumHeaderBytes, Limits.MaximumPackageBytes}, Preamble)))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidEnvelope,
					EPackageReaderReason::EnvelopeRejected, {}, FormatEnvelopeError(EnvelopeDiagnostic.error()));
			if (Preamble.HeaderBytes > Available.size()
				|| (!bComplete && Preamble.HeaderBytes != Available.size())
				|| (bComplete && PhysicalBytes != Available.size()))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidEnvelope,
					EPackageReaderReason::DeclaredExtent);
			const uint32 FormatVersion = Preamble.FormatVersion;
			if (!IsSupportedPackageReaderVersion(FormatVersion))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidEnvelope, EPackageReaderReason::UnsupportedVersion);
			FBinaryFormatRegistry Registry;
			const FBinaryFormatDescriptor Descriptor{
				.FormatId = DastFormatId, .DebugName = std::string(DastFormatName),
				.MinimumFormatVersion = FormatVersion,
				.MaximumFormatVersion = FormatVersion,
				.SupportedRequiredFeatures = 0,
				.Limits = {Limits.MaximumHeaderBytes, Limits.MaximumPackageBytes}};
			if (!(EnvelopeDiagnostic = FBinaryFormatRegistry::Create(std::span(&Descriptor, 1), Registry)))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidEnvelope,
					EPackageReaderReason::EnvelopeRejected, {}, FormatEnvelopeError(EnvelopeDiagnostic.error()));
			FValidatedBinaryEnvelope Validated;
			if (!(EnvelopeDiagnostic = ValidateBinaryEnvelopeHeader(Available.first(static_cast<size_t>(Preamble.HeaderBytes)), PhysicalBytes, {Limits.MaximumHeaderBytes, Limits.MaximumPackageBytes}, Registry, Validated)))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidEnvelope,
					EPackageReaderReason::EnvelopeRejected, {}, FormatEnvelopeError(EnvelopeDiagnostic.error()));

			uint32 PackageKind = 0, Flags = 0, SectionCount = 0, EntryBytes = 0;
			uint64 Directory = 0, Reserved = 0;
			if (!ReadAt(Available, DastFormatHeaderOffset, PackageKind)
				|| !ReadAt(Available, DastFormatHeaderOffset + 4, Flags)
				|| !ReadAt(Available, DastFormatHeaderOffset + 8, Directory)
				|| !ReadAt(Available, DastFormatHeaderOffset + 16, SectionCount)
				|| !ReadAt(Available, DastFormatHeaderOffset + 20, EntryBytes)
				|| !ReadAt(Available, DastFormatHeaderOffset + 24, Reserved)
				|| PackageKind > 1 || (PackageKind != 0)
				|| Flags != 0 || Directory != DastDirectoryOffset
				|| SectionCount != DastSectionCount || EntryBytes != DastSectionEntryBytes
				|| Reserved != 0)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidFormatHeader,
					EPackageReaderReason::FormatHeader);

			FParsedLayout Parsed;
			Parsed.FormatVersion = FormatVersion;
			Parsed.AvailableBytes = Available;
			uint64 ExpectedOffset = DastFirstSectionOffset;
			for (uint32 Index = 0; Index < DastSectionCount; ++Index)
			{
				const uint64 Base = DastDirectoryOffset + uint64(Index) * DastSectionEntryBytes;
				uint32 Kind = 0, EntryFlags = 0;
				uint64 Offset = 0, Size = 0, HashLow = 0, HashHigh = 0, EntryReserved = 0;
				if (!ReadAt(Available, Base, Kind) || !ReadAt(Available, Base + 4, EntryFlags)
					|| !ReadAt(Available, Base + 8, Offset) || !ReadAt(Available, Base + 16, Size)
					|| !ReadAt(Available, Base + 24, HashLow) || !ReadAt(Available, Base + 32, HashHigh)
					|| !ReadAt(Available, Base + 40, EntryReserved)
					|| Kind != Index + 1 || EntryFlags != 1 || EntryReserved != 0
					|| Offset != ExpectedOffset || Size > PhysicalBytes - std::min(Offset, PhysicalBytes))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidDirectory,
						EPackageReaderReason::Directory, "Directory[" + std::to_string(Index) + "]");
				if (Offset > PhysicalBytes || Size > PhysicalBytes - Offset)
					return Fail(Diagnostic, EPackageReaderFailure::ArithmeticOverflow,
						EPackageReaderReason::SectionOverflow, "Directory[" + std::to_string(Index) + "]");
				Parsed.Entries[Index] = {static_cast<EDastSection>(Kind), Offset, Size, {HashLow, HashHigh}};
				ExpectedOffset = Offset + Size;
				if ((bComplete || Index <= 2)
					&& FXxHash128::HashBuffer(Available.subspan(static_cast<size_t>(Offset), static_cast<size_t>(Size)))
						!= Parsed.Entries[Index].Hash)
					return Fail(Diagnostic, EPackageReaderFailure::HashMismatch,
						EPackageReaderReason::SectionHash, "Directory[" + std::to_string(Index) + "]");
			}
			if (ExpectedOffset != PhysicalBytes
				|| Preamble.HeaderBytes != Parsed.Entries[2].Offset + Parsed.Entries[2].Size)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidDirectory,
					EPackageReaderReason::SectionCoverage);
			Out = Parsed;
			return true;
		}

		auto Section(const FParsedLayout& Layout, EDastSection Kind) -> FByteView
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
			std::vector<std::string>& Out, FPackageReaderResult* Diagnostic) -> bool
		{
			FBinaryReader Reader(Section(Layout, EDastSection::Names),
				{Layout.Entries[1].Size, Limits.MaximumStringBytes});
			uint32 Version = 0;
			uint64 Count = 0;
			if (!Reader.ReadU32(Version) || Version != DastTableVersion
				|| !Reader.ReadVarUInt(Count) || Count > Limits.MaximumTableEntries)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
					EPackageReaderReason::NameTableHeader, "Names");
			std::vector<std::string> Names;
			Names.reserve(static_cast<size_t>(Count));
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				std::string Name;
				if (!Reader.ReadString(Name, Limits.MaximumStringBytes) || Name.empty() || !IsValidUtf8(Name)
					|| (!Names.empty() && !BytewiseLess(Names.back(), Name)))
					return Fail(Diagnostic, EPackageReaderFailure::NonCanonical,
						EPackageReaderReason::NonCanonicalNames,
						"Names[" + std::to_string(Index) + "]");
				Names.push_back(std::move(Name));
			}
			if (!Reader.IsAtEnd()) return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
				EPackageReaderReason::NameTableTrailing, "Names");
			Out = std::move(Names);
			return true;
		}

		auto DecodeImports(const FParsedLayout& Layout, const std::vector<std::string>& Names,
			const FPackageReaderLimits& Limits, std::vector<FPackageImport>& Out,
			FPackageReaderResult* Diagnostic) -> bool
		{
			FBinaryReader Reader(Section(Layout, EDastSection::Imports),
				{Layout.Entries[2].Size, Limits.MaximumStringBytes});
			uint32 Version = 0;
			uint64 Count = 0;
			if (!Reader.ReadU32(Version) || Version != DastTableVersion
				|| !Reader.ReadVarUInt(Count) || Count > Limits.MaximumTableEntries)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
					EPackageReaderReason::ImportTableHeader, "Imports");
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
						EPackageReaderReason::ImportRecord, "Imports[" + std::to_string(Index) + "]");
				FObjectPath ObjectPath;
				if (ObjectId != 0 || !OuterIndex.IsNull()
					|| !FObjectPath::TryCreate(Names[PackageId - 1], ObjectPath))
				{
					return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
						EPackageReaderReason::ImportPath,
						"Imports[" + std::to_string(Index) + "]");
				}
				if (!Imports.empty() && !BytewiseLess(Imports.back().ObjectPath.ToString(), ObjectPath.ToString()))
					return Fail(Diagnostic, EPackageReaderFailure::NonCanonical, EPackageReaderReason::NonCanonicalBytes, "Imports");
				Imports.push_back({.ObjectPath = std::move(ObjectPath),
					.ClassName = ClassId ? Names[ClassId - 1] : std::string{},
					.Outer = OuterIndex});
			}
			if (!Reader.IsAtEnd()) return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
				EPackageReaderReason::ImportTableTrailing, "Imports");
			Out = std::move(Imports);
			return true;
		}

		auto DecodeRegistry(const FParsedLayout& Layout,
			const std::vector<std::string>& Names, const FPackagePath& PackagePath,
			uint64 PhysicalBulkBytes, FPackageRegistryData& Out,
			FPackageReaderResult* Diagnostic) -> bool
		{
			if (!PackagePath.IsValid()
				|| !std::ranges::binary_search(Names, PackagePath.GetView(), BytewiseLess))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidRegistry,
					EPackageReaderReason::MissingPackageIdentity,
					"Registry.PackagePath");
			FBinaryReader Reader(Section(Layout, EDastSection::Registry));
			uint32 Version = 0;
			uint64 ExportCount = 0, AssetCount = 0;
			if (!Reader.ReadU32(Version) || Version != DastRegistryVersion
				|| !Reader.ReadVarUInt(ExportCount) || ExportCount > DastMaximumTableEntries
				|| !Reader.ReadVarUInt(AssetCount) || AssetCount > ExportCount)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidRegistry,
					EPackageReaderReason::RegistryIdentity, "Registry");
			FPackageRegistryData Registry{
				.PackagePath = PackagePath,
				.ExportCount = static_cast<uint32>(ExportCount)};
			std::unordered_set<uint32> ExportIds;
			std::string PreviousPath;
			for (uint64 Index = 0; Index < AssetCount; ++Index)
			{
				uint64 ExportId = 0;
				uint32 PathId = 0, ClassId = 0, RedirectId = 0;
				if (!Reader.ReadVarUInt(ExportId) || ExportId == 0 || ExportId > ExportCount
					|| ExportId > std::numeric_limits<uint32>::max()
					|| !ReadNameId(Reader, Names, PathId)
					|| !ReadNameId(Reader, Names, ClassId)
					|| !ReadNameId(Reader, Names, RedirectId, true)
					|| !ExportIds.insert(static_cast<uint32>(ExportId)).second)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidRegistry,
						EPackageReaderReason::TopLevelRecord,
						"Registry.TopLevelAssets[" + std::to_string(Index) + "]");
				FTopLevelAssetPath AssetPath;
				FObjectPath RedirectDestination;
				const std::string PackagePrefix = PackagePath.ToString() + ".";
				const std::string_view SerializedAssetPath = Names[PathId - 1];
				const std::string_view AssetName = SerializedAssetPath.starts_with(PackagePrefix)
					? SerializedAssetPath.substr(PackagePrefix.size()) : std::string_view{};
				if (AssetName.empty() || AssetName.find_first_of(".:") != std::string_view::npos
					|| !FTopLevelAssetPath::TryCreate(PackagePath, AssetName, AssetPath))
					return Fail(Diagnostic, EPackageReaderFailure::NonCanonical,
						EPackageReaderReason::TopLevelPath,
						"Registry.TopLevelAssets[" + std::to_string(Index) + "]", {}, Names[PathId - 1]);
				if (!PreviousPath.empty() && !BytewiseLess(PreviousPath, AssetPath.ToString()))
					return Fail(Diagnostic, EPackageReaderFailure::NonCanonical,
						EPackageReaderReason::TopLevelOrder,
						"Registry.TopLevelAssets[" + std::to_string(Index) + "]");
				if (RedirectId && !FObjectPath::TryCreate(
					Names[RedirectId - 1], RedirectDestination))
					return Fail(Diagnostic, EPackageReaderFailure::NonCanonical,
						EPackageReaderReason::RedirectPath,
						"Registry.TopLevelAssets[" + std::to_string(Index) + "]");
				PreviousPath = AssetPath.ToString();
				Registry.TopLevelAssets.push_back({
					.ExportId = static_cast<uint32>(ExportId),
					.AssetPath = std::move(AssetPath),
					.ClassName = Names[ClassId - 1],
					.RedirectDestination = std::move(RedirectDestination)});
			}
			for (uint32 ListIndex = 0; ListIndex < 4u; ++ListIndex)
			{
				uint64 Count = 0;
				if (!Reader.ReadVarUInt(Count) || Count > DastMaximumTableEntries)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidRegistry,
						EPackageReaderReason::RegistryListCount, "Registry");
				uint32 Previous = 0;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					uint32 Id = 0;
					if (!ReadNameId(Reader, Names, Id) || Id <= Previous)
						return Fail(Diagnostic, EPackageReaderFailure::NonCanonical,
							EPackageReaderReason::RegistryListOrder, "Registry");
					Previous = Id;
					if (ListIndex == 2) Registry.SearchableNames.push_back(Names[Id - 1]);
					else
					{
						FPackagePath Path;
						if (!FPackagePath::TryCreate(Names[Id - 1], Path))
							return Fail(Diagnostic, EPackageReaderFailure::InvalidRegistry,
								EPackageReaderReason::DependencyPath, "Registry");
						(ListIndex == 0 ? Registry.HardPackageReferences
							: ListIndex == 1 ? Registry.SoftPackageReferences : Registry.BuildPackageReferences).push_back(std::move(Path));
					}
				}
			}
			if (!Reader.ReadU64(Registry.ExternalBulkBytes)
				|| !Reader.ReadHash128(Registry.ExternalBulkHash) || !Reader.IsAtEnd()
				|| Registry.ExternalBulkBytes != PhysicalBulkBytes
				|| ((Registry.ExternalBulkBytes == 0) != Registry.ExternalBulkHash.IsZero()))
				return Fail(Diagnostic, EPackageReaderFailure::InvalidRegistry,
					EPackageReaderReason::RegistryBulk, "Registry.Bulk");
			Out = std::move(Registry);
			return true;
		}

		auto DecodeExports(const FParsedLayout& Layout, const std::vector<std::string>& Names,
			const FPackageReaderLimits& Limits, std::vector<FPackageExport>& Out,
			FPackageReaderResult* Diagnostic) -> bool
		{
			FBinaryReader Reader(Section(Layout, EDastSection::Exports));
			uint32 Version = 0;
			uint64 Count = 0;
			if (!Reader.ReadU32(Version) || Version != DastTableVersion
				|| !Reader.ReadVarUInt(Count) || Count > Limits.MaximumTableEntries)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
					EPackageReaderReason::ExportTableHeader, "Exports");
			std::vector<FPackageExport> Exports;
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				uint32 ObjectId = 0, ClassId = 0;
				int64 Outer = 0;
				FPackageIndex OuterIndex;
				if (!ReadNameId(Reader, Names, ObjectId) || !ReadNameId(Reader, Names, ClassId)
					|| !Reader.ReadVarInt(Outer) || !FPackageIndex::TryFromRaw(Outer, OuterIndex))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
						EPackageReaderReason::ExportRecord, "Exports[" + std::to_string(Index) + "]");
				Exports.push_back({.ObjectName = Names[ObjectId - 1], .ClassName = Names[ClassId - 1],
					.Outer = OuterIndex});
			}
			if (!Reader.IsAtEnd()) return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
				EPackageReaderReason::ExportTableTrailing, "Exports");
			Out = std::move(Exports);
			return true;
		}

		auto DecodeTypes(const FParsedLayout& Layout, const std::vector<std::string>& Names,
			const FPackageReaderLimits& Limits, std::vector<FSerializedType>& Out,
			FPackageReaderResult* Diagnostic) -> bool
		{
			FBinaryReader Reader(Section(Layout, EDastSection::Types));
			uint32 Version = 0;
			uint64 Count = 0;
			if (!Reader.ReadU32(Version) || Version != DastTableVersion
				|| !Reader.ReadVarUInt(Count) || Count > Limits.MaximumTableEntries)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
					EPackageReaderReason::TypeTableHeader, "Types");
			std::vector<FRawType> Raw;
			Raw.reserve(static_cast<size_t>(Count));
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				uint64 RecordBytes = 0;
				if (!Reader.ReadVarUInt(RecordBytes) || RecordBytes > Reader.GetRemainingBytes())
					return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
						EPackageReaderReason::TypeRecordExtent, "Types[" + std::to_string(Index) + "]");
				FByteView RecordSpan;
				if (!Reader.ReadRegion(RecordSpan, RecordBytes, Layout.Entries[4].Size)) return false;
				FBinaryReader Record(RecordSpan);
				uint8 Tag = 0;
				uint32 NameId = 0;
				uint64 Parameter = 0, ChildCount = 0;
				if (!Record.ReadU8(Tag) || Tag == 0 || Tag > uint8(EValueKind::BulkData) + 1
					|| !ReadNameId(Record, Names, NameId, true) || !Record.ReadVarUInt(Parameter)
					|| !Record.ReadVarUInt(ChildCount) || ChildCount > Limits.MaximumTableEntries)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
						EPackageReaderReason::TypeRecordHeader, "Types[" + std::to_string(Index) + "]");
				FRawType Type{static_cast<EValueKind>(Tag - 1), NameId, Parameter};
				for (uint64 Child = 0; Child < ChildCount; ++Child)
				{
					uint64 Id = 0;
					if (!Record.ReadVarUInt(Id) || Id == 0 || Id > Count)
						return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
							EPackageReaderReason::ChildTypeIndex, "Types[" + std::to_string(Index) + "]");
					Type.Children.push_back(static_cast<uint32>(Id));
				}
				if (!Record.IsAtEnd()) return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
					EPackageReaderReason::TypeRecordTrailing, "Types[" + std::to_string(Index) + "]");
				Raw.push_back(std::move(Type));
			}
			if (!Reader.IsAtEnd()) return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
				EPackageReaderReason::TypeTableTrailing, "Types");

			std::vector<FSerializedType> Types(Raw.size());
			std::vector<uint8> States(Raw.size());
			std::vector<uint32> Heights(Raw.size());
			std::function<bool(uint32, uint32)> Build = [&](uint32 Id, uint32 Depth)
			{
				const size_t Index = Id - 1;
				if (Depth > Limits.MaximumValueDepth)
					return Fail(Diagnostic, EPackageReaderFailure::LimitExceeded, EPackageReaderReason::ValueDepth, "Types");
				if (States[Index] == 2) return true;
				if (States[Index] == 1) return Fail(Diagnostic, EPackageReaderFailure::InvalidType,
					EPackageReaderReason::TypeCycle, "Types[" + std::to_string(Index) + "]");
				States[Index] = 1;
				const FRawType& Source = Raw[Index];
				FSerializedType& Type = Types[Index];
				Type.Kind = Source.Kind;
				Type.QualifiedName = Source.NameId ? Names[Source.NameId - 1] : std::string{};
				Type.Parameter = Source.Parameter;
				if (!IsValidTypeShape(Type.Kind, Type.QualifiedName, Type.Parameter, Source.Children.size()))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidType, EPackageReaderReason::TypeRecordHeader, "Types[" + std::to_string(Index) + "]");
				for (uint32 ChildId : Source.Children)
				{
					if (!Build(ChildId, Depth + 1)) return false;
					Heights[Index] = std::max(Heights[Index], Heights[ChildId - 1] + 1);
					if (Heights[Index] > Limits.MaximumValueDepth)
						return Fail(Diagnostic, EPackageReaderFailure::LimitExceeded, EPackageReaderReason::ValueDepth, "Types");
					Type.Children.push_back(Types[ChildId - 1]);
				}
				States[Index] = 2;
				return true;
			};
			for (uint32 Id = 1; Id <= Raw.size(); ++Id)
			{
				if (!Build(Id, 0)) return false;
				if (Id > 1 && !(Types[Id - 2] < Types[Id - 1]))
					return Fail(Diagnostic, EPackageReaderFailure::NonCanonical, EPackageReaderReason::NonCanonicalBytes, "Types");
			}
			Out = std::move(Types);
			return true;
		}

		auto DecodeSchemas(const FParsedLayout& Layout, const std::vector<std::string>& Names,
			const std::vector<FSerializedType>& Types, const FPackageReaderLimits& Limits,
			std::vector<FCustomVersion>& OutVersions, std::vector<FSerializedSchema>& OutSchemas,
			FPackageReaderResult* Diagnostic) -> bool
		{
			FBinaryReader Reader(Section(Layout, EDastSection::Schemas));
			uint32 Version = 0;
			uint64 VersionCount = 0;
			if (!Reader.ReadU32(Version) || Version != DastTableVersion
				|| !Reader.ReadVarUInt(VersionCount) || VersionCount > Limits.MaximumTableEntries)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
					EPackageReaderReason::SchemaSectionHeader, "Schemas");
			std::vector<FCustomVersion> Versions;
			for (uint64 Index = 0; Index < VersionCount; ++Index)
			{
				FCustomVersion Custom;
				uint8 Flags = 0;
				uint32 Value = 0;
				if (!Reader.ReadGuid(Custom.Guid) || !Reader.ReadU32(Value)
					|| !Custom.Guid.IsValid() || Value > static_cast<uint32>(std::numeric_limits<int32>::max())
					|| (!Versions.empty() && !(std::tie(Versions.back().Guid.A, Versions.back().Guid.B, Versions.back().Guid.C, Versions.back().Guid.D)
						< std::tie(Custom.Guid.A, Custom.Guid.B, Custom.Guid.C, Custom.Guid.D)))
					|| !Reader.ReadU8(Flags) || Flags != 0)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
						EPackageReaderReason::CustomVersionRecord, "CustomVersions");
				Custom.Version = static_cast<int32>(Value);

				Versions.push_back(Custom);
			}
			uint64 SchemaCount = 0;
			if (!Reader.ReadVarUInt(SchemaCount) || SchemaCount > Limits.MaximumTableEntries)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
					EPackageReaderReason::SchemaCount, "Schemas");
			std::vector<FSerializedSchema> Schemas;
			for (uint64 SchemaIndex = 0; SchemaIndex < SchemaCount; ++SchemaIndex)
			{
				uint32 NameId = 0;
				uint64 FieldCount = 0;
				if (!ReadNameId(Reader, Names, NameId) || !Reader.ReadVarUInt(FieldCount)
					|| FieldCount > Limits.MaximumTableEntries)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
						EPackageReaderReason::SchemaHeader, "Schemas");
				FSerializedSchema Schema{.QualifiedName = Names[NameId - 1]};
				if (!Schemas.empty() && !BytewiseLess(Schemas.back().QualifiedName, Schema.QualifiedName))
					return Fail(Diagnostic, EPackageReaderFailure::NonCanonical, EPackageReaderReason::NonCanonicalBytes, "Schemas");
				for (uint64 FieldIndex = 0; FieldIndex < FieldCount; ++FieldIndex)
				{
					uint32 FieldNameId = 0;
					uint64 TypeId = 0, Flags = 0;
					if (!ReadNameId(Reader, Names, FieldNameId) || !Reader.ReadVarUInt(TypeId)
						|| TypeId == 0 || TypeId > Types.size() || !Reader.ReadVarUInt(Flags))
						return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
							EPackageReaderReason::SchemaField, "Schemas");
					if (!Schema.Fields.empty() && !BytewiseLess(Schema.Fields.back().Name, Names[FieldNameId - 1]))
						return Fail(Diagnostic, EPackageReaderFailure::NonCanonical, EPackageReaderReason::NonCanonicalBytes, Schema.QualifiedName);
					Schema.Fields.push_back({Names[FieldNameId - 1], Types[TypeId - 1], Flags});
				}
				Schemas.push_back(std::move(Schema));
			}
			if (!Reader.IsAtEnd()) return Fail(Diagnostic, EPackageReaderFailure::InvalidTable,
				EPackageReaderReason::SchemaTrailing, "Schemas");
			OutVersions = std::move(Versions);
			OutSchemas = std::move(Schemas);
			return true;
		}

		auto ReadValue(FBinaryReader& Reader, const FLinkerTables& Linker, const FSerializedType& Type,
			const FPackageReaderLimits& Limits, FSerializedValue& Out, uint32 Depth,
			FPackageReaderResult* Diagnostic, std::string Path,
			bool bAllowBaseline = false, bool bAllowTypeDefault = false, bool bTypeRelative = false) -> bool
		{
			const auto& Names = Linker.Names;
			const auto& Types = Linker.Types;
			if (Depth > Limits.MaximumValueDepth) return Fail(Diagnostic,
				EPackageReaderFailure::LimitExceeded, EPackageReaderReason::ValueDepth, Path);
			uint8 Tag = 0;
			if (!Reader.ReadU8(Tag) || Tag != static_cast<uint8>(Type.Kind) + 1)
				return Fail(Diagnostic, EPackageReaderFailure::InvalidValue,
					EPackageReaderReason::ValueTag, Path);
			FSerializedValue Value;
			switch (Type.Kind)
			{
			case EValueKind::Bool:
			{ uint8 V = 0; if (!Reader.ReadU8(V) || V > 1) return false; Value.Bool = V != 0; break; }
			case EValueKind::I8: case EValueKind::I16: case EValueKind::I32: case EValueKind::I64:
				if (!Reader.ReadVarInt(Value.Signed) || !SignedFits(Type.Kind, Value.Signed))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidValue, EPackageReaderReason::ValueShape, Path); break;
			case EValueKind::U8: case EValueKind::U16: case EValueKind::U32: case EValueKind::U64:
				if (!Reader.ReadVarUInt(Value.Unsigned) || !UnsignedFits(Type.Kind, Value.Unsigned))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidValue, EPackageReaderReason::ValueShape, Path); break;
			case EValueKind::F32:
			{ uint32 V = 0; if (!Reader.ReadU32(V)) return false;
				if (V != CanonicalFloat32(V)) return Fail(Diagnostic, EPackageReaderFailure::NonCanonical, EPackageReaderReason::NonCanonicalBytes, Path); Value.FloatingBits = V; break; }
			case EValueKind::F64:
				if (!Reader.ReadU64(Value.FloatingBits)) return false;
				if (Value.FloatingBits != CanonicalFloat64(Value.FloatingBits)) return Fail(Diagnostic, EPackageReaderFailure::NonCanonical, EPackageReaderReason::NonCanonicalBytes, Path); break;
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
				{ if (!Reader.ReadVarInt(Value.Signed) || !SignedFits(static_cast<EValueKind>(Type.Parameter), Value.Signed))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidValue, EPackageReaderReason::ValueShape, Path); }
				else if (!Reader.ReadVarUInt(Value.Unsigned) || !UnsignedFits(static_cast<EValueKind>(Type.Parameter), Value.Unsigned))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidValue, EPackageReaderReason::ValueShape, Path);
				break;
			case EValueKind::Intrinsic:
			{
				uint64 Count = 0;
				const uint64 Expected = Type.Parameter == 1 ? 2 : Type.Parameter == 2 ? 3 : Type.Parameter == 5 ? 10 : 4;
				if (!Reader.ReadVarUInt(Count) || Count != Expected)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidValue, EPackageReaderReason::ValueShape, Path);
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					uint64 Bits = 0;
					if (Type.Parameter == 6) { uint32 Narrow = 0; if (!Reader.ReadU32(Narrow)) return false; Bits = Narrow; }
					else if (!Reader.ReadU64(Bits)) return false;
					if ((Type.Parameter == 6 && Bits != CanonicalFloat32(static_cast<uint32>(Bits)))
						|| (Type.Parameter != 6 && Bits != CanonicalFloat64(Bits)))
						return Fail(Diagnostic, EPackageReaderFailure::NonCanonical, EPackageReaderReason::NonCanonicalBytes, Path);
					Value.ComponentBits.push_back(Bits);
				}
				break;
			}
			case EValueKind::Struct:
			{
				uint64 Count = 0;
				uint8 Baseline = 0;
				if (!Reader.ReadU8(Baseline) || Baseline > 2)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidValue, EPackageReaderReason::StructBaseline, Path);
				Value.Baseline = static_cast<EArchiveStructBaseline>(Baseline);
				if ((Value.Baseline == EArchiveStructBaseline::Parent && !bAllowBaseline)
					|| (Value.Baseline == EArchiveStructBaseline::TypeDefault && !bAllowTypeDefault))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidValue, EPackageReaderReason::StructBaseline, Path);
				bTypeRelative = Value.Baseline == EArchiveStructBaseline::TypeDefault
					|| (bTypeRelative && Value.Baseline == EArchiveStructBaseline::Parent);
				const auto Schema = std::ranges::find(Linker.Schemas, Type.QualifiedName, &FSerializedSchema::QualifiedName);
				if (Schema == Linker.Schemas.end() || !Reader.ReadVarUInt(Count) || Count > Limits.MaximumTableEntries
					|| (Value.Baseline == EArchiveStructBaseline::Complete && Count != Type.Children.size()))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidValue, EPackageReaderReason::ValueShape, Path);
				Value.FieldTypes.emplace();
				uint32 Previous = 0;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					uint32 NameId = 0;
					uint64 TypeId = 0;
					uint8 Provenance = 0;
					if (!ReadNameId(Reader, Names, NameId) || NameId <= Previous || !Reader.ReadVarUInt(TypeId)
						|| TypeId == 0 || TypeId > Types.size() || !Reader.ReadU8(Provenance)
						|| Provenance > uint8(EPropertyProvenance::Forced))
						return Fail(Diagnostic, EPackageReaderFailure::InvalidValue, EPackageReaderReason::ValueShape, Path);
					Previous = NameId;
					const auto Field = std::ranges::find(Schema->Fields, Names[NameId - 1], &FSerializedField::Name);
					if (Field == Schema->Fields.end() || Field->Type != Types[TypeId - 1])
						return Fail(Diagnostic, EPackageReaderFailure::InvalidValue, EPackageReaderReason::ValueShape, Path);
					FSerializedValue Child;
					const bool bInherited = Value.Baseline != EArchiveStructBaseline::Complete && Provenance != uint8(EPropertyProvenance::Forced);
					if (!ReadValue(Reader, Linker, Types[TypeId - 1], Limits, Child, Depth + 1,
						Diagnostic, Path + "." + Names[NameId - 1], bInherited, bAllowTypeDefault && bInherited, bTypeRelative)) return false;
					Value.FieldNames.push_back(Names[NameId - 1]);
					Value.FieldTypes->push_back(Types[TypeId - 1]);
					Value.Provenances.push_back(static_cast<EPropertyProvenance>(Provenance));
					Value.Elements.push_back(std::move(Child));
				}
				if (bTypeRelative)
					for (const auto& Field : Schema->Fields)
						if (ContainsHardReferenceType(Field.Type) && !std::ranges::contains(Value.FieldNames, Field.Name))
							return Fail(Diagnostic, EPackageReaderFailure::InvalidValue, EPackageReaderReason::ValueShape, Path);
				break;
			}
			case EValueKind::FixedArray:
			case EValueKind::Array:
			{
				uint64 Count = 0;
				if (!Reader.ReadVarUInt(Count) || Count > Limits.MaximumContainerElements
					|| (Type.Kind == EValueKind::FixedArray && Count != Type.Parameter)) return false;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					FSerializedValue Child;
					if (!ReadValue(Reader, Linker, Type.Children.front(), Limits, Child, Depth + 1,
						Diagnostic, Path + "[" + std::to_string(Index) + "]", false, bAllowTypeDefault)) return false;
					Value.Elements.push_back(std::move(Child));
				}
				break;
			}
			case EValueKind::Map:
			{
				FByteBuffer PreviousKey;
				uint64 Count = 0;
				if (!Reader.ReadVarUInt(Count) || Count > Limits.MaximumContainerElements) return false;
				for (uint64 Index = 0; Index < Count; ++Index)
					for (uint32 Part = 0; Part < 2; ++Part)
					{
						FSerializedValue Child;
						if (!ReadValue(Reader, Linker, Type.Children[Part], Limits, Child, Depth + 1,
							Diagnostic, Path + "[" + std::to_string(Index) + "]", false, Part == 1 && bAllowTypeDefault)) return false;
						if (Part == 0)
						{
							FByteBuffer Key;
							if (!BuildCanonicalMapKeyToken(Type.Children[0], Child, Key)
								|| (Index != 0 && !(PreviousKey < Key)))
								return Fail(Diagnostic, EPackageReaderFailure::NonCanonical, EPackageReaderReason::NonCanonicalBytes, Path);
							PreviousKey = std::move(Key);
						}
						Value.Elements.push_back(std::move(Child));
					}
				break;
			}
			case EValueKind::HardReference:
			{ int64 Raw = 0; if (!Reader.ReadVarInt(Raw) || !FPackageIndex::TryFromRaw(Raw, Value.Reference)) return false;
				if ((Value.Reference.IsExport() && Value.Reference.GetTableIndex() >= Linker.Exports.size())
					|| (Value.Reference.IsImport() && Value.Reference.GetTableIndex() >= Linker.Imports.size()))
					return Fail(Diagnostic, EPackageReaderFailure::InvalidIndex, EPackageReaderReason::PropertyValue, Path); break; }
			case EValueKind::SoftReference:
			{ uint32 NameId = 0; if (!ReadNameId(Reader, Names, NameId, true)) return false;
				if (NameId) { Value.Text = Names[NameId - 1]; FObjectPath ObjectPath;
					if (!FObjectPath::TryCreate(Value.Text, ObjectPath)) return Fail(Diagnostic, EPackageReaderFailure::InvalidValue, EPackageReaderReason::PropertyValue, Path); } break; }
			case EValueKind::Byte:
			{ uint8 V = 0; if (!Reader.ReadU8(V)) return false; Value.Unsigned = V; break; }
			case EValueKind::Bytes:
			{
				uint64 Count = 0; FByteView Bytes;
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

		auto DecodeValues(const FParsedLayout& Layout, FLinkerTables& Linker, const FPackageReaderLimits& Limits,
			FPackageReaderResult* Diagnostic) -> bool
		{
			const auto& Types = Linker.Types;
			const auto& Schemas = Linker.Schemas;
			auto& Exports = Linker.Exports;
			FBinaryReader Reader(Section(Layout, EDastSection::Values));
			uint32 Version = 0;
			uint64 ExportCount = 0;
			if (!Reader.ReadU32(Version) || Version != DastTableVersion
				|| !Reader.ReadVarUInt(ExportCount) || ExportCount != Exports.size()) return false;
			for (uint64 ExportIndex = 0; ExportIndex < ExportCount; ++ExportIndex)
			{
				uint64 ExportId = 0, PropertyCount = 0;
				if (!Reader.ReadVarUInt(ExportId) || ExportId != ExportIndex + 1) return false;
				uint8 Baseline = 0;
				if (!Reader.ReadU8(Baseline) || Baseline > 1)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidValue, EPackageReaderReason::ExportBaseline, "Values");
				Exports[ExportIndex].bUseClassDefaults = Baseline != 0;
				if (!Reader.ReadVarUInt(PropertyCount) || PropertyCount > Limits.MaximumTableEntries) return false;
				std::pair<uint64, uint64> Previous{};
				for (uint64 PropertyIndex = 0; PropertyIndex < PropertyCount; ++PropertyIndex)
				{
					uint64 SchemaId = 0, FieldId = 0, TypeId = 0;
					uint8 Provenance = 0;
					if (!Reader.ReadVarUInt(SchemaId) || SchemaId == 0 || SchemaId > Schemas.size()
						|| !Reader.ReadVarUInt(FieldId) || FieldId == 0 || FieldId > Schemas[SchemaId - 1].Fields.size()
						|| !Reader.ReadVarUInt(TypeId) || TypeId == 0 || TypeId > Types.size()
						|| !Reader.ReadU8(Provenance) || Provenance > uint8(EPropertyProvenance::Forced)) return false;
					if (std::pair{SchemaId, FieldId} <= Previous)
						return Fail(Diagnostic, EPackageReaderFailure::NonCanonical, EPackageReaderReason::NonCanonicalBytes, "Values");
					Previous = {SchemaId, FieldId};
					const FSerializedSchema& Schema = Schemas[SchemaId - 1];
					const FSerializedField& Field = Schema.Fields[FieldId - 1];
					if (Field.Type != Types[TypeId - 1]) return false;
					FPropertyTag Property{.DeclaringType = Schema.QualifiedName, .FieldName = Field.Name,
						.Type = Types[TypeId - 1], .Provenance = static_cast<EPropertyProvenance>(Provenance)};
					const std::string Path = Exports[ExportIndex].ObjectName + "." + Schema.QualifiedName + "." + Field.Name;
					if (!ReadValue(Reader, Linker, Property.Type, Limits, Property.Value, 0, Diagnostic, Path,
						Exports[ExportIndex].bUseClassDefaults && Property.Provenance != EPropertyProvenance::Forced,
						Exports[ExportIndex].bUseClassDefaults && Property.Provenance != EPropertyProvenance::Forced))
						return Diagnostic && !Diagnostic->Succeeded() ? false
							: Fail(Diagnostic, EPackageReaderFailure::InvalidValue,
								EPackageReaderReason::PropertyValue, Path);
					Exports[ExportIndex].Properties.push_back(std::move(Property));
				}
			}
			return Reader.IsAtEnd() || Fail(Diagnostic, EPackageReaderFailure::InvalidValue,
				EPackageReaderReason::ValueTrailing, "Values");
		}

		auto DecodeBulkDirectory(const FParsedLayout& Layout, const std::vector<std::string>& Names,
			const FPackageReaderLimits& Limits, std::vector<FBulkEntry>& Out,
			FPackageReaderResult* Diagnostic) -> bool
		{
			FBinaryReader Reader(Section(Layout, EDastSection::BulkDirectory));
			uint32 Version = 0;
			uint64 Count = 0;
			if (!Reader.ReadU32(Version) || Version != DastTableVersion
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
			FByteView Inline, FByteView External,
			uint64 ExternalExtent, bool bExternalPayloadAvailable,
			std::array<uint64, 2>& Cursors, size_t& Used, FPackageReaderResult* Diagnostic) -> bool
		{
			if (Type.Kind == EValueKind::BulkData)
			{
				const uint64 Id = Value.Unsigned;
				if (Id != Used + 1 || Id > Entries.size()) return Fail(Diagnostic,
					EPackageReaderFailure::InvalidBulkData, EPackageReaderReason::BulkHandle, Path);
				const FBulkEntry& Entry = Entries[Id - 1];
				if (Entry.ExportId != ExportId || Entry.SchemaId != SchemaId || Entry.FieldId != FieldId
					|| Names[Entry.PathNameId - 1] != Path || Entry.LogicalSize != Entry.Size
					|| Entry.ElementSize == 0 || Entry.Alignment == 0 || Entry.Alignment > 4096
					|| (Entry.Alignment & (Entry.Alignment - 1)) != 0 || Entry.Size % Entry.ElementSize != 0)
					return Fail(Diagnostic, EPackageReaderFailure::InvalidBulkData,
						EPackageReaderReason::BulkOwner, Path);
				const size_t SegmentIndex = Entry.Storage == EBulkStorageKind::Inline ? 0 : 1;
				const bool bPayloadAvailable = SegmentIndex == 0 || bExternalPayloadAvailable;
				const FByteView Segment = SegmentIndex == 0 ? Inline : External;
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
						EPackageReaderReason::BulkRange, Path);
				Value.Unsigned = 0;
				if (bPayloadAvailable)
				{
					const auto Payload = Segment.subspan(static_cast<size_t>(Entry.Offset),
						static_cast<size_t>(Entry.Size));
					if (FXxHash128::HashBuffer(Payload) != Entry.Hash)
						return Fail(Diagnostic, EPackageReaderFailure::HashMismatch,
							EPackageReaderReason::BulkHash, Path);
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
					if (!BindBulkValue(Value.Elements[Index], StructFieldTypes(Type, Value)[Index], Path + "." + Value.FieldNames[Index],
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

	static auto ReadPackageRegistryInternal(FByteView FrontMatter,
		uint64 PhysicalPackageBytes, uint64 PhysicalBulkBytes,
		const FPackagePath& PackagePath, FPackageRegistryData& OutRegistry,
		FPackageReaderResult* OutDiagnostic, const FPackageReaderLimits& Limits) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		FParsedLayout Layout;
		if (!ParseLayout(FrontMatter, PhysicalPackageBytes, false,
			Limits, Layout, OutDiagnostic)) return false;
		std::vector<std::string> Names;
		std::vector<FPackageImport> Imports;
		FPackageRegistryData Registry;
		if (!DecodeNames(Layout, Limits, Names, OutDiagnostic)
			|| !DecodeImports(Layout, Names, Limits, Imports, OutDiagnostic)
			|| !DecodeRegistry(Layout, Names, PackagePath, PhysicalBulkBytes,
				Registry, OutDiagnostic)) return false;
		OutRegistry = std::move(Registry);
		return true;
	}

	namespace
	{
		auto ReadPackageImpl(FByteView PackageBytes,
			FByteView BulkBytes, uint64 PhysicalBulkBytes,
			bool bExternalPayloadAvailable, const FPackagePath& PackagePath,
			FLinkerTables& OutLinker, FPackageReaderResult* OutDiagnostic,
			const FPackageReaderLimits& Limits) -> bool
		{
			if (OutDiagnostic) OutDiagnostic->Reset();
			if (PhysicalBulkBytes > Limits.MaximumBulkBytes)
				return Fail(OutDiagnostic, EPackageReaderFailure::LimitExceeded,
					EPackageReaderReason::BulkLimit);
			FParsedLayout Layout;
			if (!ParseLayout(PackageBytes, PackageBytes.size(), true,
				Limits, Layout, OutDiagnostic)) return false;
			FLinkerTables Linker;
			Linker.FormatVersion = Layout.FormatVersion;
			FPackageRegistryData Registry;
			std::vector<FBulkEntry> BulkEntries;
			if (!DecodeNames(Layout, Limits, Linker.Names, OutDiagnostic)
				|| !DecodeImports(Layout, Linker.Names, Limits, Linker.Imports, OutDiagnostic)
				|| !DecodeRegistry(Layout, Linker.Names, PackagePath, PhysicalBulkBytes,
					Registry, OutDiagnostic)
				|| !DecodeExports(Layout, Linker.Names, Limits, Linker.Exports, OutDiagnostic)
				|| Registry.ExportCount != Linker.Exports.size()
				|| !DecodeTypes(Layout, Linker.Names, Limits, Linker.Types, OutDiagnostic)
				|| !DecodeSchemas(Layout, Linker.Names, Linker.Types, Limits,
					Linker.CustomVersions, Linker.Schemas, OutDiagnostic)
				|| !DecodeValues(Layout, Linker, Limits, OutDiagnostic)
				|| !DecodeBulkDirectory(Layout, Linker.Names, Limits, BulkEntries, OutDiagnostic))
			{
				return OutDiagnostic && OutDiagnostic->Failure != EPackageReaderFailure::None ? false
					: Fail(OutDiagnostic, EPackageReaderFailure::InvalidTable,
						EPackageReaderReason::MalformedTable);
			}

			Linker.Summary.PackagePath = PackagePath;
			Linker.Summary.HardPackageDependencies = Registry.HardPackageReferences;
			Linker.Summary.SoftPackageDependencies = Registry.SoftPackageReferences;
			Linker.Summary.BuildPackageDependencies = Registry.BuildPackageReferences;
			Linker.Summary.SearchableNames = Registry.SearchableNames;
			std::unordered_set<uint32> TopLevelExports;
			for (const FPackageAssetRegistryData& Asset : Registry.TopLevelAssets)
			{
				const uint32 ExportIndex = Asset.ExportId - 1;
				if (ExportIndex >= Linker.Exports.size()
					|| !TopLevelExports.insert(ExportIndex).second)
					return Fail(OutDiagnostic, EPackageReaderFailure::InvalidRegistry,
						EPackageReaderReason::TopLevelExportIndex, "Registry.TopLevelAssets");
				const FPackageExport& Export = Linker.Exports[ExportIndex];
				if (!Export.Outer.IsNull() || Export.ObjectName != Asset.AssetPath.GetAssetName()
					|| Export.ClassName != Asset.ClassName)
					return Fail(OutDiagnostic, EPackageReaderFailure::InvalidTopology,
						EPackageReaderReason::TopLevelTopology,
						Asset.AssetPath.ToString());
				FPackageIndex ExportId;
				if (!FPackageIndex::TryExport(ExportIndex, ExportId))
					return Fail(OutDiagnostic, EPackageReaderFailure::InvalidIndex,
						EPackageReaderReason::TopLevelIndexOverflow);
				Linker.Summary.TopLevelAssets.push_back({
					.Export = ExportId,
					.AssetPath = Asset.AssetPath,
					.ClassName = Asset.ClassName,
					.RedirectDestination = Asset.RedirectDestination});
			}
			for (uint32 ExportIndex = 0; ExportIndex < Linker.Exports.size(); ++ExportIndex)
				if (Linker.Exports[ExportIndex].Outer.IsNull()
					&& !TopLevelExports.contains(ExportIndex))
					return Fail(OutDiagnostic, EPackageReaderFailure::InvalidTopology,
						EPackageReaderReason::MissingTopLevelAsset,
						Linker.Exports[ExportIndex].ObjectName);

			if (Registry.ExternalBulkBytes != PhysicalBulkBytes
				|| (bExternalPayloadAvailable
					&& (BulkBytes.empty() ? !Registry.ExternalBulkHash.IsZero()
						: FXxHash128::HashBuffer(BulkBytes) != Registry.ExternalBulkHash)))
				return Fail(OutDiagnostic, EPackageReaderFailure::HashMismatch,
					EPackageReaderReason::ExternalBulkHash, "Registry.Bulk");
			const auto Inline = Section(Layout, EDastSection::InlineBulk);
			std::array<uint64, 2> Cursors{};
			size_t UsedBulk = 0;
			std::string PreviousExportPath;
			for (uint32 ExportIndex = 0; ExportIndex < Linker.Exports.size(); ++ExportIndex)
			{
				FPackageExport& Export = Linker.Exports[ExportIndex];
				std::string ExportPath;
				FPackageIndex Index;
				FPackageIndex::TryExport(ExportIndex, Index);
				if (const auto Result = Linker.TryResolvePath(Index, ExportPath); !Result) return Fail(OutDiagnostic,
					EPackageReaderFailure::InvalidTopology, EPackageReaderReason::ExportTopology, "Exports", ToString(Result.error()));
				if (ExportIndex != 0 && !BytewiseLess(PreviousExportPath, ExportPath))
					return Fail(OutDiagnostic, EPackageReaderFailure::NonCanonical, EPackageReaderReason::NonCanonicalBytes, "Exports");
				PreviousExportPath = ExportPath;
				for (FPropertyTag& Property : Export.Properties)
				{
					const auto SchemaIt = std::ranges::find(Linker.Schemas, Property.DeclaringType,
						&FSerializedSchema::QualifiedName);
					const uint32 SchemaId = static_cast<uint32>(SchemaIt - Linker.Schemas.begin() + 1);
					const auto FieldIt = std::ranges::find(SchemaIt->Fields, Property.FieldName,
						&FSerializedField::Name);
					const uint32 FieldId = static_cast<uint32>(FieldIt - SchemaIt->Fields.begin() + 1);
					const std::string Path = ExportPath + "." + Property.DeclaringType + "." + Property.FieldName;
					if (!BindBulkValue(Property.Value, Property.Type, Path, ExportIndex + 1,
						SchemaId, FieldId, Linker.Names, BulkEntries, Inline, BulkBytes,
						PhysicalBulkBytes, bExternalPayloadAvailable, Cursors, UsedBulk,
						OutDiagnostic)) return false;
				}
			}
			if (UsedBulk != BulkEntries.size() || Cursors[0] != Inline.size()
				|| Cursors[1] != PhysicalBulkBytes)
				return Fail(OutDiagnostic, EPackageReaderFailure::InvalidBulkData,
					EPackageReaderReason::BulkCoverage,
					"BulkDirectory");

			OutLinker = std::move(Linker);
			return true;
		}
	}

	static auto ReadPackageInternal(FByteView PackageBytes,
		FByteView BulkBytes, const FPackagePath& PackagePath,
		FLinkerTables& OutLinker, FPackageReaderResult* OutDiagnostic,
		const FPackageReaderLimits& Limits) -> bool
	{
		return ReadPackageImpl(PackageBytes, BulkBytes, BulkBytes.size(), true,
			PackagePath, OutLinker, OutDiagnostic, Limits);
	}

	static auto ReadPackageMetadataInternal(FByteView PackageBytes,
		uint64 PhysicalBulkBytes, const FPackagePath& PackagePath,
		FLinkerTables& OutLinker, FPackageReaderResult* OutDiagnostic,
		const FPackageReaderLimits& Limits) -> bool
	{
		return ReadPackageImpl(PackageBytes, {}, PhysicalBulkBytes, false,
			PackagePath, OutLinker, OutDiagnostic, Limits);
	}

	auto ReadPackageRegistry(FByteView FrontMatter,
		uint64 PhysicalPackageBytes, uint64 PhysicalBulkBytes,
		const FPackagePath& PackagePath, FPackageRegistryData& OutRegistry,
		const FPackageReaderLimits& Limits) -> FPackageReaderResult
	{
		FPackageReaderResult Result;
		if (!ReadPackageRegistryInternal(FrontMatter, PhysicalPackageBytes, PhysicalBulkBytes, PackagePath, OutRegistry, &Result, Limits) && Result.Succeeded())
			Result = {EPackageReaderFailure::InvalidTable, {}, EPackageReaderReason::MalformedTable};
		return Result;
	}

	auto ReadPackage(FByteView PackageBytes,
		FByteView BulkBytes, const FPackagePath& PackagePath,
		FLinkerTables& OutLinker,
		const FPackageReaderLimits& Limits) -> FPackageReaderResult
	{
		FPackageReaderResult Result;
		if (!ReadPackageInternal(PackageBytes, BulkBytes, PackagePath, OutLinker, &Result, Limits) && Result.Succeeded())
			Result = {EPackageReaderFailure::InvalidTable, {}, EPackageReaderReason::MalformedTable};
		return Result;
	}

	auto ReadPackageMetadata(FByteView PackageBytes,
		uint64 PhysicalBulkBytes, const FPackagePath& PackagePath,
		FLinkerTables& OutLinker,
		const FPackageReaderLimits& Limits) -> FPackageReaderResult
	{
		FPackageReaderResult Result;
		if (!ReadPackageMetadataInternal(PackageBytes, PhysicalBulkBytes, PackagePath, OutLinker, &Result, Limits) && Result.Succeeded())
			Result = {EPackageReaderFailure::InvalidTable, {}, EPackageReaderReason::MalformedTable};
		return Result;
	}

}
