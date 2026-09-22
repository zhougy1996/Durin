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
		const IAssetThumbnailGenerationInput&) -> std::expected<std::unique_ptr<IThumbnailRendererSession>, std::string>
	{
		return std::unexpected("The thumbnail renderer produces no preview-scene session.");
	}
	namespace Detail
	{
		struct FAssetThumbnailGenerationLeaseState
		{
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
				std::shared_ptr<DThumbnailRenderer> Renderer;
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
			std::shared_ptr<DThumbnailRenderer> Renderer) -> std::expected<uint64, std::string>
		{
			if (State->bShuttingDown)
			{
				return std::unexpected("Thumbnail renderer registration is closed during shutdown.");
			}
			if (!Renderer)
			{
				return std::unexpected("Cannot register a null thumbnail renderer.");
			}
			const FThumbnailRenderingInfo Registration =
				Renderer->GetRegistration();
			if (Registration.AssetClassName.empty() || Registration.RendererName.empty()
				|| Registration.GeneratorSchemaVersion == 0)
			{
				return std::unexpected("Thumbnail renderers require an asset class, renderer name, and nonzero generator schema.");
			}
			if (State->Renderers.contains(Registration.AssetClassName))
			{
				return std::unexpected(std::format(
					"A thumbnail renderer is already registered for asset class {}.",
					Registration.AssetClassName));
			}
			const uint64 Generation = State->NextGeneration++;
			State->Renderers.emplace(
				Registration.AssetClassName,
				Detail::DThumbnailManagerState::FEntry{
					.Renderer = std::move(Renderer),
					.Generation = Generation});
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

	auto FAssetThumbnailGenerationRequest::BeginRenderedSession() const -> std::expected<IThumbnailRendererSession*, std::string>
	{
		const std::shared_ptr LeaseState = RendererLease.State;
		if (!LeaseState || !LeaseState->bActive.load(std::memory_order_acquire))
		{
			return std::unexpected("The thumbnail renderer registration is no longer active.");
		}
		if (LeaseState->Session)
		{
			return LeaseState->Session.get();
		}
		if (!LeaseState->Renderer)
		{
			return std::unexpected("The thumbnail renderer does not implement rendered generation sessions.");
		}
		if (!LeaseState->Input)
		{
			return std::unexpected("The thumbnail renderer did not capture generation input.");
		}
		auto Session = LeaseState->Renderer->CreateGenerationSession(*this, *LeaseState->Input);
		if (!Session)
			return std::unexpected(Session.error().empty()
				? "The thumbnail renderer could not create a generation session."
				: std::move(Session.error()));
		if (!*Session)
			return std::unexpected("The thumbnail renderer returned an empty generation session.");
		LeaseState->Session = std::move(*Session);
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
		std::shared_ptr<DThumbnailRenderer> Renderer) -> std::expected<void, std::string>
	{
		const auto Registration = RegisterRenderer(State, std::move(Renderer));
		if (!Registration) return std::unexpected(Registration.error());
		return {};
	}

	auto DThumbnailManager::RegisterScoped(
		std::unique_ptr<DThumbnailRenderer> Renderer)
		-> std::expected<FThumbnailRendererRegistrationHandle, std::string>
	{
		std::shared_ptr<DThumbnailRenderer> SharedRenderer = std::move(Renderer);
		const auto RegistrationId = RegisterRenderer(State, std::move(SharedRenderer));
		if (!RegistrationId) return std::unexpected(RegistrationId.error());
		return FThumbnailRendererRegistrationHandle(State, *RegistrationId);
	}

	auto DThumbnailManager::Unregister(std::string_view AssetClassName) -> bool
	{
		const auto It = State->Renderers.find(std::string(AssetClassName));
		return It != State->Renderers.end()
			&& RemoveRendererRegistration(State, It->second.Generation);
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
		uint64 RendererGeneration) -> std::expected<FAssetThumbnailGenerationRequest, std::string>
	{
		const auto It = State->Renderers.find(Request.Asset.AssetClassName);
		if (It == State->Renderers.end() || It->second.Generation != RendererGeneration)
		{
			return std::unexpected(std::format(
				"No current thumbnail renderer is registered for asset class {}.",
				Request.Asset.AssetClassName));
		}
		Detail::DThumbnailManagerState::FEntry& Entry = It->second;
		auto Captured = Entry.Renderer->CaptureGenerationRequest(Request, RendererGeneration);
		if (!Captured) return std::unexpected(std::move(Captured.error()));
		auto& GenerationRequest = *Captured;

		const auto Registration = Entry.Renderer->GetRegistration();
		GenerationRequest.KeyInput.Asset = Request.Asset;
		GenerationRequest.KeyInput.RendererName = Registration.RendererName;
		GenerationRequest.KeyInput.GeneratorSchemaVersion = Registration.GeneratorSchemaVersion;
		GenerationRequest.RendererGeneration = RendererGeneration;
		GenerationRequest.RequestSerial = Request.RequestSerial;
		auto LeaseState =
			std::make_shared<Detail::FAssetThumbnailGenerationLeaseState>();
		LeaseState->Cancellation = GenerationRequest.Cancellation;
		LeaseState->Input = std::move(GenerationRequest.Input);
		LeaseState->Renderer = Entry.Renderer;
		Entry.Leases.erase(
			std::remove_if(
				Entry.Leases.begin(),
				Entry.Leases.end(),
				[](const auto& Lease) { return Lease.expired(); }),
			Entry.Leases.end());
		Entry.Leases.push_back(LeaseState);
		GenerationRequest.RendererLease = FAssetThumbnailGenerationLease(std::move(LeaseState));
		return std::move(GenerationRequest);
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
