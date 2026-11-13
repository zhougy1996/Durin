#include "Thumbnail/ThumbnailManager.h"

#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailGenerationRequest.h"
#include "Thumbnail/ThumbnailRenderJob.h"
#include "Thumbnail/AssetThumbnailRequestQueue.h"
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
			};

			std::unordered_map<std::string, FEntry> Renderers;
			std::vector<FAssetThumbnailPool*> Pools;
			std::vector<FAssetThumbnailRequestQueue*> Queues;
			auto RetireRenderer(std::string_view AssetClassName) -> void
			{
				for (auto* Queue : Queues) Queue->CancelRenderer(AssetClassName);
				for (auto* Pool : Pools) Pool->InvalidateRenderer(AssetClassName);
			}

			bool bShuttingDown = false;
		};
	} // namespace ThumbnailPrivate

	namespace
	{
		auto RemoveRendererRegistration(
			const std::shared_ptr<ThumbnailPrivate::DThumbnailManagerState>& State,
			std::string_view AssetClassName, DObject* ExpectedRenderer) -> bool
		{
			if (!State || !ExpectedRenderer) return false;
			const auto It = State->Renderers.find(std::string(AssetClassName));
			if (It == State->Renderers.end() || It->second.Renderer.Get() != ExpectedRenderer) return false;
			auto Entry = std::move(It->second);
			const std::string Name = It->first;
			State->Renderers.erase(It);
			State->RetireRenderer(Name);
			return true;
		}

		auto RegisterRenderer(
			const std::shared_ptr<ThumbnailPrivate::DThumbnailManagerState>& State,
			TStrongObjectPtr<DThumbnailRenderer> Renderer, std::string AssetClassName) -> bool
		{
			if (State->bShuttingDown)
			{
				return false;
			}
			if (!Renderer)
			{
				return false;
			}
			FThumbnailRenderingInfo Registration = Renderer->GetRegistration();
			Registration.AssetClassName = std::move(AssetClassName);
			if (Registration.AssetClassName.empty() || Registration.RendererName.empty()
				|| Registration.GeneratorSchemaVersion == 0)
			{
				return false;
			}
			if (State->Renderers.contains(Registration.AssetClassName))
			{
				return false;
			}
			State->Renderers.emplace(
				Registration.AssetClassName,
				ThumbnailPrivate::DThumbnailManagerState::FEntry{
					.Renderer = std::move(Renderer)});
			return true;
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

	FThumbnailRendererRegistrationHandle::FThumbnailRendererRegistrationHandle(
		std::weak_ptr<ThumbnailPrivate::DThumbnailManagerState> InState,
		std::string InAssetClassName, DThumbnailRenderer* InRenderer)
		: State(std::move(InState)), AssetClassName(std::move(InAssetClassName)), Renderer(InRenderer) {}

	FThumbnailRendererRegistrationHandle::~FThumbnailRendererRegistrationHandle()
	{
		Reset();
	}

	FThumbnailRendererRegistrationHandle::FThumbnailRendererRegistrationHandle(
		FThumbnailRendererRegistrationHandle&& Other) noexcept
		: State(std::move(Other.State))
		, AssetClassName(std::exchange(Other.AssetClassName, {}))
		, Renderer(std::exchange(Other.Renderer, {}))
	{
	}

	auto FThumbnailRendererRegistrationHandle::operator=(
		FThumbnailRendererRegistrationHandle&& Other) noexcept
		-> FThumbnailRendererRegistrationHandle&
	{
		if (this == &Other) return *this;
		Reset();
		State = std::move(Other.State);
		AssetClassName = std::exchange(Other.AssetClassName, {});
		Renderer = std::exchange(Other.Renderer, {});
		return *this;
	}

	auto FThumbnailRendererRegistrationHandle::Reset() -> void
	{
		if (AssetClassName.empty()) return;
		if (const std::shared_ptr RegistryState = State.lock())
			RemoveRendererRegistration(RegistryState, AssetClassName, Renderer.Get());
		State.reset();
		AssetClassName.clear();
		Renderer = nullptr;
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
		const bool bRegistered = RegisterRenderer(State, TStrongObjectPtr<DThumbnailRenderer>(Renderer), AssetClassName);
		if (!bRegistered) return {};
		State->Renderers.at(AssetClassName).ClassNeedingThumbnail = AssetClass;
		return FThumbnailRendererRegistrationHandle(State, std::move(AssetClassName), Renderer);
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
		return It != State->Renderers.end() && RemoveRendererRegistration(State, It->first, It->second.Renderer.Get());
	}

	auto DThumbnailManager::Find(std::string_view AssetClassName) const -> DThumbnailRenderer*
	{
		const auto It = State->Renderers.find(std::string(AssetClassName));
		return It == State->Renderers.end() ? nullptr : It->second.Renderer.Get();
	}

	auto DThumbnailManager::AttachQueue(FAssetThumbnailRequestQueue* Queue) -> void { State->Queues.push_back(Queue); }
	auto DThumbnailManager::DetachQueue(FAssetThumbnailRequestQueue* Queue) -> void { std::erase(State->Queues, Queue); }

	auto DThumbnailManager::AttachPool(FAssetThumbnailPool* Pool) -> void { State->Pools.push_back(Pool); }
	auto DThumbnailManager::DetachPool(FAssetThumbnailPool* Pool) -> void { std::erase(State->Pools, Pool); }
	auto DThumbnailManager::SetLoadedObjectOverride(std::string_view Name, DObject* Object) -> void
	{
		State->Renderers.at(std::string(Name)).LoadedOverride = TStrongObjectPtr<DObject>(Object);
	}
	auto DThumbnailManager::CreateRenderJob(const FAssetThumbnailGenerationRequest& Request) -> std::unique_ptr<FThumbnailRenderJob>
	{
		const auto It = State->Renderers.find(Request.KeyInput.Asset.AssetClassName);
		if (It == State->Renderers.end() || Request.Cancellation.IsCancelled()) return nullptr;
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
			RemoveRendererRegistration(State, State->Renderers.begin()->first, State->Renderers.begin()->second.Renderer.Get());
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
