#include "RendererModule.h"

#include "Console/ConsoleCommand.h"
#include "CoreGlobals.h"
#include "Renderers/SceneRenderer.h"
#include "RDG.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "Scene.h"

#include <mutex>

namespace Durin
{
	namespace
	{
		struct FViewStateRoute
		{
			FRendererModule* Module = nullptr;
			FSceneRenderer* Renderer = nullptr;
		};

		std::atomic<uint64> GNextViewStateId = 1;
		std::mutex GViewStateRouteMutex;
		std::map<uint64, FViewStateRoute> GViewStateRoutes;

		auto AllocateViewStateId() -> uint64
		{
			uint64 Current = GNextViewStateId.load(std::memory_order_relaxed);
			while (Current != std::numeric_limits<uint64>::max())
			{
				if (GNextViewStateId.compare_exchange_weak(
						Current, Current + 1,
						std::memory_order_relaxed,
						std::memory_order_relaxed
					))
					return Current;
			}
			return 0;
		}

		auto ReportRejectedViewStateOperation(
			std::string_view Operation,
			FSceneViewStateId Id
		) -> void
		{
			static uint32 DiagnosticCount = 0;
			if (DiagnosticCount >= 16)
				return;
			++DiagnosticCount;
			DURIN_WARN(
				"Renderer ignored {} for stale view-state identity {}.",
				Operation, FSceneViewStateIdAccess::GetValue(Id)
			);
		}

	} // namespace

	FRendererModule::FRendererModule() = default;

	FRendererModule::~FRendererModule() = default;

	auto FRendererModule::StartupModule() -> void
	{
		ConsoleCallbacks =
			FModuleStartup::CreateOwnedCallbackRegistration("Core.ConsoleCommands");
		require(ConsoleCallbacks.IsValid());
		check(SceneRenderer == nullptr);
		SceneRenderer = std::make_unique<FSceneRenderer>();
		SetActiveDefaultTextureResources(
			&SceneRenderer->GetDefaultTextures()
		);
		const bool bCommandsRegistered =
			SceneRenderer->Start(
				FConsoleCommandRegistry::Get(), ConsoleCallbacks.GetGate()
			);
		checkf(
			bCommandsRegistered,
			"Failed to register renderer resource invalidation commands"
		);
		if (!bCommandsRegistered)
		{
			DURIN_ERROR(
				"Failed to register renderer resource invalidation commands"
			);
		}
		if (GDynamicRHI != nullptr)
		{
			FSceneRenderer* Renderer = SceneRenderer.get();
			ENQUEUE_RENDER_COMMAND(InitializeDefaultTextures)(
				[Renderer](FRHICommandListImmediate& CommandList) {
					Renderer->InitializeStartupResources_RenderThread(
						CommandList
					);
				}
			);
		}
	}

	auto FRendererModule::ShutdownModule() -> void
	{
		requiref(FScene::GetActiveSceneCount() == 0, "Renderer shutdown requires every scene owner to call Release first.");
		if (SceneRenderer == nullptr)
		{
			requiref(FScene::GetAllocatedSceneCount() == 0, "Renderer shutdown found scenes that were not deleted after Release.");
			return;
		}
		SceneRenderer->Stop();
		FSceneRenderer* Renderer = SceneRenderer.get();
		size_t LeakedViewStateCount = 0;
		ENQUEUE_RENDER_COMMAND(ReleaseRendererResources)(
			[Renderer, &LeakedViewStateCount](FRHICommandListImmediate&) {
				LeakedViewStateCount = Renderer->ReleaseViewStates_RenderThread();
				Renderer->ReleaseResources_RenderThread();
			}
		);
		FlushRenderingCommands();
		requiref(FScene::GetAllocatedSceneCount() == 0, "Renderer shutdown found scenes that were not deleted after Release.");
		{
			std::scoped_lock Lock(GViewStateRouteMutex);
			std::erase_if(GViewStateRoutes, [this](const auto& Entry) {
				return Entry.second.Module == this;
			});
		}
		if (LeakedViewStateCount != 0)
			DURIN_WARN(
				"Renderer shutdown released {} leaked persistent view state(s).",
				LeakedViewStateCount
			);
		SetActiveDefaultTextureResources(nullptr);
		SceneRenderer.reset();
	}

	auto FRendererModule::RequestResourceInvalidation(
		ERendererResourceInvalidationCause Cause
	) -> FConsoleCommandResult
	{
		if (SceneRenderer == nullptr)
			return FConsoleCommandResult::Failure(
				"Renderer resource invalidation is not available."
			);
		return SceneRenderer->GetResourceCoordinator().Request(Cause);
	}

	auto FRendererModule::GetResourceInvalidationSnapshot_RenderThread() const
		-> FRendererResourceInvalidationSnapshot
	{
		check(SceneRenderer != nullptr);
		return SceneRenderer->GetResourceCoordinator().GetSnapshot_RenderThread();
	}

	auto FRendererModule::CreateScene() -> FScenePtr
	{
		check(IsInGameThread());
		return FScenePtr(new FScene(), FSceneDeleter(&FScene::DestroyScene));
	}

	auto FRendererModule::CreateViewState() -> FSceneViewStateOwner
	{
		check(IsInGameThread());
		if (SceneRenderer == nullptr)
			return {};
		const uint64 Value = AllocateViewStateId();
		if (Value == 0)
		{
			DURIN_ERROR("Persistent view-state identity space is exhausted.");
			return {};
		}
		const FSceneViewStateId Id(Value);
		FSceneRenderer* Renderer = SceneRenderer.get();
		{
			std::scoped_lock Lock(GViewStateRouteMutex);
			const bool bInserted = GViewStateRoutes.emplace(
													   Value, FViewStateRoute{this, Renderer}
			)
									   .second;
			check(bInserted);
			if (!bInserted)
				return {};
		}
		ENQUEUE_RENDER_COMMAND(CreateSceneViewState)(
			[Renderer, Id](FRHICommandListImmediate&) {
				const bool bAdded = Renderer->AddViewState_RenderThread(Id);
				checkf(bAdded, "Renderer view-state IDs must be unique.");
			}
		);
		return FSceneViewStateOwner(Id, &FRendererModule::ReleaseViewState);
	}

	auto FRendererModule::ReleaseViewState(FSceneViewStateId Id) -> void
	{
		FSceneRenderer* Renderer = nullptr;
		{
			std::scoped_lock Lock(GViewStateRouteMutex);
			const auto Iterator = GViewStateRoutes.find(
				FSceneViewStateIdAccess::GetValue(Id)
			);
			if (Iterator == GViewStateRoutes.end())
				return;
			Renderer = Iterator->second.Renderer;
			GViewStateRoutes.erase(Iterator);
		}
		ENQUEUE_RENDER_COMMAND(RemoveSceneViewState)(
			[Renderer, Id](FRHICommandListImmediate&) {
				if (!Renderer->RemoveViewState_RenderThread(Id))
					ReportRejectedViewStateOperation("removal", Id);
			}
		);
	}

	auto FRendererModule::InvalidateViewState(FSceneViewStateId Id) -> void
	{
		if (SceneRenderer == nullptr || !Id.IsValid())
			return;
		FSceneRenderer* Renderer = SceneRenderer.get();
		ENQUEUE_RENDER_COMMAND(InvalidateSceneViewState)(
			[Renderer, Id](FRHICommandListImmediate&) {
				if (!Renderer->InvalidateViewState_RenderThread(Id))
					ReportRejectedViewStateOperation("invalidation", Id);
			}
		);
	}

	auto FRendererModule::InvalidateAllViewStates() -> void
	{
		if (SceneRenderer == nullptr)
			return;
		FSceneRenderer* Renderer = SceneRenderer.get();
		ENQUEUE_RENDER_COMMAND(InvalidateAllSceneViewStates)(
			[Renderer](FRHICommandListImmediate&) {
				Renderer->InvalidateAllViewStates_RenderThread();
			}
		);
	}

	auto FRendererModule::RenderView(
		FRHICommandListImmediate& CommandList,
		FSceneInterface* Scene,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		FSceneViewStatistics* OutStatistics,
		FRDGCapture* OutRenderGraphCapture
	) -> ERenderViewResult
	{
		if (OutStatistics != nullptr) *OutStatistics = {};
		if (OutRenderGraphCapture != nullptr)
			*OutRenderGraphCapture = FRDGCapture{};
		auto* RendererScene = dynamic_cast<FScene*>(Scene);
		const uint64 DrawsBefore = CommandList.GetNumRecordedDrawCommands();
		const ERenderViewResult Result = SceneRenderer->RenderView_RenderThread(
			CommandList,
			RendererScene,
			View,
			OutputTarget,
			bPresentOutput,
			Options,
			OutStatistics,
			OutRenderGraphCapture
		);
		if (OutStatistics != nullptr)
		{
			if (Result == ERenderViewResult::Success)
				OutStatistics->Summary.DrawCalls =
					CommandList.GetNumRecordedDrawCommands() - DrawsBefore;
			else
				*OutStatistics = {};
		}
		return Result;
	}

	IMPLEMENT_MODULE(FRendererModule, Renderer)
} // namespace Durin
