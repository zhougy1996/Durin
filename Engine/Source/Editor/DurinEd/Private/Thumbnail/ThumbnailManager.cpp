#include "Thumbnail/ThumbnailManager.h"

#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/ThumbnailRenderer.h"

namespace Durin::Editor
{
	DThumbnailRenderer::DThumbnailRenderer()
		: DObject()
	{
	}

	DThumbnailRenderer::DThumbnailRenderer(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DThumbnailRenderer::CreateGenerationSession(
		const FAssetThumbnailGenerationRequest&,
		const IAssetThumbnailGenerationInput&,
		std::string& OutError) -> std::unique_ptr<IThumbnailRendererSession>
	{
		OutError = "The thumbnail renderer produces no preview-scene session.";
		return {};
	}
	namespace Detail
	{
		struct FAssetThumbnailGenerationLeaseState
		{
			FModuleOwnedResourceLease OwnerResource;
			FModuleOwnedCallbackGate OwnerGate;
			FAssetThumbnailCancellation Cancellation;
			std::shared_ptr<const IAssetThumbnailGenerationInput> Input;
			std::shared_ptr<DThumbnailRenderer> Renderer;
			std::unique_ptr<IThumbnailRendererSession> Session;
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
				Renderer.reset();
			}
		};

		struct DThumbnailManagerState
		{
			struct FEntry
			{
				FModuleOwnedResourceLease OwnerResource;
				std::shared_ptr<DThumbnailRenderer> Renderer;
				FModuleOwnedCallbackGate OwnerGate;
				std::vector<std::weak_ptr<FAssetThumbnailGenerationLeaseState>> Leases;
				uint64 Generation = 0;
			};

			std::unordered_map<std::string, FEntry> Renderers;
			uint64 NextGeneration = 1;
			bool bShuttingDown = false;
		};
	} // namespace Detail

	namespace
	{
		auto InvalidateRendererEntry(
			Detail::DThumbnailManagerState::FEntry& Entry) -> void
		{
			for (const std::weak_ptr<Detail::FAssetThumbnailGenerationLeaseState>& WeakLease
				: Entry.Leases)
			{
				if (const std::shared_ptr Lease = WeakLease.lock()) Lease->Invalidate();
			}
			Entry.Leases.clear();
			Entry.Renderer.reset();
		}

		auto RemoveRendererRegistration(
			const std::shared_ptr<Detail::DThumbnailManagerState>& State,
			uint64 RegistrationId) -> bool
		{
			if (!State || RegistrationId == 0) return false;
			const auto It = std::ranges::find_if(
				State->Renderers,
				[RegistrationId](const auto& Pair) {
					return Pair.second.Generation == RegistrationId;
				});
			if (It == State->Renderers.end()) return false;
			Detail::DThumbnailManagerState::FEntry Entry =
				std::move(It->second);
			State->Renderers.erase(It);
			InvalidateRendererEntry(Entry);
			return true;
		}

		auto RegisterRenderer(
			const std::shared_ptr<Detail::DThumbnailManagerState>& State,
			std::shared_ptr<DThumbnailRenderer> Renderer,
			FModuleOwnedCallbackGate OwnerGate,
			std::string& OutError) -> uint64
		{
			auto Invocation = OwnerGate.TryEnter();
			if (OwnerGate.IsValid() && !Invocation)
			{
				OutError = "Thumbnail renderer owner is retiring.";
				return 0;
			}
			if (State->bShuttingDown)
			{
				OutError = "Thumbnail renderer registration is closed during shutdown.";
				return 0;
			}
			if (!Renderer)
			{
				OutError = "Cannot register a null thumbnail renderer.";
				return 0;
			}
			const FThumbnailRenderingInfo Registration =
				Renderer->GetRegistration();
			if (Registration.AssetClassName.empty() || Registration.RendererName.empty()
				|| Registration.GeneratorSchemaVersion == 0)
			{
				OutError = "Thumbnail renderers require an asset class, renderer name, and nonzero generator schema.";
				return 0;
			}
			if (State->Renderers.contains(Registration.AssetClassName))
			{
				OutError = std::format(
					"A thumbnail renderer is already registered for asset class {}.",
					Registration.AssetClassName);
				return 0;
			}
			const uint64 Generation = State->NextGeneration++;
			FModuleOwnedResourceLease Resource = OwnerGate.RetainResource();
			if (OwnerGate.IsValid() && !Resource)
			{
				OutError = "Thumbnail renderer owner is retiring.";
				return 0;
			}
			State->Renderers.emplace(
				Registration.AssetClassName,
				Detail::DThumbnailManagerState::FEntry{
					.OwnerResource = std::move(Resource),
					.Renderer = std::move(Renderer),
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
		-> IThumbnailRendererSession*
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
		return RendererLease.GetInput();
	}

	auto FAssetThumbnailGenerationRequest::BeginRenderedSession(
		std::string& OutError) const -> IThumbnailRendererSession*
	{
		const std::shared_ptr LeaseState = RendererLease.State;
		if (!LeaseState || !LeaseState->bActive.load(std::memory_order_acquire))
		{
			OutError = "The thumbnail renderer registration is no longer active.";
			return nullptr;
		}
		auto Invocation = LeaseState->OwnerGate.TryEnter();
		if (LeaseState->OwnerGate.IsValid() && !Invocation)
		{
			OutError = "The thumbnail renderer owner is retiring.";
			return nullptr;
		}
		if (LeaseState->Session)
		{
			OutError.clear();
			return LeaseState->Session.get();
		}
		if (!LeaseState->Renderer)
		{
			OutError = "The thumbnail renderer does not implement rendered generation sessions.";
			return nullptr;
		}
		if (!LeaseState->Input)
		{
			OutError = "The thumbnail renderer did not capture generation input.";
			return nullptr;
		}
		LeaseState->Session = LeaseState->Renderer->CreateGenerationSession(
			*this, *LeaseState->Input, OutError);
		if (!LeaseState->Session)
		{
			if (OutError.empty())
				OutError = "The thumbnail renderer could not create a generation session.";
			return nullptr;
		}
		OutError.clear();
		return LeaseState->Session.get();
	}

	auto FAssetThumbnailGenerationRequest::GetRenderedSession() const
		-> IThumbnailRendererSession*
	{
		return RendererLease.GetRenderedSession();
	}

	auto FAssetThumbnailGenerationRequest::ReleaseRenderedSession() const -> void
	{
		RendererLease.ReleaseRenderedSession();
	}

	FThumbnailRendererRegistrationHandle::~FThumbnailRendererRegistrationHandle()
	{
		Reset();
	}

	FThumbnailRendererRegistrationHandle::FThumbnailRendererRegistrationHandle(
		FThumbnailRendererRegistrationHandle&& Other) noexcept
		: State(std::move(Other.State))
		, RegistrationId(std::exchange(Other.RegistrationId, 0))
	{
	}

	auto FThumbnailRendererRegistrationHandle::operator=(
		FThumbnailRendererRegistrationHandle&& Other) noexcept
		-> FThumbnailRendererRegistrationHandle&
	{
		if (this == &Other) return *this;
		Reset();
		State = std::move(Other.State);
		RegistrationId = std::exchange(Other.RegistrationId, 0);
		return *this;
	}

	auto FThumbnailRendererRegistrationHandle::Reset() -> void
	{
		if (RegistrationId == 0) return;
		if (const std::shared_ptr RegistryState = State.lock())
			RemoveRendererRegistration(RegistryState, RegistrationId);
		State.reset();
		RegistrationId = 0;
	}

	DThumbnailManager::DThumbnailManager()
		: State(std::make_shared<Detail::DThumbnailManagerState>())
	{
	}

	DThumbnailManager::~DThumbnailManager()
	{
		Shutdown();
	}

	auto DThumbnailManager::Register(
		std::shared_ptr<DThumbnailRenderer> Renderer,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError
	) -> bool
	{
		return RegisterRenderer(
			State, std::move(Renderer), std::move(OwnerGate), OutError) != 0;
	}

	auto DThumbnailManager::RegisterScoped(
		std::unique_ptr<DThumbnailRenderer> Renderer,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FThumbnailRendererRegistrationHandle
	{
		std::shared_ptr<DThumbnailRenderer> SharedRenderer = std::move(Renderer);
		const uint64 RegistrationId =
			RegisterRenderer(State, std::move(SharedRenderer),
				std::move(OwnerGate), OutError);
		return RegistrationId != 0
			? FThumbnailRendererRegistrationHandle(State, RegistrationId)
			: FThumbnailRendererRegistrationHandle{};
	}

	auto DThumbnailManager::Unregister(
		std::string_view AssetClassName,
		std::string& OutError
	) -> bool
	{
		const auto It = State->Renderers.find(std::string(AssetClassName));
		if (It == State->Renderers.end())
		{
			OutError = std::format(
				"No thumbnail renderer is registered for asset class {}.",
				AssetClassName);
			return false;
		}
		const uint64 RegistrationId = It->second.Generation;
		RemoveRendererRegistration(State, RegistrationId);
		OutError.clear();
		return true;
	}

	auto DThumbnailManager::Find(
		std::string_view AssetClassName
	) const -> FThumbnailRendererHandle
	{
		const auto It = State->Renderers.find(std::string(AssetClassName));
		if (It == State->Renderers.end()) return {};
		return {.Generation = It->second.Generation};
	}

	auto DThumbnailManager::Capture(
		const FAssetThumbnailRequest& Request,
		uint64 RendererGeneration,
		FAssetThumbnailGenerationRequest& OutRequest,
		FThumbnailRenderingInfo& OutRegistration,
		std::string& OutError) -> bool
	{
		const auto It = State->Renderers.find(Request.Asset.AssetClassName);
		if (It == State->Renderers.end() || It->second.Generation != RendererGeneration)
		{
			OutError = std::format(
				"No current thumbnail renderer is registered for asset class {}.",
				Request.Asset.AssetClassName);
			return false;
		}
		Detail::DThumbnailManagerState::FEntry& Entry = It->second;
		auto Invocation = Entry.OwnerGate.TryEnter();
		if (Entry.OwnerGate.IsValid() && !Invocation)
		{
			OutError = "The thumbnail renderer owner is retiring.";
			return false;
		}
		if (!Entry.Renderer->CaptureGenerationRequest(
				Request, RendererGeneration, OutRequest, OutError))
			return false;

		OutRegistration = Entry.Renderer->GetRegistration();
		auto LeaseState =
			std::make_shared<Detail::FAssetThumbnailGenerationLeaseState>();
		LeaseState->OwnerResource = Entry.OwnerGate.RetainResource();
		if (Entry.OwnerGate.IsValid() && !LeaseState->OwnerResource)
		{
			OutError = "The thumbnail renderer owner is retiring.";
			return false;
		}
		LeaseState->OwnerGate = Entry.OwnerGate;
		LeaseState->Cancellation = OutRequest.Cancellation;
		LeaseState->Input = std::move(OutRequest.Input);
		LeaseState->Renderer = Entry.Renderer;
		Entry.Leases.erase(
			std::remove_if(
				Entry.Leases.begin(),
				Entry.Leases.end(),
				[](const auto& Lease) { return Lease.expired(); }),
			Entry.Leases.end());
		Entry.Leases.push_back(LeaseState);
		OutRequest.RendererLease = FAssetThumbnailGenerationLease(std::move(LeaseState));
		OutError.clear();
		return true;
	}

	auto DThumbnailManager::Shutdown() -> void
	{
		if (State->bShuttingDown) return;
		State->bShuttingDown = true;
		ResetSharedPool();
		while (!State->Renderers.empty())
			RemoveRendererRegistration(State, State->Renderers.begin()->second.Generation);
	}

	auto DThumbnailManager::IsShuttingDown() const -> bool
	{
		return State->bShuttingDown;
	}

	auto DThumbnailManager::Num() const -> size_t
	{
		return State->Renderers.size();
	}

	auto DThumbnailManager::GetSharedPool() -> FAssetThumbnailPool&
	{
		check(!State->bShuttingDown);
		if (!SharedPool) SharedPool = std::make_unique<FAssetThumbnailPool>(*this);
		return *SharedPool;
	}

	auto DThumbnailManager::ResetSharedPool() -> void
	{
		if (!SharedPool) return;
		SharedPool->Clear();
		SharedPool.reset();
	}

	auto GetDefaultThumbnailManager()
		-> DThumbnailManager&
	{
		static DThumbnailManager Registry;
		return Registry;
	}

} // namespace Durin::Editor
