#include "Asset/Cook.h"
#include "Asset/PackageSerialization.h"

#include "AssetPublicationCoordinatorInternal.h"
#include "BulkContainerInfrastructure.h"
#include "DObject/Package.h"
#include "Hash/XxHash.h"
#include "Misc/Paths.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 ManifestMagic = 0x464e4d43;
		constexpr uint32 ManifestVersion = 1;
		constexpr uint32 ManifestHeaderSize = 48;
		constexpr uint64 MaximumManifestRecordBytes = 256ull * 1024 * 1024;
		constexpr uint32 MaximumManifestEntries = 1'000'000;

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

		auto ReadManifestHeader(
			BulkContainer::FBoundedReader& Reader,
			FManifestHeader& OutHeader
		) -> bool
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
			const FManifestHeader& Header
		) -> bool
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
			FManifestRecordHeader& OutHeader
		) -> bool
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
			const FCookManifestEntry& Entry
		) -> bool
		{
			Writer.Write(static_cast<uint8>(Entry.Kind));
			Writer.Write(Entry.Flags);
			Writer.Write(uint16{0});
			Writer.Write(static_cast<uint32>(Entry.RelativePath.size()));
			Writer.Write(Entry.FileSize);
			Writer.Write(Entry.HashLow);
			Writer.Write(Entry.HashHigh);
			Writer.Write(std::as_bytes(std::span(Entry.RelativePath)));
			return Writer.IsValid();
		}

		auto CanonicalizeCookVirtualPath(
			std::string& VirtualPackagePath,
			std::string* OutError
		) -> bool
		{
			FPackagePath RequestedPath;
			if (!FPackagePath::TryCreate(
					VirtualPackagePath, RequestedPath
				))
				return true;
			const FAssetPublicationCoordinator& Registry = GetAssetPublicationCoordinator();
			if (!Durin::FindAssetExact(RequestedPath)) return true;
			const FAssetPathResolveResult Resolution =
				Durin::ResolveAssetPath(RequestedPath);
			if (!Resolution || !Resolution.FinalAssetData
				|| Resolution.FinalAssetData->EntryKind
					   != EAssetRegistryEntryKind::Asset)
				return Fail(std::format("Cook output path {} does not resolve to a final real asset.", RequestedPath.ToString()), OutError);
			VirtualPackagePath = Resolution.FinalPath.ToString();
			return true;
		}

		auto IsValidTarget(ECookTargetPlatform Platform, ECookTargetProfile Profile) -> bool
		{
			return Platform == ECookTargetPlatform::Win64
				   && (Profile == ECookTargetProfile::Game || Profile == ECookTargetProfile::EditorValidation);
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
				if ((Lead & 0xe0) == 0xc0)
				{
					CodePoint = Lead & 0x1f;
					Continuations = 1;
				}
				else if ((Lead & 0xf0) == 0xe0)
				{
					CodePoint = Lead & 0x0f;
					Continuations = 2;
				}
				else if ((Lead & 0xf8) == 0xf0)
				{
					CodePoint = Lead & 0x07;
					Continuations = 3;
				}
				else
					return false;
				if (Byte + Continuations > Value.size()) return false;
				for (size_t Index = 0; Index < Continuations; ++Index)
				{
					const uint8 Tail = static_cast<uint8>(Value[Byte++]);
					if ((Tail & 0xc0) != 0x80) return false;
					CodePoint = (CodePoint << 6) | (Tail & 0x3f);
				}
				const uint32 Minimum = Continuations == 1 ? 0x80 : Continuations == 2 ? 0x800 :
																						0x10000;
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

	} // namespace

	auto FAssetRuntimeConfiguration::Authored() -> FAssetRuntimeConfiguration
	{
		return {};
	}

	auto FAssetRuntimeConfiguration::Cooked(
		std::filesystem::path InCookRoot,
		FAssetRuntimeConfiguration& OutConfiguration
	) -> FAssetResult
	{
		if (InCookRoot.empty() || !InCookRoot.is_absolute()
			|| InCookRoot.lexically_normal() != InCookRoot)
		{
			return {
				.Error = EAssetError::InvalidPath,
				.Message = "Cooked asset execution requires an absolute normalized cook root."
			};
		}
		FAssetRuntimeConfiguration Result;
		Result.ExecutionDomain = EAssetExecutionDomain::Cooked;
		Result.PayloadPolicy = EAssetPayloadPolicy::CookedPayloadRequired;
		Result.CookRoot = std::move(InCookRoot);
		OutConfiguration = std::move(Result);
		return {};
	}

	auto ResolveCookedPackagePath(
		const std::filesystem::path& CookRoot,
		std::string_view VirtualPackagePath,
		std::filesystem::path& OutPackagePath,
		std::string* OutError
	) -> bool
	{
		OutPackagePath.clear();
		if (CookRoot.empty() || !CookRoot.is_absolute() || VirtualPackagePath.empty()
			|| VirtualPackagePath.front() != '/' || VirtualPackagePath.back() == '/'
			|| VirtualPackagePath.find('\\') != std::string_view::npos)
			return Fail("Cooked package path or root is invalid.", OutError);
		const size_t Slash = VirtualPackagePath.find('/', 1);
		const std::string_view Mount = Slash == std::string_view::npos ? VirtualPackagePath.substr(1) : VirtualPackagePath.substr(1, Slash - 1);
		if (Mount.empty() || Slash == std::string_view::npos)
			return Fail("Cooked package mount is invalid.", OutError);
		const std::string Relative(VirtualPackagePath.substr(1));
		if (!IsValidRelativeManifestPath(Relative)) return Fail("Cooked package path is not normalized.", OutError);
		const std::filesystem::path Root = CookRoot.lexically_normal();
		std::filesystem::path Candidate = (Root / std::filesystem::path(Relative)).lexically_normal();
		Candidate += ".dasset";
		if (!FPaths::IsLexicalDescendantPath(Candidate, Root, true))
			return Fail("Cooked package path escapes the cook root.", OutError);
		OutPackagePath = std::move(Candidate);
		return true;
	}

	auto ResolveCookedCompanionPath(
		const std::filesystem::path& CookRoot,
		const std::filesystem::path& PackagePath,
		std::filesystem::path& OutCompanionPath,
		std::string* OutError
	) -> bool
	{
		OutCompanionPath.clear();
		const std::filesystem::path Root = CookRoot.lexically_normal();
		const std::filesystem::path Normalized = PackagePath.lexically_normal();
		if (Root.empty() || !Root.is_absolute() || PackagePath.extension() != ".dasset"
			|| !FPaths::IsLexicalDescendantPath(Normalized, Root, true))
			return Fail("Cooked companion package path is invalid or outside the cook root.", OutError);
		OutCompanionPath = Normalized;
		OutCompanionPath.replace_extension(".dbulk");
		if (!FPaths::IsLexicalDescendantPath(OutCompanionPath, Root, true))
			return Fail("Cooked companion path escapes the cook root.", OutError);
		return true;
	}

	auto EncodeCookManifest(const FCookManifest& Manifest, FByteArray& OutBytes, std::string* OutError) -> bool
	{
		OutBytes.clear();
		if (!IsValidTarget(Manifest.TargetPlatform, Manifest.TargetProfile))
			return Fail("Cook manifest target is invalid.", OutError);
		if (Manifest.Entries.size() > MaximumManifestEntries)
			return Fail("Cook manifest entry count exceeds its bound.", OutError);
		std::vector<const FCookManifestEntry*> Entries;
		if (!BulkContainer::TryMakeSortedProjection<FCookManifestEntry>(
				Manifest.Entries, &FCookManifestEntry::RelativePath, Entries
			))
			return Fail("Cook manifest entry is invalid.", OutError);
		BulkContainer::FBoundedWriter Records(MaximumManifestRecordBytes);
		for (const FCookManifestEntry* EntryPointer : Entries)
		{
			const FCookManifestEntry& Entry = *EntryPointer;
			if (!IsValidRelativeManifestPath(Entry.RelativePath)
				|| (Entry.Kind != ECookManifestEntryKind::CookedPackage
					&& Entry.Kind != ECookManifestEntryKind::CookedBulk
					&& Entry.Kind != ECookManifestEntryKind::PackageBulk
					&& Entry.Kind != ECookManifestEntryKind::ShaderLibrary)
				|| (Entry.Flags & CookManifestEntryPresent) == 0
				|| (Entry.Flags & ~CookManifestEntryKnownFlags) != 0
				|| (Entry.Kind != ECookManifestEntryKind::CookedPackage
					&& (Entry.Flags & CookManifestEntryCookedFieldProjection) != 0)
				|| Entry.FileSize == 0)
				return Fail("Cook manifest entry is invalid.", OutError);
			if (!WriteManifestRecord(Records, Entry))
				return Fail("Cook manifest records exceed their byte bound.", OutError);
		}
		uint64 MaximumManifestBytes = 0;
		if (!BulkContainer::TryAdd(ManifestHeaderSize, MaximumManifestRecordBytes, std::numeric_limits<uint64>::max(), MaximumManifestBytes))
			return Fail("Cook manifest records exceed their byte bound.", OutError);
		BulkContainer::FBoundedWriter Writer(MaximumManifestBytes);
		const uint64 RecordBytes = Records.Tell();
		uint64 FileSize = 0;
		FByteArray Candidate;
		if (!BulkContainer::TryAdd(
				ManifestHeaderSize, RecordBytes, MaximumManifestBytes, FileSize
			))
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
			.FileSize = FileSize
		};
		if (!WriteManifestHeader(Writer, Header)
			|| !Writer.Write(Records.View()) || !Writer.TryTake(Candidate))
			return Fail("Cook manifest encoding failed.", OutError);
		FCookManifest Validation;
		if (!DecodeCookManifest(Candidate, Validation, OutError)) return false;
		OutBytes = std::move(Candidate);
		return true;
	}

	auto DecodeCookManifest(std::span<const std::byte> Bytes, FCookManifest& OutManifest, std::string* OutError) -> bool
	{
		OutManifest = {};
		if (Bytes.size() < ManifestHeaderSize) return Fail("Cook manifest is truncated.", OutError);
		BulkContainer::FBoundedReader Reader(
			Bytes, ManifestHeaderSize + MaximumManifestRecordBytes
		);
		FManifestHeader Header;
		if (!ReadManifestHeader(Reader, Header))
			return Fail("Cook manifest header is truncated.", OutError);
		if (Header.Magic != ManifestMagic || Header.Version != ManifestVersion
			|| Header.HeaderSize != ManifestHeaderSize
			|| Header.Count > MaximumManifestEntries
			|| Header.RecordBytes > MaximumManifestRecordBytes
			|| Header.FileSize != Bytes.size()
			|| Header.RecordBytes != Bytes.size() - ManifestHeaderSize
			|| !IsValidTarget(static_cast<ECookTargetPlatform>(Header.Platform), static_cast<ECookTargetProfile>(Header.Profile)))
			return Fail("Cook manifest header is invalid.", OutError);
		const std::span<const std::byte> Records = Bytes.subspan(ManifestHeaderSize);
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
			std::span<const std::byte> Path;
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
				|| (Entry.Kind != ECookManifestEntryKind::CookedPackage
					&& Entry.Kind != ECookManifestEntryKind::CookedBulk
					&& Entry.Kind != ECookManifestEntryKind::PackageBulk
					&& Entry.Kind != ECookManifestEntryKind::ShaderLibrary)
				|| (Entry.Flags & CookManifestEntryPresent) == 0
				|| (Entry.Flags & ~CookManifestEntryKnownFlags) != 0
				|| (Entry.Kind != ECookManifestEntryKind::CookedPackage
					&& (Entry.Flags & CookManifestEntryCookedFieldProjection) != 0)
				|| Entry.FileSize == 0)
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
		ECookTargetProfile InTargetProfile,
		bool bInRetainEditorOnlyData
	)
		: CookRoot(InCookRoot.lexically_normal())
		, TargetPlatform(InTargetPlatform)
		, TargetProfile(InTargetProfile)
		, bRetainEditorOnlyData(bInRetainEditorOnlyData)
	{
	}

	namespace
	{
		auto ValidateCookCapturePath(const std::filesystem::path& CookRoot, std::string_view VirtualPackagePath, std::string* OutError) -> bool
		{
			if (!CookRoot.empty())
			{
				std::filesystem::path Ignored;
				return ResolveCookedPackagePath(
					CookRoot, VirtualPackagePath, Ignored, OutError
				);
			}
			if (VirtualPackagePath.empty()
				|| VirtualPackagePath.front() != '/'
				|| VirtualPackagePath.back() == '/'
				|| VirtualPackagePath.find("//") != std::string_view::npos
				|| VirtualPackagePath.find("\\") != std::string_view::npos
				|| VirtualPackagePath.find("/../") != std::string_view::npos
				|| VirtualPackagePath.find('/', 1) == std::string_view::npos)
				return Fail("Cook package path is invalid or uses an unsupported mount.", OutError);
			return true;
		}
	} // namespace

	auto FCookContext::MakePackageSerializationOptions() const
		-> FAssetPackageSerializationOptions
	{
		return {
			.Domain = EAssetPackageSaveDomain::Cooked,
			.TargetPlatform = TargetPlatform,
			.TargetProfile = TargetProfile,
			.bRetainEditorOnlyData = bRetainEditorOnlyData
		};
	}

	auto FCookContext::AddPackage(
		std::string VirtualPackagePath,
		FByteArray PackageBytes,
		std::string* OutError
	) -> bool
	{
		FPackagePath SourcePackagePath;
		if (!FPackagePath::TryCreate(VirtualPackagePath, SourcePackagePath)
			&& !FPackagePath::TryCreateProjectContent(
				VirtualPackagePath, SourcePackagePath))
			return Fail("Cook package path is not a canonical asset identity.", OutError);
		return AddPackage(std::move(VirtualPackagePath), SourcePackagePath,
			std::move(PackageBytes), OutError);
	}

	auto FCookContext::AddPackage(
		std::string VirtualPackagePath,
		const FPackagePath& SourcePackagePath,
		FByteArray PackageBytes,
		std::string* OutError
	) -> bool
	{
		if (!ValidateCookCapturePath(CookRoot, VirtualPackagePath, OutError))
			return false;
		if (!SourcePackagePath.IsValid())
			return Fail("Cook source package identity is invalid.", OutError);
		if (PackageBytes.empty()) return Fail("Cook package bytes must be nonempty.", OutError);
		if (std::ranges::any_of(Packages, [&](const FCookSavePlan& Existing) {
				return Existing.VirtualPath == VirtualPackagePath;
			})) return Fail("Cook package path is duplicated.", OutError);
		Packages.push_back({
			.VirtualPath = std::move(VirtualPackagePath),
			.SourcePackagePath = SourcePackagePath,
			.PackageBytes = std::move(PackageBytes)});
		if (OutError) OutError->clear();
		return true;
	}

	auto FCookContext::AddPackage(
		std::string VirtualPackagePath,
		DPackage* Package,
		std::string* OutError
	) -> bool
	{
		if (!ValidateCookCapturePath(CookRoot, VirtualPackagePath, OutError)) return false;
		if (!Package || !Package->IsAssetPackage()
			|| Package->GetTopLevelAssets().empty())
			return Fail("Cook package projection requires a valid asset package.", OutError);
		FPackagePath SourcePackagePath;
		if (!FPackagePath::TryCreate(Package->GetPackagePath(), SourcePackagePath))
			return Fail("Cook source package identity is invalid.", OutError);
		if (std::ranges::any_of(Packages, [&](const FCookSavePlan& Existing) {
				return Existing.VirtualPath == VirtualPackagePath;
			})) return Fail("Cook package path is duplicated.", OutError);

		FAssetPackageSerializationOptions Options = MakePackageSerializationOptions();
		FByteArray PackageBytes;
		FByteArray Segment;
		const FAssetResult Result = SerializeAssetPackageClosure(
			Package, PackageBytes, Segment, Options);
		if (!Result)
			return Fail(std::format("Cook package projection failed: {}", Result.Message), OutError);
		FPackageBulkSegmentSummary Summary{
			.Extent = Segment.size(),
			.Digest = Segment.empty() ? FXxHash128{} : FXxHash128::HashBuffer(Segment)};
		FCookSavePlan Pending{
			.VirtualPath = std::move(VirtualPackagePath),
			.SourcePackagePath = std::move(SourcePackagePath),
			.PackageBytes = std::move(PackageBytes),
			.BulkBytes = std::move(Segment),
			.BulkSummary = Summary,
			.bRawBulkSegment = true
		};
		Packages.push_back(std::move(Pending));
		if (OutError) OutError->clear();
		return true;
	}

	auto FCookContext::AddRawPackage(
		std::string VirtualPackagePath,
		FByteArray PackageBytes,
		FByteArray RawSegmentBytes,
		std::string* OutError
	) -> bool
	{
		if (!ValidateCookCapturePath(CookRoot, VirtualPackagePath, OutError))
			return false;
		if (PackageBytes.empty() || RawSegmentBytes.empty())
			return Fail("Opaque raw Cook packages require package and segment bytes.", OutError);
		if (RawSegmentBytes.size() > PackageBulkDataMaximumSegmentBytes)
			return Fail("Opaque raw Cook segment exceeds the 1 GiB limit.", OutError);
		if (std::ranges::any_of(Packages, [&](const FCookSavePlan& Existing) {
				return Existing.VirtualPath == VirtualPackagePath;
			})) return Fail("Cook package path is duplicated.", OutError);
		FPackageBulkSegmentSummary Summary{
			.Extent = RawSegmentBytes.size(),
			.Digest = FXxHash128::HashBuffer(RawSegmentBytes)
		};
		Packages.push_back({.VirtualPath = std::move(VirtualPackagePath), .PackageBytes = std::move(PackageBytes), .BulkBytes = std::move(RawSegmentBytes), .BulkSummary = Summary, .bOpaqueRawSegment = true});
		if (OutError) OutError->clear();
		return true;
	}

	auto FCookContext::TakeSavePlans(
		std::vector<FCookSavePlan>& OutPlans,
		std::string* OutError
	) -> bool
	{
		OutPlans = Packages;
		if (!IsValidTarget(TargetPlatform, TargetProfile))
			return Fail("Cook capture target is invalid.", OutError);
		for (FCookSavePlan& Plan : OutPlans)
		{
			if (!Plan.bOpaqueRawSegment)
			{
				FByteArray CanonicalBytes;
				FByteArray CanonicalBulkBytes;
				FPackagePath PackagePath;
				if (!FPackagePath::TryCreate(Plan.VirtualPath, PackagePath)
					&& !FPackagePath::TryCreateProjectContent(
						Plan.VirtualPath, PackagePath))
					return Fail("Cook package path is not a canonical asset identity.", OutError);
				const FAssetResult CanonicalResult = CanonicalizeAssetPackageForCook(
					Plan.PackageBytes, Plan.BulkBytes,
					Plan.SourcePackagePath.IsValid()
						? Plan.SourcePackagePath : PackagePath,
					PackagePath,
					CanonicalBytes, CanonicalBulkBytes
				);
				if (!CanonicalResult)
					return Fail(std::format("Cook package {} could not be canonicalized: {}", Plan.VirtualPath, CanonicalResult.Message), OutError);
				Plan.PackageBytes = std::move(CanonicalBytes);
				Plan.BulkBytes = std::move(CanonicalBulkBytes);
			}
			if (!CanonicalizeCookVirtualPath(Plan.VirtualPath, OutError))
				return false;
			Plan.PackageDigest = FXxHash128::HashBuffer(Plan.PackageBytes);
			Plan.SegmentDigest = FXxHash128::HashBuffer(Plan.BulkBytes);
			Plan.PackageFileSize = Plan.PackageBytes.size();
			Plan.SegmentFileSize = Plan.BulkBytes.size();
			Plan.TargetPlatform = TargetPlatform;
			Plan.TargetProfile = TargetProfile;
		}
		std::ranges::sort(OutPlans, {}, &FCookSavePlan::VirtualPath);
		for (size_t Index = 1; Index < OutPlans.size(); ++Index)
			if (OutPlans[Index - 1].VirtualPath == OutPlans[Index].VirtualPath)
				return Fail(std::format("Cook package path {} is duplicated after redirect canonicalization.", OutPlans[Index].VirtualPath), OutError);
		if (OutError) OutError->clear();
		return true;
	}

	auto FCookContext::Publish(std::string* OutError) -> bool
	{
		std::vector<FCookSavePlan> Plans;
		if (!TakeSavePlans(Plans, OutError)) return false;
		FCookState State{TargetPlatform, TargetProfile};
		for (FCookSavePlan& Plan : Plans)
		{
			Plan.Contributor = "compatibility-context";
			Plan.BuildProvenance = "captured";
			State.Entries.push_back({Plan.VirtualPath, Plan.InputFingerprint, Plan.PackageDigest, Plan.SegmentDigest, Plan.PackageFileSize, Plan.SegmentFileSize, Plan.ContributorVersion, Plan.FamilyProducerVersion, Plan.Contributor, Plan.BuildProvenance});
			State.Entries.back().SegmentFlags = static_cast<uint8>(
				(Plan.bRawBulkSegment ? 1 : 0)
				| (Plan.bOpaqueRawSegment ? 2 : 0)
			);
		}
		FCookRunResult Result;
		std::unique_ptr<ICookOutputStore> Store = CreateLocalLooseCookOutputStore(
			CookRoot, TargetPlatform, TargetProfile
		);
		std::string Error;
		const bool bPublished = Store->Publish(
			Plans, {}, State, Result, {}, {}, Error
		);
		if (!bPublished && OutError)
			*OutError = std::move(Error);
		else if (OutError)
			OutError->clear();
		return bPublished;
	}
} // namespace Durin
