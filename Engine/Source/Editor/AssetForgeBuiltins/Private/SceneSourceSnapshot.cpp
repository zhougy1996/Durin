#include "SceneSourceSnapshot.h"
#include "Asset/Load.h"

#include "Misc/Paths.h"
#include "Misc/StringHelper.h"
#include "Profiling/Profiling.h"

namespace Durin::AssetForge
{
	namespace
	{
		auto AddDiagnostic(
			std::vector<FImportDiagnostic>& Diagnostics,
			EImportDiagnosticSeverity Severity,
			EImportDiagnosticCategory Category,
			std::string_view Phase,
			std::string_view SourceIdentity,
			std::string_view Message) -> void
		{
			Diagnostics.push_back({
				.Severity = Severity,
				.Category = Category,
				.Phase = std::string(Phase),
				.SourceIdentity = std::string(SourceIdentity),
				.OutputIdentity = "request",
				.Message = std::string(Message)});
		}

		auto IsStableIdentifier(std::string_view Value) -> bool
		{
			return !Value.empty() && Value.size() <= 256
				&& std::ranges::all_of(Value, [](const char Character) {
					return std::isalnum(static_cast<unsigned char>(Character))
						|| Character == '.' || Character == '_' || Character == '-'
						|| Character == ':' || Character == '/';
				});
		}

		auto HasError(std::span<const FImportDiagnostic> Diagnostics) -> bool
		{
			return std::ranges::any_of(Diagnostics, [](const FImportDiagnostic& Diagnostic) {
				return Diagnostic.Severity == EImportDiagnosticSeverity::Error;
			});
		}

		auto StablePhysicalIdentity(const std::filesystem::path& Path) -> std::string
		{
			std::error_code Error;
			const std::filesystem::path Canonical = std::filesystem::weakly_canonical(Path, Error);
			return StringUtils::FoldAscii(
				(Error ? Path.lexically_normal() : Canonical).generic_string());
		}

		class FDiagnosticFinalizer
		{
		public:
			FDiagnosticFinalizer(
				std::vector<FImportDiagnostic>& InDiagnostics,
				std::string_view InPhase,
				std::string_view InSourceIdentity = "root",
				std::string_view InOutputIdentity = "request")
				: Diagnostics(InDiagnostics), Phase(InPhase),
				  SourceIdentity(InSourceIdentity), OutputIdentity(InOutputIdentity) {}
			~FDiagnosticFinalizer()
			{
				FinalizeImportDiagnostics(
					Diagnostics, Phase, SourceIdentity, OutputIdentity);
			}
		private:
			std::vector<FImportDiagnostic>& Diagnostics;
			std::string Phase;
			std::string SourceIdentity;
			std::string OutputIdentity;
		};
	}

	auto FinalizeImportDiagnostics(
		std::vector<FImportDiagnostic>& Diagnostics,
		std::string_view DefaultPhase,
		std::string_view DefaultSourceIdentity,
		std::string_view DefaultOutputIdentity) -> void
	{
		for (FImportDiagnostic& Diagnostic : Diagnostics)
		{
			if (Diagnostic.Phase.empty()) Diagnostic.Phase = DefaultPhase;
			if (Diagnostic.SourceIdentity.empty())
				Diagnostic.SourceIdentity = DefaultSourceIdentity;
			if (Diagnostic.OutputIdentity.empty())
				Diagnostic.OutputIdentity = DefaultOutputIdentity;
		}
	}

	auto FSourceSnapshotEntry::GetBytes() const -> std::span<const std::byte>
	{
		return Bytes ? std::span<const std::byte>(*Bytes) : std::span<const std::byte>{};
	}

	auto FSourceSnapshot::FindSource(std::string_view StableIdentity) const -> const FSourceSnapshotEntry*
	{
		const auto It = std::ranges::find(Sources, StableIdentity, &FSourceSnapshotEntry::StableIdentity);
		return It == Sources.end() ? nullptr : &*It;
	}

	auto FDependencyRequestSink::AddRelative(
		std::string_view DeclaringIdentity,
		std::string_view StableIdentity,
		std::string_view RelativePath,
		bool bOptional) -> bool
	{
		if (!IsStableIdentifier(DeclaringIdentity) || !IsStableIdentifier(StableIdentity)
			|| RelativePath.empty())
		{
			AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "dependency-discovery",
				StableIdentity, "Scene discovery emitted an invalid relative dependency request.");
			return false;
		}
		if (Requests.size() >= MaximumRequests)
		{
			AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::ResourceLimitExceeded, "dependency-discovery",
				StableIdentity, "Scene dependency-request count limit was exceeded.");
			return false;
		}
		Requests.push_back({
			.DeclaringIdentity = std::string(DeclaringIdentity),
			.StableIdentity = std::string(StableIdentity),
			.RelativePath = std::string(RelativePath),
			.bOptional = bOptional});
		return true;
	}

	struct FSourceSnapshotBuilder::FImpl
	{
		FSourceCaptureLimits Limits;
		std::vector<FSourceSnapshotEntry> Sources;
		std::unordered_map<std::string, size_t> SourceByIdentity;
		std::unordered_map<std::string, std::shared_ptr<const std::vector<std::byte>>> PhysicalBytes;
		std::unordered_set<std::string> ProcessedRequests;
		uint64 AggregateBytes = 0;
		bool bRootCaptured = false;
		bool bDiscoveryComplete = false;
		bool bFrozen = false;
		std::function<bool()> IsCancellationRequested;
		static constexpr size_t CancellationChunkBytes = 4ull * 1024ull * 1024ull;

		auto CheckCanceled(
			std::string_view StableIdentity,
			std::vector<FImportDiagnostic>& Diagnostics) const -> bool
		{
			if (!IsCancellationRequested || !IsCancellationRequested()) return false;
			AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::Canceled, "source-capture",
				StableIdentity, "Import source capture was canceled.");
			return true;
		}

		auto HashBytes(
			std::span<const std::byte> Bytes,
			std::string_view StableIdentity,
			std::vector<FImportDiagnostic>& Diagnostics,
			FXxHash128& OutHash) const -> bool
		{
			FXxHash128Builder Builder;
			for (size_t Offset = 0; Offset < Bytes.size(); Offset += CancellationChunkBytes)
			{
				if (CheckCanceled(StableIdentity, Diagnostics)) return false;
				Builder.Update(Bytes.subspan(Offset,
					std::min(CancellationChunkBytes, Bytes.size() - Offset)));
			}
			if (CheckCanceled(StableIdentity, Diagnostics)) return false;
			OutHash = Builder.Finalize();
			return true;
		}

		auto CapturePhysical(
			std::string StableIdentity,
			std::string Filename,
			const std::filesystem::path& PhysicalPath,
			uint32 Depth,
			bool bOptional,
			std::vector<FImportDiagnostic>& Diagnostics) -> bool
		{
			if (Sources.size() >= Limits.MaximumSourceCount)
			{
				AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
					EImportDiagnosticCategory::ResourceLimitExceeded, "source-capture",
					StableIdentity, "Import source-count limit was exceeded.");
				return false;
			}
			if (!std::filesystem::is_regular_file(PhysicalPath))
			{
				AddDiagnostic(Diagnostics,
					bOptional ? EImportDiagnosticSeverity::Warning : EImportDiagnosticSeverity::Error,
					bOptional || Depth != 0
						? EImportDiagnosticCategory::MissingDependency
						: EImportDiagnosticCategory::InvalidSource,
					"source-capture", StableIdentity,
					"Source file does not exist or is not a regular file.");
				return bOptional;
			}
			const std::string PhysicalIdentity = StablePhysicalIdentity(PhysicalPath);
			std::error_code MetadataError;
			const auto CapturedLastWriteTime =
				std::filesystem::last_write_time(PhysicalPath, MetadataError);
			if (MetadataError)
			{
				AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
					EImportDiagnosticCategory::InvalidSource, "source-capture",
					StableIdentity, MetadataError.message());
				return false;
			}
			std::shared_ptr<const std::vector<std::byte>> Bytes;
			if (const auto Existing = PhysicalBytes.find(PhysicalIdentity);
				Existing != PhysicalBytes.end())
			{
				Bytes = Existing->second;
			}
			else
			{
				std::error_code Error;
				const uint64 FileSize = std::filesystem::file_size(PhysicalPath, Error);
				if (Error || FileSize > Limits.MaximumBytesPerSource
					|| FileSize > Limits.MaximumAggregateBytes - AggregateBytes)
				{
					AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
						EImportDiagnosticCategory::ResourceLimitExceeded, "source-capture",
						StableIdentity, Error ? Error.message() : "Import source byte limit was exceeded.");
					return false;
				}
				auto MutableBytes = std::make_shared<std::vector<std::byte>>(
					static_cast<size_t>(FileSize));
				std::ifstream Stream(PhysicalPath, std::ios::binary);
				bool bRead = Stream.is_open();
				for (size_t Offset = 0; bRead && Offset < MutableBytes->size();
					Offset += CancellationChunkBytes)
				{
					if (CheckCanceled(StableIdentity, Diagnostics)) return false;
					const size_t Count = std::min(
						CancellationChunkBytes, MutableBytes->size() - Offset);
					Stream.read(reinterpret_cast<char*>(MutableBytes->data() + Offset),
						static_cast<std::streamsize>(Count));
					bRead = Stream.gcount() == static_cast<std::streamsize>(Count);
				}
				if (!bRead)
				{
					AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
						EImportDiagnosticCategory::InvalidSource, "source-capture",
						StableIdentity, "Failed to read source bytes.");
					return false;
				}
				const uint64 SizeAfter = std::filesystem::file_size(PhysicalPath, Error);
				const auto TimeAfter = std::filesystem::last_write_time(PhysicalPath, Error);
				if (Error || SizeAfter != FileSize || TimeAfter != CapturedLastWriteTime
					|| MutableBytes->size() != FileSize)
				{
					AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
						EImportDiagnosticCategory::InvalidSource, "source-capture",
						StableIdentity, "Source file changed while its snapshot was captured.");
					return false;
				}
				Bytes = std::move(MutableBytes);
				PhysicalBytes.emplace(PhysicalIdentity, Bytes);
				AggregateBytes += FileSize;
			}
			if (const auto Existing = SourceByIdentity.find(StableIdentity);
				Existing != SourceByIdentity.end())
			{
				const FSourceSnapshotEntry& Prior = Sources[Existing->second];
				if (Prior.Filename != Filename
					|| !std::ranges::equal(Prior.GetBytes(), *Bytes))
				{
					AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
						EImportDiagnosticCategory::DuplicateSource, "source-capture",
						StableIdentity, "One stable source identity resolved to different bytes.");
					return false;
				}
				return true;
			}
			FXxHash128 ContentHash;
			if (!HashBytes(*Bytes, StableIdentity, Diagnostics, ContentHash)) return false;
			FSourceSnapshotEntry Entry{
				.StableIdentity = std::move(StableIdentity),
				.Filename = std::move(Filename),
				.ContentHash = ContentHash,
				.ByteCount = Bytes->size(),
				.Depth = Depth};
			Entry.Bytes = std::move(Bytes);
			SourceByIdentity.emplace(Entry.StableIdentity, Sources.size());
			Sources.push_back(std::move(Entry));
			return true;
		}

		auto CaptureFilename(
			std::string StableIdentity,
			std::string Filename,
			uint32 Depth,
			bool bOptional,
			std::vector<FImportDiagnostic>& Diagnostics) -> bool
		{
			const std::filesystem::path PhysicalPath =
				std::filesystem::absolute(Filename).lexically_normal();
			return CapturePhysical(std::move(StableIdentity),
				PhysicalPath.generic_string(), PhysicalPath, Depth, bOptional, Diagnostics);
		}
	};

	FSourceSnapshotBuilder::FSourceSnapshotBuilder(
		std::function<bool()> IsCancellationRequested,
		FSourceCaptureLimits InLimits)
		: Impl(std::make_unique<FImpl>())
	{
		Impl->Limits = InLimits;
		Impl->IsCancellationRequested = std::move(IsCancellationRequested);
	}

	FSourceSnapshotBuilder::~FSourceSnapshotBuilder() = default;
	FSourceSnapshotBuilder::FSourceSnapshotBuilder(FSourceSnapshotBuilder&&) noexcept = default;
	auto FSourceSnapshotBuilder::operator=(FSourceSnapshotBuilder&&) noexcept
		-> FSourceSnapshotBuilder& = default;

	auto FSourceSnapshotBuilder::CaptureRootFilename(
		std::string_view RootFilename,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool
	{
		if (Impl->bRootCaptured || Impl->bFrozen || RootFilename.empty())
		{
			AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "capture-root", "root",
				"Root filename capture was requested in an invalid state.");
			return false;
		}
		if (Impl->Limits.MaximumSourceCount == 0
			|| Impl->Limits.MaximumBytesPerSource == 0
			|| Impl->Limits.MaximumAggregateBytes == 0)
		{
			AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::ResourceLimitExceeded, "capture-root", "root",
				"Source capture limits must be non-zero.");
			return false;
		}
		if (!Impl->CaptureFilename(
			"root", std::string(RootFilename), 0, false, OutDiagnostics)) return false;
		Impl->bRootCaptured = true;
		return true;
	}

	auto FSourceSnapshotBuilder::DiscoverSourceDependencies(
		const FDependencyDiscovery& Discovery,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool
	{
		if (!Impl->bRootCaptured || Impl->bDiscoveryComplete || Impl->bFrozen
			|| !Discovery)
		{
			AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "dependency-discovery", {},
				"Source dependency discovery was requested in an invalid state.");
			return false;
		}

		for (uint32 Round = 0; Round <= Impl->Limits.MaximumDependencyDepth; ++Round)
		{
			std::vector<FDependencyRequest> Requests;
			FDependencyRequestSink Sink(
				Requests, OutDiagnostics, Impl->Limits.MaximumSourceCount);
			if (!Discovery(Impl->Sources, Sink, OutDiagnostics)
				|| HasError(OutDiagnostics)) return false;
			bool bCapturedNewSource = false;
			for (FDependencyRequest& Request : Requests)
			{
				const auto DeclaringIt = Impl->SourceByIdentity.find(Request.DeclaringIdentity);
				if (DeclaringIt == Impl->SourceByIdentity.end())
				{
					AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
						EImportDiagnosticCategory::InvalidRequest, "dependency-discovery",
						Request.StableIdentity, "Dependency declaring source was not captured.");
					return false;
				}
				const FSourceSnapshotEntry& Declaring = Impl->Sources[DeclaringIt->second];
				const uint32 Depth = Declaring.Depth + 1;
				if (Depth > Impl->Limits.MaximumDependencyDepth)
				{
					AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
						EImportDiagnosticCategory::ResourceLimitExceeded, "dependency-discovery",
						Request.StableIdentity, "Import dependency depth limit was exceeded.");
					return false;
				}
				const std::string RequestKey = std::format("{}|{}|{}|{}",
					Request.DeclaringIdentity, Request.StableIdentity,
					Request.RelativePath, Request.bOptional);
				if (!Impl->ProcessedRequests.insert(RequestKey).second) continue;

				const std::filesystem::path Relative(Request.RelativePath);
				if (Declaring.Filename.empty() || Relative.is_absolute()
					|| Relative.has_root_name()
					|| Request.RelativePath.find(':') != std::string::npos
					|| std::ranges::find(Relative, std::filesystem::path("..")) != Relative.end())
				{
					AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
						EImportDiagnosticCategory::UnsafeDependency, "dependency-capture",
						Request.StableIdentity, "Relative source dependency is unsafe.");
					return false;
				}
				const size_t CountBefore = Impl->Sources.size();
				const std::filesystem::path DeclaringPhysical =
					std::filesystem::absolute(Declaring.Filename).lexically_normal();
				const std::filesystem::path DependencyPhysical =
					(DeclaringPhysical.parent_path()
						/ Relative).lexically_normal();
				std::string Filename = DependencyPhysical.generic_string();
				if (!Impl->CaptureFilename(std::move(Request.StableIdentity),
					std::move(Filename), Depth, Request.bOptional,
					OutDiagnostics)) return false;
				bCapturedNewSource = bCapturedNewSource || Impl->Sources.size() != CountBefore;
			}
			if (!bCapturedNewSource)
			{
				Impl->bDiscoveryComplete = true;
				return true;
			}
		}
		AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
			EImportDiagnosticCategory::ResourceLimitExceeded, "dependency-discovery", {},
			"Dependency discovery did not converge within the depth limit.");
		return false;
	}

	auto FSourceSnapshotBuilder::Freeze(
		std::vector<FImportDiagnostic>& OutDiagnostics) -> std::shared_ptr<const FSourceSnapshot>
	{
		if (!Impl->bRootCaptured || !Impl->bDiscoveryComplete || Impl->bFrozen)
		{
			AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "snapshot-freeze", {},
				"Source snapshot cannot be frozen in its current state.");
			return {};
		}
		auto Snapshot = std::make_shared<FSourceSnapshot>();
		Snapshot->Sources = Impl->Sources;
		std::ranges::sort(Snapshot->Sources, [](const FSourceSnapshotEntry& A,
			const FSourceSnapshotEntry& B) {
			return std::tie(A.StableIdentity, A.Filename)
				< std::tie(B.StableIdentity, B.Filename);
		});
		Impl->bFrozen = true;
		return Snapshot;
	}


}
