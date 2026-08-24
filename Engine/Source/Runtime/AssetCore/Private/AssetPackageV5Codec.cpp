#include "AssetPackageV5Codec.h"

#include "Asset/EditorBulkDataStorage.h"
#include "Asset/PackageTrailer.h"
#include "Asset/PackageV4Reader.h"
#include "Asset/PackageV4Writer.h"

namespace Durin::Asset::Private::DastV5
{
	namespace
	{
		using PackageTrailer::FEntry;
		using PackageTrailer::FInspection;

		auto Error(std::string Message) -> FAssetResult
		{
			return {EAssetError::CorruptFile, std::move(Message)};
		}

		auto SetVersion(std::vector<std::byte>& Bytes, uint32 Value) -> bool
		{
			if (Bytes.size() < sizeof(uint32) * 2) return false;
			std::memcpy(Bytes.data() + sizeof(uint32), &Value, sizeof(Value));
			return true;
		}

		auto MakeEntries(
			const FAssetPackageInspection& Inspection,
			std::vector<FEntry>& OutEntries) -> FAssetResult
		{
			OutEntries.clear();
			std::vector<FEditorBulkDataStorageDescriptor> Descriptors;
			std::string DescriptorError;
			if (!InspectEditorBulkDataStorageDescriptors(
					Inspection, Descriptors, &DescriptorError))
				return Error(std::move(DescriptorError));
			for (const FEditorBulkDataStorageDescriptor& Descriptor : Descriptors)
			{
				if (Descriptor.StorageKind != EEditorBulkDataStorageKind::External) continue;
				OutEntries.push_back({
					.PayloadId = Descriptor.PayloadId,
					.Placement = PackageTrailer::EPlacement::ExternalDabkV1,
					.LogicalByteCount = Descriptor.LogicalByteCount,
					.StoredByteCount = Descriptor.StoredByteCount,
					.ContentHash = Descriptor.ContentHash,
					.ContainerHash = Descriptor.ContainerHash});
			}
			std::ranges::sort(OutEntries, {}, &FEntry::PayloadId);
			for (size_t Index = 1; Index < OutEntries.size(); ++Index)
				if (OutEntries[Index - 1].PayloadId == OutEntries[Index].PayloadId)
					return Error("DAST v5 object stream contains duplicate external payload ids.");
			return {};
		}

		auto PrepareV4(
			std::span<const std::byte> Bytes,
			std::vector<std::byte>& OutV4,
			FAssetPackageInspection* OutInspection = nullptr) -> FAssetResult
		{
			OutV4.clear();
			FInspection Trailer;
			std::string TrailerError;
			if (!PackageTrailer::Inspect(Bytes, Trailer, &TrailerError))
				return Error(std::move(TrailerError));
			if (Trailer.ObjectStreamEnd > Bytes.size())
				return Error("DAST v5 object-stream extent is invalid.");
			OutV4.assign(
				Bytes.begin(), Bytes.begin() + static_cast<size_t>(Trailer.ObjectStreamEnd));
			if (!SetVersion(OutV4, DastV4::Version))
				return Error("DAST v5 object-stream preamble is truncated.");

			FAssetPackageInspection Inspection;
			DastV4::FReaderDiagnostic Diagnostic;
			if (FAssetResult Result = DastV4::InspectPackage(
					OutV4, Inspection, {}, &Diagnostic); !Result)
				return Result;
			std::vector<FEntry> Expected;
			if (FAssetResult Result = MakeEntries(Inspection, Expected); !Result)
				return Result;
			if (Expected != Trailer.Entries)
				return Error(
					"DAST v5 trailer entries disagree with object-stream bulk descriptors.");
			if (OutInspection) *OutInspection = std::move(Inspection);
			return {};
		}

		auto ReadObjectStreamEnd(
			std::span<const std::byte> Bytes,
			uint64 PhysicalSize,
			uint64& OutEnd) -> bool
		{
			OutEnd = 0;
			if (Bytes.size() < 13) return false;
			uint32 SummaryBytes = 0;
			std::memcpy(&SummaryBytes, Bytes.data() + 8, sizeof(SummaryBytes));
			if (SummaryBytes > DastV4::MaximumSummaryBytes
				|| std::to_integer<uint8>(Bytes[12]) != DastV4::RequiredSectionCount)
				return false;
			const uint64 LastEntry = 13ull + SummaryBytes + 4ull * 9ull;
			if (LastEntry + 9 > Bytes.size()) return false;
			uint32 Offset = 0;
			uint32 Length = 0;
			std::memcpy(&Offset, Bytes.data() + LastEntry + 1, sizeof(Offset));
			std::memcpy(&Length, Bytes.data() + LastEntry + 5, sizeof(Length));
			const uint64 End = uint64(Offset) + Length;
			if (End < Offset || End > PackageTrailer::MaximumObjectStreamBytes
				|| End >= PhysicalSize)
				return false;
			OutEnd = End;
			return true;
		}

		auto ReadHeader(
			std::span<const std::byte> Bytes,
			uint64 PackageSize,
			FAssetPackageHeader& OutHeader) -> FAssetResult
		{
			uint64 ObjectStreamEnd = 0;
			if (!ReadObjectStreamEnd(Bytes, PackageSize, ObjectStreamEnd))
				return Error("DAST v5 public header or object-stream extent is invalid.");
			const size_t HeaderByteCount = static_cast<size_t>(
				std::min<uint64>(Bytes.size(), ObjectStreamEnd));
			std::vector<std::byte> HeaderBytes(
				Bytes.begin(), Bytes.begin() + HeaderByteCount);
			SetVersion(HeaderBytes, DastV4::Version);
			DastV4::FValidatedHeader Header;
			DastV4::FReaderDiagnostic Diagnostic;
			if (!DastV4::ReadHeader(
					HeaderBytes, Header, {}, &Diagnostic, ObjectStreamEnd))
				return Error(Diagnostic.Message);
			FAssetPackageHeader Result{
				.AssetClassName = std::move(Header.AssetClass),
				.EntryKind = Header.EntryKind,
				.FormatVersion = Version,
				.ObjectCount = Header.ObjectCount,
				.BytesRead = Header.BytesRead};
			if (!Header.RedirectDestination.empty()
				&& !FAssetPath::TryCreate(
					Header.RedirectDestination, Result.RedirectDestination))
				return Error("The DAST v5 redirect destination path is invalid.");
			for (const std::string& DependencyString : Header.Dependencies)
			{
				FAssetPath Dependency;
				if (!FAssetPath::TryCreate(DependencyString, Dependency))
					return Error("A DAST v5 dependency path is invalid.");
				Result.Dependencies.push_back(std::move(Dependency));
			}
			OutHeader = std::move(Result);
			return {};
		}

		auto Validate(std::span<const std::byte> Bytes) -> FAssetResult
		{
			std::vector<std::byte> V4;
			return PrepareV4(Bytes, V4);
		}

		auto Inspect(
			std::span<const std::byte> Bytes,
			FAssetPackageInspection& OutInspection) -> FAssetResult
		{
			OutInspection = {};
			std::vector<std::byte> V4;
			FAssetPackageInspection Inspection;
			if (FAssetResult Result = PrepareV4(Bytes, V4, &Inspection); !Result)
				return Result;
			Inspection.Header.FormatVersion = Version;
			Inspection.Fingerprint = {
				.FileSize = Bytes.size(),
				.ContentHash = FXxHash128::HashBuffer(Bytes),
				.ReaderVersion = Version};
			OutInspection = std::move(Inspection);
			return {};
		}

		auto ExtractReferences(
			std::span<const std::byte> Bytes,
			const FAssetPath& SourcePackage,
			std::vector<FAssetReferenceEdge>& OutReferences) -> FAssetResult
		{
			std::vector<std::byte> V4;
			if (FAssetResult Result = PrepareV4(Bytes, V4); !Result) return Result;
			DastV4::FReaderDiagnostic Diagnostic;
			return DastV4::ExtractReferences(
				V4, SourcePackage, OutReferences, {}, &Diagnostic);
		}

		auto ProbeCompatibility(
			std::span<const std::byte> Bytes,
			const FAssetPath& PackagePath,
			const FReflectionCompatibilityCatalog& Catalog,
			FAssetPackageCompatibilityRecord& OutRecord,
			FAssetCompatibilityProbeStats* OutStats) -> FAssetResult
		{
			std::vector<std::byte> V4;
			if (FAssetResult Result = PrepareV4(Bytes, V4); !Result) return Result;
			DastV4::FReaderDiagnostic Diagnostic;
			FAssetResult Result = DastV4::ProbeCompatibility(
				V4, PackagePath, Catalog, OutRecord, OutStats, {}, &Diagnostic);
			if (Result)
			{
				OutRecord.FormatVersion = Version;
				OutRecord.Fingerprint = {
					.FileSize = Bytes.size(),
					.ContentHash = FXxHash128::HashBuffer(Bytes),
					.ReaderVersion = Version};
			}
			return Result;
		}

		auto Load(
			std::span<const std::byte> Bytes,
			const FAssetPath& PackagePath,
			DPackage*& OutPackage,
			FAssetLoadReport* OutReport,
			const std::function<FAssetResult(DPackage*)>& OnSkeletonReady,
			const std::function<void(DPackage*)>& OnSkeletonRollback) -> FAssetResult
		{
			OutPackage = nullptr;
			std::vector<std::byte> V4;
			if (FAssetResult Result = PrepareV4(Bytes, V4); !Result) return Result;
			DastV4::FLoadedAssetPackage Loaded;
			DastV4::FReaderDiagnostic Diagnostic;
			FAssetResult Result = DastV4::LoadAssetPackage(
				V4, PackagePath, Loaded, OutReport,
				{.OnSkeletonReady = OnSkeletonReady,
					.OnSkeletonRollback = OnSkeletonRollback}, {}, &Diagnostic);
			if (!Result) return Result;
			OutPackage = Loaded.Release();
			return {};
		}

		auto Write(
			DPackage* Package,
			std::vector<std::byte>& OutBytes,
			EDefaultDeltaMode DeltaMode,
			const FAssetPackageSerializationOptions& Serialization) -> FAssetResult
		{
			std::vector<std::byte> V4;
			DastV4::FWriterDiagnostic Diagnostic;
			if (FAssetResult Result = DastV4::WriteAssetPackage(
					Package, V4,
					{.DeltaMode = DeltaMode, .Serialization = Serialization},
					&Diagnostic); !Result)
				return Result;
			return ConvertV4Package(V4, OutBytes);
		}

		template<typename Operation>
		auto Rewrite(
			std::span<const std::byte> Bytes,
			std::vector<std::byte>& OutBytes,
			Operation&& Apply) -> FAssetResult
		{
			OutBytes.clear();
			std::vector<std::byte> V4;
			if (FAssetResult Result = PrepareV4(Bytes, V4); !Result) return Result;
			std::vector<std::byte> Rewritten;
			if (FAssetResult Result = Apply(V4, Rewritten); !Result) return Result;
			return ConvertV4Package(Rewritten, OutBytes);
		}

		auto RewriteReferences(
			std::span<const std::byte> Bytes,
			std::span<const FAssetRedirectorFixupMapping> Mappings,
			uint64 ExpectedRewriteCount,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			return Rewrite(Bytes, OutBytes, [&](auto V4, auto& Rewritten) {
				return DastV4::RewriteReferences(
					V4, Mappings, ExpectedRewriteCount, Rewritten);
			});
		}

		auto Relocate(
			std::span<const std::byte> Bytes,
			const FAssetPath& DestinationPath,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			return Rewrite(Bytes, OutBytes, [&](auto V4, auto& Rewritten) {
				return DastV4::RelocatePackage(V4, DestinationPath, Rewritten);
			});
		}

		auto WriteRedirector(
			const FAssetPath& SourcePath,
			const FAssetPath& DestinationPath,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::vector<std::byte> V4;
			if (FAssetResult Result = DastV4::WriteRedirectorPackage(
					SourcePath, DestinationPath, V4); !Result)
				return Result;
			return ConvertV4Package(V4, OutBytes);
		}
	}

	auto ConvertV4Package(
		std::span<const std::byte> V4Bytes,
		std::vector<std::byte>& OutV5Bytes) -> FAssetResult
	{
		OutV5Bytes.clear();
		FAssetPackageInspection Inspection;
		DastV4::FReaderDiagnostic Diagnostic;
		if (FAssetResult Result = DastV4::InspectPackage(
				V4Bytes, Inspection, {}, &Diagnostic); !Result)
			return Result;
		std::vector<FEntry> Entries;
		if (FAssetResult Result = MakeEntries(Inspection, Entries); !Result)
			return Result;
		std::vector<std::byte> Detached;
		std::string TrailerError;
		if (!PackageTrailer::Build(
				Entries, V4Bytes.size(), Detached, &TrailerError))
			return Error(std::move(TrailerError));
		std::vector<std::byte> Candidate(V4Bytes.begin(), V4Bytes.end());
		if (!SetVersion(Candidate, Version))
			return Error("DAST v4 source preamble is truncated.");
		Candidate.insert(Candidate.end(), Detached.begin(), Detached.end());
		if (FAssetResult Result = Validate(Candidate); !Result) return Result;
		OutV5Bytes = std::move(Candidate);
		return {};
	}

	auto GetCodec() -> const FAssetPackageCodec&
	{
		static const FAssetPackageCodec Codec{
			.CodecId = "dast-v5",
			.FormatVersion = Version,
			.bCanRead = true,
			.bCanWrite = true,
			.bCanMutate = true,
			.ReadHeader = &ReadHeader,
			.Validate = &Validate,
			.Inspect = &Inspect,
			.ExtractReferences = &ExtractReferences,
			.ProbeCompatibility = &ProbeCompatibility,
			.Load = &Load,
			.Write = &Write,
			.RewriteReferences = &RewriteReferences,
			.Relocate = &Relocate,
			.WriteRedirector = &WriteRedirector};
		return Codec;
	}
}
