#include "Thumbnail/AssetThumbnail.h"

#include "Hash/XxHash.h"

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

	struct FAssetThumbnailProviderRegistry::FImpl
	{
		struct FEntry
		{
			std::shared_ptr<IAssetThumbnailProvider> Provider;
			uint64 Generation = 0;
		};

		std::unordered_map<std::string, FEntry> Providers;
		uint64 NextGeneration = 1;
		bool bShuttingDown = false;
	};

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

	FAssetThumbnailProviderRegistry::FAssetThumbnailProviderRegistry()
		: Impl(std::make_unique<FImpl>())
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
		if (Impl->bShuttingDown)
		{
			OutError = "Thumbnail provider registration is closed during shutdown.";
			return false;
		}
		if (!Provider)
		{
			OutError = "Cannot register a null thumbnail provider.";
			return false;
		}
		const FAssetThumbnailProviderRegistration Registration = Provider->GetRegistration();
		if (Registration.AssetClassName.empty() || Registration.ProviderName.empty()
			|| Registration.GeneratorSchemaVersion == 0)
		{
			OutError = "Thumbnail providers require an asset class, provider name, and nonzero generator schema.";
			return false;
		}
		if (Impl->Providers.contains(Registration.AssetClassName))
		{
			OutError = std::format(
				"A thumbnail provider is already registered for asset class {}.",
				Registration.AssetClassName);
			return false;
		}
		Impl->Providers.emplace(
			Registration.AssetClassName,
			FImpl::FEntry{std::move(Provider), Impl->NextGeneration++});
		OutError.clear();
		return true;
	}

	auto FAssetThumbnailProviderRegistry::Unregister(
		std::string_view AssetClassName,
		std::string& OutError
	) -> bool
	{
		const auto It = Impl->Providers.find(std::string(AssetClassName));
		if (It == Impl->Providers.end())
		{
			OutError = std::format(
				"No thumbnail provider is registered for asset class {}.",
				AssetClassName);
			return false;
		}
		Impl->Providers.erase(It);
		OutError.clear();
		return true;
	}

	auto FAssetThumbnailProviderRegistry::Find(
		std::string_view AssetClassName
	) const -> FAssetThumbnailProviderHandle
	{
		const auto It = Impl->Providers.find(std::string(AssetClassName));
		if (It == Impl->Providers.end()) return {};
		return {It->second.Provider, It->second.Generation};
	}

	auto FAssetThumbnailProviderRegistry::Shutdown() -> void
	{
		if (Impl->bShuttingDown) return;
		Impl->bShuttingDown = true;
		Impl->Providers.clear();
	}

	auto FAssetThumbnailProviderRegistry::IsShuttingDown() const -> bool
	{
		return Impl->bShuttingDown;
	}

	auto FAssetThumbnailProviderRegistry::Num() const -> size_t
	{
		return Impl->Providers.size();
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
		if (!Handle.Provider->CaptureGenerationRequest(
				Request,
				Handle.Generation,
				GenerationRequest,
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

		const FAssetThumbnailProviderRegistration Registration = Handle.Provider->GetRegistration();
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
			if (Entry.CacheKey == CacheKey
				&& Entry.State != EAssetThumbnailState::Invalid
				&& Entry.State != EAssetThumbnailState::Failed)
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
		if (Entry == Impl->Entries.end() || Entry->second.CacheKey != Job.CacheKey
			|| Entry->second.GenerationRequest.RequestSerial != Job.GenerationRequest.RequestSerial
			|| Entry->second.GenerationRequest.ProviderGeneration != Job.GenerationRequest.ProviderGeneration)
		{
			Job.GenerationRequest.Cancellation.Cancel();
			return TakeNext();
		}
		Entry->second.State = EAssetThumbnailState::Loading;
		return Job;
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
