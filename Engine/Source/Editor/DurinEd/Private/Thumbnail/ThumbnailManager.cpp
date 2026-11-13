#include "Thumbnail/ThumbnailManager.h"

#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailGenerationRequest.h"
#include "Thumbnail/ThumbnailRenderJob.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"

namespace Durin
{
	namespace ThumbnailPrivate
	{
		struct DThumbnailManagerState
		{
			struct FEntry
			{
				DClass* ClassNeedingThumbnail = nullptr;
				TStrongObjectPtr<DThumbnailRenderer> Renderer;
				TStrongObjectPtr<DObject> LoadedOverride;
				std::vector<std::weak_ptr<std::atomic<bool>>> Cancellations;
				uint64 Generation = 0;
			};

			std::unordered_map<std::string, FEntry> Renderers;
			std::vector<FAssetThumbnailPool*> Pools;
			auto InvalidatePools(uint64 Generation) -> void
			{
				for (auto* Pool : Pools) Pool->InvalidateRenderer(Generation);
			}
			uint64 NextGeneration = 1;
			bool bShuttingDown = false;
		};
	} // namespace ThumbnailPrivate

	namespace
	{
		auto RemoveRendererRegistration(
			const std::shared_ptr<ThumbnailPrivate::DThumbnailManagerState>& State,
			uint64 RegistrationId) -> bool
		{
			if (!State || RegistrationId == 0) return false;
			const auto It = std::ranges::find_if(
				State->Renderers,
				[RegistrationId](const auto& Pair) {
					return Pair.second.Generation == RegistrationId;
				});
			if (It == State->Renderers.end()) return false;
			ThumbnailPrivate::DThumbnailManagerState::FEntry Entry =
				std::move(It->second);
			State->Renderers.erase(It);
			for (const auto& Weak : Entry.Cancellations) if (auto Flag = Weak.lock()) Flag->store(true, std::memory_order_release);
			State->InvalidatePools(Entry.Generation);
			Entry.Renderer.Reset();
			return true;
		}

		auto RegisterRenderer(
			const std::shared_ptr<ThumbnailPrivate::DThumbnailManagerState>& State,
			TStrongObjectPtr<DThumbnailRenderer> Renderer, std::string AssetClassName) -> uint64
		{
			if (State->bShuttingDown)
			{
				return 0;
			}
			if (!Renderer)
			{
				return 0;
			}
			FThumbnailRenderingInfo Registration = Renderer->GetRegistration();
			Registration.AssetClassName = std::move(AssetClassName);
			if (Registration.AssetClassName.empty() || Registration.RendererName.empty()
				|| Registration.GeneratorSchemaVersion == 0)
			{
				return 0;
			}
			if (State->Renderers.contains(Registration.AssetClassName))
			{
				return 0;
			}
			const uint64 Generation = State->NextGeneration++;
			State->Renderers.emplace(
				Registration.AssetClassName,
				ThumbnailPrivate::DThumbnailManagerState::FEntry{
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
		: State(std::make_shared<ThumbnailPrivate::DThumbnailManagerState>())
	{
	}

	DThumbnailManager::~DThumbnailManager()
	{
		Shutdown();
	}

	auto DThumbnailManager::RegisterCustomRenderer(DClass* AssetClass, DClass* RendererClass)
		-> FThumbnailRendererRegistrationHandle
	{
		if (!AssetClass || !AssetClass->IsChildOf(DObject::StaticClass())
			|| !CanConstructObjectOfClass(RendererClass, DThumbnailRenderer::StaticClass()))
			return {};
		if (State->bShuttingDown) return {};
		const auto Name = AssetClass->GetQualifiedName().ToString();
		if (State->Renderers.contains(Name)) return {};
		TStrongObjectPtr<DThumbnailRenderer> Renderer(static_cast<DThumbnailRenderer*>(NewObject(RendererClass, nullptr, {}, EObjectFlags::Transient)));
		return RegisterRendererInstance(Renderer.Get(), Name, AssetClass);
	}

	auto DThumbnailManager::RegisterRendererInstance(DThumbnailRenderer* Renderer, std::string AssetClassName, DClass* AssetClass)
		-> FThumbnailRendererRegistrationHandle
	{
		if (!Renderer || !IsValid(Renderer) || !Renderer->GetClass()->IsChildOf(DThumbnailRenderer::StaticClass()))
			return {};
		const auto Id = RegisterRenderer(State, TStrongObjectPtr<DThumbnailRenderer>(Renderer), AssetClassName);
		if (!Id) return {};
		State->Renderers.at(AssetClassName).ClassNeedingThumbnail = AssetClass;
		return FThumbnailRendererRegistrationHandle(State, Id);
	}

	auto DThumbnailManager::GetRenderingInfo(DClass* AssetClass) const -> std::optional<FThumbnailRenderingInfo>
	{
		if (!AssetClass) return std::nullopt;
		const auto It = State->Renderers.find(AssetClass->GetQualifiedName().ToString());
		if (It == State->Renderers.end()) return std::nullopt;
		auto Info = It->second.Renderer->GetRegistration();
		Info.AssetClassName = AssetClass->GetQualifiedName().ToString();
		Info.ClassNeedingThumbnail = It->second.ClassNeedingThumbnail;
		Info.RendererClass = It->second.Renderer->GetClass();
		Info.Renderer = It->second.Renderer.Get();
		return Info;
	}

	auto DThumbnailManager::UnregisterCustomRenderer(DClass* AssetClass) -> bool
	{
		return AssetClass && UnregisterRendererName(AssetClass->GetQualifiedName().ToString());
	}

	auto DThumbnailManager::UnregisterRendererName(std::string_view AssetClassName) -> bool
	{
		const auto It = State->Renderers.find(std::string(AssetClassName));
		return It != State->Renderers.end() && RemoveRendererRegistration(State, It->second.Generation);
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
		ThumbnailPrivate::DThumbnailManagerState::FEntry& Entry = It->second;
		auto Captured = Entry.Renderer->DescribeThumbnail(Request.Asset);
		if (!Captured) return std::unexpected(std::move(Captured.error()));
		FAssetThumbnailGenerationRequest GenerationRequest;
		GenerationRequest.KeyInput = Captured->MakeKeyInput(Request.Asset, Entry.Renderer->GetRegistration());
		GenerationRequest.bHasTransparency = Captured->bHasTransparency;
		GenerationRequest.RendererGeneration = RendererGeneration;
		GenerationRequest.RequestSerial = Request.RequestSerial;
		std::erase_if(Entry.Cancellations, [](const auto& Weak) { return Weak.expired(); });
		Entry.Cancellations.push_back(GenerationRequest.Cancellation.State);
		return std::move(GenerationRequest);
	}

	auto DThumbnailManager::AttachPool(FAssetThumbnailPool* Pool) -> void { State->Pools.push_back(Pool); }
	auto DThumbnailManager::DetachPool(FAssetThumbnailPool* Pool) -> void { std::erase(State->Pools, Pool); }
	auto DThumbnailManager::SetLoadedObjectOverride(std::string_view Name, DObject* Object) -> void
	{
		State->Renderers.at(std::string(Name)).LoadedOverride = TStrongObjectPtr<DObject>(Object);
	}
	auto DThumbnailManager::CreateRenderJob(const FAssetThumbnailGenerationRequest& Request) -> std::unique_ptr<FThumbnailRenderJob>
	{
		const auto It = State->Renderers.find(Request.KeyInput.Asset.AssetClassName);
		if (It == State->Renderers.end() || It->second.Generation != Request.RendererGeneration) return nullptr;
		const auto& Entry = It->second;
		return std::make_unique<FThumbnailRenderJob>(Entry.Renderer.Get(), Request.KeyInput.Asset,
			Entry.ClassNeedingThumbnail, Entry.LoadedOverride.Get());
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
		if (!SharedPool) SharedPool = std::make_unique<FAssetThumbnailPool>(
			*this, FAssetThumbnailBudgets{.bPreGenerateOnAssetChanges = true});
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

} // namespace Durin
