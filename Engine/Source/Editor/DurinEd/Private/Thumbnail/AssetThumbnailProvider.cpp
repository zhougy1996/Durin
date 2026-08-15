#include "Thumbnail/AssetThumbnailProvider.h"

#include "AssetLoad.h"
#include "Thumbnail/RenderedAssetThumbnailExtension.h"

namespace Durin::Editor
{
	namespace Detail
	{
		struct FAssetThumbnailGenerationLeaseState
		{
			FModuleOwnedResourceLease OwnerResource;
			FModuleOwnedCallbackGate OwnerGate;
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
				FModuleOwnedResourceLease OwnerResource;
				std::shared_ptr<IAssetThumbnailProvider> Provider;
				FModuleOwnedCallbackGate OwnerGate;
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
			FModuleOwnedCallbackGate OwnerGate,
			std::string& OutError) -> uint64
		{
			auto Invocation = OwnerGate.TryEnter();
			if (OwnerGate.IsValid() && !Invocation)
			{
				OutError = "Thumbnail provider owner is retiring.";
				return 0;
			}
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
			FModuleOwnedResourceLease Resource = OwnerGate.RetainResource();
			if (OwnerGate.IsValid() && !Resource)
			{
				OutError = "Thumbnail provider owner is retiring.";
				return 0;
			}
			State->Providers.emplace(
				Registration.AssetClassName,
				Detail::FAssetThumbnailProviderRegistryState::FEntry{
					.OwnerResource = std::move(Resource),
					.Provider = std::move(Provider),
					.OwnerGate = std::move(OwnerGate),
					.Generation = Generation});
			OutError.clear();
			return Generation;
		}
	} // namespace

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
		auto Invocation = LeaseState->OwnerGate.TryEnter();
		if (LeaseState->OwnerGate.IsValid() && !Invocation)
		{
			OutError = "The thumbnail provider owner is retiring.";
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
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError
	) -> bool
	{
		return RegisterProvider(
			State, std::move(Provider), std::move(OwnerGate), OutError) != 0;
	}

	auto FAssetThumbnailProviderRegistry::RegisterScoped(
		std::unique_ptr<IAssetThumbnailProvider> Provider,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FAssetThumbnailProviderRegistrationHandle
	{
		std::shared_ptr<IAssetThumbnailProvider> SharedProvider = std::move(Provider);
		const uint64 RegistrationId =
			RegisterProvider(State, std::move(SharedProvider),
				std::move(OwnerGate), OutError);
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
		auto Invocation = It->second.OwnerGate.TryEnter();
		if (It->second.OwnerGate.IsValid() && !Invocation)
		{
			OutError = "The thumbnail provider owner is retiring.";
			return false;
		}
		return It->second.Provider->CaptureSourceImage(Asset, OutSource, OutError);
	}

	auto FAssetThumbnailProviderRegistry::UsesSourceImage(
		std::string_view AssetClassName) const -> bool
	{
		const auto It = State->Providers.find(std::string(AssetClassName));
		if (It == State->Providers.end()) return false;
		auto Invocation = It->second.OwnerGate.TryEnter();
		return (!It->second.OwnerGate.IsValid() || Invocation)
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
		auto Invocation = Entry.OwnerGate.TryEnter();
		if (Entry.OwnerGate.IsValid() && !Invocation)
		{
			OutError = "The thumbnail provider owner is retiring.";
			return false;
		}
		if (!Entry.Provider->CaptureGenerationRequest(
				Request, ProviderGeneration, OutRequest, OutError))
			return false;

		OutRegistration = Entry.Provider->GetRegistration();
		auto LeaseState =
			std::make_shared<Detail::FAssetThumbnailGenerationLeaseState>();
		LeaseState->OwnerResource = Entry.OwnerGate.RetainResource();
		if (Entry.OwnerGate.IsValid() && !LeaseState->OwnerResource)
		{
			OutError = "The thumbnail provider owner is retiring.";
			return false;
		}
		LeaseState->OwnerGate = Entry.OwnerGate;
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

} // namespace Durin::Editor
