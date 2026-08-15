#include "CookedAsset.h"

#include "AssetSystemInternal.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"

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

		auto IsPowerOfTwo(uint32 Value) -> bool
		{
			return Value != 0 && (Value & (Value - 1)) == 0;
		}

		auto Align(uint64 Value, uint32 Alignment, uint64& OutValue) -> bool
		{
			const uint64 Mask = Alignment - 1;
			if (Value > std::numeric_limits<uint64>::max() - Mask) return false;
			OutValue = (Value + Mask) & ~Mask;
			return true;
		}

		struct FWriter
		{
			std::vector<uint8> Bytes;

			template<typename T>
			auto Write(T Value) -> void
			{
				static_assert(std::is_integral_v<T>);
				for (size_t Index = 0; Index < sizeof(T); ++Index)
					Bytes.push_back(static_cast<uint8>(static_cast<std::make_unsigned_t<T>>(Value) >> (Index * 8)));
			}

			auto WriteBytes(std::span<const uint8> Value) -> void
			{
				Bytes.insert(Bytes.end(), Value.begin(), Value.end());
			}

			auto PadTo(uint64 Offset) -> bool
			{
				if (Offset > std::numeric_limits<size_t>::max() || Offset < Bytes.size()) return false;
				Bytes.resize(static_cast<size_t>(Offset), 0);
				return true;
			}
		};

		struct FReader
		{
			std::span<const uint8> Bytes;
			size_t Offset = 0;

			template<typename T>
			auto Read(T& OutValue) -> bool
			{
				static_assert(std::is_integral_v<T>);
				if (sizeof(T) > Bytes.size() - std::min(Offset, Bytes.size())) return false;
				std::make_unsigned_t<T> Value = 0;
				for (size_t Index = 0; Index < sizeof(T); ++Index)
					Value |= static_cast<std::make_unsigned_t<T>>(Bytes[Offset++]) << (Index * 8);
				OutValue = static_cast<T>(Value);
				return true;
			}

			auto ReadBytes(size_t Size, std::span<const uint8>& OutValue) -> bool
			{
				if (Size > Bytes.size() - std::min(Offset, Bytes.size())) return false;
				OutValue = Bytes.subspan(Offset, Size);
				Offset += Size;
				return true;
			}
		};

		auto WriteGuid(FWriter& Writer, const FGuid& Guid) -> void
		{
			Writer.Write(Guid.A);
			Writer.Write(Guid.B);
			Writer.Write(Guid.C);
			Writer.Write(Guid.D);
		}

		auto ReadGuid(FReader& Reader, FGuid& Guid) -> bool
		{
			return Reader.Read(Guid.A) && Reader.Read(Guid.B) && Reader.Read(Guid.C) && Reader.Read(Guid.D);
		}

		auto IsLexicalChild(const std::filesystem::path& Root, const std::filesystem::path& Candidate) -> bool
		{
			const std::filesystem::path Relative = Candidate.lexically_relative(Root);
			return !Relative.empty() && !Relative.is_absolute() && *Relative.begin() != "..";
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
		if (!IsValidTarget(TargetPlatform, TargetProfile)) return Fail("DBLK target is invalid.", OutError);
		if (Payloads.empty() || Payloads.size() > MaximumPayloadCount) return Fail("DBLK payload count is outside its bound.", OutError);

		std::vector<FCookedBulkPayload> Sorted(Payloads.begin(), Payloads.end());
		std::ranges::sort(Sorted, {}, &FCookedBulkPayload::PayloadId);
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			const FCookedBulkPayload& Payload = Sorted[Index];
			if (!Payload.PayloadId.IsValid() || (Index && Sorted[Index - 1].PayloadId == Payload.PayloadId))
				return Fail("DBLK payload identifiers must be nonzero and unique.", OutError);
			if ((Payload.Flags & ~1u) != 0 || Payload.PayloadSchemaVersion == 0)
				return Fail("DBLK payload flags or schema are invalid.", OutError);
			if (!IsPowerOfTwo(Payload.Alignment) || Payload.Alignment < 16 || Payload.Alignment > 4096)
				return Fail("DBLK payload alignment is invalid.", OutError);
			if (Payload.Compression != ECookedPayloadCompression::None)
				return Fail("DBLK writer supports only uncompressed version 1 payloads.", OutError);
			if (Payload.Bytes.empty() || Payload.Bytes.size() > MaximumPayloadBytes)
				return Fail("DBLK payload size is outside its bound.", OutError);
		}

		uint64 DataOffset = 0;
		if (!Align(BulkHeaderSize + static_cast<uint64>(Sorted.size()) * BulkEntrySize, 16, DataOffset))
			return Fail("DBLK table size overflowed.", OutError);
		std::vector<FCookedPayloadDescriptor> Descriptors;
		Descriptors.reserve(Sorted.size());
		for (const FCookedBulkPayload& Payload : Sorted)
		{
			if (!Align(DataOffset, Payload.Alignment, DataOffset)) return Fail("DBLK payload offset overflowed.", OutError);
			if (Payload.Bytes.size() > MaximumBulkBytes - DataOffset) return Fail("DBLK container exceeds its size bound.", OutError);
			const FXxHash128 Hash = FXxHash128::HashBuffer(Payload.Bytes);
			Descriptors.push_back({
				.PayloadId = Payload.PayloadId,
				.LocationKind = static_cast<uint32>(ECookedPayloadLocationKind::PackageCompanion),
				.Offset = DataOffset,
				.StoredSize = Payload.Bytes.size(),
				.UncompressedSize = Payload.Bytes.size(),
				.Alignment = Payload.Alignment,
				.PayloadHashLow = Hash.HashLow,
				.PayloadHashHigh = Hash.HashHigh,
				.PayloadSchemaVersion = Payload.PayloadSchemaVersion,
				.TargetPlatform = static_cast<uint32>(TargetPlatform),
				.TargetProfile = static_cast<uint32>(TargetProfile),
				.CompressionMethod = static_cast<uint32>(Payload.Compression)});
			DataOffset += Payload.Bytes.size();
		}
		if (DataOffset > MaximumBulkBytes || DataOffset > std::numeric_limits<size_t>::max())
			return Fail("DBLK container exceeds its size bound.", OutError);

		FWriter Table;
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			const FCookedBulkPayload& Payload = Sorted[Index];
			const FCookedPayloadDescriptor& Descriptor = Descriptors[Index];
			WriteGuid(Table, Payload.PayloadId);
			Table.Write(Payload.Flags);
			Table.Write(Payload.PayloadSchemaVersion);
			Table.Write(static_cast<uint32>(TargetPlatform));
			Table.Write(static_cast<uint32>(TargetProfile));
			Table.Write(static_cast<uint32>(Payload.Compression));
			Table.Write(Payload.Alignment);
			Table.Write(Descriptor.Offset);
			Table.Write(Descriptor.StoredSize);
			Table.Write(Descriptor.UncompressedSize);
			Table.Write(Descriptor.PayloadHashLow);
			Table.Write(Descriptor.PayloadHashHigh);
		}
		const uint64 TableHash = FXxHash64::HashBuffer(Table.Bytes).HashValue;
		FWriter Writer;
		Writer.Write(BulkMagic);
		Writer.Write(BulkVersion);
		Writer.Write(static_cast<uint32>(TargetPlatform));
		Writer.Write(static_cast<uint32>(TargetProfile));
		Writer.Write(uint32{0});
		Writer.Write(BulkHeaderSize);
		Writer.Write(static_cast<uint32>(Sorted.size()));
		Writer.Write(BulkEntrySize);
		Writer.Write(uint64{BulkHeaderSize});
		Writer.Write(DataOffset);
		Writer.Write(TableHash);
		Writer.Write(uint64{0});
		Writer.WriteBytes(Table.Bytes);
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			if (!Writer.PadTo(Descriptors[Index].Offset)) return Fail("DBLK payload layout overflowed.", OutError);
			Writer.WriteBytes(Sorted[Index].Bytes);
		}
		OutBytes = std::move(Writer.Bytes);
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
		if (Bytes.size() < BulkHeaderSize || Bytes.size() > MaximumBulkBytes) return Fail("DBLK file size is invalid.", OutError);
		FReader Reader{Bytes};
		uint32 Magic = 0, Version = 0, Platform = 0, Profile = 0, Flags = 0, HeaderSize = 0;
		uint32 Count = 0, EntrySize = 0;
		uint64 TableOffset = 0, FileSize = 0, TableHash = 0, Reserved = 0;
		if (!Reader.Read(Magic) || !Reader.Read(Version) || !Reader.Read(Platform) || !Reader.Read(Profile)
			|| !Reader.Read(Flags) || !Reader.Read(HeaderSize) || !Reader.Read(Count) || !Reader.Read(EntrySize)
			|| !Reader.Read(TableOffset) || !Reader.Read(FileSize) || !Reader.Read(TableHash) || !Reader.Read(Reserved))
			return Fail("DBLK header is truncated.", OutError);
		if (Magic != BulkMagic || Version != BulkVersion || Flags != 0 || HeaderSize != BulkHeaderSize
			|| Count == 0 || Count > MaximumPayloadCount || EntrySize != BulkEntrySize || TableOffset != BulkHeaderSize
			|| FileSize != Bytes.size() || Reserved != 0)
			return Fail("DBLK header is invalid.", OutError);
		if (Platform != static_cast<uint32>(ExpectedPlatform) || Profile != static_cast<uint32>(ExpectedProfile)
			|| !IsValidTarget(static_cast<ECookTargetPlatform>(Platform), static_cast<ECookTargetProfile>(Profile)))
			return Fail("DBLK target does not match the load context.", OutError);
		const uint64 TableBytes = static_cast<uint64>(Count) * BulkEntrySize;
		if (TableBytes > Bytes.size() - BulkHeaderSize) return Fail("DBLK table is truncated.", OutError);
		const std::span<const uint8> Table = Bytes.subspan(BulkHeaderSize, static_cast<size_t>(TableBytes));
		if (FXxHash64::HashBuffer(Table).HashValue != TableHash) return Fail("DBLK table checksum is invalid.", OutError);

		FReader TableReader{Table};
		std::vector<FCookedPayloadDescriptor> Entries;
		std::vector<uint32> EntryFlags;
		Entries.reserve(Count);
		uint64 PreviousEnd = 0;
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			FCookedPayloadDescriptor Entry;
			uint32 PayloadFlags = 0;
			if (!ReadGuid(TableReader, Entry.PayloadId) || !TableReader.Read(PayloadFlags)
				|| !TableReader.Read(Entry.PayloadSchemaVersion) || !TableReader.Read(Entry.TargetPlatform)
				|| !TableReader.Read(Entry.TargetProfile) || !TableReader.Read(Entry.CompressionMethod)
				|| !TableReader.Read(Entry.Alignment) || !TableReader.Read(Entry.Offset)
				|| !TableReader.Read(Entry.StoredSize) || !TableReader.Read(Entry.UncompressedSize)
				|| !TableReader.Read(Entry.PayloadHashLow) || !TableReader.Read(Entry.PayloadHashHigh))
				return Fail("DBLK table entry is truncated.", OutError);
			Entry.LocationKind = static_cast<uint32>(ECookedPayloadLocationKind::PackageCompanion);
			if (!Entry.PayloadId.IsValid() || (Index && !(Entries.back().PayloadId < Entry.PayloadId))
				|| (PayloadFlags & ~1u) != 0 || Entry.PayloadSchemaVersion == 0
				|| Entry.TargetPlatform != Platform || Entry.TargetProfile != Profile
				|| !IsPowerOfTwo(Entry.Alignment) || Entry.Alignment < 16 || Entry.Alignment > 4096
				|| Entry.StoredSize == 0 || Entry.StoredSize > MaximumPayloadBytes
				|| Entry.UncompressedSize == 0 || Entry.UncompressedSize > MaximumPayloadBytes)
				return Fail("DBLK table entry is invalid.", OutError);
			if (Entry.CompressionMethod == static_cast<uint32>(ECookedPayloadCompression::None))
			{
				if (Entry.StoredSize != Entry.UncompressedSize) return Fail("DBLK uncompressed sizes differ.", OutError);
			}
			else if (Entry.CompressionMethod == static_cast<uint32>(ECookedPayloadCompression::Zstandard))
			{
				if (Entry.UncompressedSize / Entry.StoredSize > 64
					|| (Entry.UncompressedSize % Entry.StoredSize != 0 && Entry.UncompressedSize / Entry.StoredSize == 64))
					return Fail("DBLK compression ratio exceeds its bound.", OutError);
				return Fail("DBLK Zstandard compression is unsupported by this build.", OutError);
			}
			else return Fail("DBLK compression method is unknown.", OutError);
			uint64 MinimumDataOffset = 0;
			if (!Align(BulkHeaderSize + TableBytes, 16, MinimumDataOffset)
				|| Entry.Offset < MinimumDataOffset || Entry.Offset % Entry.Alignment != 0
				|| Entry.Offset < PreviousEnd || Entry.StoredSize > FileSize - std::min(Entry.Offset, FileSize))
				return Fail("DBLK payload range is invalid.", OutError);
			for (uint64 Byte = PreviousEnd ? PreviousEnd : MinimumDataOffset; Byte < Entry.Offset; ++Byte)
				if (Bytes[static_cast<size_t>(Byte)] != 0) return Fail("DBLK alignment padding is nonzero.", OutError);
			PreviousEnd = Entry.Offset + Entry.StoredSize;
			Entries.push_back(Entry);
			EntryFlags.push_back(PayloadFlags);
		}
		for (uint64 Byte = PreviousEnd; Byte < FileSize; ++Byte)
			if (Bytes[static_cast<size_t>(Byte)] != 0) return Fail("DBLK trailing padding is nonzero.", OutError);

		std::vector<std::vector<uint8>> Payloads;
		Payloads.reserve(Entries.size());
		for (const FCookedPayloadDescriptor& Entry : Entries)
		{
			std::span<const uint8> Stored = Bytes.subspan(static_cast<size_t>(Entry.Offset), static_cast<size_t>(Entry.StoredSize));
			const FXxHash128 Hash = FXxHash128::HashBuffer(Stored);
			if (Hash.HashLow != Entry.PayloadHashLow || Hash.HashHigh != Entry.PayloadHashHigh)
				return Fail("DBLK payload checksum is invalid.", OutError);
			Payloads.emplace_back(Stored.begin(), Stored.end());
		}
		OutContainer.TargetPlatform = static_cast<ECookTargetPlatform>(Platform);
		OutContainer.TargetProfile = static_cast<ECookTargetProfile>(Profile);
		OutContainer.Entries = std::move(Entries);
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
		if (!IsLexicalChild(Root, Candidate)) return Fail("Cooked package path escapes the cook root.", OutError);
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
			|| !IsLexicalChild(Root, Normalized))
			return Fail("Cooked companion package path is invalid or outside the cook root.", OutError);
		OutCompanionPath = Normalized;
		OutCompanionPath.replace_extension(".dbulk");
		if (!IsLexicalChild(Root, OutCompanionPath)) return Fail("Cooked companion path escapes the cook root.", OutError);
		return true;
	}

	auto EncodeCookManifest(const FCookManifest& Manifest, std::vector<uint8>& OutBytes, std::string* OutError) -> bool
	{
		OutBytes.clear();
		if (!IsValidTarget(Manifest.TargetPlatform, Manifest.TargetProfile))
			return Fail("Cook manifest target is invalid.", OutError);
		if (Manifest.Entries.size() > MaximumManifestEntries)
			return Fail("Cook manifest entry count exceeds its bound.", OutError);
		std::vector<FCookManifestEntry> Entries = Manifest.Entries;
		std::ranges::sort(Entries, {}, &FCookManifestEntry::RelativePath);
		FWriter Records;
		for (size_t Index = 0; Index < Entries.size(); ++Index)
		{
			const FCookManifestEntry& Entry = Entries[Index];
			if (!IsValidRelativeManifestPath(Entry.RelativePath)
				|| (Index && Entries[Index - 1].RelativePath == Entry.RelativePath)
				|| (Entry.Kind != ECookManifestEntryKind::CookedPackage && Entry.Kind != ECookManifestEntryKind::CookedBulk)
				|| Entry.Flags != 1 || Entry.FileSize == 0)
				return Fail("Cook manifest entry is invalid.", OutError);
			Records.Write(static_cast<uint8>(Entry.Kind));
			Records.Write(Entry.Flags);
			Records.Write(uint16{0});
			Records.Write(static_cast<uint32>(Entry.RelativePath.size()));
			Records.Write(Entry.FileSize);
			Records.Write(Entry.HashLow);
			Records.Write(Entry.HashHigh);
			Records.WriteBytes(std::span{reinterpret_cast<const uint8*>(Entry.RelativePath.data()), Entry.RelativePath.size()});
			if (Records.Bytes.size() > MaximumManifestRecordBytes)
				return Fail("Cook manifest records exceed their byte bound.", OutError);
		}
		FWriter Writer;
		Writer.Write(ManifestMagic);
		Writer.Write(ManifestVersion);
		Writer.Write(static_cast<uint32>(Manifest.TargetPlatform));
		Writer.Write(static_cast<uint32>(Manifest.TargetProfile));
		Writer.Write(static_cast<uint32>(Entries.size()));
		Writer.Write(ManifestHeaderSize);
		Writer.Write(static_cast<uint64>(Records.Bytes.size()));
		Writer.Write(FXxHash64::HashBuffer(Records.Bytes).HashValue);
		Writer.Write(static_cast<uint64>(ManifestHeaderSize + Records.Bytes.size()));
		Writer.WriteBytes(Records.Bytes);
		OutBytes = std::move(Writer.Bytes);
		return true;
	}

	auto DecodeCookManifest(std::span<const uint8> Bytes, FCookManifest& OutManifest, std::string* OutError) -> bool
	{
		OutManifest = {};
		if (Bytes.size() < ManifestHeaderSize) return Fail("Cook manifest is truncated.", OutError);
		FReader Reader{Bytes};
		uint32 Magic = 0, Version = 0, Platform = 0, Profile = 0, Count = 0, HeaderSize = 0;
		uint64 RecordBytes = 0, RecordHash = 0, FileSize = 0;
		if (!Reader.Read(Magic) || !Reader.Read(Version) || !Reader.Read(Platform) || !Reader.Read(Profile)
			|| !Reader.Read(Count) || !Reader.Read(HeaderSize) || !Reader.Read(RecordBytes)
			|| !Reader.Read(RecordHash) || !Reader.Read(FileSize))
			return Fail("Cook manifest header is truncated.", OutError);
		if (Magic != ManifestMagic || Version != ManifestVersion || HeaderSize != ManifestHeaderSize
			|| Count > MaximumManifestEntries || RecordBytes > MaximumManifestRecordBytes
			|| FileSize != Bytes.size() || RecordBytes != Bytes.size() - ManifestHeaderSize
			|| !IsValidTarget(static_cast<ECookTargetPlatform>(Platform), static_cast<ECookTargetProfile>(Profile)))
			return Fail("Cook manifest header is invalid.", OutError);
		const std::span<const uint8> Records = Bytes.subspan(ManifestHeaderSize);
		if (FXxHash64::HashBuffer(Records).HashValue != RecordHash)
			return Fail("Cook manifest record checksum is invalid.", OutError);
		FReader RecordReader{Records};
		std::vector<FCookManifestEntry> Entries;
		Entries.reserve(Count);
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			uint8 Kind = 0, Flags = 0;
			uint16 Reserved = 0;
			uint32 PathBytes = 0;
			FCookManifestEntry Entry;
			if (!RecordReader.Read(Kind) || !RecordReader.Read(Flags) || !RecordReader.Read(Reserved)
				|| !RecordReader.Read(PathBytes) || !RecordReader.Read(Entry.FileSize)
				|| !RecordReader.Read(Entry.HashLow) || !RecordReader.Read(Entry.HashHigh)
				|| Reserved != 0 || PathBytes == 0 || PathBytes > 1024)
				return Fail("Cook manifest record is invalid.", OutError);
			std::span<const uint8> Path;
			if (!RecordReader.ReadBytes(PathBytes, Path)) return Fail("Cook manifest path is truncated.", OutError);
			Entry.Kind = static_cast<ECookManifestEntryKind>(Kind);
			Entry.Flags = Flags;
			Entry.RelativePath.assign(reinterpret_cast<const char*>(Path.data()), Path.size());
			if (!IsValidRelativeManifestPath(Entry.RelativePath)
				|| (Index && !(Entries.back().RelativePath < Entry.RelativePath))
				|| (Entry.Kind != ECookManifestEntryKind::CookedPackage && Entry.Kind != ECookManifestEntryKind::CookedBulk)
				|| Entry.Flags != 1 || Entry.FileSize == 0)
				return Fail("Cook manifest entry is invalid.", OutError);
			Entries.push_back(std::move(Entry));
		}
		if (RecordReader.Offset != Records.size()) return Fail("Cook manifest has trailing record bytes.", OutError);
		OutManifest.TargetPlatform = static_cast<ECookTargetPlatform>(Platform);
		OutManifest.TargetProfile = static_cast<ECookTargetProfile>(Profile);
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
				if (!IsLexicalChild(CookRoot.lexically_normal(), Stale)) continue;
				std::error_code ErrorCode;
				std::filesystem::remove(Stale, ErrorCode);
			}
		}
		if (OutError) OutError->clear();
		return true;
	}
}
