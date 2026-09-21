#include "Asset/RegistryOperations.h"
#include "Asset/Cook.h"
#include "CookOutputInternal.h"
#include "Asset/PackageSerialization.h"

#include "AssetRegistryOperationsInternal.h"
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
			std::string& VirtualPackagePath
		) -> FCookPlanResult
		{
			FPackagePath RequestedPath;
			if (!FPackagePath::TryCreate(
					VirtualPackagePath, RequestedPath
				))
				return {};

			if (!Durin::FindAssetExact(RequestedPath)) return {};
			const FAssetPathResolveResult Resolution =
				Durin::ResolveAssetPathForOperation(RequestedPath);
			if (!Resolution || !Resolution.FinalAssetData
				|| Resolution.FinalAssetData->EntryKind
					   != EAssetRegistryEntryKind::Asset)
				return {.Error = {.Code = ECookPlanError::Resolution, .VirtualPath = VirtualPackagePath,
					.ResolutionCause = std::make_shared<FAssetPathResolveResult>(Resolution)}};
			VirtualPackagePath = Resolution.FinalPath.ToString();
			return {};
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
	) -> FAssetReadResult
	{
		if (InCookRoot.empty() || !InCookRoot.is_absolute()
			|| InCookRoot.lexically_normal() != InCookRoot)
		{
			return {
				.Error = EAssetReadError::InvalidPath,
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

	auto FormatCookedPathError(const FCookedPathResult& Result) -> std::string
	{
		switch (Result.Error)
		{
		case ECookedPathError::None: return {};
		case ECookedPathError::Root: return "Cook root must be a nonempty absolute path.";
		case ECookedPathError::VirtualPath: return "Cooked package virtual path is invalid: " + Result.VirtualPath;
		case ECookedPathError::Mount: return "Cooked package mount is invalid: " + Result.VirtualPath;
		case ECookedPathError::NotNormalized: return "Cooked package path is not normalized: " + Result.VirtualPath;
		case ECookedPathError::Escape: return "Cooked package path escapes the cook root: " + Result.PackagePath.generic_string();
		case ECookedPathError::PackageExtension: return "Cooked companion requires a .dasset package: " + Result.PackagePath.generic_string();
		}
		return {};
	}

	auto ResolveCookedPackagePath(
		const std::filesystem::path& CookRoot,
		std::string_view VirtualPackagePath,
		std::filesystem::path& OutPackagePath
	) -> FCookedPathResult
	{
		OutPackagePath.clear();
		auto Reject = [&](ECookedPathError Error) -> FCookedPathResult {
			return {.Error = Error, .CookRoot = CookRoot, .VirtualPath = std::string(VirtualPackagePath)};
		};
		if (CookRoot.empty() || !CookRoot.is_absolute()) return Reject(ECookedPathError::Root);
		if (VirtualPackagePath.empty() || VirtualPackagePath.front() != '/'
			|| VirtualPackagePath.back() == '/' || VirtualPackagePath.find('\\') != std::string_view::npos)
			return Reject(ECookedPathError::VirtualPath);
		const size_t Slash = VirtualPackagePath.find('/', 1);
		const std::string_view Mount = Slash == std::string_view::npos ? VirtualPackagePath.substr(1) : VirtualPackagePath.substr(1, Slash - 1);
		if (Mount.empty() || Slash == std::string_view::npos) return Reject(ECookedPathError::Mount);
		const std::string Relative(VirtualPackagePath.substr(1));
		if (!IsValidRelativeManifestPath(Relative)) return Reject(ECookedPathError::NotNormalized);
		const std::filesystem::path Root = CookRoot.lexically_normal();
		std::filesystem::path Candidate = (Root / std::filesystem::path(Relative)).lexically_normal();
		Candidate += ".dasset";
		if (!FPaths::IsLexicalDescendantPath(Candidate, Root, true))
			return {.Error = ECookedPathError::Escape, .CookRoot = CookRoot,
				.VirtualPath = std::string(VirtualPackagePath), .PackagePath = Candidate};
		OutPackagePath = std::move(Candidate);
		return {};
	}

	auto ResolveCookedCompanionPath(
		const std::filesystem::path& CookRoot,
		const std::filesystem::path& PackagePath,
		std::filesystem::path& OutCompanionPath
	) -> FCookedPathResult
	{
		OutCompanionPath.clear();
		auto Reject = [&](ECookedPathError Error) -> FCookedPathResult {
			return {.Error = Error, .CookRoot = CookRoot, .PackagePath = PackagePath};
		};
		const std::filesystem::path Root = CookRoot.lexically_normal();
		const std::filesystem::path Normalized = PackagePath.lexically_normal();
		if (Root.empty() || !Root.is_absolute()) return Reject(ECookedPathError::Root);
		if (PackagePath.extension() != ".dasset") return Reject(ECookedPathError::PackageExtension);
		if (!FPaths::IsLexicalDescendantPath(Normalized, Root, true)) return Reject(ECookedPathError::Escape);
		auto Candidate = Normalized;
		Candidate.replace_extension(".dbulk");
		if (!FPaths::IsLexicalDescendantPath(Candidate, Root, true)) return Reject(ECookedPathError::Escape);
		OutCompanionPath = std::move(Candidate);
		return {};
	}

	auto FormatCookManifestError(const FCookManifestResult& Result) -> std::string
	{
		switch (Result.Error)
		{
		case ECookManifestError::None: return {};
		case ECookManifestError::Target: return "Cook manifest target is invalid.";
		case ECookManifestError::EntryCount: return "Cook manifest entry count exceeds its bound.";
		case ECookManifestError::Entry: return "Cook manifest entry is invalid: " + Result.RelativePath;
		case ECookManifestError::RecordLimit: return "Cook manifest records exceed their byte bound.";
		case ECookManifestError::Encoding: return "Cook manifest encoding failed.";
		case ECookManifestError::Truncated: return "Cook manifest is truncated.";
		case ECookManifestError::Header: return "Cook manifest header is invalid.";
		case ECookManifestError::Checksum: return "Cook manifest record checksum is invalid.";
		case ECookManifestError::Record: return "Cook manifest record is invalid.";
		case ECookManifestError::PathTruncated: return "Cook manifest path is truncated.";
		case ECookManifestError::TrailingBytes: return "Cook manifest has trailing record bytes.";
		}
		return {};
	}

	auto EncodeCookManifest(const FCookManifest& Manifest, FByteBuffer& OutBytes) -> FCookManifestResult
	{
		OutBytes.clear();
		if (!IsValidTarget(Manifest.TargetPlatform, Manifest.TargetProfile))
			return {.Error = ECookManifestError::Target, .TargetPlatform = Manifest.TargetPlatform, .TargetProfile = Manifest.TargetProfile};
		if (Manifest.Entries.size() > MaximumManifestEntries)
			return {.Error = ECookManifestError::EntryCount, .Actual = Manifest.Entries.size(), .Expected = MaximumManifestEntries};
		std::vector<const FCookManifestEntry*> Entries;
		if (!BulkContainer::TryMakeSortedProjection<FCookManifestEntry>(
				Manifest.Entries, &FCookManifestEntry::RelativePath, Entries
			))
			return {.Error = ECookManifestError::Entry};
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
				return {.Error = ECookManifestError::Entry, .RelativePath = Entry.RelativePath, .Offset = Records.Tell()};
			if (!WriteManifestRecord(Records, Entry))
				return {.Error = ECookManifestError::RecordLimit, .Actual = Records.Tell(), .Expected = MaximumManifestRecordBytes};
		}
		uint64 MaximumManifestBytes = 0;
		if (!BulkContainer::TryAdd(ManifestHeaderSize, MaximumManifestRecordBytes, std::numeric_limits<uint64>::max(), MaximumManifestBytes))
			return {.Error = ECookManifestError::RecordLimit, .Actual = Records.Tell(), .Expected = MaximumManifestRecordBytes};
		BulkContainer::FBoundedWriter Writer(MaximumManifestBytes);
		const uint64 RecordBytes = Records.Tell();
		uint64 FileSize = 0;
		FByteBuffer Candidate;
		if (!BulkContainer::TryAdd(
				ManifestHeaderSize, RecordBytes, MaximumManifestBytes, FileSize
			))
			return {.Error = ECookManifestError::Encoding, .Actual = RecordBytes, .Expected = MaximumManifestBytes};
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
			return {.Error = ECookManifestError::Encoding, .Actual = RecordBytes, .Expected = MaximumManifestBytes};
		FCookManifest Validation;
		if (const auto Validated = DecodeCookManifest(Candidate, Validation); !Validated) return Validated;
		OutBytes = std::move(Candidate);
		return {};
	}

	auto DecodeCookManifest(FByteView Bytes, FCookManifest& OutManifest) -> FCookManifestResult
	{
		OutManifest = {};
		if (Bytes.size() < ManifestHeaderSize) return {.Error = ECookManifestError::Truncated, .Actual = Bytes.size(), .Expected = ManifestHeaderSize};
		BulkContainer::FBoundedReader Reader(
			Bytes, ManifestHeaderSize + MaximumManifestRecordBytes
		);
		FManifestHeader Header;
		if (!ReadManifestHeader(Reader, Header))
			return {.Error = ECookManifestError::Truncated, .Offset = Reader.Tell(), .Actual = Bytes.size(), .Expected = ManifestHeaderSize};
		if (Header.Magic != ManifestMagic || Header.Version != ManifestVersion
			|| Header.HeaderSize != ManifestHeaderSize
			|| Header.Count > MaximumManifestEntries
			|| Header.RecordBytes > MaximumManifestRecordBytes
			|| Header.FileSize != Bytes.size()
			|| Header.RecordBytes != Bytes.size() - ManifestHeaderSize
			|| !IsValidTarget(static_cast<ECookTargetPlatform>(Header.Platform), static_cast<ECookTargetProfile>(Header.Profile)))
			return {.Error = ECookManifestError::Header, .Actual = Bytes.size(), .Expected = Header.FileSize,
				.TargetPlatform = static_cast<ECookTargetPlatform>(Header.Platform), .TargetProfile = static_cast<ECookTargetProfile>(Header.Profile)};
		const FByteView Records = Bytes.subspan(ManifestHeaderSize);
		if (FXxHash64::HashBuffer(Records).HashValue != Header.RecordHash)
			return {.Error = ECookManifestError::Checksum, .Offset = ManifestHeaderSize,
				.Actual = FXxHash64::HashBuffer(Records).HashValue, .Expected = Header.RecordHash};
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
				return {.Error = ECookManifestError::Record, .Offset = RecordReader.Tell(), .Actual = RecordHeader.PathBytes, .Expected = 1024, .EntryIndex = Index};
			FByteView Path;
			if (!RecordReader.ReadBytes(RecordHeader.PathBytes, Path))
				return {.Error = ECookManifestError::PathTruncated, .Offset = RecordReader.Tell(), .Actual = Records.size() - RecordReader.Tell(), .Expected = RecordHeader.PathBytes, .EntryIndex = Index};
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
				return {.Error = ECookManifestError::Entry, .RelativePath = Entry.RelativePath, .Offset = RecordReader.Tell(), .EntryIndex = Index};
			Entries.push_back(std::move(Entry));
		}
		if (RecordReader.Tell() != Records.size()) return {.Error = ECookManifestError::TrailingBytes, .Offset = RecordReader.Tell(), .Actual = Records.size(), .Expected = RecordReader.Tell()};
		OutManifest.TargetPlatform = static_cast<ECookTargetPlatform>(Header.Platform);
		OutManifest.TargetProfile = static_cast<ECookTargetProfile>(Header.Profile);
		OutManifest.Entries = std::move(Entries);
		return {};
	}

	FCookContext::FCookContext(
		ECookTargetPlatform InTargetPlatform,
		ECookTargetProfile InTargetProfile,
		bool bInRetainEditorOnlyData
	)
		: TargetPlatform(InTargetPlatform)
		, TargetProfile(InTargetProfile)
		, bRetainEditorOnlyData(bInRetainEditorOnlyData)
	{
	}

	namespace
	{
		auto ValidateCookPlanPath(std::string_view VirtualPackagePath) -> FCookPlanResult
		{
			if (VirtualPackagePath.empty()
				|| VirtualPackagePath.front() != '/'
				|| VirtualPackagePath.back() == '/'
				|| VirtualPackagePath.find("//") != std::string_view::npos
				|| VirtualPackagePath.find("\\") != std::string_view::npos
				|| VirtualPackagePath.find("/../") != std::string_view::npos
				|| VirtualPackagePath.find('/', 1) == std::string_view::npos)
				return {{.Code = ECookPlanError::Path, .VirtualPath = std::string(VirtualPackagePath)}};
			return {};
		}
	} // namespace

	auto FormatCookPlanError(const FCookPlanError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case ECookPlanError::None: return {};
		case ECookPlanError::Target: return "Cook save-plan target is invalid.";
		case ECookPlanError::Canonicalization: return "Cook package " + Error.VirtualPath + " could not be canonicalized: " + Error.CanonicalizationDiagnostic;
		case ECookPlanError::Resolution: return "Cook output path " + Error.VirtualPath + " does not resolve to a final real asset.";
		case ECookPlanError::CanonicalDuplicate: return "Cook package path " + Error.VirtualPath + " is duplicated after redirect canonicalization.";
		case ECookPlanError::Path: return "Cook package path is invalid or uses an unsupported mount: " + Error.VirtualPath;
		case ECookPlanError::SourceIdentity: return "Cook source package identity is invalid: " + Error.VirtualPath;
		case ECookPlanError::EmptyRawPackage: return "Opaque raw Cook packages require package and segment bytes.";
		case ECookPlanError::SegmentLimit: return std::format("Opaque raw Cook segment has {} bytes, exceeding the {} byte limit.", Error.SegmentBytes, Error.MaximumSegmentBytes);
		case ECookPlanError::EmptyPackage: return "Cook package bytes must be nonempty.";
		case ECookPlanError::DuplicatePath: return "Cook package path is duplicated: " + Error.VirtualPath;
		case ECookPlanError::InvalidPackage: return "Cook package projection requires a valid asset package.";
		case ECookPlanError::Projection: return !Error.ProjectionDiagnostic.empty() ? "Cook package projection failed: " + Error.ProjectionDiagnostic : "Cook package projection failed.";
		}
		return {};
	}

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
		FByteBuffer PackageBytes
	) -> FCookPlanResult
	{
		FPackagePath SourcePackagePath;
		if (!FPackagePath::TryCreate(VirtualPackagePath, SourcePackagePath)
			&& !FPackagePath::TryCreateProjectContent(
				VirtualPackagePath, SourcePackagePath))
			return {{.Code = ECookPlanError::SourceIdentity, .VirtualPath = VirtualPackagePath}};
		return AddPackage(std::move(VirtualPackagePath), SourcePackagePath,
			std::move(PackageBytes));
	}

	auto FCookContext::AddPackage(
		std::string VirtualPackagePath,
		const FPackagePath& SourcePackagePath,
		FByteBuffer PackageBytes
	) -> FCookPlanResult
	{
		if (const auto Validated = ValidateCookPlanPath(VirtualPackagePath); !Validated) return Validated;
		if (!SourcePackagePath.IsValid())
			return {{.Code = ECookPlanError::SourceIdentity, .VirtualPath = VirtualPackagePath}};
		if (PackageBytes.empty()) return {{.Code = ECookPlanError::EmptyPackage, .VirtualPath = VirtualPackagePath}};
		if (std::ranges::any_of(Packages, [&](const FCookSavePlan& Existing) {
				return Existing.VirtualPath == VirtualPackagePath;
			})) return {{.Code = ECookPlanError::DuplicatePath, .VirtualPath = VirtualPackagePath}};
		Packages.push_back({
			.VirtualPath = std::move(VirtualPackagePath),
			.SourcePackagePath = SourcePackagePath,
			.PackageBytes = std::move(PackageBytes)});
		return {};
	}

	auto FCookContext::AddPackage(
		std::string VirtualPackagePath,
		DPackage* Package
	) -> FCookPlanResult
	{
		if (const auto Validated = ValidateCookPlanPath(VirtualPackagePath); !Validated) return Validated;
		if (!Package || !Package->IsAssetPackage()
			|| Package->GetTopLevelAssets().empty())
			return {{.Code = ECookPlanError::InvalidPackage, .VirtualPath = VirtualPackagePath}};
		FPackagePath SourcePackagePath;
		if (!FPackagePath::TryCreate(Package->GetPackagePath(), SourcePackagePath))
			return {{.Code = ECookPlanError::SourceIdentity, .VirtualPath = VirtualPackagePath}};
		if (std::ranges::any_of(Packages, [&](const FCookSavePlan& Existing) {
				return Existing.VirtualPath == VirtualPackagePath;
			})) return {{.Code = ECookPlanError::DuplicatePath, .VirtualPath = VirtualPackagePath}};

		FAssetPackageSerializationOptions Options = MakePackageSerializationOptions();
		FByteBuffer PackageBytes;
		FByteBuffer Segment;
		const auto Result = SerializeAssetPackageClosure(
			Package, PackageBytes, Segment, Options);
		if (!Result)
			return {{.Code = ECookPlanError::Projection, .VirtualPath = VirtualPackagePath,
				.SourcePath = SourcePackagePath.ToString(), .ProjectionDiagnostic = ObjectPackage::FormatPackageError(Result)}};
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
		return {};
	}

	auto FCookContext::AddRawPackage(
		std::string VirtualPackagePath,
		FByteBuffer PackageBytes,
		FByteBuffer RawSegmentBytes
	) -> FCookPlanResult
	{
		if (const auto Validated = ValidateCookPlanPath(VirtualPackagePath); !Validated)
			return Validated;
		if (PackageBytes.empty() || RawSegmentBytes.empty())
			return {.Error = {.Code = ECookPlanError::EmptyRawPackage, .VirtualPath = VirtualPackagePath,
				.PackageBytes = PackageBytes.size(), .SegmentBytes = RawSegmentBytes.size()}};
		if (RawSegmentBytes.size() > PackageBulkDataMaximumSegmentBytes)
			return {.Error = {.Code = ECookPlanError::SegmentLimit, .VirtualPath = VirtualPackagePath,
				.PackageBytes = PackageBytes.size(), .SegmentBytes = RawSegmentBytes.size(),
				.MaximumSegmentBytes = PackageBulkDataMaximumSegmentBytes}};
		if (std::ranges::any_of(Packages, [&](const FCookSavePlan& Existing) {
				return Existing.VirtualPath == VirtualPackagePath;
			})) return {.Error = {.Code = ECookPlanError::DuplicatePath, .VirtualPath = VirtualPackagePath}};
		FPackageBulkSegmentSummary Summary{
			.Extent = RawSegmentBytes.size(),
			.Digest = FXxHash128::HashBuffer(RawSegmentBytes)
		};
		Packages.push_back({.VirtualPath = std::move(VirtualPackagePath), .PackageBytes = std::move(PackageBytes), .BulkBytes = std::move(RawSegmentBytes), .BulkSummary = Summary, .bOpaqueRawSegment = true});
		return {};
	}

	auto FCookContext::TakeSavePlans(
		std::vector<FCookSavePlan>& OutPlans
	) -> FCookPlanResult
	{
		OutPlans.clear();
		auto Plans = std::exchange(Packages, {});
		if (!IsValidTarget(TargetPlatform, TargetProfile))
			return {.Error = {.Code = ECookPlanError::Target, .TargetPlatform = TargetPlatform, .TargetProfile = TargetProfile}};
		for (FCookSavePlan& Plan : Plans)
		{
			if (!Plan.bOpaqueRawSegment)
			{
				FByteBuffer CanonicalBytes;
				FByteBuffer CanonicalBulkBytes;
				FPackagePath PackagePath;
				if (!FPackagePath::TryCreate(Plan.VirtualPath, PackagePath)
					&& !FPackagePath::TryCreateProjectContent(
						Plan.VirtualPath, PackagePath))
					return {.Error = {.Code = ECookPlanError::SourceIdentity, .VirtualPath = Plan.VirtualPath}};
				const auto CanonicalResult = CanonicalizeAssetPackageForCook(
					Plan.PackageBytes, Plan.BulkBytes,
					Plan.SourcePackagePath.IsValid()
						? Plan.SourcePackagePath : PackagePath,
					PackagePath,
					CanonicalBytes, CanonicalBulkBytes
				);
				if (!CanonicalResult)
					return {.Error = {.Code = ECookPlanError::Canonicalization, .VirtualPath = Plan.VirtualPath,
					.CanonicalizationDiagnostic = CanonicalResult.Message}};
				Plan.PackageBytes = std::move(CanonicalBytes);
				Plan.BulkBytes = std::move(CanonicalBulkBytes);
			}
			if (const auto Resolved = CanonicalizeCookVirtualPath(Plan.VirtualPath); !Resolved)
				return Resolved;
			Plan.PackageDigest = FXxHash128::HashBuffer(Plan.PackageBytes);
			Plan.SegmentDigest = FXxHash128::HashBuffer(Plan.BulkBytes);
			Plan.PackageFileSize = Plan.PackageBytes.size();
			Plan.SegmentFileSize = Plan.BulkBytes.size();
			Plan.TargetPlatform = TargetPlatform;
			Plan.TargetProfile = TargetProfile;
		}
		std::ranges::sort(Plans, {}, &FCookSavePlan::VirtualPath);
		for (size_t Index = 1; Index < Plans.size(); ++Index)
			if (Plans[Index - 1].VirtualPath == Plans[Index].VirtualPath)
				return {.Error = {.Code = ECookPlanError::CanonicalDuplicate, .VirtualPath = Plans[Index].VirtualPath}};
		OutPlans = std::move(Plans);
		return {};
	}

	auto FormatCookContextPublishError(const FCookContextPublishResult& Result) -> std::string
	{
		switch (Result.Error)
		{
		case ECookContextPublishError::None: return {};
		case ECookContextPublishError::Finalization:
			return Result.PlanCause ? FormatCookPlanError(*Result.PlanCause) : "Cook plan finalization failed.";
		case ECookContextPublishError::Publication:
			return Result.PublicationCause ? FormatCookPublishError(*Result.PublicationCause) : "Cook publication failed.";
		}
		return {};
	}

	auto PublishCookContext(FCookContext& Context,
		const std::filesystem::path& OutputRoot) -> FCookContextPublishResult
	{
		std::vector<FCookSavePlan> Plans;
		if (const auto Taken = Context.TakeSavePlans(Plans); !Taken)
			return {.Error = ECookContextPublishError::Finalization, .OutputRoot = OutputRoot, .PlanCause = Taken.Error};
		FCookState State{Context.GetTargetPlatform(), Context.GetTargetProfile()};
		for (FCookSavePlan& Plan : Plans)
		{
			Plan.Contributor = "compatibility-context";
			Plan.BuildProvenance = "captured";
			State.Entries.push_back(AssetPrivate::MakeCookStateEntry(Plan));
		}
		FCookRunResult Result;
		std::unique_ptr<ICookOutputStore> Store = CreateLocalLooseCookOutputStore(
			OutputRoot, State.TargetPlatform, State.TargetProfile
		);
		FCookPublishResult PublishResult = Store->Publish(
			Plans, {}, State, Result, {}, {});
		if (!PublishResult)
			return {.Error = ECookContextPublishError::Publication, .OutputRoot = OutputRoot,
				.PublicationCause = std::move(PublishResult)};
		return {};
	}
} // namespace Durin
