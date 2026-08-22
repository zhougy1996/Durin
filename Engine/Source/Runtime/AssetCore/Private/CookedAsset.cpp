#include "Asset/Cook.h"

#include "AssetCatalogStoreInternal.h"
#include "BulkContainerInfrastructure.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Misc/LexicalPath.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint32 BulkMagic = 0x4b4c4244;
		constexpr uint32 BulkVersion = 1;
		constexpr uint32 BulkHeaderSize = 64;
		constexpr uint32 BulkEntrySize = 80;
		constexpr uint32 MaximumPayloadCount = 64;
		constexpr uint64 MaximumPayloadBytes = 8ull * 1024 * 1024 * 1024;
		constexpr uint64 MaximumBulkBytes = 64ull * 1024 * 1024 * 1024;
		constexpr uint32 ManifestMagic = 0x464e4d43;
		constexpr uint32 ManifestVersion = 1;
		constexpr uint32 ManifestHeaderSize = 48;
		constexpr uint64 MaximumManifestRecordBytes = 256ull * 1024 * 1024;
		constexpr uint32 MaximumManifestEntries = 1'000'000;

		struct FBulkHeader
		{
			uint32 Magic = 0;
			uint32 Version = 0;
			uint32 Platform = 0;
			uint32 Profile = 0;
			uint32 Flags = 0;
			uint32 HeaderSize = 0;
			uint32 Count = 0;
			uint32 EntrySize = 0;
			uint64 TableOffset = 0;
			uint64 FileSize = 0;
			uint64 TableHash = 0;
			uint64 Reserved = 0;
		};

		struct FBulkWireEntry
		{
			FCookedPayloadDescriptor Descriptor;
			uint32 Flags = 0;
		};

		struct FParsedCookedBulk
		{
			ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
			ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
			std::vector<FCookedPayloadDescriptor> Entries;
			std::vector<std::span<const uint8>> Payloads;
		};

		struct FManifestHeader
		{
			uint32 Magic = 0;
			uint32 Version = 0;
			uint32 Platform = 0;
			uint32 Profile = 0;
			uint32 Count = 0;
			uint32 HeaderSize = 0;
			uint64 RecordBytes = 0;
			uint64 RecordHash = 0;
			uint64 FileSize = 0;
		};

		struct FManifestRecordHeader
		{
			uint8 Kind = 0;
			uint8 Flags = 0;
			uint16 Reserved = 0;
			uint32 PathBytes = 0;
			uint64 FileSize = 0;
			uint64 HashLow = 0;
			uint64 HashHigh = 0;
		};

		auto ReadBulkHeader(
			BulkContainer::FBoundedReader& Reader,
			FBulkHeader& OutHeader) -> bool
		{
			FBulkHeader Header;
			Reader.Read(Header.Magic);
			Reader.Read(Header.Version);
			Reader.Read(Header.Platform);
			Reader.Read(Header.Profile);
			Reader.Read(Header.Flags);
			Reader.Read(Header.HeaderSize);
			Reader.Read(Header.Count);
			Reader.Read(Header.EntrySize);
			Reader.Read(Header.TableOffset);
			Reader.Read(Header.FileSize);
			Reader.Read(Header.TableHash);
			Reader.Read(Header.Reserved);
			if (!Reader.IsValid()) return false;
			OutHeader = Header;
			return true;
		}

		auto WriteBulkHeader(
			BulkContainer::FBoundedWriter& Writer,
			const FBulkHeader& Header) -> bool
		{
			Writer.Write(Header.Magic);
			Writer.Write(Header.Version);
			Writer.Write(Header.Platform);
			Writer.Write(Header.Profile);
			Writer.Write(Header.Flags);
			Writer.Write(Header.HeaderSize);
			Writer.Write(Header.Count);
			Writer.Write(Header.EntrySize);
			Writer.Write(Header.TableOffset);
			Writer.Write(Header.FileSize);
			Writer.Write(Header.TableHash);
			Writer.Write(Header.Reserved);
			return Writer.IsValid();
		}

		auto ReadBulkEntry(
			BulkContainer::FBoundedReader& Reader,
			FBulkWireEntry& OutEntry) -> bool
		{
			FBulkWireEntry Entry;
			Reader.ReadGuid(Entry.Descriptor.PayloadId);
			Reader.Read(Entry.Flags);
			Reader.Read(Entry.Descriptor.PayloadSchemaVersion);
			Reader.Read(Entry.Descriptor.TargetPlatform);
			Reader.Read(Entry.Descriptor.TargetProfile);
			Reader.Read(Entry.Descriptor.CompressionMethod);
			Reader.Read(Entry.Descriptor.Alignment);
			Reader.Read(Entry.Descriptor.Offset);
			Reader.Read(Entry.Descriptor.StoredSize);
			Reader.Read(Entry.Descriptor.UncompressedSize);
			Reader.Read(Entry.Descriptor.PayloadHashLow);
			Reader.Read(Entry.Descriptor.PayloadHashHigh);
			if (!Reader.IsValid()) return false;
			Entry.Descriptor.LocationKind = static_cast<uint32>(
				ECookedPayloadLocationKind::PackageCompanion);
			OutEntry = Entry;
			return true;
		}

		auto WriteBulkEntry(
			BulkContainer::FBoundedWriter& Writer,
			const FCookedBulkPayload& Payload,
			const FCookedPayloadDescriptor& Descriptor) -> bool
		{
			Writer.WriteGuid(Payload.PayloadId);
			Writer.Write(Payload.Flags);
			Writer.Write(Payload.PayloadSchemaVersion);
			Writer.Write(Descriptor.TargetPlatform);
			Writer.Write(Descriptor.TargetProfile);
			Writer.Write(static_cast<uint32>(Payload.Compression));
			Writer.Write(Payload.Alignment);
			Writer.Write(Descriptor.Offset);
			Writer.Write(Descriptor.StoredSize);
			Writer.Write(Descriptor.UncompressedSize);
			Writer.Write(Descriptor.PayloadHashLow);
			Writer.Write(Descriptor.PayloadHashHigh);
			return Writer.IsValid();
		}

		auto ReadManifestHeader(
			BulkContainer::FBoundedReader& Reader,
			FManifestHeader& OutHeader) -> bool
		{
			FManifestHeader Header;
			Reader.Read(Header.Magic);
			Reader.Read(Header.Version);
			Reader.Read(Header.Platform);
			Reader.Read(Header.Profile);
			Reader.Read(Header.Count);
			Reader.Read(Header.HeaderSize);
			Reader.Read(Header.RecordBytes);
			Reader.Read(Header.RecordHash);
			Reader.Read(Header.FileSize);
			if (!Reader.IsValid()) return false;
			OutHeader = Header;
			return true;
		}

		auto WriteManifestHeader(
			BulkContainer::FBoundedWriter& Writer,
			const FManifestHeader& Header) -> bool
		{
			Writer.Write(Header.Magic);
			Writer.Write(Header.Version);
			Writer.Write(Header.Platform);
			Writer.Write(Header.Profile);
			Writer.Write(Header.Count);
			Writer.Write(Header.HeaderSize);
			Writer.Write(Header.RecordBytes);
			Writer.Write(Header.RecordHash);
			Writer.Write(Header.FileSize);
			return Writer.IsValid();
		}

		auto ReadManifestRecordHeader(
			BulkContainer::FBoundedReader& Reader,
			FManifestRecordHeader& OutHeader) -> bool
		{
			FManifestRecordHeader Header;
			Reader.Read(Header.Kind);
			Reader.Read(Header.Flags);
			Reader.Read(Header.Reserved);
			Reader.Read(Header.PathBytes);
			Reader.Read(Header.FileSize);
			Reader.Read(Header.HashLow);
			Reader.Read(Header.HashHigh);
			if (!Reader.IsValid()) return false;
			OutHeader = Header;
			return true;
		}

		auto WriteManifestRecord(
			BulkContainer::FBoundedWriter& Writer,
			const FCookManifestEntry& Entry) -> bool
		{
			Writer.Write(static_cast<uint8>(Entry.Kind));
			Writer.Write(Entry.Flags);
			Writer.Write(uint16{0});
			Writer.Write(static_cast<uint32>(Entry.RelativePath.size()));
			Writer.Write(Entry.FileSize);
			Writer.Write(Entry.HashLow);
			Writer.Write(Entry.HashHigh);
			Writer.WriteBytes(std::span{
				reinterpret_cast<const uint8*>(Entry.RelativePath.data()),
				Entry.RelativePath.size()});
			return Writer.IsValid();
		}

		class FCookedPackageBulkDataProvider final : public IBulkDataProvider
		{
		public:
			FCookedPackageBulkDataProvider(
				FAssetRuntimeConfiguration InRuntimeConfiguration,
				std::string InVirtualPackagePath,
				FCookedPayloadDescriptor InDescriptor,
				ECookTargetPlatform InExpectedPlatform,
				ECookTargetProfile InExpectedProfile)
				: RuntimeConfiguration(std::move(InRuntimeConfiguration)),
				  VirtualPackagePath(std::move(InVirtualPackagePath)),
				  Descriptor(InDescriptor),
				  ExpectedPlatform(InExpectedPlatform),
				  ExpectedProfile(InExpectedProfile)
			{
			}

			auto GetStorageDomain() const -> EBulkDataStorageDomain override
			{
				return EBulkDataStorageDomain::Cooked;
			}

			auto LoadSynchronous(
				const FBulkDataDescriptor&,
				FSharedByteBuffer& OutBuffer,
				std::string& OutError) const -> bool override
			{
				FCookedPackagePayload Loaded;
				if (!LoadCookedPackagePayload(RuntimeConfiguration, VirtualPackagePath,
					Descriptor, ExpectedPlatform, ExpectedProfile, Loaded, &OutError))
					return false;
				OutBuffer = FSharedByteBuffer::Copy(std::as_bytes(Loaded.Payload));
				OutError.clear();
				return true;
			}

		private:
			FAssetRuntimeConfiguration RuntimeConfiguration;
			std::string VirtualPackagePath;
			FCookedPayloadDescriptor Descriptor;
			ECookTargetPlatform ExpectedPlatform;
			ECookTargetProfile ExpectedProfile;
		};

		auto Fail(std::string Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto CanonicalizeCookVirtualPath(
			std::string& VirtualPackagePath,
			std::string* OutError) -> bool
		{
			FAssetPath RequestedPath;
			if (!FAssetPath::TryCreate(
				VirtualPackagePath, RequestedPath))
				return true;
			const FAssetCatalogStore& Registry = GetAssetCatalogStore();
			if (!Registry.FindAssetExact(RequestedPath)) return true;
			const FAssetPathResolveResult Resolution =
				Registry.ResolveAssetPath(RequestedPath);
			if (!Resolution || !Resolution.FinalAssetData
				|| Resolution.FinalAssetData->EntryKind
					!= EAssetRegistryEntryKind::Asset)
				return Fail(std::format(
					"Cook output path {} does not resolve to a final real asset.",
					RequestedPath.ToString()), OutError);
			VirtualPackagePath = Resolution.FinalPath.ToString();
			return true;
		}

		auto IsValidTarget(ECookTargetPlatform Platform, ECookTargetProfile Profile) -> bool
		{
			return Platform == ECookTargetPlatform::Win64
				&& (Profile == ECookTargetProfile::Game || Profile == ECookTargetProfile::EditorValidation);
		}

		auto ParseCookedBulk(
			std::span<const uint8> Bytes,
			ECookTargetPlatform ExpectedPlatform,
			ECookTargetProfile ExpectedProfile,
			FParsedCookedBulk& OutParsed,
			std::string* OutError) -> bool
		{
			if (Bytes.size() < BulkHeaderSize || Bytes.size() > MaximumBulkBytes)
				return Fail("DBLK file size is invalid.", OutError);
			BulkContainer::FBoundedReader Reader(Bytes, MaximumBulkBytes);
			FBulkHeader Header;
			if (!ReadBulkHeader(Reader, Header))
				return Fail("DBLK header is truncated.", OutError);
			if (Header.Magic != BulkMagic || Header.Version != BulkVersion
				|| Header.Flags != 0 || Header.HeaderSize != BulkHeaderSize
				|| Header.Count == 0 || Header.Count > MaximumPayloadCount
				|| Header.EntrySize != BulkEntrySize || Header.TableOffset != BulkHeaderSize
				|| Header.FileSize != Bytes.size() || Header.Reserved != 0)
				return Fail("DBLK header is invalid.", OutError);
			if (Header.Platform != static_cast<uint32>(ExpectedPlatform)
				|| Header.Profile != static_cast<uint32>(ExpectedProfile)
				|| !IsValidTarget(static_cast<ECookTargetPlatform>(Header.Platform),
					static_cast<ECookTargetProfile>(Header.Profile)))
				return Fail("DBLK target does not match the load context.", OutError);

			uint64 TableBytes = 0, DirectoryEnd = 0, MinimumDataOffset = 0;
			if (!BulkContainer::TryMultiply(
					Header.Count, BulkEntrySize, MaximumBulkBytes, TableBytes)
				|| !BulkContainer::TryAdd(
					BulkHeaderSize, TableBytes, MaximumBulkBytes, DirectoryEnd)
				|| DirectoryEnd > Bytes.size())
				return Fail("DBLK table is truncated.", OutError);
			std::span<const uint8> Table;
			if (!BulkContainer::TryProjectRange(Bytes, BulkHeaderSize, TableBytes, Table))
				return Fail("DBLK table is truncated.", OutError);
			if (FXxHash64::HashBuffer(Table).HashValue != Header.TableHash)
				return Fail("DBLK table checksum is invalid.", OutError);
			if (!BulkContainer::TryAlignUp(
				DirectoryEnd, 16, MaximumBulkBytes, MinimumDataOffset))
				return Fail("DBLK payload range is invalid.", OutError);

			BulkContainer::FBoundedReader TableReader(Table, TableBytes);
			FParsedCookedBulk Parsed;
			Parsed.TargetPlatform = static_cast<ECookTargetPlatform>(Header.Platform);
			Parsed.TargetProfile = static_cast<ECookTargetProfile>(Header.Profile);
			Parsed.Entries.reserve(Header.Count);
			std::vector<BulkContainer::FPayloadRange> Ranges;
			Ranges.reserve(Header.Count);
			for (uint32 Index = 0; Index < Header.Count; ++Index)
			{
				FBulkWireEntry WireEntry;
				if (!ReadBulkEntry(TableReader, WireEntry))
					return Fail("DBLK table entry is truncated.", OutError);
				const FCookedPayloadDescriptor& Entry = WireEntry.Descriptor;
				if (!Entry.PayloadId.IsValid()
					|| (Index && !(Parsed.Entries.back().PayloadId < Entry.PayloadId))
					|| (WireEntry.Flags & ~1u) != 0 || Entry.PayloadSchemaVersion == 0
					|| Entry.TargetPlatform != Header.Platform
					|| Entry.TargetProfile != Header.Profile
					|| !BulkContainer::IsPowerOfTwo(Entry.Alignment)
					|| Entry.Alignment < 16 || Entry.Alignment > 4096
					|| Entry.StoredSize == 0 || Entry.StoredSize > MaximumPayloadBytes
					|| Entry.UncompressedSize == 0
					|| Entry.UncompressedSize > MaximumPayloadBytes)
					return Fail("DBLK table entry is invalid.", OutError);
				if (Entry.CompressionMethod
					== static_cast<uint32>(ECookedPayloadCompression::None))
				{
					if (Entry.StoredSize != Entry.UncompressedSize)
						return Fail("DBLK uncompressed sizes differ.", OutError);
				}
				else if (Entry.CompressionMethod
					== static_cast<uint32>(ECookedPayloadCompression::Zstandard))
				{
					const uint64 Ratio = Entry.UncompressedSize / Entry.StoredSize;
					if (Ratio > 64 || (Ratio == 64
						&& Entry.UncompressedSize % Entry.StoredSize != 0))
						return Fail("DBLK compression ratio exceeds its bound.", OutError);
					return Fail("DBLK Zstandard compression is unsupported by this build.", OutError);
				}
				else
				{
					return Fail("DBLK compression method is unknown.", OutError);
				}
				Ranges.push_back({Entry.Offset, Entry.StoredSize, Entry.Alignment});
				Parsed.Entries.push_back(Entry);
			}

			const BulkContainer::FLayoutPolicy LayoutPolicy{
				.MaximumCount = MaximumPayloadCount,
				.MaximumPayloadBytes = MaximumPayloadBytes,
				.MaximumContainerBytes = MaximumBulkBytes,
				.RequireCanonicalOffsets = false,
				.AllowTrailingZeroPadding = true};
			BulkContainer::FFailure LayoutFailure;
			if (!BulkContainer::ValidateLayout(
				Bytes, DirectoryEnd, MinimumDataOffset, Ranges, LayoutPolicy, &LayoutFailure))
			{
				if (LayoutFailure.Category == BulkContainer::EFailure::TrailingNonzeroPadding)
					return Fail("DBLK trailing padding is nonzero.", OutError);
				if (LayoutFailure.Category == BulkContainer::EFailure::NonzeroPadding)
					return Fail("DBLK alignment padding is nonzero.", OutError);
				return Fail("DBLK payload range is invalid.", OutError);
			}

			Parsed.Payloads.reserve(Parsed.Entries.size());
			for (const FCookedPayloadDescriptor& Entry : Parsed.Entries)
			{
				std::span<const uint8> Stored;
				if (!BulkContainer::TryProjectRange(
					Bytes, Entry.Offset, Entry.StoredSize, Stored))
					return Fail("DBLK payload range is invalid.", OutError);
				const FXxHash128 Hash = FXxHash128::HashBuffer(Stored);
				if (Hash.HashLow != Entry.PayloadHashLow
					|| Hash.HashHigh != Entry.PayloadHashHigh)
					return Fail("DBLK payload checksum is invalid.", OutError);
				Parsed.Payloads.push_back(Stored);
			}
			OutParsed = std::move(Parsed);
			if (OutError) OutError->clear();
			return true;
		}

		auto IsValidRelativeManifestPath(std::string_view Value) -> bool
		{
			if (Value.empty() || Value.size() > 1024 || Value.front() == '/' || Value.back() == '/') return false;
			if (Value.find('\\') != std::string_view::npos || Value.find('\0') != std::string_view::npos) return false;
			size_t Byte = 0;
			while (Byte < Value.size())
			{
				const uint8 Lead = static_cast<uint8>(Value[Byte++]);
				if (Lead < 0x80) continue;
				uint32 CodePoint = 0;
				size_t Continuations = 0;
				if ((Lead & 0xe0) == 0xc0) { CodePoint = Lead & 0x1f; Continuations = 1; }
				else if ((Lead & 0xf0) == 0xe0) { CodePoint = Lead & 0x0f; Continuations = 2; }
				else if ((Lead & 0xf8) == 0xf0) { CodePoint = Lead & 0x07; Continuations = 3; }
				else return false;
				if (Byte + Continuations > Value.size()) return false;
				for (size_t Index = 0; Index < Continuations; ++Index)
				{
					const uint8 Tail = static_cast<uint8>(Value[Byte++]);
					if ((Tail & 0xc0) != 0x80) return false;
					CodePoint = (CodePoint << 6) | (Tail & 0x3f);
				}
				const uint32 Minimum = Continuations == 1 ? 0x80 : Continuations == 2 ? 0x800 : 0x10000;
				if (CodePoint < Minimum || CodePoint > 0x10ffff || (CodePoint >= 0xd800 && CodePoint <= 0xdfff))
					return false;
			}
			const std::filesystem::path Path(Value);
			if (Path.is_absolute() || Path.has_root_path() || Path.lexically_normal().generic_string() != Value) return false;
			return std::ranges::none_of(Path, [](const std::filesystem::path& Part) {
				if (Part.empty() || Part == "." || Part == "..") return true;
				std::string Stem = Part.stem().generic_string();
				std::ranges::transform(Stem, Stem.begin(), [](char Character) {
					return static_cast<char>(std::toupper(static_cast<unsigned char>(Character)));
				});
				static constexpr std::array<std::string_view, 4> FixedDevices = {"CON", "PRN", "AUX", "NUL"};
				if (std::ranges::find(FixedDevices, Stem) != FixedDevices.end()) return true;
				return Stem.size() == 4 && (Stem.starts_with("COM") || Stem.starts_with("LPT"))
					&& Stem[3] >= '1' && Stem[3] <= '9';
			});
		}

		auto MakeTemporaryPath(const std::filesystem::path& Destination) -> std::filesystem::path
		{
			return Destination.parent_path() / std::format(".{}.cooktmp", FGuid::NewGuid().ToString());
		}

		auto WriteValidatedTemporary(
			const std::filesystem::path& Destination,
			std::span<const uint8> Bytes,
			const std::function<bool(std::span<const uint8>, std::string*)>& Validate,
			std::filesystem::path& OutTemporary,
			std::string* OutError) -> bool
		{
			std::error_code ErrorCode;
			std::filesystem::create_directories(Destination.parent_path(), ErrorCode);
			if (ErrorCode) return Fail(std::format("Failed to create cook directory: {}", ErrorCode.message()), OutError);
			OutTemporary = MakeTemporaryPath(Destination);
			FFileHelper::FAtomicFileError FileError;
			if (!FFileHelper::SaveArrayToFileAtomically(
				std::span{reinterpret_cast<const std::byte*>(Bytes.data()), Bytes.size()}, OutTemporary, &FileError))
				return Fail(FileError.ToString(), OutError);
			std::vector<uint8> Reloaded;
			std::string ValidationError;
			if (!FFileHelper::LoadFileToArray(Reloaded, OutTemporary.generic_string())
				|| !Validate(Reloaded, &ValidationError))
			{
				std::filesystem::remove(OutTemporary, ErrorCode);
				return Fail(ValidationError.empty() ? "Failed to reopen temporary cook output." : ValidationError, OutError);
			}
			return true;
		}

		auto PublishTemporary(
			const std::filesystem::path& Temporary,
			const std::filesystem::path& Destination,
			std::string* OutError) -> bool
		{
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Temporary.generic_string()))
				return Fail("Failed to reopen validated temporary cook output for publication.", OutError);
			FFileHelper::FAtomicFileError FileError;
			if (!FFileHelper::SaveArrayToFileAtomically(
				std::span{reinterpret_cast<const std::byte*>(Bytes.data()), Bytes.size()}, Destination, &FileError))
				return Fail(FileError.ToString(), OutError);
			std::error_code ErrorCode;
			std::filesystem::remove(Temporary, ErrorCode);
			return true;
		}
	}

	auto FAssetRuntimeConfiguration::Authored() -> FAssetRuntimeConfiguration
	{
		return {};
	}

	auto FAssetRuntimeConfiguration::Cooked(
		std::filesystem::path InCookRoot,
		FAssetRuntimeConfiguration& OutConfiguration) -> FAssetResult
	{
		if (InCookRoot.empty() || !InCookRoot.is_absolute()
			|| InCookRoot.lexically_normal() != InCookRoot)
		{
			return {
				.Error = EAssetError::InvalidPath,
				.Message = "Cooked asset execution requires an absolute normalized cook root."};
		}
		FAssetRuntimeConfiguration Result;
		Result.ExecutionDomain = EAssetExecutionDomain::Cooked;
		Result.PayloadPolicy = EAssetPayloadPolicy::CookedPayloadRequired;
		Result.CookRoot = std::move(InCookRoot);
		OutConfiguration = std::move(Result);
		return {};
	}

	auto EncodeCookedBulk(
		std::span<const FCookedBulkPayload> Payloads,
		ECookTargetPlatform TargetPlatform,
		ECookTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::vector<FCookedPayloadDescriptor>* OutDescriptors,
		std::string* OutError) -> bool
	{
		OutBytes.clear();
		if (OutDescriptors) OutDescriptors->clear();
		if (!IsValidTarget(TargetPlatform, TargetProfile))
			return Fail("DBLK target is invalid.", OutError);
		if (Payloads.empty() || Payloads.size() > MaximumPayloadCount)
			return Fail("DBLK payload count is outside its bound.", OutError);

		std::vector<const FCookedBulkPayload*> Sorted;
		if (!BulkContainer::TryMakeSortedProjection<FCookedBulkPayload>(
			Payloads, &FCookedBulkPayload::PayloadId, Sorted))
			return Fail("DBLK payload identifiers must be nonzero and unique.", OutError);
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			const FCookedBulkPayload& Payload = *Sorted[Index];
			if (!Payload.PayloadId.IsValid())
				return Fail("DBLK payload identifiers must be nonzero and unique.", OutError);
			if ((Payload.Flags & ~1u) != 0 || Payload.PayloadSchemaVersion == 0)
				return Fail("DBLK payload flags or schema are invalid.", OutError);
			if (!BulkContainer::IsPowerOfTwo(Payload.Alignment) || Payload.Alignment < 16 || Payload.Alignment > 4096)
				return Fail("DBLK payload alignment is invalid.", OutError);
			if (Payload.Compression != ECookedPayloadCompression::None)
				return Fail("DBLK writer supports only uncompressed version 1 payloads.", OutError);
			if (Payload.Bytes.empty() || Payload.Bytes.size() > MaximumPayloadBytes)
				return Fail("DBLK payload size is outside its bound.", OutError);
		}

		uint64 TableBytes = 0, TableEnd = 0, DataOffset = 0;
		if (!BulkContainer::TryMultiply(Sorted.size(), BulkEntrySize, MaximumBulkBytes, TableBytes)
			|| !BulkContainer::TryAdd(BulkHeaderSize, TableBytes, MaximumBulkBytes, TableEnd)
			|| !BulkContainer::TryAlignUp(TableEnd, 16, MaximumBulkBytes, DataOffset))
			return Fail("DBLK table size overflowed.", OutError);
		std::vector<BulkContainer::FLayoutItem> LayoutItems;
		LayoutItems.reserve(Sorted.size());
		for (const FCookedBulkPayload* Payload : Sorted)
			LayoutItems.push_back({Payload->Bytes.size(), Payload->Alignment});
		std::vector<BulkContainer::FPayloadRange> Ranges;
		uint64 FileSize = 0;
		const BulkContainer::FLayoutPolicy LayoutPolicy{
			.MaximumCount = MaximumPayloadCount,
			.MaximumPayloadBytes = MaximumPayloadBytes,
			.MaximumContainerBytes = MaximumBulkBytes,
			.RequireCanonicalOffsets = true,
			.AllowTrailingZeroPadding = false};
		if (!BulkContainer::TryBuildLayout(
			DataOffset, LayoutItems, LayoutPolicy, Ranges, FileSize))
			return Fail("DBLK container exceeds its size bound.", OutError);
		std::vector<FCookedPayloadDescriptor> Descriptors;
		Descriptors.reserve(Sorted.size());
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			const FCookedBulkPayload& Payload = *Sorted[Index];
			const FXxHash128 Hash = FXxHash128::HashBuffer(Payload.Bytes);
			Descriptors.push_back({
				.PayloadId = Payload.PayloadId,
				.LocationKind = static_cast<uint32>(ECookedPayloadLocationKind::PackageCompanion),
				.Offset = Ranges[Index].Offset,
				.StoredSize = Payload.Bytes.size(),
				.UncompressedSize = Payload.Bytes.size(),
				.Alignment = Payload.Alignment,
				.PayloadHashLow = Hash.HashLow,
				.PayloadHashHigh = Hash.HashHigh,
				.PayloadSchemaVersion = Payload.PayloadSchemaVersion,
				.TargetPlatform = static_cast<uint32>(TargetPlatform),
				.TargetProfile = static_cast<uint32>(TargetProfile),
				.CompressionMethod = static_cast<uint32>(Payload.Compression)});
		}

		BulkContainer::FBoundedWriter Table(TableBytes);
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			const FCookedBulkPayload& Payload = *Sorted[Index];
			const FCookedPayloadDescriptor& Descriptor = Descriptors[Index];
			if (!WriteBulkEntry(Table, Payload, Descriptor))
				return Fail("DBLK table encoding failed.", OutError);
		}
		const uint64 TableHash = FXxHash64::HashBuffer(Table.View()).HashValue;
		BulkContainer::FBoundedWriter Writer(MaximumBulkBytes);
		const FBulkHeader Header{
			.Magic = BulkMagic,
			.Version = BulkVersion,
			.Platform = static_cast<uint32>(TargetPlatform),
			.Profile = static_cast<uint32>(TargetProfile),
			.Flags = 0,
			.HeaderSize = BulkHeaderSize,
			.Count = static_cast<uint32>(Sorted.size()),
			.EntrySize = BulkEntrySize,
			.TableOffset = BulkHeaderSize,
			.FileSize = FileSize,
			.TableHash = TableHash,
			.Reserved = 0};
		if (!WriteBulkHeader(Writer, Header) || !Writer.WriteBytes(Table.View()))
			return Fail("DBLK encoding failed.", OutError);
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			if (!Writer.PadTo(Descriptors[Index].Offset)) return Fail("DBLK payload layout overflowed.", OutError);
			if (!Writer.WriteBytes(Sorted[Index]->Bytes)) return Fail("DBLK encoding failed.", OutError);
		}
		std::vector<uint8> Candidate;
		if (Writer.Tell() != FileSize || !Writer.TryTake(Candidate))
			return Fail("DBLK encoding failed.", OutError);
		FParsedCookedBulk Validation;
		if (!ParseCookedBulk(
			Candidate, TargetPlatform, TargetProfile, Validation, OutError)) return false;
		OutBytes = std::move(Candidate);
		if (OutDescriptors) *OutDescriptors = std::move(Descriptors);
		if (OutError) OutError->clear();
		return true;
	}

	auto DecodeCookedBulk(
		std::span<const uint8> Bytes,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FCookedBulkContainer& OutContainer,
		std::string* OutError) -> bool
	{
		OutContainer = {};
		FParsedCookedBulk Parsed;
		if (!ParseCookedBulk(
			Bytes, ExpectedPlatform, ExpectedProfile, Parsed, OutError)) return false;
		std::vector<std::vector<uint8>> Payloads;
		Payloads.reserve(Parsed.Payloads.size());
		for (std::span<const uint8> Stored : Parsed.Payloads)
			Payloads.emplace_back(Stored.begin(), Stored.end());
		OutContainer.TargetPlatform = Parsed.TargetPlatform;
		OutContainer.TargetProfile = Parsed.TargetProfile;
		OutContainer.Entries = std::move(Parsed.Entries);
		OutContainer.Payloads = std::move(Payloads);
		if (OutError) OutError->clear();
		return true;
	}

	auto ResolveCookedPayload(
		const FCookedBulkContainer& Container,
		const FCookedPayloadDescriptor& Descriptor,
		std::span<const uint8>& OutPayload,
		std::string* OutError) -> bool
	{
		OutPayload = {};
		if (Descriptor.LocationKind != static_cast<uint32>(ECookedPayloadLocationKind::PackageCompanion))
			return Fail("Cooked payload location kind is unknown.", OutError);
		auto It = std::ranges::find(Container.Entries, Descriptor.PayloadId, &FCookedPayloadDescriptor::PayloadId);
		if (It == Container.Entries.end() || *It != Descriptor)
			return Fail("Cooked payload descriptor does not exactly match its DBLK entry.", OutError);
		const size_t Index = static_cast<size_t>(std::distance(Container.Entries.begin(), It));
		OutPayload = Container.Payloads[Index];
		return true;
	}

	auto LoadCookedPackagePayload(
		const FAssetRuntimeConfiguration& RuntimeConfiguration,
		std::string_view VirtualPackagePath,
		const FCookedPayloadDescriptor& Descriptor,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FCookedPackagePayload& OutPayload,
		std::string* OutError) -> bool
	{
		std::filesystem::path PackagePath;
		std::filesystem::path CompanionPath;
		if (!RuntimeConfiguration.RequiresCookedPayload())
			return Fail("Cooked payload loading requires the cooked runtime configuration.", OutError);
		if (!ResolveCookedPackagePath(
				RuntimeConfiguration.GetCookRoot(), VirtualPackagePath, PackagePath, OutError)
			|| !ResolveCookedCompanionPath(
				RuntimeConfiguration.GetCookRoot(), PackagePath, CompanionPath, OutError))
		{
			return false;
		}

		FCookedBulkContainer Candidate;
		if (!LoadCookedBulkFile(
			CompanionPath, ExpectedPlatform, ExpectedProfile, Candidate, OutError))
		{
			return false;
		}
		std::span<const uint8> CandidatePayload;
		if (!ResolveCookedPayload(Candidate, Descriptor, CandidatePayload, OutError))
			return false;

		OutPayload.Payload = {};
		OutPayload.Container = std::move(Candidate);
		if (!ResolveCookedPayload(
			OutPayload.Container, Descriptor, OutPayload.Payload, OutError))
		{
			OutPayload.Container = {};
			OutPayload.Payload = {};
			return false;
		}
		return true;
	}

	auto CreateCookedPackageBulkData(
		const FAssetRuntimeConfiguration& RuntimeConfiguration,
		std::string_view VirtualPackagePath,
		const FCookedPayloadDescriptor& Descriptor,
		FGuid FormatId,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FBulkData& OutBulkData,
		std::string* OutError) -> bool
	{
		if (!RuntimeConfiguration.RequiresCookedPayload())
			return Fail("Cooked bulk data requires the cooked runtime configuration.", OutError);
		if (VirtualPackagePath.empty()
			|| Descriptor.LocationKind != static_cast<uint32>(
				ECookedPayloadLocationKind::PackageCompanion)
			|| Descriptor.TargetPlatform != static_cast<uint32>(ExpectedPlatform)
			|| Descriptor.TargetProfile != static_cast<uint32>(ExpectedProfile))
			return Fail(
				"Cooked bulk data descriptor does not match its package, location, target, or profile.",
				OutError);

		FBulkDataDescriptor BulkDescriptor{
			.PayloadId = Descriptor.PayloadId,
			.FormatId = FormatId,
			.FormatVersion = Descriptor.PayloadSchemaVersion,
			.LogicalByteCount = Descriptor.UncompressedSize,
			.StoredByteCount = Descriptor.StoredSize,
			.ContentHash = {Descriptor.PayloadHashLow, Descriptor.PayloadHashHigh}};
		auto Provider = std::make_shared<FCookedPackageBulkDataProvider>(
			RuntimeConfiguration, std::string(VirtualPackagePath), Descriptor,
			ExpectedPlatform, ExpectedProfile);
		return FBulkData::TryCreateUnloaded(
			std::move(BulkDescriptor), std::move(Provider), OutBulkData, OutError);
	}

	auto LoadCookedBulkFile(
		const std::filesystem::path& Path,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FCookedBulkContainer& OutContainer,
		std::string* OutError) -> bool
	{
		OutContainer = {};
		std::error_code ErrorCode;
		const std::filesystem::file_status Status = std::filesystem::symlink_status(Path, ErrorCode);
		if (ErrorCode || !std::filesystem::is_regular_file(Status))
			return Fail("Cooked bulk companion is missing or is not a regular file.", OutError);
		const uint64 FileSize = std::filesystem::file_size(Path, ErrorCode);
		if (ErrorCode || FileSize > MaximumBulkBytes || FileSize > std::numeric_limits<size_t>::max())
			return Fail("Cooked bulk companion size is invalid.", OutError);
		std::vector<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, Path.generic_string()))
			return Fail("Failed to read cooked bulk companion.", OutError);
		return DecodeCookedBulk(Bytes, ExpectedPlatform, ExpectedProfile, OutContainer, OutError);
	}

	auto ResolveCookedPackagePath(
		const std::filesystem::path& CookRoot,
		std::string_view VirtualPackagePath,
		std::filesystem::path& OutPackagePath,
		std::string* OutError) -> bool
	{
		OutPackagePath.clear();
		if (CookRoot.empty() || !CookRoot.is_absolute() || VirtualPackagePath.empty()
			|| VirtualPackagePath.front() != '/' || VirtualPackagePath.back() == '/'
			|| VirtualPackagePath.find('\\') != std::string_view::npos)
			return Fail("Cooked package path or root is invalid.", OutError);
		const size_t Slash = VirtualPackagePath.find('/', 1);
		const std::string_view Mount = Slash == std::string_view::npos
			? VirtualPackagePath.substr(1) : VirtualPackagePath.substr(1, Slash - 1);
		if ((Mount != "Engine" && Mount != "Game") || Slash == std::string_view::npos)
			return Fail("Cooked package mount is not Engine or Game.", OutError);
		const std::string Relative(VirtualPackagePath.substr(1));
		if (!IsValidRelativeManifestPath(Relative)) return Fail("Cooked package path is not normalized.", OutError);
		const std::filesystem::path Root = CookRoot.lexically_normal();
		std::filesystem::path Candidate = (Root / std::filesystem::path(Relative)).lexically_normal();
		Candidate += ".dasset";
		if (!PathUtilities::IsLexicalDescendantPath(Candidate, Root, true))
			return Fail("Cooked package path escapes the cook root.", OutError);
		OutPackagePath = std::move(Candidate);
		return true;
	}

	auto ResolveCookedCompanionPath(
		const std::filesystem::path& CookRoot,
		const std::filesystem::path& PackagePath,
		std::filesystem::path& OutCompanionPath,
		std::string* OutError) -> bool
	{
		OutCompanionPath.clear();
		const std::filesystem::path Root = CookRoot.lexically_normal();
		const std::filesystem::path Normalized = PackagePath.lexically_normal();
		if (Root.empty() || !Root.is_absolute() || PackagePath.extension() != ".dasset"
			|| !PathUtilities::IsLexicalDescendantPath(Normalized, Root, true))
			return Fail("Cooked companion package path is invalid or outside the cook root.", OutError);
		OutCompanionPath = Normalized;
		OutCompanionPath.replace_extension(".dbulk");
		if (!PathUtilities::IsLexicalDescendantPath(OutCompanionPath, Root, true))
			return Fail("Cooked companion path escapes the cook root.", OutError);
		return true;
	}

	auto EncodeCookManifest(const FCookManifest& Manifest, std::vector<uint8>& OutBytes, std::string* OutError) -> bool
	{
		OutBytes.clear();
		if (!IsValidTarget(Manifest.TargetPlatform, Manifest.TargetProfile))
			return Fail("Cook manifest target is invalid.", OutError);
		if (Manifest.Entries.size() > MaximumManifestEntries)
			return Fail("Cook manifest entry count exceeds its bound.", OutError);
		std::vector<const FCookManifestEntry*> Entries;
		if (!BulkContainer::TryMakeSortedProjection<FCookManifestEntry>(
			Manifest.Entries, &FCookManifestEntry::RelativePath, Entries))
			return Fail("Cook manifest entry is invalid.", OutError);
		BulkContainer::FBoundedWriter Records(MaximumManifestRecordBytes);
		for (const FCookManifestEntry* EntryPointer : Entries)
		{
			const FCookManifestEntry& Entry = *EntryPointer;
			if (!IsValidRelativeManifestPath(Entry.RelativePath)
				|| (Entry.Kind != ECookManifestEntryKind::CookedPackage && Entry.Kind != ECookManifestEntryKind::CookedBulk)
				|| Entry.Flags != 1 || Entry.FileSize == 0)
				return Fail("Cook manifest entry is invalid.", OutError);
			if (!WriteManifestRecord(Records, Entry))
				return Fail("Cook manifest records exceed their byte bound.", OutError);
		}
		uint64 MaximumManifestBytes = 0;
		if (!BulkContainer::TryAdd(ManifestHeaderSize, MaximumManifestRecordBytes,
			std::numeric_limits<uint64>::max(), MaximumManifestBytes))
			return Fail("Cook manifest records exceed their byte bound.", OutError);
		BulkContainer::FBoundedWriter Writer(MaximumManifestBytes);
		const uint64 RecordBytes = Records.Tell();
		uint64 FileSize = 0;
		std::vector<uint8> Candidate;
		if (!BulkContainer::TryAdd(
			ManifestHeaderSize, RecordBytes, MaximumManifestBytes, FileSize))
			return Fail("Cook manifest encoding failed.", OutError);
		const FManifestHeader Header{
			.Magic = ManifestMagic,
			.Version = ManifestVersion,
			.Platform = static_cast<uint32>(Manifest.TargetPlatform),
			.Profile = static_cast<uint32>(Manifest.TargetProfile),
			.Count = static_cast<uint32>(Entries.size()),
			.HeaderSize = ManifestHeaderSize,
			.RecordBytes = RecordBytes,
			.RecordHash = FXxHash64::HashBuffer(Records.View()).HashValue,
			.FileSize = FileSize};
		if (!WriteManifestHeader(Writer, Header)
			|| !Writer.WriteBytes(Records.View()) || !Writer.TryTake(Candidate))
			return Fail("Cook manifest encoding failed.", OutError);
		FCookManifest Validation;
		if (!DecodeCookManifest(Candidate, Validation, OutError)) return false;
		OutBytes = std::move(Candidate);
		return true;
	}

	auto DecodeCookManifest(std::span<const uint8> Bytes, FCookManifest& OutManifest, std::string* OutError) -> bool
	{
		OutManifest = {};
		if (Bytes.size() < ManifestHeaderSize) return Fail("Cook manifest is truncated.", OutError);
		BulkContainer::FBoundedReader Reader(
			Bytes, ManifestHeaderSize + MaximumManifestRecordBytes);
		FManifestHeader Header;
		if (!ReadManifestHeader(Reader, Header))
			return Fail("Cook manifest header is truncated.", OutError);
		if (Header.Magic != ManifestMagic || Header.Version != ManifestVersion
			|| Header.HeaderSize != ManifestHeaderSize
			|| Header.Count > MaximumManifestEntries
			|| Header.RecordBytes > MaximumManifestRecordBytes
			|| Header.FileSize != Bytes.size()
			|| Header.RecordBytes != Bytes.size() - ManifestHeaderSize
			|| !IsValidTarget(static_cast<ECookTargetPlatform>(Header.Platform),
				static_cast<ECookTargetProfile>(Header.Profile)))
			return Fail("Cook manifest header is invalid.", OutError);
		const std::span<const uint8> Records = Bytes.subspan(ManifestHeaderSize);
		if (FXxHash64::HashBuffer(Records).HashValue != Header.RecordHash)
			return Fail("Cook manifest record checksum is invalid.", OutError);
		BulkContainer::FBoundedReader RecordReader(Records, MaximumManifestRecordBytes);
		std::vector<FCookManifestEntry> Entries;
		Entries.reserve(Header.Count);
		for (uint32 Index = 0; Index < Header.Count; ++Index)
		{
			FManifestRecordHeader RecordHeader;
			FCookManifestEntry Entry;
			if (!ReadManifestRecordHeader(RecordReader, RecordHeader)
				|| RecordHeader.Reserved != 0 || RecordHeader.PathBytes == 0
				|| RecordHeader.PathBytes > 1024)
				return Fail("Cook manifest record is invalid.", OutError);
			std::span<const uint8> Path;
			if (!RecordReader.ReadBytes(RecordHeader.PathBytes, Path))
				return Fail("Cook manifest path is truncated.", OutError);
			Entry.Kind = static_cast<ECookManifestEntryKind>(RecordHeader.Kind);
			Entry.Flags = RecordHeader.Flags;
			Entry.FileSize = RecordHeader.FileSize;
			Entry.HashLow = RecordHeader.HashLow;
			Entry.HashHigh = RecordHeader.HashHigh;
			Entry.RelativePath.assign(reinterpret_cast<const char*>(Path.data()), Path.size());
			if (!IsValidRelativeManifestPath(Entry.RelativePath)
				|| (Index && !(Entries.back().RelativePath < Entry.RelativePath))
				|| (Entry.Kind != ECookManifestEntryKind::CookedPackage && Entry.Kind != ECookManifestEntryKind::CookedBulk)
				|| Entry.Flags != 1 || Entry.FileSize == 0)
				return Fail("Cook manifest entry is invalid.", OutError);
			Entries.push_back(std::move(Entry));
		}
		if (RecordReader.Tell() != Records.size()) return Fail("Cook manifest has trailing record bytes.", OutError);
		OutManifest.TargetPlatform = static_cast<ECookTargetPlatform>(Header.Platform);
		OutManifest.TargetProfile = static_cast<ECookTargetProfile>(Header.Profile);
		OutManifest.Entries = std::move(Entries);
		return true;
	}

	FCookContext::FCookContext(
		std::filesystem::path InCookRoot,
		ECookTargetPlatform InTargetPlatform,
		ECookTargetProfile InTargetProfile)
		: CookRoot(InCookRoot.lexically_normal())
		, TargetPlatform(InTargetPlatform)
		, TargetProfile(InTargetProfile)
	{
	}

	auto FCookContext::AddPackage(
		std::string VirtualPackagePath,
		std::vector<uint8> PackageBytes,
		std::vector<FCookedBulkPayload> Payloads,
		std::string* OutError) -> bool
	{
		std::filesystem::path PackagePath;
		if (!ResolveCookedPackagePath(CookRoot, VirtualPackagePath, PackagePath, OutError)) return false;
		if (PackageBytes.empty()) return Fail("Cook package bytes must be nonempty.", OutError);
		if (std::ranges::any_of(Packages, [&](const FPendingPackage& Existing) {
			return Existing.VirtualPath == VirtualPackagePath;
		})) return Fail("Cook package path is duplicated.", OutError);
		std::vector<uint8> BulkBytes;
		if (!Payloads.empty()
			&& !EncodeCookedBulk(
				Payloads, TargetPlatform, TargetProfile,
				BulkBytes, nullptr, OutError)) return false;
		Packages.push_back({std::move(VirtualPackagePath), std::move(PackageBytes), std::move(BulkBytes)});
		return true;
	}

	auto FCookContext::AddPackage(
		std::string VirtualPackagePath,
		std::vector<FCookedBulkPayload> Payloads,
		FPackageByteBuilder BuildPackageBytes,
		std::string* OutError) -> bool
	{
		std::filesystem::path PackagePath;
		if (!ResolveCookedPackagePath(CookRoot, VirtualPackagePath, PackagePath, OutError)) return false;
		if (!BuildPackageBytes)
			return Fail("Cook package-byte builder must be nonempty.", OutError);
		if (std::ranges::any_of(Packages, [&](const FPendingPackage& Existing) {
			return Existing.VirtualPath == VirtualPackagePath;
		})) return Fail("Cook package path is duplicated.", OutError);

		std::vector<uint8> BulkBytes;
		std::vector<FCookedPayloadDescriptor> Descriptors;
		if (!Payloads.empty()
			&& !EncodeCookedBulk(
				Payloads, TargetPlatform, TargetProfile,
				BulkBytes, &Descriptors, OutError)) return false;
		std::vector<uint8> PackageBytes;
		if (!BuildPackageBytes(Descriptors, PackageBytes, OutError)) return false;
		if (PackageBytes.empty()) return Fail("Cook package-byte builder returned an empty package.", OutError);
		const FAssetResult Validation = ValidateAssetPackageBytes(PackageBytes);
		if (!Validation)
			return Fail(std::format("Cook package-byte builder returned invalid bytes: {}", Validation.Message), OutError);
		Packages.push_back({std::move(VirtualPackagePath), std::move(PackageBytes), std::move(BulkBytes)});
		return true;
	}

	auto FCookContext::Publish(std::string* OutError) -> bool
	{
		if (!IsValidTarget(TargetPlatform, TargetProfile) || CookRoot.empty() || !CookRoot.is_absolute())
			return Fail("Cook context is invalid.", OutError);
		for (FPendingPackage& Package : Packages)
		{
			if (!CanonicalizeCookVirtualPath(Package.VirtualPath, OutError))
				return false;
			std::vector<uint8> CanonicalBytes;
			const FAssetResult CanonicalResult = CanonicalizeAssetPackageForCook(
				Package.PackageBytes, CanonicalBytes);
			if (!CanonicalResult)
				return Fail(std::format(
					"Cook package {} could not be canonicalized: {}",
					Package.VirtualPath, CanonicalResult.Message), OutError);
			Package.PackageBytes = std::move(CanonicalBytes);
		}
		std::ranges::sort(Packages, {}, &FPendingPackage::VirtualPath);
		for (size_t Index = 1; Index < Packages.size(); ++Index)
			if (Packages[Index - 1].VirtualPath == Packages[Index].VirtualPath)
				return Fail(std::format(
					"Cook package path {} is duplicated after redirect canonicalization.",
					Packages[Index].VirtualPath), OutError);
		struct FOutput
		{
			ECookManifestEntryKind Kind;
			std::filesystem::path Destination;
			std::filesystem::path Temporary;
			std::vector<uint8> Bytes;
		};
		std::vector<FOutput> BulkOutputs;
		std::vector<FOutput> PackageOutputs;
		std::vector<std::filesystem::path> Temporaries;
		auto CleanupTemporaries = [&]() {
			std::error_code ErrorCode;
			for (const std::filesystem::path& Temporary : Temporaries) std::filesystem::remove(Temporary, ErrorCode);
		};

		for (const FPendingPackage& Package : Packages)
		{
			std::filesystem::path PackagePath, BulkPath;
			if (!ResolveCookedPackagePath(CookRoot, Package.VirtualPath, PackagePath, OutError)
				|| !ResolveCookedCompanionPath(CookRoot, PackagePath, BulkPath, OutError))
				return false;
			if (!Package.BulkBytes.empty())
			{
				BulkOutputs.push_back({
					ECookManifestEntryKind::CookedBulk,
					BulkPath,
					{},
					Package.BulkBytes});
			}
			PackageOutputs.push_back({ECookManifestEntryKind::CookedPackage, PackagePath, {}, Package.PackageBytes});
		}

		for (FOutput& Output : BulkOutputs)
		{
			auto Validate = [&](std::span<const uint8> Bytes, std::string* Error) {
				FCookedBulkContainer Container;
				return DecodeCookedBulk(Bytes, TargetPlatform, TargetProfile, Container, Error);
			};
			if (!WriteValidatedTemporary(Output.Destination, Output.Bytes, Validate, Output.Temporary, OutError))
			{
				CleanupTemporaries();
				return false;
			}
			Temporaries.push_back(Output.Temporary);
		}
		for (FOutput& Output : PackageOutputs)
		{
			auto Validate = [&](std::span<const uint8> Bytes, std::string* Error) {
				if (!std::ranges::equal(Bytes, Output.Bytes)) return Fail("Temporary cooked package bytes changed.", Error);
				const FAssetResult Result = ValidateAssetPackageBytes(Bytes);
				if (!Result) return Fail(std::format("Temporary cooked package is invalid: {}", Result.Message), Error);
				return true;
			};
			if (!WriteValidatedTemporary(Output.Destination, Output.Bytes, Validate, Output.Temporary, OutError))
			{
				CleanupTemporaries();
				return false;
			}
			Temporaries.push_back(Output.Temporary);
		}

		FCookManifest PreviousManifest;
		const std::filesystem::path ManifestPath = CookRoot / "CookManifest.bin";
		std::vector<uint8> PreviousBytes;
		const bool bHasPreviousManifest = FFileHelper::LoadFileToArray(PreviousBytes, ManifestPath.generic_string())
			&& DecodeCookManifest(PreviousBytes, PreviousManifest);

		for (FOutput& Output : BulkOutputs)
			if (!PublishTemporary(Output.Temporary, Output.Destination, OutError))
			{
				CleanupTemporaries();
				return false;
			}
		for (FOutput& Output : PackageOutputs)
			if (!PublishTemporary(Output.Temporary, Output.Destination, OutError))
			{
				CleanupTemporaries();
				return false;
			}

		FCookManifest Manifest{TargetPlatform, TargetProfile};
		auto AddManifestEntries = [&](const std::vector<FOutput>& Outputs) {
			for (const FOutput& Output : Outputs)
			{
				const std::string Relative = Output.Destination.lexically_relative(CookRoot).generic_string();
				const FXxHash128 Hash = FXxHash128::HashBuffer(Output.Bytes);
				Manifest.Entries.push_back({Output.Kind, 1, Relative, Output.Bytes.size(), Hash.HashLow, Hash.HashHigh});
			}
		};
		AddManifestEntries(PackageOutputs);
		AddManifestEntries(BulkOutputs);
		std::vector<uint8> ManifestBytes;
		if (!EncodeCookManifest(Manifest, ManifestBytes, OutError))
		{
			CleanupTemporaries();
			return false;
		}
		std::filesystem::path ManifestTemporary;
		auto ValidateManifest = [&](std::span<const uint8> Bytes, std::string* Error) {
			FCookManifest Decoded;
			return DecodeCookManifest(Bytes, Decoded, Error);
		};
		if (!WriteValidatedTemporary(ManifestPath, ManifestBytes, ValidateManifest, ManifestTemporary, OutError))
			return false;
		Temporaries.push_back(ManifestTemporary);
		if (!PublishTemporary(ManifestTemporary, ManifestPath, OutError))
		{
			CleanupTemporaries();
			return false;
		}

		if (bHasPreviousManifest)
		{
			std::unordered_set<std::string> CurrentPaths;
			for (const FCookManifestEntry& Entry : Manifest.Entries) CurrentPaths.insert(Entry.RelativePath);
			for (const FCookManifestEntry& Entry : PreviousManifest.Entries)
			{
				if (CurrentPaths.contains(Entry.RelativePath) || !IsValidRelativeManifestPath(Entry.RelativePath)) continue;
				const std::filesystem::path Stale = (CookRoot / Entry.RelativePath).lexically_normal();
				std::filesystem::path ResolvedStale;
				std::error_code ResolveError;
				if (!PathUtilities::TryResolveContainedPath(
					Stale, CookRoot.lexically_normal(), ResolvedStale, ResolveError)) continue;
				std::error_code ErrorCode;
				std::filesystem::remove(ResolvedStale, ErrorCode);
			}
		}
		if (OutError) OutError->clear();
		return true;
	}
}
