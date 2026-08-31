#include "AssetMaintenance/CompatibilityAudit.h"

#include "Hash/XxHash.h"
#include "Json/Json.h"
#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr size_t FingerprintBufferSize = 64 * 1024;

		class FSha256
		{
		public:
			auto Update(std::span<const std::byte> Data) -> void
			{
				TotalBytes += Data.size();
				while (!Data.empty())
				{
					const size_t Count = std::min(Data.size(), Block.size() - BlockSize);
					std::memcpy(Block.data() + BlockSize, Data.data(), Count);
					BlockSize += Count;
					Data = Data.subspan(Count);
					if (BlockSize == Block.size()) { Transform(Block.data()); BlockSize = 0; }
				}
			}

			auto Finalize() -> std::string
			{
				const uint64 BitCount = TotalBytes * 8;
				Block[BlockSize++] = std::byte{0x80};
				if (BlockSize > 56)
				{
					std::fill(Block.begin() + static_cast<ptrdiff_t>(BlockSize), Block.end(), std::byte{0});
					Transform(Block.data());
					BlockSize = 0;
				}
				std::fill(Block.begin() + static_cast<ptrdiff_t>(BlockSize), Block.begin() + 56, std::byte{0});
				for (size_t Index = 0; Index < 8; ++Index)
					Block[63 - Index] = static_cast<std::byte>(BitCount >> (Index * 8));
				Transform(Block.data());
				std::string Hex = "sha256:";
				for (const uint32 Value : State) Hex += std::format("{:08x}", Value);
				return Hex;
			}

		private:
			auto Transform(const std::byte* Data) -> void
			{
				static constexpr std::array<uint32, 64> K = {
					0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
					0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
					0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
					0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
					0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
					0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
					0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
					0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
				std::array<uint32, 64> Words{};
				for (size_t Index = 0; Index < 16; ++Index)
					Words[Index] = std::to_integer<uint32>(Data[Index * 4]) << 24
						| std::to_integer<uint32>(Data[Index * 4 + 1]) << 16
						| std::to_integer<uint32>(Data[Index * 4 + 2]) << 8
						| std::to_integer<uint32>(Data[Index * 4 + 3]);
				for (size_t Index = 16; Index < Words.size(); ++Index)
				{
					const uint32 S0 = std::rotr(Words[Index - 15], 7)
						^ std::rotr(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
					const uint32 S1 = std::rotr(Words[Index - 2], 17)
						^ std::rotr(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
					Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
				}
				auto [A, B, C, D, E, F, G, H] = State;
				for (size_t Index = 0; Index < Words.size(); ++Index)
				{
					const uint32 S1 = std::rotr(E, 6) ^ std::rotr(E, 11) ^ std::rotr(E, 25);
					const uint32 Choice = (E & F) ^ (~E & G);
					const uint32 Temp1 = H + S1 + Choice + K[Index] + Words[Index];
					const uint32 S0 = std::rotr(A, 2) ^ std::rotr(A, 13) ^ std::rotr(A, 22);
					const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
					const uint32 Temp2 = S0 + Majority;
					H=G; G=F; F=E; E=D+Temp1; D=C; C=B; B=A; A=Temp1+Temp2;
				}
				State[0]+=A; State[1]+=B; State[2]+=C; State[3]+=D;
				State[4]+=E; State[5]+=F; State[6]+=G; State[7]+=H;
			}

			std::array<uint32, 8> State = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
				0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
			std::array<std::byte, 64> Block{};
			size_t BlockSize = 0;
			uint64 TotalBytes = 0;
		};

		auto CaptureFingerprint(const std::filesystem::path& Path,
			const FAssetCompatibilityCancellationCheck& IsCancelled,
			FAssetPackageFingerprint& OutFingerprint,
			std::string& OutReportContentHash,
			std::string& OutError) -> EAssetPackageSnapshotStatus
		{
			std::error_code Error;
			const uintmax_t InitialSize = std::filesystem::file_size(Path, Error);
			const auto InitialTime = Error ? std::filesystem::file_time_type{}
				: std::filesystem::last_write_time(Path, Error);
			if (Error)
			{
				OutError = std::format("Failed to fingerprint package '{}': {}",
					Path.generic_string(), Error.message());
				return EAssetPackageSnapshotStatus::Failed;
			}
			std::ifstream Stream(Path, std::ios::binary);
			if (!Stream.is_open())
			{
				OutError = std::format("Failed to open package '{}' for fingerprinting.",
					Path.generic_string());
				return EAssetPackageSnapshotStatus::Failed;
			}
			FXxHash128Builder Builder;
			FSha256 Sha256;
			std::array<char, FingerprintBufferSize> Buffer{};
			while (Stream)
			{
				if (IsCancelled && IsCancelled()) return EAssetPackageSnapshotStatus::Cancelled;
				Stream.read(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
				const std::streamsize Read = Stream.gcount();
				if (Read > 0)
				{
					Builder.Update(Buffer.data(), static_cast<uint64>(Read));
					Sha256.Update(std::as_bytes(std::span(Buffer).first(static_cast<size_t>(Read))));
				}
			}
			if (Stream.bad())
			{
				OutError = std::format("Failed while hashing package '{}'.", Path.generic_string());
				return EAssetPackageSnapshotStatus::Failed;
			}
			const uintmax_t FinalSize = std::filesystem::file_size(Path, Error);
			const auto FinalTime = Error ? std::filesystem::file_time_type{}
				: std::filesystem::last_write_time(Path, Error);
			if (Error || InitialSize != FinalSize || InitialTime != FinalTime)
			{
				OutError = std::format("Package '{}' changed while its fingerprint was captured.",
					Path.generic_string());
				return EAssetPackageSnapshotStatus::Failed;
			}
			OutFingerprint = {.FileSize = FinalSize,
				.LastWriteTimeTicks = FileTime::ToStableTicks(FinalTime),
				.ContentHash = Builder.Finalize()};
			OutReportContentHash = Sha256.Finalize();
			return EAssetPackageSnapshotStatus::Completed;
		}

		auto PropertyKindName(DurinCodeGen::EPropertyGenFlags Value) -> std::string_view
		{
			switch (Value)
			{
			case DurinCodeGen::EPropertyGenFlags::None: return "None";
			case DurinCodeGen::EPropertyGenFlags::Bool: return "Bool";
			case DurinCodeGen::EPropertyGenFlags::Int8: return "Int8";
			case DurinCodeGen::EPropertyGenFlags::Int16: return "Int16";
			case DurinCodeGen::EPropertyGenFlags::Int32: return "Int32";
			case DurinCodeGen::EPropertyGenFlags::Int64: return "Int64";
			case DurinCodeGen::EPropertyGenFlags::UInt8: return "UInt8";
			case DurinCodeGen::EPropertyGenFlags::UInt16: return "UInt16";
			case DurinCodeGen::EPropertyGenFlags::UInt32: return "UInt32";
			case DurinCodeGen::EPropertyGenFlags::UInt64: return "UInt64";
			case DurinCodeGen::EPropertyGenFlags::Float: return "Float";
			case DurinCodeGen::EPropertyGenFlags::Double: return "Double";
			case DurinCodeGen::EPropertyGenFlags::String: return "String";
			case DurinCodeGen::EPropertyGenFlags::Enum: return "Enum";
			case DurinCodeGen::EPropertyGenFlags::Object: return "Object";
			case DurinCodeGen::EPropertyGenFlags::Array: return "Array";
			case DurinCodeGen::EPropertyGenFlags::Map: return "Map";
			case DurinCodeGen::EPropertyGenFlags::Struct: return "Struct";
			case DurinCodeGen::EPropertyGenFlags::Name: return "Name";
			case DurinCodeGen::EPropertyGenFlags::Guid: return "Guid";
			case DurinCodeGen::EPropertyGenFlags::SoftObject: return "SoftObject";
			case DurinCodeGen::EPropertyGenFlags::WeakObject: return "WeakObject";
			case DurinCodeGen::EPropertyGenFlags::Byte: return "Byte";
			case DurinCodeGen::EPropertyGenFlags::Blob: return "Blob";
			case DurinCodeGen::EPropertyGenFlags::BulkData: return "BulkData";
			case DurinCodeGen::EPropertyGenFlags::Count: return "Unknown";
			}
			return "Unknown";
		}

		auto ToCompatibilityCode(EPackageSchemaIssueCode Code)
			-> EAssetCompatibilityFindingCode
		{
			switch (Code)
			{
			case EPackageSchemaIssueCode::UnknownField:
				return EAssetCompatibilityFindingCode::UnknownField;
			case EPackageSchemaIssueCode::IncompatibleFieldSignature:
				return EAssetCompatibilityFindingCode::IncompatibleFieldSignature;
			case EPackageSchemaIssueCode::DeprecatedRouteUsed:
				return EAssetCompatibilityFindingCode::DeprecatedRouteUsed;
			case EPackageSchemaIssueCode::UnavailableClass:
				return EAssetCompatibilityFindingCode::UnavailableClass;
			case EPackageSchemaIssueCode::InvalidObjectGraph:
				return EAssetCompatibilityFindingCode::InvalidObjectGraph;
			}
			return EAssetCompatibilityFindingCode::InvalidObjectGraph;
		}

		auto AddTerminalFailure(FAssetPackageCompatibilityRecord& Record,
			EAssetCompatibilityFindingCode Code, std::string Diagnostic) -> void
		{
			Record.Inspection = EAssetCompatibilityInspection::Failed;
			Record.Compatibility = EAssetPackageCompatibility::Unsupported;
			Record.Findings.push_back({.Code = Code, .Diagnostic = std::move(Diagnostic)});
		}
	}

	auto CaptureMountedAssetPackageSnapshot(
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested)
		-> FAssetPackageDiscoverySnapshot
	{
		FAssetPackageDiscoverySnapshot Result;
		for (const FMountPoint& Mount : FMountPaths::GetRegisteredMountPoints())
		{
			if (IsCancellationRequested && IsCancellationRequested())
			{
				Result.Status = EAssetPackageSnapshotStatus::Cancelled;
				return Result;
			}
			if (!Mount.bAutoScan) continue;
			const std::filesystem::path ContentRoot = Mount.GetContentDir();
			std::error_code Error;
			if (!std::filesystem::exists(ContentRoot, Error))
			{
				if (Error)
				{
					Result.Status = EAssetPackageSnapshotStatus::Failed;
					Result.Error = std::format("Failed to inspect mount {}: {}",
						Mount.VirtualRoot, Error.message());
					return Result;
				}
				continue;
			}
			std::filesystem::recursive_directory_iterator It(ContentRoot,
				std::filesystem::directory_options::skip_permission_denied, Error);
			const std::filesystem::recursive_directory_iterator End;
			if (Error)
			{
				Result.Status = EAssetPackageSnapshotStatus::Failed;
				Result.Error = std::format("Failed to enumerate mount {}: {}",
					Mount.VirtualRoot, Error.message());
				return Result;
			}
			while (It != End)
			{
				if (IsCancellationRequested && IsCancellationRequested())
				{
					Result.Status = EAssetPackageSnapshotStatus::Cancelled;
					return Result;
				}
				std::error_code FileError;
				if (It->is_regular_file(FileError) && It->path().extension() == ".dasset")
				{
					std::filesystem::path Relative = std::filesystem::relative(
						It->path(), ContentRoot, FileError);
					if (FileError)
					{
						Result.Status = EAssetPackageSnapshotStatus::Failed;
						Result.Error = std::format("Failed to classify package '{}': {}",
							It->path().generic_string(), FileError.message());
						return Result;
					}
					Relative.replace_extension();
					FPackagePath PackagePath;
					std::string PathError;
					if (!FPackagePath::TryCreate(Mount.VirtualRoot + Relative.generic_string(),
						PackagePath, &PathError))
					{
						Result.Status = EAssetPackageSnapshotStatus::Failed;
						Result.Error = std::format("Invalid mounted package path '{}': {}",
							It->path().generic_string(), PathError);
						return Result;
					}
					FAssetPackageFingerprint Fingerprint;
					std::string ReportContentHash;
					Result.Status = CaptureFingerprint(It->path(), IsCancellationRequested,
						Fingerprint, ReportContentHash, Result.Error);
					if (Result.Status != EAssetPackageSnapshotStatus::Completed) return Result;
					Result.Packages.push_back({.PackagePath = std::move(PackagePath),
						.PhysicalPath = It->path().generic_string(),
						.ExpectedFileSize = Fingerprint.FileSize,
						.ExpectedLastWriteTimeTicks = Fingerprint.LastWriteTimeTicks,
						.ExpectedContentHash = Fingerprint.ContentHash,
						.ExpectedReportContentHash = std::move(ReportContentHash)});
				}
				else if (FileError)
				{
					Result.Status = EAssetPackageSnapshotStatus::Failed;
					Result.Error = std::format("Failed to inspect package candidate '{}': {}",
						It->path().generic_string(), FileError.message());
					return Result;
				}
				It.increment(Error);
				if (Error)
				{
					Result.Status = EAssetPackageSnapshotStatus::Failed;
					Result.Error = std::format("Failed to enumerate mount {}: {}",
						Mount.VirtualRoot, Error.message());
					return Result;
				}
			}
		}
		std::ranges::sort(Result.Packages, [](const auto& Left, const auto& Right) {
			return Left.PackagePath.GetView() < Right.PackagePath.GetView();
		});
		const auto Duplicate = std::ranges::adjacent_find(Result.Packages,
			[](const auto& Left, const auto& Right) {
				return Left.PackagePath == Right.PackagePath;
			});
		if (Duplicate != Result.Packages.end())
		{
			Result.Status = EAssetPackageSnapshotStatus::Failed;
			Result.Error = std::format("Duplicate mounted package path {}.",
				Duplicate->PackagePath.ToString());
			Result.Packages.clear();
		}
		return Result;
	}

	auto ProbeAssetPackageCompatibility(
		const FAssetPackageCompatibilityProbeInput& Input,
		const FReflectionCompatibilityCatalog& Catalog,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested)
		-> FAssetPackageCompatibilityProbeResult
	{
		FAssetPackageCompatibilityProbeResult Result;
		auto IsCancelled = [&]() {
			return IsCancellationRequested && IsCancellationRequested();
		};
		if (IsCancelled())
		{
			Result.Status = EAssetCompatibilityProbeStatus::Cancelled;
			return Result;
		}
		FAssetPackageCompatibilityRecord Record{
			.PackagePath = Input.PackagePath,
			.PhysicalPath = Input.PhysicalPath,
			.ReportContentHash = Input.ExpectedReportContentHash,
			.Inspection = EAssetCompatibilityInspection::Ready,
			.Compatibility = EAssetPackageCompatibility::Compatible};
		FFileHelper::FFileIoError OpenError;
		auto Handle = FFileHelper::OpenRead(Input.PhysicalPath, &OpenError);
		if (!Handle)
		{
			AddTerminalFailure(Record, EAssetCompatibilityFindingCode::IoFailure,
				OpenError.ToString());
			Result.Record = std::move(Record);
			return Result;
		}
		Record.Fingerprint = {.FileSize = Handle->GetSize(),
			.ContentHash = Input.ExpectedContentHash};
		std::error_code TimeError;
		const auto InitialTime = std::filesystem::last_write_time(Input.PhysicalPath, TimeError);
		if (TimeError)
		{
			AddTerminalFailure(Record, EAssetCompatibilityFindingCode::IoFailure,
				std::format("Failed to read package timestamp for {}.", Input.PhysicalPath));
			Result.Record = std::move(Record);
			return Result;
		}
		Record.Fingerprint.LastWriteTimeTicks = FileTime::ToStableTicks(InitialTime);

		FPackageSchemaInspection Inspection;
		const FAssetResult InspectionResult = InspectAssetPackageSchema(*Handle,
			Input.PackagePath, Catalog, Inspection, &Result.Stats,
			Input.bIncludeNestedMigrationEvidence, IsCancelled);
		if (IsCancelled())
		{
			Result.Status = EAssetCompatibilityProbeStatus::Cancelled;
			return Result;
		}
		if (!InspectionResult)
		{
			const bool bUnsupported = InspectionResult.Error == EAssetError::UnsupportedVersion;
			const bool bIo = InspectionResult.Error == EAssetError::IoError
				|| InspectionResult.Error == EAssetError::NotFound
				|| InspectionResult.Message.starts_with("File I/O failed");
			AddTerminalFailure(Record, bUnsupported
				? EAssetCompatibilityFindingCode::UnsupportedPackageFormat
				: bIo ? EAssetCompatibilityFindingCode::IoFailure
				: EAssetCompatibilityFindingCode::CorruptPackage,
				InspectionResult.Message);
		}
		else
		{
			Record.FormatVersion = Inspection.FormatVersion;
			Record.Fingerprint.ReaderVersion = Inspection.FormatVersion;
			Record.EntryKind = Inspection.EntryKind;
			Record.Dependencies = std::move(Inspection.Dependencies);
			Record.Compatibility = Inspection.Status == EPackageSchemaStatus::Compatible
				? EAssetPackageCompatibility::Compatible
				: Inspection.Status == EPackageSchemaStatus::Incompatible
					? EAssetPackageCompatibility::Incompatible
					: EAssetPackageCompatibility::Unsupported;
			Record.CanonicalizationEvidence = std::move(Inspection.CanonicalizationEvidence);
			Record.DeprecatedRouteEvidence = std::move(Inspection.DeprecatedRouteEvidence);
			for (FPackageSchemaIssue& Issue : Inspection.Issues)
				Record.Findings.push_back({
					.Code = ToCompatibilityCode(Issue.Code),
					.ObjectPath = std::move(Issue.ObjectPath),
					.ClassIdentity = std::move(Issue.ClassIdentity),
					.DeclaringType = std::move(Issue.DeclaringType),
					.FieldName = std::move(Issue.FieldName),
					.StoredKind = Issue.StoredKind,
					.StoredTypeSignature = std::move(Issue.StoredTypeSignature),
					.ExpectedKind = Issue.ExpectedKind,
					.ExpectedTypeSignature = std::move(Issue.ExpectedTypeSignature),
					.PayloadSize = Issue.PayloadSize,
					.PayloadOffset = Issue.PayloadOffset,
					.Diagnostic = std::move(Issue.Diagnostic)});
		}
		std::error_code FinalError;
		const uintmax_t FinalSize = std::filesystem::file_size(Input.PhysicalPath, FinalError);
		const auto FinalTime = std::filesystem::last_write_time(Input.PhysicalPath, FinalError);
		const int64 FinalTicks = FinalError ? 0 : FileTime::ToStableTicks(FinalTime);
		if (FinalError || FinalSize != Input.ExpectedFileSize
			|| FinalTicks != Input.ExpectedLastWriteTimeTicks
			|| FinalSize != Record.Fingerprint.FileSize
			|| FinalTicks != Record.Fingerprint.LastWriteTimeTicks)
			Record.Freshness = EAssetCompatibilityFreshness::Stale;
		Result.Record = std::move(Record);
		return Result;
	}

	auto IsAssetPackageCompatibilityRecordCurrent(
		const FAssetPackageCompatibilityRecord& Record, uintmax_t FileSize,
		int64 LastWriteTimeTicks) -> bool
	{
		return Record.Freshness == EAssetCompatibilityFreshness::Current
			&& Record.Fingerprint.FileSize == FileSize
			&& Record.Fingerprint.LastWriteTimeTicks == LastWriteTimeTicks;
	}

	auto AssetCompatibilityFindingCodeName(EAssetCompatibilityFindingCode Code)
		-> std::string_view
	{
		switch (Code)
		{
		case EAssetCompatibilityFindingCode::UnknownField: return "UnknownField";
		case EAssetCompatibilityFindingCode::IncompatibleFieldSignature: return "IncompatibleFieldSignature";
		case EAssetCompatibilityFindingCode::DeprecatedRouteUsed: return "DeprecatedRouteUsed";
		case EAssetCompatibilityFindingCode::UnavailableClass: return "UnavailableClass";
		case EAssetCompatibilityFindingCode::UnsupportedPackageFormat: return "UnsupportedPackageFormat";
		case EAssetCompatibilityFindingCode::InvalidObjectGraph: return "InvalidObjectGraph";
		case EAssetCompatibilityFindingCode::CorruptPackage: return "CorruptPackage";
		case EAssetCompatibilityFindingCode::IoFailure: return "IoFailure";
		}
		return "CorruptPackage";
	}

	auto RunAssetCompatibilityAudit(
		std::span<const FAssetPackageCompatibilityProbeInput> Inputs,
		const FReflectionCompatibilityCatalog& Catalog,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested,
		const FAssetCompatibilityRecordSink& OnRecord,
		const FAssetCompatibilityProbeOperation& ProbeOperation)
		-> FAssetCompatibilityAuditResult
	{
		FAssetCompatibilityAuditResult Result;
		std::vector<const FAssetPackageCompatibilityProbeInput*> Sorted;
		Sorted.reserve(Inputs.size());
		for (const auto& Input : Inputs) Sorted.push_back(&Input);
		std::ranges::sort(Sorted, [](const auto* Left, const auto* Right) {
			return Left->PackagePath.GetView() < Right->PackagePath.GetView();
		});
		Result.Records.reserve(Sorted.size());
		for (const auto* Input : Sorted)
		{
			if (IsCancellationRequested && IsCancellationRequested())
			{
				Result.Status = EAssetCompatibilityAuditStatus::Cancelled;
				return Result;
			}
			auto Probe = ProbeOperation
				? ProbeOperation(*Input, Catalog, IsCancellationRequested)
				: ProbeAssetPackageCompatibility(*Input, Catalog, IsCancellationRequested);
			if (Probe.Status == EAssetCompatibilityProbeStatus::Cancelled)
			{
				Result.Status = EAssetCompatibilityAuditStatus::Cancelled;
				return Result;
			}
			if (!Probe.Record) continue;
			Result.Records.push_back(std::move(*Probe.Record));
			if (OnRecord) OnRecord(Result.Records.back(), Result.Records.size(), Sorted.size());
		}
		return Result;
	}

	auto SerializeAssetCompatibilityReport(
		std::span<const FAssetPackageCompatibilityRecord> Records) -> std::string
	{
		std::vector<const FAssetPackageCompatibilityRecord*> Sorted;
		for (const auto& Record : Records) Sorted.push_back(&Record);
		std::ranges::sort(Sorted, [](const auto* Left, const auto* Right) {
			return Left->PackagePath.GetView() < Right->PackagePath.GetView();
		});
		auto InspectionName = [](EAssetCompatibilityInspection Value) -> std::string_view {
			switch (Value) { case EAssetCompatibilityInspection::NotChecked: return "NotChecked";
			case EAssetCompatibilityInspection::Ready: return "Ready";
			case EAssetCompatibilityInspection::Failed: return "Failed"; }
			return "Failed";
		};
		auto CompatibilityName = [](EAssetPackageCompatibility Value) -> std::string_view {
			switch (Value) { case EAssetPackageCompatibility::Compatible: return "Compatible";
			case EAssetPackageCompatibility::Incompatible: return "Incompatible";
			case EAssetPackageCompatibility::Unsupported: return "Unsupported"; }
			return "Unsupported";
		};
		FJsonDocument Document;
		FJsonNodeRef Root = Document.GetMutableRoot();
		Root.EnsureObject();
		Root.SetChildValue("schemaVersion", AssetCompatibilityReportSchemaVersion);
		FJsonNodeRef Packages = Root.AddArray("packages");
		for (const FAssetPackageCompatibilityRecord* RecordPtr : Sorted)
		{
			const auto& Record = *RecordPtr;
			FJsonNodeRef Package = Packages.AppendObject();
			Package.SetChildValue("packagePath", Record.PackagePath.GetView());
			Package.SetChildValue("physicalPath", Record.PhysicalPath);
			Package.SetChildValue("formatVersion", Record.FormatVersion);
			Package.SetChildValue("inspection", InspectionName(Record.Inspection));
			Package.SetChildValue("compatibility", CompatibilityName(Record.Compatibility));
			Package.SetChildValue("freshness",
				Record.Freshness == EAssetCompatibilityFreshness::Current ? "Current" : "Stale");
			Package.SetChildValue("fileSize", static_cast<uint64>(Record.Fingerprint.FileSize));
			Package.SetChildValue("lastWriteTimeTicks", Record.Fingerprint.LastWriteTimeTicks);
			FJsonNodeRef Findings = Package.AddArray("findings");
			for (const auto& Finding : Record.Findings)
			{
				FJsonNodeRef Node = Findings.AppendObject();
				Node.SetChildValue("code", AssetCompatibilityFindingCodeName(Finding.Code));
				Node.SetChildValue("objectPath", Finding.ObjectPath);
				Node.SetChildValue("classIdentity", Finding.ClassIdentity);
				Node.SetChildValue("declaringType", Finding.DeclaringType);
				Node.SetChildValue("fieldName", Finding.FieldName);
				Node.SetChildValue("storedKind", PropertyKindName(Finding.StoredKind));
				Node.SetChildValue("storedTypeSignature", Finding.StoredTypeSignature);
				Node.SetChildValue("expectedKind", PropertyKindName(Finding.ExpectedKind));
				Node.SetChildValue("expectedTypeSignature", Finding.ExpectedTypeSignature);
				Node.SetChildValue("payloadSize", Finding.PayloadSize);
				Node.SetChildValue("payloadOffset", Finding.PayloadOffset);
				Node.SetChildValue("diagnostic", Finding.Diagnostic);
			}
			FJsonNodeRef CanonicalizationEvidence = Package.AddArray("canonicalizationEvidence");
			for (const auto& Evidence : Record.CanonicalizationEvidence)
			{
				auto Kind = [](EAssetReflectedIdentityKind Value) -> std::string_view {
					switch (Value) { case EAssetReflectedIdentityKind::Class: return "Class";
					case EAssetReflectedIdentityKind::Struct: return "Struct";
					case EAssetReflectedIdentityKind::Enum: return "Enum";
					case EAssetReflectedIdentityKind::Property: return "Property"; }
					return "Class";
				};
				auto Location = [](EAssetSerializedIdentityLocation Value) -> std::string_view {
					switch (Value) { case EAssetSerializedIdentityLocation::PackageHeader: return "PackageHeader";
					case EAssetSerializedIdentityLocation::ObjectRecord: return "ObjectRecord";
					case EAssetSerializedIdentityLocation::Schema: return "Schema";
					case EAssetSerializedIdentityLocation::TypeDescriptor: return "TypeDescriptor"; }
					return "PackageHeader";
				};
				FJsonNodeRef Node = CanonicalizationEvidence.AppendObject();
				Node.SetChildValue("storedIdentity", Evidence.StoredIdentity);
				Node.SetChildValue("currentIdentity", Evidence.CurrentIdentity);
				Node.SetChildValue("kind", Kind(Evidence.Kind));
				Node.SetChildValue("location", Location(Evidence.Location));
				Node.SetChildValue("logicalPath", Evidence.LogicalPath);
			}
			FJsonNodeRef DeprecatedRouteEvidence = Package.AddArray("deprecatedRouteEvidence");
			for (const auto& Evidence : Record.DeprecatedRouteEvidence)
			{
				FJsonNodeRef Node = DeprecatedRouteEvidence.AppendObject();
				Node.SetChildValue("objectPath", Evidence.ObjectPath);
				Node.SetChildValue("declaringType", Evidence.DeclaringType);
				Node.SetChildValue("storedFieldName", Evidence.StoredFieldName);
				Node.SetChildValue("deprecatedPropertyName", Evidence.DeprecatedPropertyName);
				Node.SetChildValue("customVersionGuid", Evidence.CustomVersionGuid.ToString());
				Node.SetChildValue("sourceVersion", Evidence.SourceVersion);
				Node.SetChildValue("deprecatedBefore", Evidence.DeprecatedBefore);
				FJsonNodeRef MigrationTargets = Node.AddArray("migrationTargets");
				for (const auto& Target : Evidence.MigrationTargets)
					MigrationTargets.AppendValue(Target);
			}
		}
		return Document.ToString();
	}
}
