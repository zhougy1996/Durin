#include "AssetForge/ImportTypes.h"
#include "Asset/Load.h"
#include "AssetForge/Extensions/ComponentRegistration.h"
#include "AssetForge/ImportService.h"
#include "AssetForge/Operations/ImportOperation.h"

#include "Misc/Paths.h"
#include "Misc/FileTime.h"
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

		auto FoldAscii(std::string_view Value) -> std::string
		{
			std::string Result(Value);
			std::ranges::transform(Result, Result.begin(), [](const char Character) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
			});
			return Result;
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
			return FoldAscii((Error ? Path.lexically_normal() : Canonical).generic_string());
		}

		auto UpdateFingerprint(FXxHash128Builder& Builder, std::string_view Value) -> void
		{
			const uint64 Size = Value.size();
			Builder.Update(&Size, sizeof(Size));
			Builder.Update(Value.data(), Value.size());
		}

		auto UpdateFingerprint(FXxHash128Builder& Builder, const std::string& Value) -> void
		{
			UpdateFingerprint(Builder, std::string_view(Value));
		}

		template<typename T>
		auto UpdateFingerprint(FXxHash128Builder& Builder, const T& Value) -> void
		{
			static_assert(std::is_trivially_copyable_v<T>);
			Builder.Update(&Value, sizeof(Value));
		}

		class FImportProgressPhaseScope
		{
		public:
			FImportProgressPhaseScope(
				IImportProgressReporter* InReporter,
				EImportPhase InPhase,
				std::string_view InSourceIdentity = "root",
				std::string_view InOutputIdentity = "request")
				: Reporter(InReporter), Phase(InPhase),
				  SourceIdentity(InSourceIdentity), OutputIdentity(InOutputIdentity)
			{
				ReportImportProgress(Reporter, Phase, EImportProgressState::Started,
					SourceIdentity, OutputIdentity);
			}

			~FImportProgressPhaseScope()
			{
				if (!bFinished)
					ReportImportProgress(Reporter, Phase, EImportProgressState::Failed,
						SourceIdentity, OutputIdentity, 0, 0, "Import phase failed.");
			}

			auto Succeed(uint64 CompletedWork = 1, uint64 TotalWork = 1) -> void
			{
				if (bFinished) return;
				bFinished = true;
				ReportImportProgress(Reporter, Phase, EImportProgressState::Succeeded,
					SourceIdentity, OutputIdentity, CompletedWork, TotalWork);
			}

		private:
			IImportProgressReporter* Reporter = nullptr;
			EImportPhase Phase = EImportPhase::Snapshot;
			std::string SourceIdentity;
			std::string OutputIdentity;
			bool bFinished = false;
		};

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

	auto GetImportDiagnosticIdentity(const FImportDiagnostic& Diagnostic) -> std::string
	{
		if (!Diagnostic.Identity.empty()) return Diagnostic.Identity;
		FXxHash128Builder Builder;
		UpdateFingerprint(Builder, Diagnostic.Severity);
		UpdateFingerprint(Builder, Diagnostic.Category);
		UpdateFingerprint(Builder, Diagnostic.Phase);
		UpdateFingerprint(Builder, Diagnostic.SourceIdentity);
		UpdateFingerprint(Builder, Diagnostic.OutputIdentity);
		UpdateFingerprint(Builder, Diagnostic.Message);
		return Builder.Finalize().ToString();
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
			if (Diagnostic.Identity.empty())
				Diagnostic.Identity = GetImportDiagnosticIdentity(Diagnostic);
		}
	}

	auto ReportImportProgress(
		IImportProgressReporter* Reporter,
		EImportPhase Phase,
		EImportProgressState State,
		std::string_view SourceIdentity,
		std::string_view OutputIdentity,
		uint64 CompletedWork,
		uint64 TotalWork,
		std::string_view Message) noexcept -> void
	{
		if (!Reporter) return;
		Reporter->Report({
			.Phase = Phase,
			.State = State,
			.SourceIdentity = std::string(SourceIdentity),
			.OutputIdentity = std::string(OutputIdentity),
			.CompletedWork = CompletedWork,
			.TotalWork = TotalWork,
			.Message = std::string(Message)});
	}

	auto GetImportPhaseLabel(EImportPhase Phase) -> std::string_view
	{
		switch (Phase)
		{
		case EImportPhase::Snapshot: return "Capturing source";
		case EImportPhase::Parse: return "Parsing source";
		case EImportPhase::Plan: return "Planning outputs";
		case EImportPhase::CandidateBuild: return "Building candidates";
		case EImportPhase::Validation: return "Validating";
		case EImportPhase::Publication: return "Publishing";
		case EImportPhase::Restore: return "Restoring";
		}
		return "Importing";
	}

	auto FImportPayload::Finalize(std::string& OutError) -> bool
	{
		if (!IsStableIdentifier(SchemaId) || SchemaVersion == 0)
		{
			OutError = "Import payload schema identity or version is invalid.";
			return false;
		}
		ContentHash = FXxHash128::HashBuffer(std::span<const std::byte>(Bytes));
		OutError.clear();
		return true;
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
		std::string_view Role,
		std::string_view RelativePath,
		bool bOptional) -> bool
	{
		if (!IsStableIdentifier(DeclaringIdentity) || !IsStableIdentifier(StableIdentity)
			|| Role.empty() || RelativePath.empty())
		{
			AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "dependency-discovery",
				StableIdentity, "Provider emitted an invalid relative dependency request.");
			return false;
		}
		if (Requests.size() >= MaximumRequests)
		{
			AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::ResourceLimitExceeded, "dependency-discovery",
				StableIdentity, "Provider dependency-request count limit was exceeded.");
			return false;
		}
		Requests.push_back({
			.DeclaringIdentity = std::string(DeclaringIdentity),
			.StableIdentity = std::string(StableIdentity),
			.Role = std::string(Role),
			.RelativePath = std::string(RelativePath),
			.bOptional = bOptional});
		return true;
	}

	auto FDependencyRequestSink::AddEmbedded(
		std::string_view DeclaringIdentity,
		std::string_view StableIdentity,
		std::string_view Role,
		std::span<const std::byte> Bytes) -> bool
	{
		if (!IsStableIdentifier(DeclaringIdentity) || !IsStableIdentifier(StableIdentity)
			|| Role.empty() || Bytes.empty())
		{
			AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "dependency-discovery",
				StableIdentity, "Provider emitted an invalid embedded dependency request.");
			return false;
		}
		if (Requests.size() >= MaximumRequests || Bytes.size() > MaximumBytesPerSource
			|| Bytes.size() > MaximumEmbeddedBytes - RequestedEmbeddedBytes)
		{
			AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::ResourceLimitExceeded, "dependency-discovery",
				StableIdentity, "Provider embedded dependency budget was exceeded.");
			return false;
		}
		RequestedEmbeddedBytes += Bytes.size();
		Requests.push_back({
			.DeclaringIdentity = std::string(DeclaringIdentity),
			.StableIdentity = std::string(StableIdentity),
			.Role = std::string(Role),
			.EmbeddedBytes = std::vector<std::byte>(Bytes.begin(), Bytes.end())});
		return true;
	}

	auto GetImportPublicationMutex() -> std::mutex&
	{
		static std::mutex Mutex;
		return Mutex;
	}

	struct FSourceSnapshotBuilder::FImpl
	{
		FSourceCaptureLimits Limits;
		std::vector<FSourceSnapshotEntry> Sources;
		std::unordered_map<std::string, size_t> SourceByIdentity;
		std::unordered_map<std::string, std::shared_ptr<const std::vector<std::byte>>> PhysicalBytes;
		std::unordered_set<std::string> ProcessedRequests;
		uint64 AggregateBytes = 0;
		uint64 EmbeddedBytes = 0;
		bool bRootCaptured = false;
		bool bDiscoveryComplete = false;
		bool bFrozen = false;
		static constexpr size_t CancellationChunkBytes = 4ull * 1024ull * 1024ull;

		auto CheckCanceled(
			std::string_view StableIdentity,
			std::vector<FImportDiagnostic>& Diagnostics) const -> bool
		{
			if (!IsImportCancellationRequested()) return false;
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

		auto CaptureBytes(
			std::string StableIdentity,
			std::string Role,
			FSourcePath SourcePath,
			std::span<const std::byte> InputBytes,
			std::string DeclaringIdentity,
			uint32 Depth,
			bool bEmbedded,
			std::vector<FImportDiagnostic>& Diagnostics) -> bool
		{
			if (InputBytes.empty() || InputBytes.size() > Limits.MaximumBytesPerSource
				|| InputBytes.size() > Limits.MaximumAggregateBytes - AggregateBytes
				|| (bEmbedded && InputBytes.size() > Limits.MaximumEmbeddedBytes - EmbeddedBytes)
				|| Sources.size() >= Limits.MaximumSourceCount)
			{
				AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
					EImportDiagnosticCategory::ResourceLimitExceeded, "source-capture",
					StableIdentity, "Captured source byte limit was exceeded.");
				return false;
			}
			if (const auto Existing = SourceByIdentity.find(StableIdentity);
				Existing != SourceByIdentity.end())
			{
				const FSourceSnapshotEntry& Prior = Sources[Existing->second];
				if (Prior.SourcePath != SourcePath
					|| !std::ranges::equal(Prior.GetBytes(), InputBytes))
				{
					AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
						EImportDiagnosticCategory::DuplicateSource, "source-capture",
						StableIdentity, "One stable source identity resolved to different bytes.");
					return false;
				}
				return true;
			}
			auto MutableBytes = std::make_shared<std::vector<std::byte>>(InputBytes.size());
			for (size_t Offset = 0; Offset < InputBytes.size();
				Offset += CancellationChunkBytes)
			{
				if (CheckCanceled(StableIdentity, Diagnostics)) return false;
				const size_t Count = std::min(
					CancellationChunkBytes, InputBytes.size() - Offset);
				std::memcpy(MutableBytes->data() + Offset, InputBytes.data() + Offset, Count);
			}
			std::shared_ptr<const std::vector<std::byte>> Bytes = std::move(MutableBytes);
			FXxHash128 ContentHash;
			if (!HashBytes(*Bytes, StableIdentity, Diagnostics, ContentHash)) return false;
			FSourceSnapshotEntry Entry{
				.StableIdentity = std::move(StableIdentity),
				.Role = std::move(Role),
				.SourcePath = std::move(SourcePath),
				.DeclaringIdentity = std::move(DeclaringIdentity),
				.ContentHash = ContentHash,
				.ByteCount = Bytes->size(),
				.Depth = Depth,
				.bEmbedded = bEmbedded};
			Entry.Bytes = std::move(Bytes);
			AggregateBytes += Entry.ByteCount;
			if (bEmbedded) EmbeddedBytes += Entry.ByteCount;
			SourceByIdentity.emplace(Entry.StableIdentity, Sources.size());
			Sources.push_back(std::move(Entry));
			return true;
		}

		auto CaptureMounted(
			std::string StableIdentity,
			std::string Role,
			FSourcePath SourcePath,
			std::string DeclaringIdentity,
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
			const PathUtilities::FSourcePathResult Resolved =
				PathUtilities::ResolveSourcePath(
					SourcePath.Path, PathUtilities::EPathExistence::RequireFile);
			if (!Resolved)
			{
				AddDiagnostic(Diagnostics,
					bOptional ? EImportDiagnosticSeverity::Warning : EImportDiagnosticSeverity::Error,
					bOptional || !DeclaringIdentity.empty()
						? EImportDiagnosticCategory::MissingDependency
						: EImportDiagnosticCategory::InvalidSource,
					"source-capture", StableIdentity, Resolved.Message);
				return bOptional;
			}
			SourcePath.Path = Resolved.NormalizedVirtualPath;
			const std::string PhysicalIdentity = StablePhysicalIdentity(Resolved.PhysicalPath);
			std::error_code MetadataError;
			const auto CapturedLastWriteTime =
				std::filesystem::last_write_time(Resolved.PhysicalPath, MetadataError);
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
				const uint64 FileSize = std::filesystem::file_size(Resolved.PhysicalPath, Error);
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
				std::ifstream Stream(Resolved.PhysicalPath, std::ios::binary);
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
						StableIdentity, "Failed to read mounted source bytes.");
					return false;
				}
				const uint64 SizeAfter = std::filesystem::file_size(Resolved.PhysicalPath, Error);
				const auto TimeAfter = std::filesystem::last_write_time(Resolved.PhysicalPath, Error);
				if (Error || SizeAfter != FileSize || TimeAfter != CapturedLastWriteTime
					|| MutableBytes->size() != FileSize)
				{
					AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
						EImportDiagnosticCategory::InvalidSource, "source-capture",
						StableIdentity, "Mounted source changed while its snapshot was captured.");
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
				if (Prior.SourcePath != SourcePath
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
				.Role = std::move(Role),
				.SourcePath = std::move(SourcePath),
				.DeclaringIdentity = std::move(DeclaringIdentity),
				.ContentHash = ContentHash,
				.ByteCount = Bytes->size(),
				.LastWriteTime = FileTime::ToStableTicks(CapturedLastWriteTime),
				.Depth = Depth,
				.bEmbedded = false};
			Entry.Bytes = std::move(Bytes);
			SourceByIdentity.emplace(Entry.StableIdentity, Sources.size());
			Sources.push_back(std::move(Entry));
			return true;
		}
	};

	FSourceSnapshotBuilder::FSourceSnapshotBuilder(FSourceCaptureLimits InLimits)
		: Impl(std::make_unique<FImpl>())
	{
		Impl->Limits = InLimits;
	}

	FSourceSnapshotBuilder::~FSourceSnapshotBuilder() = default;
	FSourceSnapshotBuilder::FSourceSnapshotBuilder(FSourceSnapshotBuilder&&) noexcept = default;
	auto FSourceSnapshotBuilder::operator=(FSourceSnapshotBuilder&&) noexcept
		-> FSourceSnapshotBuilder& = default;

	auto FSourceSnapshotBuilder::CaptureRoot(
		const FSourcePath& RootSource,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool
	{
		if (Impl->bRootCaptured || Impl->bFrozen || RootSource.IsEmpty())
		{
			AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "capture-root", "root",
				"Root source capture was requested in an invalid state.");
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
		if (!Impl->CaptureMounted("root", "Root", RootSource, {}, 0, false, OutDiagnostics))
			return false;
		Impl->bRootCaptured = true;
		return true;
	}

	auto FSourceSnapshotBuilder::CaptureRootBytes(
		const FSourcePath& RootSource,
		std::span<const std::byte> Bytes,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool
	{
		if (Impl->bRootCaptured || Impl->bFrozen || RootSource.IsEmpty())
		{
			AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "capture-root", "root",
				"Root byte capture was requested in an invalid state.");
			return false;
		}
		if (!Impl->CaptureBytes(
			"root", "Root", RootSource, Bytes, {}, 0, false, OutDiagnostics)) return false;
		Impl->bRootCaptured = true;
		return true;
	}

	auto FSourceSnapshotBuilder::CaptureDeclaredSource(
		std::string_view StableIdentity,
		std::string_view Role,
		const FSourcePath& Source,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool
	{
		if (!Impl->bRootCaptured || Impl->bFrozen || Source.IsEmpty()
			|| !IsStableIdentifier(StableIdentity) || StableIdentity == "root"
			|| Role.empty())
		{
			AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "capture-declared",
				StableIdentity, "Declared source capture was requested in an invalid state.");
			return false;
		}
		return Impl->CaptureMounted(
			std::string(StableIdentity), std::string(Role), Source, {}, 0, false,
			OutDiagnostics);
	}

	auto FSourceSnapshotBuilder::CaptureDeclaredBytes(
		std::string_view StableIdentity,
		std::string_view Role,
		const FSourcePath& Source,
		std::span<const std::byte> Bytes,
		bool bEmbedded,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool
	{
		if (!Impl->bRootCaptured || Impl->bFrozen
			|| !IsStableIdentifier(StableIdentity) || StableIdentity == "root"
			|| Role.empty() || (!bEmbedded && Source.IsEmpty()))
		{
			AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "capture-declared",
				StableIdentity, "Declared byte capture was requested in an invalid state.");
			return false;
		}
		return Impl->CaptureBytes(
			std::string(StableIdentity), std::string(Role), Source, Bytes,
			"root", 1, bEmbedded, OutDiagnostics);
	}

	auto FSourceSnapshotBuilder::DiscoverSourceDependencies(
		const FComponentLease& Translator,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool
	{
		if (!Impl->bRootCaptured || Impl->bDiscoveryComplete || Impl->bFrozen
			|| !Translator || !Translator.GetSourceTranslator())
		{
			AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "dependency-discovery", {},
				"AssetForge dependency discovery was requested in an invalid state.");
			return false;
		}

		for (uint32 Round = 0; Round <= Impl->Limits.MaximumDependencyDepth; ++Round)
		{
			std::vector<FDependencyRequest> Requests;
			FDependencyRequestSink Sink(
				Requests,
				OutDiagnostics,
				Impl->Limits.MaximumSourceCount,
				Impl->Limits.MaximumBytesPerSource,
				Impl->Limits.MaximumEmbeddedBytes - Impl->EmbeddedBytes);
			auto Invocation = Translator.TryEnter();
			if (!Invocation || !Translator.GetSourceTranslator()->DiscoverDependencies(
				Impl->Sources, Sink, OutDiagnostics) || HasError(OutDiagnostics)) return false;
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
				const std::string RequestKey = std::format("{}|{}|{}|{}|{}|{}", Request.DeclaringIdentity,
					Request.StableIdentity, Request.Role, Request.RelativePath, Request.bOptional,
					FXxHash128::HashBuffer(std::span<const std::byte>(Request.EmbeddedBytes)).ToString());
				if (!Impl->ProcessedRequests.insert(RequestKey).second) continue;

				if (Request.IsEmbedded())
				{
					if (Impl->Sources.size() >= Impl->Limits.MaximumSourceCount
						|| Request.EmbeddedBytes.size() > Impl->Limits.MaximumBytesPerSource
						|| Request.EmbeddedBytes.size()
							> Impl->Limits.MaximumAggregateBytes - Impl->AggregateBytes
						|| Request.EmbeddedBytes.size()
							> Impl->Limits.MaximumEmbeddedBytes - Impl->EmbeddedBytes)
					{
						AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
							EImportDiagnosticCategory::ResourceLimitExceeded, "dependency-capture",
							Request.StableIdentity, "Embedded source byte limit was exceeded.");
						return false;
					}
					if (const auto Existing = Impl->SourceByIdentity.find(Request.StableIdentity);
						Existing != Impl->SourceByIdentity.end())
					{
						if (!std::ranges::equal(
							Impl->Sources[Existing->second].GetBytes(), Request.EmbeddedBytes))
						{
							AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
								EImportDiagnosticCategory::DuplicateSource, "dependency-capture",
								Request.StableIdentity, "Embedded stable identity changed bytes.");
							return false;
						}
						continue;
					}
					auto Bytes = std::make_shared<const std::vector<std::byte>>(
						std::move(Request.EmbeddedBytes));
					FSourceSnapshotEntry Entry{
						.StableIdentity = std::move(Request.StableIdentity),
						.Role = std::move(Request.Role),
						.DeclaringIdentity = Request.DeclaringIdentity,
						.ContentHash = FXxHash128::HashBuffer(std::span<const std::byte>(*Bytes)),
						.ByteCount = Bytes->size(),
						.Depth = Depth,
						.bEmbedded = true};
					Entry.Bytes = std::move(Bytes);
					Impl->AggregateBytes += Entry.ByteCount;
					Impl->EmbeddedBytes += Entry.ByteCount;
					Impl->SourceByIdentity.emplace(Entry.StableIdentity, Impl->Sources.size());
					Impl->Sources.push_back(std::move(Entry));
					bCapturedNewSource = true;
					continue;
				}

				const std::filesystem::path Relative(Request.RelativePath);
				if (Declaring.SourcePath.IsEmpty() || Relative.is_absolute()
					|| Relative.has_root_name()
					|| Request.RelativePath.find(':') != std::string::npos
					|| std::ranges::find(Relative, std::filesystem::path("..")) != Relative.end())
				{
					AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
						EImportDiagnosticCategory::UnsafeDependency, "dependency-capture",
						Request.StableIdentity, "Relative source dependency is unsafe.");
					return false;
				}
				FSourcePath Source{
					.Path = (std::filesystem::path(Declaring.SourcePath.Path).parent_path()
						/ Relative).lexically_normal().generic_string()};
				const size_t CountBefore = Impl->Sources.size();
				if (!Impl->CaptureMounted(std::move(Request.StableIdentity), std::move(Request.Role),
					std::move(Source), Request.DeclaringIdentity, Depth, Request.bOptional,
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
			return std::tie(A.StableIdentity, A.Role, A.SourcePath.Path)
				< std::tie(B.StableIdentity, B.Role, B.SourcePath.Path);
		});
		Snapshot->AggregateByteCount = Impl->AggregateBytes;
		Impl->bFrozen = true;
		return Snapshot;
	}

	auto FSourceSnapshotBuilder::GetCapturedSources() const
		-> std::span<const FSourceSnapshotEntry>
	{
		return Impl->Sources;
	}

	auto FSourceSnapshotBuilder::GetLimits() const -> const FSourceCaptureLimits&
	{
		return Impl->Limits;
	}


}
