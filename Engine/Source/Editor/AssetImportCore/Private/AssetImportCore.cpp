#include "AssetImportCore.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::AssetImport
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

	auto FImportPayload::Finalize(std::string& OutError) -> bool
	{
		if (!IsStableIdentifier(SchemaId) || SchemaVersion == 0)
		{
			OutError = "Import payload schema identity or version is invalid.";
			return false;
		}
		ContentHash = FXxHash128::HashBuffer(std::span<const uint8>(Bytes));
		OutError.clear();
		return true;
	}

	auto FSourceSnapshotEntry::GetBytes() const -> std::span<const uint8>
	{
		return Bytes ? std::span<const uint8>(*Bytes) : std::span<const uint8>{};
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
		std::span<const uint8> Bytes) -> bool
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
			.EmbeddedBytes = std::vector<uint8>(Bytes.begin(), Bytes.end())});
		return true;
	}

	struct FProviderLeaseState
	{
		std::shared_ptr<IImportProvider> Provider;
		std::string ProviderId;
		uint32 ContractVersion = 0;
	};

	FProviderLease::FProviderLease() = default;
	FProviderLease::~FProviderLease() = default;
	FProviderLease::FProviderLease(const FProviderLease&) = default;
	FProviderLease::FProviderLease(FProviderLease&&) noexcept = default;
	auto FProviderLease::operator=(const FProviderLease&) -> FProviderLease& = default;
	auto FProviderLease::operator=(FProviderLease&&) noexcept -> FProviderLease& = default;

	auto FProviderLease::GetProvider() const -> const IImportProvider*
	{
		return State ? State->Provider.get() : nullptr;
	}

	auto FProviderLease::GetProviderId() const -> std::string_view
	{
		return State ? std::string_view(State->ProviderId) : std::string_view{};
	}

	auto FProviderLease::GetContractVersion() const -> uint32
	{
		return State ? State->ContractVersion : 0;
	}

	struct FProviderRegistry::FImpl
	{
		std::map<std::string, std::shared_ptr<FProviderLeaseState>, std::less<>> Providers;
		std::map<std::string, std::weak_ptr<FProviderLeaseState>, std::less<>> RetiredProviders;
		uint64 Revision = 1;
	};

	FProviderRegistry::FProviderRegistry()
		: Impl(std::make_unique<FImpl>())
	{
	}

	FProviderRegistry::~FProviderRegistry() = default;

	auto FProviderRegistry::Register(
		std::shared_ptr<IImportProvider> Provider,
		std::string& OutError) -> bool
	{
		if (!Provider || !IsStableIdentifier(Provider->GetProviderId())
			|| Provider->GetContractVersion() == 0)
		{
			OutError = "Import provider identity or contract version is invalid.";
			return false;
		}
		const std::string Id(Provider->GetProviderId());
		const uint32 ContractVersion = Provider->GetContractVersion();
		if (const auto Retired = Impl->RetiredProviders.find(Id);
			Retired != Impl->RetiredProviders.end())
		{
			if (!Retired->second.expired())
			{
				OutError = std::format(
					"Import provider {} still has outstanding plan or candidate leases.", Id);
				return false;
			}
			Impl->RetiredProviders.erase(Retired);
		}
		if (Impl->Providers.contains(Id))
		{
			OutError = std::format("Import provider {} is already registered.", Id);
			return false;
		}
		Impl->Providers.emplace(Id, std::make_shared<FProviderLeaseState>(FProviderLeaseState{
			.Provider = std::move(Provider),
			.ProviderId = Id,
			.ContractVersion = ContractVersion}));
		++Impl->Revision;
		OutError.clear();
		return true;
	}

	auto FProviderRegistry::Unregister(std::string_view ProviderId) -> bool
	{
		const auto It = Impl->Providers.find(ProviderId);
		if (It == Impl->Providers.end()) return false;
		Impl->RetiredProviders[It->first] = It->second;
		Impl->Providers.erase(It);
		++Impl->Revision;
		return true;
	}

	auto FProviderRegistry::Find(std::string_view ProviderId) const -> FProviderLease
	{
		const auto It = Impl->Providers.find(ProviderId);
		return It == Impl->Providers.end() ? FProviderLease{} : FProviderLease(It->second);
	}

	auto FProviderRegistry::FindMatching(
		const FImportSourceRecognition& Source) const -> std::vector<FProviderLease>
	{
		std::vector<FProviderLease> Result;
		for (const auto& [Id, State] : Impl->Providers)
		{
			(void)Id;
			if (State->Provider->CanImport(Source)) Result.emplace_back(FProviderLease(State));
		}
		return Result;
	}

	auto FProviderRegistry::GetRevision() const -> uint64
	{
		return Impl->Revision;
	}

	auto FProviderRegistry::GetOutstandingLeaseCount(std::string_view ProviderId) const -> uint64
	{
		const auto Active = Impl->Providers.find(ProviderId);
		if (Active != Impl->Providers.end())
			return Active->second.use_count() > 0 ? Active->second.use_count() - 1 : 0;
		const auto Retired = Impl->RetiredProviders.find(ProviderId);
		if (Retired == Impl->RetiredProviders.end()) return 0;
		const std::shared_ptr<FProviderLeaseState> State = Retired->second.lock();
		return State && State.use_count() > 0 ? State.use_count() - 1 : 0;
	}

	auto GetProviderRegistry() -> FProviderRegistry&
	{
		static FProviderRegistry Registry;
		return Registry;
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
		std::unordered_map<std::string, std::shared_ptr<const std::vector<uint8>>> PhysicalBytes;
		std::unordered_set<std::string> ProcessedRequests;
		uint64 AggregateBytes = 0;
		uint64 EmbeddedBytes = 0;
		bool bRootCaptured = false;
		bool bDiscoveryComplete = false;
		bool bFrozen = false;

		auto CaptureBytes(
			std::string StableIdentity,
			std::string Role,
			FSourcePath SourcePath,
			std::span<const uint8> InputBytes,
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
			auto Bytes = std::make_shared<const std::vector<uint8>>(
				InputBytes.begin(), InputBytes.end());
			FSourceSnapshotEntry Entry{
				.StableIdentity = std::move(StableIdentity),
				.Role = std::move(Role),
				.SourcePath = std::move(SourcePath),
				.DeclaringIdentity = std::move(DeclaringIdentity),
				.ContentHash = FXxHash128::HashBuffer(std::span<const uint8>(*Bytes)),
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
			std::shared_ptr<const std::vector<uint8>> Bytes;
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
				const auto LastWriteTime = std::filesystem::last_write_time(Resolved.PhysicalPath, Error);
				if (Error)
				{
					AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
						EImportDiagnosticCategory::InvalidSource, "source-capture",
						StableIdentity, Error.message());
					return false;
				}
				auto MutableBytes = std::make_shared<std::vector<uint8>>();
				if (!FFileHelper::LoadFileToArray(*MutableBytes, Resolved.PhysicalPath.generic_string()))
				{
					AddDiagnostic(Diagnostics, EImportDiagnosticSeverity::Error,
						EImportDiagnosticCategory::InvalidSource, "source-capture",
						StableIdentity, "Failed to read mounted source bytes.");
					return false;
				}
				const uint64 SizeAfter = std::filesystem::file_size(Resolved.PhysicalPath, Error);
				const auto TimeAfter = std::filesystem::last_write_time(Resolved.PhysicalPath, Error);
				if (Error || SizeAfter != FileSize || TimeAfter != LastWriteTime
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
			FSourceSnapshotEntry Entry{
				.StableIdentity = std::move(StableIdentity),
				.Role = std::move(Role),
				.SourcePath = std::move(SourcePath),
				.DeclaringIdentity = std::move(DeclaringIdentity),
				.ContentHash = FXxHash128::HashBuffer(std::span<const uint8>(*Bytes)),
				.ByteCount = Bytes->size(),
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
		std::span<const uint8> Bytes,
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
		std::span<const uint8> Bytes,
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

	auto FSourceSnapshotBuilder::DiscoverDependencies(
		const FProviderLease& Provider,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool
	{
		if (!Impl->bRootCaptured || Impl->bDiscoveryComplete || Impl->bFrozen
			|| !Provider || !Provider.GetProvider())
		{
			AddDiagnostic(OutDiagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "dependency-discovery", {},
				"Dependency discovery was requested in an invalid state.");
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
			if (!Provider.GetProvider()->DiscoverDependencies(
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
					FXxHash128::HashBuffer(std::span<const uint8>(Request.EmbeddedBytes)).ToString());
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
					auto Bytes = std::make_shared<const std::vector<uint8>>(std::move(Request.EmbeddedBytes));
					FSourceSnapshotEntry Entry{
						.StableIdentity = std::move(Request.StableIdentity),
						.Role = std::move(Request.Role),
						.DeclaringIdentity = Request.DeclaringIdentity,
						.ContentHash = FXxHash128::HashBuffer(std::span<const uint8>(*Bytes)),
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

	auto FImportPlanBuilder::AddOutput(FImportOutputPreview Output) -> void
	{
		Outputs.push_back(std::move(Output));
	}

	auto FImportPlanBuilder::AddTargetPrecondition(
		FImportTargetPrecondition Precondition) -> void
	{
		TargetPreconditions.push_back(std::move(Precondition));
	}

	auto BuildImportPlan(
		const FProviderLease& Provider,
		std::shared_ptr<const FSourceSnapshot> Snapshot,
		const FImportPayload& Settings,
		uint64 ProviderRegistryRevision,
		std::span<const FImportDiagnostic> PriorDiagnostics,
		IImportProgressReporter* Progress) -> FImportPlanResult
	{
		FImportPlanResult Result;
		Result.Diagnostics.assign(PriorDiagnostics.begin(), PriorDiagnostics.end());
		ReportImportProgress(Progress, EImportPhase::Parse, EImportProgressState::Started);
		if (!Provider || !Provider.GetProvider() || !Snapshot)
		{
			Result.Message = "Import planning requires a provider lease and frozen snapshot.";
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "parse", "root", Result.Message);
			FinalizeImportDiagnostics(Result.Diagnostics, "parse");
			ReportImportProgress(Progress, EImportPhase::Parse,
				EImportProgressState::Failed, "root", "request", 0, 0, Result.Message);
			return Result;
		}
		if (!IsStableIdentifier(Settings.SchemaId) || Settings.SchemaVersion == 0
			|| Settings.ContentHash
				!= FXxHash128::HashBuffer(std::span<const uint8>(Settings.Bytes)))
		{
			Result.Message = "Import planning requires finalized normalized settings.";
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "plan", {}, Result.Message);
			FinalizeImportDiagnostics(Result.Diagnostics, "plan");
			ReportImportProgress(Progress, EImportPhase::Parse,
				EImportProgressState::Failed, "root", "request", 0, 0, Result.Message);
			return Result;
		}
		FImportPlanBuilder Builder;
		if (!Provider.GetProvider()->Plan(*Snapshot, Settings, Builder, Result.Diagnostics)
			|| HasError(Result.Diagnostics))
		{
			Result.Message = Result.Diagnostics.empty()
				? "Import provider planning failed." : Result.Diagnostics.back().Message;
			if (Result.Diagnostics.empty())
				AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
					EImportDiagnosticCategory::ProviderFailure, "parse", "root", Result.Message);
			FinalizeImportDiagnostics(Result.Diagnostics, "parse");
			ReportImportProgress(Progress, EImportPhase::Parse,
				EImportProgressState::Failed, "root", "request", 0, 0, Result.Message);
			return Result;
		}
		FinalizeImportDiagnostics(Result.Diagnostics, "parse");
		ReportImportProgress(Progress, EImportPhase::Parse,
			EImportProgressState::Succeeded, "root", "request", 1, 1);
		ReportImportProgress(Progress, EImportPhase::Plan, EImportProgressState::Started);
		std::ranges::sort(Builder.Outputs, [](const FImportOutputPreview& A,
			const FImportOutputPreview& B) {
			return std::tuple{A.StableIdentity, A.Role, A.AssetPath.GetView()}
				< std::tuple{B.StableIdentity, B.Role, B.AssetPath.GetView()};
		});
		std::ranges::sort(Builder.TargetPreconditions, [](const FImportTargetPrecondition& A,
			const FImportTargetPrecondition& B) {
			return A.AssetPath.GetView() < B.AssetPath.GetView();
		});
		std::unordered_set<std::string> Identities;
		std::unordered_set<FAssetPath> Paths;
		if (Builder.Outputs.empty())
		{
			Result.Message = "Import provider emitted no output previews.";
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidPlan, "plan", {}, Result.Message);
			FinalizeImportDiagnostics(Result.Diagnostics, "plan");
			ReportImportProgress(Progress, EImportPhase::Plan,
				EImportProgressState::Failed, "root", "request", 0, 0, Result.Message);
			return Result;
		}
		for (const FImportOutputPreview& Output : Builder.Outputs)
		{
			if (!IsStableIdentifier(Output.StableIdentity) || Output.Role.empty()
				|| !Output.AssetPath.IsValid() || Output.AssetClassName.empty()
				|| !Identities.insert(Output.StableIdentity).second
				|| !Paths.insert(Output.AssetPath).second)
			{
				Result.Message = "Import provider emitted an invalid or duplicate output preview.";
				AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
					EImportDiagnosticCategory::InvalidPlan, "plan", {}, Result.Message);
				FinalizeImportDiagnostics(Result.Diagnostics, "plan");
				ReportImportProgress(Progress, EImportPhase::Plan,
					EImportProgressState::Failed, "root", Output.StableIdentity, 0, 0,
					Result.Message);
				return Result;
			}
		}

		FXxHash128Builder Fingerprint;
		UpdateFingerprint(Fingerprint, Provider.GetProviderId());
		UpdateFingerprint(Fingerprint, Provider.GetContractVersion());
		UpdateFingerprint(Fingerprint, Settings.SchemaId);
		UpdateFingerprint(Fingerprint, Settings.SchemaVersion);
		UpdateFingerprint(Fingerprint, Settings.ContentHash.HashLow);
		UpdateFingerprint(Fingerprint, Settings.ContentHash.HashHigh);
		UpdateFingerprint(Fingerprint, ProviderRegistryRevision);
		for (const FSourceSnapshotEntry& Source : Snapshot->GetSources())
		{
			UpdateFingerprint(Fingerprint, Source.StableIdentity);
			UpdateFingerprint(Fingerprint, Source.Role);
			UpdateFingerprint(Fingerprint, Source.SourcePath.Path);
			UpdateFingerprint(Fingerprint, Source.DeclaringIdentity);
			UpdateFingerprint(Fingerprint, Source.ContentHash.HashLow);
			UpdateFingerprint(Fingerprint, Source.ContentHash.HashHigh);
			UpdateFingerprint(Fingerprint, Source.ByteCount);
			UpdateFingerprint(Fingerprint, Source.Depth);
			UpdateFingerprint(Fingerprint, Source.bEmbedded);
		}
		for (const FImportOutputPreview& Output : Builder.Outputs)
		{
			UpdateFingerprint(Fingerprint, Output.StableIdentity);
			UpdateFingerprint(Fingerprint, Output.Role);
			UpdateFingerprint(Fingerprint, Output.AssetPath.ToString());
			UpdateFingerprint(Fingerprint, Output.AssetClassName);
			UpdateFingerprint(Fingerprint, Output.Policy);
			UpdateFingerprint(Fingerprint, Output.Collision);
			UpdateFingerprint(Fingerprint, Output.EstimatedCpuBytes);
			UpdateFingerprint(Fingerprint, Output.EstimatedGpuBytes);
			UpdateFingerprint(Fingerprint, Output.EstimatedDiskBytes);
		}
		for (const FImportTargetPrecondition& Precondition : Builder.TargetPreconditions)
		{
			UpdateFingerprint(Fingerprint, Precondition.AssetPath.ToString());
			UpdateFingerprint(Fingerprint, Precondition.AssetClassName);
			UpdateFingerprint(Fingerprint, Precondition.PackageEditRevision);
			UpdateFingerprint(Fingerprint, Precondition.AuthoredFingerprint);
			UpdateFingerprint(Fingerprint, Precondition.ManagementOwner.ToString());
		}
		for (const FImportDiagnostic& Diagnostic : Result.Diagnostics)
		{
			UpdateFingerprint(Fingerprint, Diagnostic.Severity);
			UpdateFingerprint(Fingerprint, Diagnostic.Category);
			UpdateFingerprint(Fingerprint, Diagnostic.Identity);
			UpdateFingerprint(Fingerprint, Diagnostic.Phase);
			UpdateFingerprint(Fingerprint, Diagnostic.SourceIdentity);
			UpdateFingerprint(Fingerprint, Diagnostic.OutputIdentity);
			UpdateFingerprint(Fingerprint, Diagnostic.Message);
		}

		Result.Plan.Provider = Provider;
		Result.Plan.Snapshot = std::move(Snapshot);
		Result.Plan.Settings = Settings;
		Result.Plan.Outputs = std::move(Builder.Outputs);
		Result.Plan.TargetPreconditions = std::move(Builder.TargetPreconditions);
		FinalizeImportDiagnostics(Result.Diagnostics, "plan");
		Result.Plan.Diagnostics = Result.Diagnostics;
		Result.Plan.ProviderData = std::move(Builder.ProviderData);
		Result.Plan.Fingerprint = Fingerprint.Finalize();
		Result.Plan.ProviderRegistryRevision = ProviderRegistryRevision;
		Result.bSucceeded = true;
		ReportImportProgress(Progress, EImportPhase::Plan,
			EImportProgressState::Succeeded, "root", "request", 1, 1);
		return Result;
	}

	auto CreateImportPlan(
		const FImportPlanRequest& Request,
		FProviderRegistry& Registry) -> FImportPlanResult
	{
		FImportPlanResult Result;
		ReportImportProgress(Request.Progress, EImportPhase::Snapshot,
			EImportProgressState::Started);
		FSourceSnapshotBuilder SnapshotBuilder(Request.Limits);
		if (!SnapshotBuilder.CaptureRoot(Request.RootSource, Result.Diagnostics))
		{
			Result.Message = Result.Diagnostics.back().Message;
			FinalizeImportDiagnostics(Result.Diagnostics, "source-capture");
			ReportImportProgress(Request.Progress, EImportPhase::Snapshot,
				EImportProgressState::Failed, "root", "request", 0, 0, Result.Message);
			return Result;
		}
		const FSourceSnapshotEntry& Root = SnapshotBuilder.GetCapturedSources().front();
		const size_t PrefixSize = static_cast<size_t>(std::min<uint64>(
			Root.ByteCount, SnapshotBuilder.GetLimits().RecognitionPrefixBytes));
		const FImportSourceRecognition Recognition{
			.RootSource = Root.SourcePath,
			.Extension = std::filesystem::path(Root.SourcePath.Path).extension().generic_string(),
			.ByteCount = Root.ByteCount,
			.Prefix = Root.GetBytes().first(PrefixSize)};
		FProviderLease Provider;
		if (!Request.ProviderId.empty())
		{
			Provider = Registry.Find(Request.ProviderId);
			if (!Provider)
			{
				Result.Message = std::format("Import provider {} is unavailable.", Request.ProviderId);
				AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
					EImportDiagnosticCategory::ProviderUnavailable, "provider-discovery", "root",
					Result.Message);
				FinalizeImportDiagnostics(Result.Diagnostics, "provider-discovery");
				ReportImportProgress(Request.Progress, EImportPhase::Snapshot,
					EImportProgressState::Failed, "root", "request", 0, 0, Result.Message);
				return Result;
			}
			if (!Provider.GetProvider()->CanImport(Recognition))
			{
				Result.Message = std::format(
					"Import provider {} does not recognize the selected source.", Request.ProviderId);
				AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
					EImportDiagnosticCategory::ProviderFailure, "provider-discovery", "root",
					Result.Message);
				FinalizeImportDiagnostics(Result.Diagnostics, "provider-discovery");
				ReportImportProgress(Request.Progress, EImportPhase::Snapshot,
					EImportProgressState::Failed, "root", "request", 0, 0, Result.Message);
				return Result;
			}
		}
		else
		{
			std::vector<FProviderLease> Matches = Registry.FindMatching(Recognition);
			if (Matches.size() != 1)
			{
				Result.Message = Matches.empty()
					? "No import provider recognizes the selected source."
					: "Several import providers recognize the selected source; choose one explicitly.";
				AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
					Matches.empty() ? EImportDiagnosticCategory::ProviderUnavailable
						: EImportDiagnosticCategory::ProviderAmbiguous,
					"provider-discovery", "root", Result.Message);
				FinalizeImportDiagnostics(Result.Diagnostics, "provider-discovery");
				ReportImportProgress(Request.Progress, EImportPhase::Snapshot,
					EImportProgressState::Failed, "root", "request", 0, 0, Result.Message);
				return Result;
			}
			Provider = std::move(Matches.front());
		}

		FImportPayload Settings;
		if (!Provider.GetProvider()->CaptureSettings(Settings, Result.Diagnostics))
		{
			Result.Message = Result.Diagnostics.empty()
				? "Import provider settings capture failed." : Result.Diagnostics.back().Message;
			if (Result.Diagnostics.empty())
				AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
					EImportDiagnosticCategory::ProviderFailure, "settings-capture", "root",
					Result.Message);
			FinalizeImportDiagnostics(Result.Diagnostics, "settings-capture");
			ReportImportProgress(Request.Progress, EImportPhase::Snapshot,
				EImportProgressState::Failed, "root", "request", 0, 0, Result.Message);
			return Result;
		}
		std::string PayloadError;
		if (Settings.Bytes.size() > Request.Limits.MaximumSettingsBytes
			|| !Settings.Finalize(PayloadError))
		{
			Result.Message = Settings.Bytes.size() > Request.Limits.MaximumSettingsBytes
				? "Import settings byte limit was exceeded." : PayloadError;
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::ResourceLimitExceeded, "settings-capture", "root",
				Result.Message);
			FinalizeImportDiagnostics(Result.Diagnostics, "settings-capture");
			ReportImportProgress(Request.Progress, EImportPhase::Snapshot,
				EImportProgressState::Failed, "root", "request", 0, 0, Result.Message);
			return Result;
		}
		if (!SnapshotBuilder.DiscoverDependencies(Provider, Result.Diagnostics))
		{
			Result.Message = Result.Diagnostics.back().Message;
			FinalizeImportDiagnostics(Result.Diagnostics, "dependency-discovery");
			ReportImportProgress(Request.Progress, EImportPhase::Snapshot,
				EImportProgressState::Failed, "root", "request", 0, 0, Result.Message);
			return Result;
		}
		std::shared_ptr<const FSourceSnapshot> Snapshot =
			SnapshotBuilder.Freeze(Result.Diagnostics);
		if (!Snapshot)
		{
			Result.Message = Result.Diagnostics.back().Message;
			FinalizeImportDiagnostics(Result.Diagnostics, "source-freeze");
			ReportImportProgress(Request.Progress, EImportPhase::Snapshot,
				EImportProgressState::Failed, "root", "request", 0, 0, Result.Message);
			return Result;
		}
		ReportImportProgress(Request.Progress, EImportPhase::Snapshot,
			EImportProgressState::Succeeded, "root", "request",
			Snapshot->GetAggregateByteCount(), Snapshot->GetAggregateByteCount());
		return BuildImportPlan(
			Provider,
			std::move(Snapshot),
			Settings,
			Registry.GetRevision(),
			Result.Diagnostics,
			Request.Progress);
	}

	struct FSingleAssetHandlerRegistry::FImpl
	{
		mutable std::mutex Mutex;
		std::map<std::string, std::shared_ptr<ISingleAssetImportHandler>, std::less<>> Handlers;
		uint64 Revision = 1;
	};

	FSingleAssetHandlerRegistry::FSingleAssetHandlerRegistry()
		: Impl(std::make_unique<FImpl>()) {}
	FSingleAssetHandlerRegistry::~FSingleAssetHandlerRegistry() = default;

	auto FSingleAssetHandlerRegistry::Register(
		std::shared_ptr<ISingleAssetImportHandler> Handler,
		std::string& OutError) -> bool
	{
		if (!Handler || Handler->GetAssetClassName().empty()
			|| !IsStableIdentifier(Handler->GetProviderId()))
		{
			OutError = "Single-asset import handler identity is invalid.";
			return false;
		}
		const std::string ClassName(Handler->GetAssetClassName());
		std::lock_guard Lock(Impl->Mutex);
		if (Impl->Handlers.contains(ClassName))
		{
			OutError = std::format(
				"A single-asset import handler is already registered for {}.", ClassName);
			return false;
		}
		Impl->Handlers.emplace(ClassName, std::move(Handler));
		++Impl->Revision;
		OutError.clear();
		return true;
	}

	auto FSingleAssetHandlerRegistry::Unregister(std::string_view AssetClassName) -> bool
	{
		std::lock_guard Lock(Impl->Mutex);
		const auto It = Impl->Handlers.find(AssetClassName);
		if (It == Impl->Handlers.end()) return false;
		Impl->Handlers.erase(It);
		++Impl->Revision;
		return true;
	}

	auto FSingleAssetHandlerRegistry::Find(std::string_view AssetClassName) const
		-> std::shared_ptr<const ISingleAssetImportHandler>
	{
		std::lock_guard Lock(Impl->Mutex);
		const auto It = Impl->Handlers.find(AssetClassName);
		return It == Impl->Handlers.end() ? nullptr : It->second;
	}

	auto FSingleAssetHandlerRegistry::GetRevision() const -> uint64
	{
		std::lock_guard Lock(Impl->Mutex);
		return Impl->Revision;
	}

	auto GetSingleAssetHandlerRegistry() -> FSingleAssetHandlerRegistry&
	{
		static FSingleAssetHandlerRegistry Registry;
		return Registry;
	}

	namespace
	{
		auto GetAssetClassName(const DObject& Asset) -> std::string
		{
			return Asset.GetClass()->GetQualifiedName().ToString();
		}

		auto GetAssetPath(const DObject& Asset, FAssetPath& OutPath) -> bool
		{
			return Asset.GetPackage()
				&& FAssetPath::TryCreate(Asset.GetPackage()->GetPackagePath(), OutPath);
		}

		auto MakeUnavailableCapabilitySet(
			std::string AssetClassName,
			std::string Message,
			EImportDiagnosticCategory Category) -> FSingleAssetCapabilitySet
		{
			FSingleAssetCapabilitySet Result;
			Result.AssetClassName = std::move(AssetClassName);
			for (const ESingleAssetImportCapability Capability : {
				ESingleAssetImportCapability::Import,
				ESingleAssetImportCapability::ReimportCurrentSource,
				ESingleAssetImportCapability::ReimportNewSource,
				ESingleAssetImportCapability::RepairSource})
			{
				Result.Capabilities.push_back({
					.Capability = Capability,
					.bAvailable = false,
					.Diagnostics = {{
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = Category,
						.Phase = "capability-query",
						.Message = Message}}});
			}
			return Result;
		}

		auto AddExecutionDiagnostic(
			FSingleAssetExecutionResult& Result,
			EImportDiagnosticCategory Category,
			std::string_view Phase,
			std::string_view Message) -> void
		{
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
				Category, Phase, "root", Message);
			Result.Message = std::string(Message);
		}

	}

	auto QuerySingleAssetCapabilities(
		const DObject& Asset,
		FProviderRegistry& Providers,
		FSingleAssetHandlerRegistry& Handlers) -> FSingleAssetCapabilitySet
	{
		const std::string ClassName = GetAssetClassName(Asset);
		const std::shared_ptr<const ISingleAssetImportHandler> Handler = Handlers.Find(ClassName);
		if (!Handler)
			return MakeUnavailableCapabilitySet(ClassName,
				"No single-asset import handler is registered for this asset class.",
				EImportDiagnosticCategory::CapabilityUnavailable);

		FSingleAssetProvenance Provenance;
		std::vector<FImportDiagnostic> Diagnostics;
		if (!Handler->InspectProvenance(Asset, Provenance, Diagnostics))
		{
			FSingleAssetCapabilitySet Result = MakeUnavailableCapabilitySet(
				ClassName, "The selected asset has incomplete or incompatible import provenance.",
				EImportDiagnosticCategory::InvalidSource);
			for (FSingleAssetCapability& Capability : Result.Capabilities)
				Capability.Diagnostics.insert(Capability.Diagnostics.end(), Diagnostics.begin(), Diagnostics.end());
			return Result;
		}

		FSingleAssetCapabilitySet Result = Handler->QueryCapabilities(Asset, Provenance);
		Result.AssetClassName = ClassName;
		Result.ProviderId = Provenance.ProviderId;
		const FProviderLease Provider = Providers.Find(Provenance.ProviderId);
		if (!Provider || Provider.GetContractVersion() != Provenance.ProviderContractVersion)
		{
			for (FSingleAssetCapability& Capability : Result.Capabilities)
			{
				Capability.bAvailable = false;
				AddDiagnostic(Capability.Diagnostics, EImportDiagnosticSeverity::Error,
					EImportDiagnosticCategory::ProviderUnavailable, "capability-query", "root",
					std::format("Import provider {} version {} is unavailable.",
						Provenance.ProviderId, Provenance.ProviderContractVersion));
			}
		}
		return Result;
	}

	auto CreateSingleAssetReimportPlan(
		const FSingleAssetReimportRequest& Request,
		FProviderRegistry& Providers,
		FSingleAssetHandlerRegistry& Handlers) -> FSingleAssetPlanResult
	{
		FSingleAssetPlanResult Result;
		FDiagnosticFinalizer DiagnosticFinalizer(
			Result.Diagnostics, "single-asset-plan");
		FImportProgressPhaseScope SnapshotProgress(Request.Progress, EImportPhase::Snapshot);
		if (!Request.Asset || !Request.Asset->GetPackage())
		{
			Result.Message = "Single-asset reimport requires a packaged selected asset.";
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "single-asset-plan", "root", Result.Message);
			return Result;
		}

		const std::string ClassName = GetAssetClassName(*Request.Asset);
		const std::shared_ptr<const ISingleAssetImportHandler> Handler = Handlers.Find(ClassName);
		if (!Handler)
		{
			Result.Message = "No single-asset import handler is registered for the selected asset.";
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::CapabilityUnavailable, "single-asset-plan", "root", Result.Message);
			return Result;
		}

		FSingleAssetProvenance Observed;
		if (!Handler->InspectProvenance(*Request.Asset, Observed, Result.Diagnostics)
			|| !Observed.IsComplete())
		{
			Result.Message = "The selected asset has incomplete import provenance.";
			if (!HasError(Result.Diagnostics))
				AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
					EImportDiagnosticCategory::InvalidSource, "single-asset-plan", "root", Result.Message);
			return Result;
		}
		const FProviderLease Provider = Providers.Find(Observed.ProviderId);
		if (!Provider || Provider.GetContractVersion() != Observed.ProviderContractVersion
			|| Handler->GetProviderId() != Observed.ProviderId)
		{
			Result.Message = std::format(
				"Persisted import provider {} version {} is unavailable or incompatible.",
				Observed.ProviderId, Observed.ProviderContractVersion);
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::ProviderUnavailable, "single-asset-plan", "root", Result.Message);
			return Result;
		}

		const bool bReplacement = !Request.ReplacementSources.empty();
		if (bReplacement && Request.ReplacementSources.size() != Observed.Sources.size())
		{
			Result.Message = "Replacement source count does not match the asset provenance.";
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
				EImportDiagnosticCategory::InvalidRequest, "single-asset-plan", "root", Result.Message);
			return Result;
		}

		FSourceSnapshotBuilder SnapshotBuilder(Request.Limits);
		for (size_t Index = 0; Index < Observed.Sources.size(); ++Index)
		{
			const FSingleAssetSourceProvenance& Source = Observed.Sources[Index];
			const FSourcePath& Path = bReplacement ? Request.ReplacementSources[Index] : Source.SourcePath;
			const bool bCaptured = Index == 0
				? SnapshotBuilder.CaptureRoot(Path, Result.Diagnostics)
				: SnapshotBuilder.CaptureDeclaredSource(
					Source.StableIdentity, Source.Role, Path, Result.Diagnostics);
			if (!bCaptured)
			{
				Result.Message = Result.Diagnostics.back().Message;
				return Result;
			}
		}
		if (!SnapshotBuilder.DiscoverDependencies(Provider, Result.Diagnostics))
		{
			Result.Message = Result.Diagnostics.back().Message;
			return Result;
		}
		std::shared_ptr<const FSourceSnapshot> Snapshot = SnapshotBuilder.Freeze(Result.Diagnostics);
		if (!Snapshot)
		{
			Result.Message = Result.Diagnostics.back().Message;
			return Result;
		}
		SnapshotProgress.Succeed(
			Snapshot->GetAggregateByteCount(), Snapshot->GetAggregateByteCount());
		FImportProgressPhaseScope PlanProgress(Request.Progress, EImportPhase::Plan);

		FSingleAssetProvenance Prospective = Observed;
		for (size_t Index = 0; Index < Prospective.Sources.size(); ++Index)
		{
			FSingleAssetSourceProvenance& Source = Prospective.Sources[Index];
			const std::string_view SnapshotIdentity = Index == 0 ? std::string_view("root")
				: std::string_view(Source.StableIdentity);
			const FSourceSnapshotEntry* Captured = Snapshot->FindSource(SnapshotIdentity);
			if (!Captured)
			{
				Result.Message = "A declared source is absent from the frozen snapshot.";
				AddDiagnostic(Result.Diagnostics, EImportDiagnosticSeverity::Error,
					EImportDiagnosticCategory::InvalidPlan, "single-asset-plan",
					Source.StableIdentity, Result.Message);
				return Result;
			}
			Source.SourcePath = Captured->SourcePath;
			Source.ContentHash = Captured->ContentHash;
			Source.ByteCount = Captured->ByteCount;
		}

		FAssetPath AssetPath;
		if (!GetAssetPath(*Request.Asset, AssetPath))
		{
			Result.Message = "The selected asset package path is invalid.";
			return Result;
		}
		Result.Plan.Asset = Request.Asset;
		Result.Plan.AssetPath = std::move(AssetPath);
		Result.Plan.AssetClassName = ClassName;
		Result.Plan.Provenance = std::move(Prospective);
		Result.Plan.ObservedProvenance = std::move(Observed);
		Result.Plan.Snapshot = std::move(Snapshot);
		Result.Plan.Provider = Provider;
		Result.Plan.Handler = Handler;
		Result.Plan.ProviderRegistry = &Providers;
		Result.Plan.HandlerRegistry = &Handlers;
		Result.Plan.PackageEditRevision = Request.Asset->GetPackage()->GetEditRevision();
		Result.Plan.ProviderRegistryRevision = Providers.GetRevision();
		Result.Plan.HandlerRegistryRevision = Handlers.GetRevision();
		Result.Plan.bReplacesSource = bReplacement;
		Result.bSucceeded = true;
		PlanProgress.Succeed();
		FinalizeImportDiagnostics(Result.Diagnostics, "single-asset-plan",
			"root", Result.Plan.AssetPath.ToString());
		return Result;
	}

	auto ExecuteSingleAssetImport(
		const FSingleAssetImportPlan& Plan,
		const FSingleAssetExecutionOptions& Options) -> FSingleAssetExecutionResult
	{
		FSingleAssetExecutionResult Result;
		FDiagnosticFinalizer DiagnosticFinalizer(
			Result.Diagnostics, "single-asset-execution", "root",
			Plan.AssetPath.IsValid() ? Plan.AssetPath.GetView() : std::string_view("request"));
		Result.Provider = Plan.Provider;
		FImportProgressPhaseScope CandidateProgress(
			Options.Progress, EImportPhase::CandidateBuild, "root",
			Plan.AssetPath.IsValid() ? Plan.AssetPath.GetView() : std::string_view("request"));
		if (!Plan.Asset || !Plan.Asset->GetPackage() || !Plan.Handler || !Plan.Provider
			|| !Plan.ProviderRegistry || !Plan.HandlerRegistry || !Plan.Snapshot)
		{
			AddExecutionDiagnostic(Result, EImportDiagnosticCategory::InvalidPlan,
				"candidate-build", "Single-asset import plan is incomplete.");
			return Result;
		}

		std::unique_ptr<ISingleAssetCandidate> Candidate =
			Plan.Handler->BuildCandidate(Plan, Result.Diagnostics);
		if (!Candidate || !Candidate->GetAsset() || !Candidate->GetPackage())
		{
			AddExecutionDiagnostic(Result, EImportDiagnosticCategory::CandidateFailure,
				"candidate-build", "The provider failed to build a detached candidate.");
			if (Candidate) Candidate->Abandon();
			return Result;
		}
		CandidateProgress.Succeed();
		FImportProgressPhaseScope ValidationProgress(
			Options.Progress, EImportPhase::Validation, "root", Plan.AssetPath.GetView());
		if (!Candidate->Validate(Result.Diagnostics))
		{
			AddExecutionDiagnostic(Result, EImportDiagnosticCategory::ValidationFailure,
				"candidate-validation", "The detached import candidate is invalid.");
			Candidate->Abandon();
			return Result;
		}

		std::unique_ptr<IPreparedImportedStateExchange> Exchange =
			Plan.Handler->PrepareImportedStateExchange(
				*Plan.Asset, *Candidate, Result.Diagnostics);
		if (!Exchange)
		{
			AddExecutionDiagnostic(Result, EImportDiagnosticCategory::CandidateFailure,
				"exchange-prepare", "Imported-state exchange preparation failed.");
			Candidate->Abandon();
			return Result;
		}
		ValidationProgress.Succeed();
		FImportProgressPhaseScope PublicationProgress(
			Options.Progress, EImportPhase::Publication, "root", Plan.AssetPath.GetView());

		std::lock_guard PublicationLock(GetImportPublicationMutex());
		FSingleAssetProvenance CurrentProvenance;
		std::vector<FImportDiagnostic> PreflightDiagnostics;
		FAssetPath CurrentPath;
		const std::shared_ptr<const ISingleAssetImportHandler> CurrentHandler =
			Plan.HandlerRegistry->Find(Plan.AssetClassName);
		const FProviderLease CurrentProvider =
			Plan.ProviderRegistry->Find(Plan.Provenance.ProviderId);
		const bool bStale = Plan.Asset->GetPackage() == nullptr
			|| !GetAssetPath(*Plan.Asset, CurrentPath) || CurrentPath != Plan.AssetPath
			|| GetAssetClassName(*Plan.Asset) != Plan.AssetClassName
			|| Plan.Asset->GetPackage()->GetEditRevision() != Plan.PackageEditRevision
			|| Plan.ProviderRegistry->GetRevision() != Plan.ProviderRegistryRevision
			|| Plan.HandlerRegistry->GetRevision() != Plan.HandlerRegistryRevision
			|| CurrentHandler.get() != Plan.Handler.get()
			|| !CurrentProvider
			|| CurrentProvider.GetContractVersion() != Plan.Provider.GetContractVersion()
			|| !Plan.Handler->InspectProvenance(
				*Plan.Asset, CurrentProvenance, PreflightDiagnostics)
			|| CurrentProvenance != Plan.ObservedProvenance
			|| Asset::FindLoadedPackage(Plan.AssetPath) != Plan.Asset->GetPackage();
		if (bStale)
		{
			Result.Diagnostics.insert(Result.Diagnostics.end(),
				PreflightDiagnostics.begin(), PreflightDiagnostics.end());
			AddExecutionDiagnostic(Result, EImportDiagnosticCategory::StalePlan,
				"publication-preflight",
				"The selected asset, package, provenance, path occupant, handler, or provider changed after preview.");
			Exchange->Finalize();
			Candidate->Abandon();
			return Result;
		}

		DPackage* Package = Plan.Asset->GetPackage();
		const bool bPackageWasDirty = Package->IsDirty();
		std::vector<uint8> BeforePublicationBytes;
		const Asset::FAssetResult BeforeSerialization =
			Asset::SerializeAssetPackageBytes(Package, BeforePublicationBytes);
		if (!BeforeSerialization)
		{
			Exchange->Finalize();
			Candidate->Abandon();
			AddExecutionDiagnostic(Result, EImportDiagnosticCategory::ValidationFailure,
				"publication-preflight", BeforeSerialization.Message);
			FinalizeImportDiagnostics(Result.Diagnostics, "publication-preflight",
				"root", Plan.AssetPath.ToString());
			return Result;
		}
		Exchange->Commit();
		const Asset::FAssetResult SaveResult = Asset::SavePackagesAtomically(
			std::span<DPackage* const>(&Package, 1), Options.SaveOptions);
		if (!SaveResult)
		{
			FImportProgressPhaseScope RestoreProgress(
				Options.Progress, EImportPhase::Restore, "root", Plan.AssetPath.GetView());
			Exchange->Reverse();
			if (!bPackageWasDirty) Package->ClearDirty();
			std::vector<uint8> RestoredBytes;
			const Asset::FAssetResult RestoredSerialization =
				Asset::SerializeAssetPackageBytes(Package, RestoredBytes);
			const bool bRestored = RestoredSerialization
				&& RestoredBytes == BeforePublicationBytes;
			if (bRestored) RestoreProgress.Succeed();
			else AddExecutionDiagnostic(Result, EImportDiagnosticCategory::RestoreFailure,
				"restore", RestoredSerialization
					? "Imported state did not match its pre-publication bytes after reverse exchange."
					: RestoredSerialization.Message);
			Exchange->Finalize();
			Candidate->Abandon();
			AddExecutionDiagnostic(Result, EImportDiagnosticCategory::PublicationFailure,
				"package-publication", SaveResult.Message);
			FinalizeImportDiagnostics(Result.Diagnostics, "package-publication",
				"root", Plan.AssetPath.ToString());
			return Result;
		}

		Exchange->Finalize();
		Candidate->Abandon();
		Result.bSucceeded = true;
		Result.Asset = Plan.Asset;
		PublicationProgress.Succeed();
		FinalizeImportDiagnostics(Result.Diagnostics, "publication",
			"root", Plan.AssetPath.ToString());
		return Result;
	}

	auto RepairSingleAssetSource(
		DObject& Asset,
		std::span<const FSourcePath> Sources,
		FProviderRegistry& Providers,
		FSingleAssetHandlerRegistry& Handlers) -> FSingleAssetExecutionResult
	{
		FSingleAssetExecutionResult Result;
		FDiagnosticFinalizer DiagnosticFinalizer(
			Result.Diagnostics, "source-repair", "root", "asset");
		const std::string ClassName = GetAssetClassName(Asset);
		const std::shared_ptr<const ISingleAssetImportHandler> Handler = Handlers.Find(ClassName);
		if (!Handler)
		{
			AddExecutionDiagnostic(Result, EImportDiagnosticCategory::CapabilityUnavailable,
				"source-repair", "No single-asset import handler is registered for this asset class.");
			return Result;
		}
		FSingleAssetProvenance Provenance;
		if (!Handler->InspectProvenance(Asset, Provenance, Result.Diagnostics)
			|| !Providers.Find(Provenance.ProviderId))
		{
			AddExecutionDiagnostic(Result, EImportDiagnosticCategory::ProviderUnavailable,
				"source-repair", "The persisted import provider is unavailable.");
			return Result;
		}
		if (!Handler->RepairSource(Asset, Sources, Result.Diagnostics))
		{
			if (Result.Diagnostics.empty())
				AddExecutionDiagnostic(Result, EImportDiagnosticCategory::InvalidSource,
					"source-repair", "The asset source reference could not be repaired.");
			else Result.Message = Result.Diagnostics.back().Message;
			return Result;
		}
		Result.bSucceeded = true;
		Result.Asset = &Asset;
		return Result;
	}
}
