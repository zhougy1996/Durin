#include "Thumbnail/AssetThumbnail.h"

#include "AssetSystem.h"
#include "Hash/XxHash.h"
#include "Thumbnail/RenderedAssetThumbnailExtension.h"

namespace Durin
{
	namespace
	{
		class FThumbnailKeyWriter
		{
		public:
			auto WriteU32(uint32 Value) -> void
			{
				for (uint32 ByteIndex = 0; ByteIndex < 4; ++ByteIndex)
					Bytes.push_back(static_cast<uint8>(Value >> (ByteIndex * 8)));
			}

			auto WriteU64(uint64 Value) -> void
			{
				for (uint32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
					Bytes.push_back(static_cast<uint8>(Value >> (ByteIndex * 8)));
			}

			auto WriteI64(int64 Value) -> void
			{
				WriteU64(std::bit_cast<uint64>(Value));
			}

			auto WriteString(std::string_view Value) -> void
			{
				WriteU64(static_cast<uint64>(Value.size()));
				Bytes.insert(Bytes.end(), Value.begin(), Value.end());
			}

			auto GetBytes() const -> std::span<const uint8>
			{
				return Bytes;
			}

		private:
			std::vector<uint8> Bytes;
		};

		auto WritePackageFingerprint(FThumbnailKeyWriter& Writer, const FAssetThumbnailPackageFingerprint& Package) -> void
		{
			Writer.WriteString(Package.VirtualPath.GetView());
			Writer.WriteString(Package.AssetClassName);
			Writer.WriteU32(Package.PackageFormatVersion);
			Writer.WriteU64(Package.FileSize);
			Writer.WriteI64(Package.LastWriteTimeTicks);
		}
	} // namespace

	namespace Detail
	{
		struct FAssetThumbnailGenerationLeaseState
		{
			FAssetThumbnailCancellation Cancellation;
			std::shared_ptr<const IAssetThumbnailGenerationInput> Input;
			std::shared_ptr<IRenderedAssetThumbnailExtension> Extension;
			std::unique_ptr<IRenderedAssetThumbnailGenerationSession> Session;
			std::atomic<bool> bActive = true;

			~FAssetThumbnailGenerationLeaseState()
			{
				ReleaseSession();
			}

			auto ReleaseSession() -> void
			{
				if (!Session) return;
				Session->ResetPreview();
				Session.reset();
			}

			auto Invalidate() -> void
			{
				if (!bActive.exchange(false, std::memory_order_acq_rel)) return;
				Cancellation.Cancel();
				ReleaseSession();
				Input.reset();
				Extension.reset();
			}
		};

		struct FAssetThumbnailProviderRegistryState
		{
			struct FEntry
			{
				std::shared_ptr<IAssetThumbnailProvider> Provider;
				std::vector<std::weak_ptr<FAssetThumbnailGenerationLeaseState>> Leases;
				uint64 Generation = 0;
			};

			std::unordered_map<std::string, FEntry> Providers;
			uint64 NextGeneration = 1;
			bool bShuttingDown = false;
		};
	} // namespace Detail

	namespace
	{
		auto InvalidateProviderEntry(
			Detail::FAssetThumbnailProviderRegistryState::FEntry& Entry) -> void
		{
			for (const std::weak_ptr<Detail::FAssetThumbnailGenerationLeaseState>& WeakLease
				: Entry.Leases)
			{
				if (const std::shared_ptr Lease = WeakLease.lock()) Lease->Invalidate();
			}
			Entry.Leases.clear();
			Entry.Provider.reset();
		}

		auto RemoveProviderRegistration(
			const std::shared_ptr<Detail::FAssetThumbnailProviderRegistryState>& State,
			uint64 RegistrationId) -> bool
		{
			if (!State || RegistrationId == 0) return false;
			const auto It = std::ranges::find_if(
				State->Providers,
				[RegistrationId](const auto& Pair) {
					return Pair.second.Generation == RegistrationId;
				});
			if (It == State->Providers.end()) return false;
			Detail::FAssetThumbnailProviderRegistryState::FEntry Entry =
				std::move(It->second);
			State->Providers.erase(It);
			InvalidateProviderEntry(Entry);
			return true;
		}

		auto RegisterProvider(
			const std::shared_ptr<Detail::FAssetThumbnailProviderRegistryState>& State,
			std::shared_ptr<IAssetThumbnailProvider> Provider,
			std::string& OutError) -> uint64
		{
			if (State->bShuttingDown)
			{
				OutError = "Thumbnail provider registration is closed during shutdown.";
				return 0;
			}
			if (!Provider)
			{
				OutError = "Cannot register a null thumbnail provider.";
				return 0;
			}
			const FAssetThumbnailProviderRegistration Registration =
				Provider->GetRegistration();
			if (Registration.AssetClassName.empty() || Registration.ProviderName.empty()
				|| Registration.GeneratorSchemaVersion == 0)
			{
				OutError = "Thumbnail providers require an asset class, provider name, and nonzero generator schema.";
				return 0;
			}
			if (State->Providers.contains(Registration.AssetClassName))
			{
				OutError = std::format(
					"A thumbnail provider is already registered for asset class {}.",
					Registration.AssetClassName);
				return 0;
			}
			const uint64 Generation = State->NextGeneration++;
			State->Providers.emplace(
				Registration.AssetClassName,
				Detail::FAssetThumbnailProviderRegistryState::FEntry{
					.Provider = std::move(Provider),
					.Generation = Generation});
			OutError.clear();
			return Generation;
		}
	} // namespace

	struct FAssetThumbnailScheduler::FImpl
	{
		struct FEntry
		{
			std::string CacheKey;
			EAssetThumbnailState State = EAssetThumbnailState::NotRequested;
			FAssetThumbnailGenerationRequest GenerationRequest;
			std::string Diagnostic;
			uint64 RequestSerial = 0;
		};

		FAssetThumbnailProviderRegistry& Registry;
		FAssetThumbnailBudgets Budgets;
		std::unordered_map<std::string, FEntry> Entries;
		std::vector<FAssetThumbnailScheduledJob> Queue;
		bool bShuttingDown = false;

		auto RemoveQueuedJob(std::string_view CacheKey) -> void
		{
			std::erase_if(Queue, [CacheKey](const FAssetThumbnailScheduledJob& Job) {
				return Job.CacheKey == CacheKey;
			});
		}
	};

	FAssetThumbnailCancellation::FAssetThumbnailCancellation()
		: State(std::make_shared<std::atomic<bool>>(false))
	{
	}

	auto FAssetThumbnailCancellation::Cancel() const -> void
	{
		State->store(true, std::memory_order_release);
	}

	auto FAssetThumbnailCancellation::IsCancelled() const -> bool
	{
		return State->load(std::memory_order_acquire);
	}

	auto FAssetThumbnailGenerationLease::IsActive() const -> bool
	{
		return State != nullptr && State->bActive.load(std::memory_order_acquire);
	}

	auto FAssetThumbnailGenerationLease::GetInput() const
		-> const IAssetThumbnailGenerationInput*
	{
		return IsActive() ? State->Input.get() : nullptr;
	}

	auto FAssetThumbnailGenerationLease::GetRenderedSession() const
		-> IRenderedAssetThumbnailGenerationSession*
	{
		return IsActive() ? State->Session.get() : nullptr;
	}

	auto FAssetThumbnailGenerationLease::ReleaseRenderedSession() const -> void
	{
		if (State) State->ReleaseSession();
	}

	auto FAssetThumbnailGenerationRequest::GetInput() const
		-> const IAssetThumbnailGenerationInput*
	{
		return ProviderLease.GetInput();
	}

	auto FAssetThumbnailGenerationRequest::BeginRenderedSession(
		std::string& OutError) const -> IRenderedAssetThumbnailGenerationSession*
	{
		const std::shared_ptr LeaseState = ProviderLease.State;
		if (!LeaseState || !LeaseState->bActive.load(std::memory_order_acquire))
		{
			OutError = "The thumbnail provider registration is no longer active.";
			return nullptr;
		}
		if (LeaseState->Session)
		{
			OutError.clear();
			return LeaseState->Session.get();
		}
		if (!LeaseState->Extension)
		{
			OutError = "The thumbnail provider does not implement rendered generation sessions.";
			return nullptr;
		}
		if (!LeaseState->Input)
		{
			OutError = "The thumbnail provider did not capture generation input.";
			return nullptr;
		}
		LeaseState->Session = LeaseState->Extension->CreateGenerationSession(
			*this, *LeaseState->Input, OutError);
		if (!LeaseState->Session)
		{
			if (OutError.empty())
				OutError = "The thumbnail provider could not create a generation session.";
			return nullptr;
		}
		OutError.clear();
		return LeaseState->Session.get();
	}

	auto FAssetThumbnailGenerationRequest::GetRenderedSession() const
		-> IRenderedAssetThumbnailGenerationSession*
	{
		return ProviderLease.GetRenderedSession();
	}

	auto FAssetThumbnailGenerationRequest::ReleaseRenderedSession() const -> void
	{
		ProviderLease.ReleaseRenderedSession();
	}

	FAssetThumbnailProviderRegistrationHandle::~FAssetThumbnailProviderRegistrationHandle()
	{
		Reset();
	}

	FAssetThumbnailProviderRegistrationHandle::FAssetThumbnailProviderRegistrationHandle(
		FAssetThumbnailProviderRegistrationHandle&& Other) noexcept
		: State(std::move(Other.State))
		, RegistrationId(std::exchange(Other.RegistrationId, 0))
	{
	}

	auto FAssetThumbnailProviderRegistrationHandle::operator=(
		FAssetThumbnailProviderRegistrationHandle&& Other) noexcept
		-> FAssetThumbnailProviderRegistrationHandle&
	{
		if (this == &Other) return *this;
		Reset();
		State = std::move(Other.State);
		RegistrationId = std::exchange(Other.RegistrationId, 0);
		return *this;
	}

	auto FAssetThumbnailProviderRegistrationHandle::Reset() -> void
	{
		if (RegistrationId == 0) return;
		if (const std::shared_ptr RegistryState = State.lock())
			RemoveProviderRegistration(RegistryState, RegistrationId);
		State.reset();
		RegistrationId = 0;
	}

	FAssetThumbnailProviderRegistry::FAssetThumbnailProviderRegistry()
		: State(std::make_shared<Detail::FAssetThumbnailProviderRegistryState>())
	{
	}

	FAssetThumbnailProviderRegistry::~FAssetThumbnailProviderRegistry()
	{
		Shutdown();
	}

	auto FAssetThumbnailProviderRegistry::Register(
		std::shared_ptr<IAssetThumbnailProvider> Provider,
		std::string& OutError
	) -> bool
	{
		return RegisterProvider(State, std::move(Provider), OutError) != 0;
	}

	auto FAssetThumbnailProviderRegistry::RegisterScoped(
		std::unique_ptr<IAssetThumbnailProvider> Provider,
		std::string& OutError) -> FAssetThumbnailProviderRegistrationHandle
	{
		std::shared_ptr<IAssetThumbnailProvider> SharedProvider = std::move(Provider);
		const uint64 RegistrationId =
			RegisterProvider(State, std::move(SharedProvider), OutError);
		return RegistrationId != 0
			? FAssetThumbnailProviderRegistrationHandle(State, RegistrationId)
			: FAssetThumbnailProviderRegistrationHandle{};
	}

	auto FAssetThumbnailProviderRegistry::Unregister(
		std::string_view AssetClassName,
		std::string& OutError
	) -> bool
	{
		const auto It = State->Providers.find(std::string(AssetClassName));
		if (It == State->Providers.end())
		{
			OutError = std::format(
				"No thumbnail provider is registered for asset class {}.",
				AssetClassName);
			return false;
		}
		const uint64 RegistrationId = It->second.Generation;
		RemoveProviderRegistration(State, RegistrationId);
		OutError.clear();
		return true;
	}

	auto FAssetThumbnailProviderRegistry::Find(
		std::string_view AssetClassName
	) const -> FAssetThumbnailProviderHandle
	{
		const auto It = State->Providers.find(std::string(AssetClassName));
		if (It == State->Providers.end()) return {};
		return {.Generation = It->second.Generation};
	}

	auto FAssetThumbnailProviderRegistry::CaptureSourceImage(
		const Asset::FAssetData& Asset,
		FAssetThumbnailSourceImage& OutSource,
		std::string& OutError) const -> bool
	{
		OutSource = {};
		const auto It = State->Providers.find(Asset.AssetClassName);
		if (It == State->Providers.end())
		{
			OutError.clear();
			return false;
		}
		return It->second.Provider->CaptureSourceImage(Asset, OutSource, OutError);
	}

	auto FAssetThumbnailProviderRegistry::UsesSourceImage(
		std::string_view AssetClassName) const -> bool
	{
		const auto It = State->Providers.find(std::string(AssetClassName));
		return It != State->Providers.end()
			&& It->second.Provider->UsesSourceImage();
	}

	auto FAssetThumbnailProviderRegistry::Capture(
		const FAssetThumbnailRequest& Request,
		uint64 ProviderGeneration,
		FAssetThumbnailGenerationRequest& OutRequest,
		FAssetThumbnailProviderRegistration& OutRegistration,
		std::string& OutError) -> bool
	{
		const auto It = State->Providers.find(Request.Asset.AssetClassName);
		if (It == State->Providers.end() || It->second.Generation != ProviderGeneration)
		{
			OutError = std::format(
				"No current thumbnail provider is registered for asset class {}.",
				Request.Asset.AssetClassName);
			return false;
		}
		Detail::FAssetThumbnailProviderRegistryState::FEntry& Entry = It->second;
		if (!Entry.Provider->CaptureGenerationRequest(
				Request, ProviderGeneration, OutRequest, OutError))
			return false;

		OutRegistration = Entry.Provider->GetRegistration();
		auto LeaseState =
			std::make_shared<Detail::FAssetThumbnailGenerationLeaseState>();
		LeaseState->Cancellation = OutRequest.Cancellation;
		LeaseState->Input = std::move(OutRequest.Input);
		LeaseState->Extension =
			std::dynamic_pointer_cast<IRenderedAssetThumbnailExtension>(Entry.Provider);
		Entry.Leases.erase(
			std::remove_if(
				Entry.Leases.begin(),
				Entry.Leases.end(),
				[](const auto& Lease) { return Lease.expired(); }),
			Entry.Leases.end());
		Entry.Leases.push_back(LeaseState);
		OutRequest.ProviderLease = FAssetThumbnailGenerationLease(std::move(LeaseState));
		OutError.clear();
		return true;
	}

	auto FAssetThumbnailProviderRegistry::Shutdown() -> void
	{
		if (State->bShuttingDown) return;
		State->bShuttingDown = true;
		while (!State->Providers.empty())
			RemoveProviderRegistration(State, State->Providers.begin()->second.Generation);
	}

	auto FAssetThumbnailProviderRegistry::IsShuttingDown() const -> bool
	{
		return State->bShuttingDown;
	}

	auto FAssetThumbnailProviderRegistry::Num() const -> size_t
	{
		return State->Providers.size();
	}

	FAssetThumbnailScheduler::FAssetThumbnailScheduler(
		FAssetThumbnailProviderRegistry& Registry,
		FAssetThumbnailBudgets Budgets
	)
		: Impl(std::make_unique<FImpl>(FImpl{
			.Registry = Registry,
			.Budgets = Budgets}))
	{
	}

	FAssetThumbnailScheduler::~FAssetThumbnailScheduler()
	{
		Shutdown();
	}

	auto FAssetThumbnailScheduler::Request(
		const FAssetThumbnailRequest& Request,
		std::string& OutError
	) -> bool
	{
		if (Impl->bShuttingDown)
		{
			OutError = "Thumbnail requests are closed during shutdown.";
			return false;
		}
		const FAssetThumbnailProviderHandle Handle = Impl->Registry.Find(Request.Asset.AssetClassName);
		if (!Handle)
		{
			OutError = std::format(
				"No thumbnail provider is registered for asset class {}.",
				Request.Asset.AssetClassName);
			return false;
		}

		FAssetThumbnailGenerationRequest GenerationRequest;
		FAssetThumbnailProviderRegistration Registration;
		if (!Impl->Registry.Capture(
				Request,
				Handle.Generation,
				GenerationRequest,
				Registration,
				OutError))
		{
			FImpl::FEntry& Entry = Impl->Entries[Request.Asset.VirtualPath.ToString()];
			if (Request.RequestSerial >= Entry.GenerationRequest.RequestSerial)
			{
				Entry.GenerationRequest.Cancellation.Cancel();
				Impl->RemoveQueuedJob(Entry.CacheKey);
				Entry = {};
				Entry.State = EAssetThumbnailState::Invalid;
				Entry.GenerationRequest.RequestSerial = Request.RequestSerial;
				Entry.Diagnostic = OutError.empty()
					? "The thumbnail provider rejected the asset."
					: OutError;
				Entry.RequestSerial = Request.RequestSerial;
			}
			return false;
		}

		GenerationRequest.KeyInput.Asset = Request.Asset;
		GenerationRequest.KeyInput.ProviderName = Registration.ProviderName;
		GenerationRequest.KeyInput.GeneratorSchemaVersion = Registration.GeneratorSchemaVersion;
		GenerationRequest.ProviderGeneration = Handle.Generation;
		GenerationRequest.RequestSerial = Request.RequestSerial;
		const std::string CacheKey = BuildAssetThumbnailCacheKey(GenerationRequest.KeyInput);
		const std::string AssetPath = Request.Asset.VirtualPath.ToString();
		auto Existing = Impl->Entries.find(AssetPath);
		if (Existing != Impl->Entries.end())
		{
			FImpl::FEntry& Entry = Existing->second;
			if (Request.RequestSerial < Entry.RequestSerial)
			{
				OutError = "A newer thumbnail request is already active for this asset.";
				return false;
			}
			if (Entry.CacheKey == CacheKey)
			{
				if (Request.RequestSerial == Entry.RequestSerial)
				{
					if (Entry.State == EAssetThumbnailState::Queued
						&& Request.Priority == EAssetThumbnailPriority::Visible)
					{
						for (FAssetThumbnailScheduledJob& Job : Impl->Queue)
							if (Job.CacheKey == CacheKey)
								Job.Priority = EAssetThumbnailPriority::Visible;
					}
					OutError.clear();
					return true;
				}
				if (Entry.State == EAssetThumbnailState::Queued)
				{
					Entry.RequestSerial = Request.RequestSerial;
					Entry.GenerationRequest.RequestSerial = Request.RequestSerial;
					for (FAssetThumbnailScheduledJob& Job : Impl->Queue)
					{
						if (Job.CacheKey != CacheKey) continue;
						Job.GenerationRequest.RequestSerial = Request.RequestSerial;
						if (Request.Priority == EAssetThumbnailPriority::Visible)
							Job.Priority = EAssetThumbnailPriority::Visible;
					}
					OutError.clear();
					return true;
				}
			}
			Entry.GenerationRequest.Cancellation.Cancel();
			Impl->RemoveQueuedJob(Entry.CacheKey);
			Impl->Entries.erase(Existing);
		}

		if (Impl->Queue.size() >= Impl->Budgets.MaximumQueuedJobs)
		{
			OutError = "The thumbnail request queue budget is exhausted.";
			return false;
		}

		FImpl::FEntry Entry;
		Entry.CacheKey = CacheKey;
		Entry.State = EAssetThumbnailState::Queued;
		Entry.GenerationRequest = GenerationRequest;
		Entry.RequestSerial = Request.RequestSerial;
		Impl->Entries.emplace(AssetPath, std::move(Entry));
		Impl->Queue.push_back({
			.CacheKey = CacheKey,
			.Priority = Request.Priority,
			.GenerationRequest = std::move(GenerationRequest)});
		OutError.clear();
		return true;
	}

	auto FAssetThumbnailScheduler::Find(const FAssetPath& AssetPath) const -> FAssetThumbnailView
	{
		const auto It = Impl->Entries.find(AssetPath.ToString());
		if (It == Impl->Entries.end()) return {};
		const FImpl::FEntry& Entry = It->second;
		return {
			.State = Entry.State,
			.Diagnostic = Entry.Diagnostic,
			.RequestSerial = Entry.RequestSerial};
	}

	auto FAssetThumbnailScheduler::TakeNext() -> std::optional<FAssetThumbnailScheduledJob>
	{
		if (Impl->bShuttingDown || Impl->Queue.empty()) return std::nullopt;
		auto Selected = std::ranges::find(
			Impl->Queue,
			EAssetThumbnailPriority::Visible,
			&FAssetThumbnailScheduledJob::Priority);
		if (Selected == Impl->Queue.end()) Selected = Impl->Queue.begin();
		FAssetThumbnailScheduledJob Job = std::move(*Selected);
		Impl->Queue.erase(Selected);
		const std::string AssetPath = Job.GenerationRequest.KeyInput.Asset.VirtualPath.ToString();
		const auto Entry = Impl->Entries.find(AssetPath);
		const FAssetThumbnailProviderHandle CurrentProvider = Impl->Registry.Find(
			Job.GenerationRequest.KeyInput.Asset.AssetClassName);
		const bool bMatchesCurrentEntry = Entry != Impl->Entries.end()
			&& Entry->second.CacheKey == Job.CacheKey
			&& Entry->second.GenerationRequest.RequestSerial
				== Job.GenerationRequest.RequestSerial
			&& Entry->second.GenerationRequest.ProviderGeneration
				== Job.GenerationRequest.ProviderGeneration;
		if (!bMatchesCurrentEntry
			|| Job.GenerationRequest.Cancellation.IsCancelled()
			|| !CurrentProvider
			|| CurrentProvider.Generation != Job.GenerationRequest.ProviderGeneration)
		{
			Job.GenerationRequest.Cancellation.Cancel();
			if (bMatchesCurrentEntry) Impl->Entries.erase(Entry);
			return TakeNext();
		}
		Entry->second.State = EAssetThumbnailState::Loading;
		return Job;
	}

	auto FAssetThumbnailScheduler::Transition(
		const FAssetThumbnailScheduledJob& Job,
		EAssetThumbnailState ExpectedState,
		EAssetThumbnailState NextState,
		uint64 AssetRevision,
		uint64 ResourceRevision,
		std::string_view Diagnostic
	) -> bool
	{
		if (Impl->bShuttingDown || Job.GenerationRequest.Cancellation.IsCancelled()) return false;
		const std::string AssetPath = Job.GenerationRequest.KeyInput.Asset.VirtualPath.ToString();
		const auto It = Impl->Entries.find(AssetPath);
		if (It == Impl->Entries.end()) return false;
		FImpl::FEntry& Entry = It->second;
		const FAssetThumbnailProviderHandle CurrentProvider =
			Impl->Registry.Find(Job.GenerationRequest.KeyInput.Asset.AssetClassName);
		if (Entry.CacheKey != Job.CacheKey
			|| Entry.State != ExpectedState
			|| Entry.RequestSerial != Job.GenerationRequest.RequestSerial
			|| Entry.GenerationRequest.ProviderGeneration != Job.GenerationRequest.ProviderGeneration
			|| !CurrentProvider
			|| CurrentProvider.Generation != Job.GenerationRequest.ProviderGeneration
			|| Entry.GenerationRequest.KeyInput.Asset != Job.GenerationRequest.KeyInput.Asset
			|| (Entry.GenerationRequest.AssetRevision != 0
				&& Entry.GenerationRequest.AssetRevision != AssetRevision)
			|| (Entry.GenerationRequest.ResourceRevision != 0
				&& Entry.GenerationRequest.ResourceRevision != ResourceRevision))
			return false;

		if (AssetRevision != 0) Entry.GenerationRequest.AssetRevision = AssetRevision;
		if (ResourceRevision != 0) Entry.GenerationRequest.ResourceRevision = ResourceRevision;
		Entry.State = NextState;
		Entry.Diagnostic = std::string(Diagnostic);
		return true;
	}

	auto FAssetThumbnailScheduler::Cancel(const FAssetPath& AssetPath) -> void
	{
		const auto It = Impl->Entries.find(AssetPath.ToString());
		if (It == Impl->Entries.end()) return;
		It->second.GenerationRequest.Cancellation.Cancel();
		Impl->RemoveQueuedJob(It->second.CacheKey);
		Impl->Entries.erase(It);
	}

	auto FAssetThumbnailScheduler::CancelAll() -> void
	{
		for (auto& [AssetPath, Entry] : Impl->Entries)
			Entry.GenerationRequest.Cancellation.Cancel();
		Impl->Queue.clear();
		Impl->Entries.clear();
	}

	auto FAssetThumbnailScheduler::Shutdown() -> void
	{
		if (Impl->bShuttingDown) return;
		Impl->bShuttingDown = true;
		CancelAll();
	}

	auto FAssetThumbnailScheduler::NumQueued() const -> size_t
	{
		return Impl->Queue.size();
	}

	auto FAssetThumbnailScheduler::IsShuttingDown() const -> bool
	{
		return Impl->bShuttingDown;
	}

	auto BuildAssetThumbnailDependencyClosure(
		const FAssetPath& Root,
		std::span<const FAssetThumbnailDependencyNode> RegistrySnapshot,
		std::vector<FAssetThumbnailPackageFingerprint>& OutDependencies,
		std::string& OutError) -> bool
	{
		OutDependencies.clear();
		OutError.clear();
		if (!Root.IsValid())
		{
			OutError = "The thumbnail dependency root is invalid.";
			return false;
		}

		std::unordered_map<std::string_view, const FAssetThumbnailDependencyNode*> Nodes;
		Nodes.reserve(RegistrySnapshot.size());
		for (const FAssetThumbnailDependencyNode& Node : RegistrySnapshot)
		{
			if (!Node.Package.VirtualPath.IsValid())
			{
				OutError = "The Asset Registry snapshot contains an invalid package path.";
				return false;
			}
			const auto [It, bInserted] = Nodes.emplace(Node.Package.VirtualPath.GetView(), &Node);
			if (!bInserted)
			{
				OutError = std::format("The Asset Registry snapshot contains a duplicate entry for '{}'.",
					Node.Package.VirtualPath.GetView());
				return false;
			}
		}

		const auto RootIt = Nodes.find(Root.GetView());
		if (RootIt == Nodes.end())
		{
			OutError = std::format("The Asset Registry has no entry for thumbnail root '{}'.", Root.GetView());
			return false;
		}

		std::unordered_set<std::string_view> Visited;
		std::vector<const FAssetThumbnailDependencyNode*> Pending{RootIt->second};
		Visited.emplace(Root.GetView());
		while (!Pending.empty())
		{
			const FAssetThumbnailDependencyNode* Node = Pending.back();
			Pending.pop_back();

			std::vector<std::string_view> SortedDependencies;
			SortedDependencies.reserve(Node->Dependencies.size());
			for (const FAssetPath& Dependency : Node->Dependencies)
				SortedDependencies.push_back(Dependency.GetView());
			std::ranges::sort(SortedDependencies);

			for (const std::string_view DependencyPath : SortedDependencies)
			{
				if (!Visited.emplace(DependencyPath).second) continue;
				const auto DependencyIt = Nodes.find(DependencyPath);
				if (DependencyIt == Nodes.end())
				{
					OutDependencies.clear();
					OutError = std::format("The Asset Registry has no entry for thumbnail dependency '{}'.", DependencyPath);
					return false;
				}
				OutDependencies.push_back(DependencyIt->second->Package);
				Pending.push_back(DependencyIt->second);
			}
		}

		std::ranges::sort(OutDependencies, {}, [](const FAssetThumbnailPackageFingerprint& Package) {
			return Package.VirtualPath.GetView();
		});
		return true;
	}

	auto BuildAssetThumbnailCacheKey(const FAssetThumbnailKeyInput& Input) -> std::string
	{
		FThumbnailKeyWriter Writer;
		Writer.WriteString("DurinAssetThumbnailKey");
		Writer.WriteU32(1);
		WritePackageFingerprint(Writer, Input.Asset);
		Writer.WriteString(Input.ProviderName);
		Writer.WriteU32(Input.GeneratorSchemaVersion);
		Writer.WriteU32(Input.Output.Width);
		Writer.WriteU32(Input.Output.Height);
		Writer.WriteU32(Input.Output.ColorSpaceVersion);
		Writer.WriteU32(Input.Output.EncodingVersion);
		Writer.WriteString(Input.PreviewFixtureIdentity);
		Writer.WriteU32(Input.PreviewFixtureVersion);
		Writer.WriteU32(Input.ShaderContractVersion);

		std::vector<FAssetThumbnailPackageFingerprint> Dependencies = Input.Dependencies;
		std::ranges::sort(Dependencies, {}, [](const FAssetThumbnailPackageFingerprint& Package) {
			return Package.VirtualPath.GetView();
		});
		Writer.WriteU64(static_cast<uint64>(Dependencies.size()));
		for (const FAssetThumbnailPackageFingerprint& Dependency : Dependencies)
			WritePackageFingerprint(Writer, Dependency);
		return FXxHash128::HashBuffer(Writer.GetBytes()).ToString();
	}
} // namespace Durin
