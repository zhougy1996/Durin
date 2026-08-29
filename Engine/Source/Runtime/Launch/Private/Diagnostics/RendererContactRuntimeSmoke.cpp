#include "RendererContactRuntimeSmoke.h"

#include "Client/SceneViewport.h"
#include "Client/ViewportClient.h"
#include "Console/ConsoleCommand.h"
#include "Engine/Engine.h"
#include "EngineGlobals.h"
#include "Mona.h"
#include "Widgets/MWindow.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 SmokeStepTimeoutTicks = 300;
		constexpr uint32 SmokeStableFrameCount = 12;

		enum class ERendererContactSmokePhase : uint8
		{
			WaitingForView,
			AwaitingAuto,
			AwaitingCompute,
			AwaitingFragment,
			AwaitingDisabled,
			AwaitingDiagnostic,
			AwaitingReload,
			AwaitingResize,
			StableFrames,
			Complete,
		};

		class FContactSmokeAuxiliaryViewportClient final : public FViewportClient
		{
		public:
			explicit FContactSmokeAuxiliaryViewportClient(
				FViewportClient* InSource)
				: Source(InSource)
			{
			}

			auto CalcSceneView(
				uint32 Width, uint32 Height, FSceneView& OutView) const
				-> bool override
			{
				if (!Source || !Source->CalcSceneView(Width, Height, OutView))
					return false;
				OutView.Settings = GetViewSettings();
				return true;
			}

		private:
			FViewportClient* Source = nullptr;
		};

		auto ConfigureContact(
			FViewportClient& Client,
			bool bEnabled,
			EContactShadowRoutePreference Route,
			bool bDiagnostic) -> void
		{
			FSceneViewSettings Settings = Client.GetViewSettings();
			Settings.Mode.RenderMode = ERenderMode::Lit;
			Settings.Mode.RasterMode = ERasterMode::Solid;
			Settings.DirectionalShadow.DiagnosticMode =
				EDirectionalShadowDiagnosticMode::Lit;
			Settings.DirectionalShadow.bEnableContactShadows = bEnabled;
			Settings.DirectionalShadow.ContactRoutePreference = Route;
			Settings.DirectionalShadow.bShowContactDebug = bDiagnostic;
			Client.SetViewSettings(Settings);
		}

		auto IsExpectedRoute(
			const FSceneViewportStatisticsSnapshot& Snapshot,
			EContactShadowExecutionRoute Expected) -> bool
		{
			return Snapshot.bAvailable
				&& Snapshot.Statistics.Shadow.ContactRoute == Expected
				&& Snapshot.Statistics.Shadow.bContactEnabled
					== (Expected != EContactShadowExecutionRoute::None);
		}

		auto GetMainSceneViewport() -> std::shared_ptr<FSceneViewport>
		{
			return GEngine ? GEngine->GetMainSceneViewport() : nullptr;
		}
	}

	struct FRendererContactRuntimeSmokeState
	{
		ERendererContactSmokePhase Phase =
			ERendererContactSmokePhase::WaitingForView;
		std::shared_ptr<FSceneViewport> MainViewport;
		std::unique_ptr<FContactSmokeAuxiliaryViewportClient> AuxiliaryClient;
		std::shared_ptr<FSceneViewport> AuxiliaryViewport;
		std::shared_ptr<MWindow> MainWindow;
		FSceneViewSettings OriginalSettings;
		FVector2f OriginalWindowPosition{0.0f};
		FVector2f OriginalWindowSize{0.0f};
		uint64 MainRevision = 0;
		uint64 AuxiliaryRevision = 0;
		uint32 PhaseTicks = 0;
		uint32 StableFrames = 0;
		bool bRestored = false;
	};

	namespace
	{
		auto BeginPhase(
			FRendererContactRuntimeSmokeState& State,
			ERendererContactSmokePhase Phase) -> void
		{
			State.Phase = Phase;
			State.PhaseTicks = 0;
			State.MainRevision = State.MainViewport
				? State.MainViewport->GetRenderStatisticsSnapshot().Revision : 0;
			State.AuxiliaryRevision = State.AuxiliaryViewport
				? State.AuxiliaryViewport->GetRenderStatisticsSnapshot().Revision : 0;
			DURIN_DEBUG("Renderer contact runtime smoke entered phase {}.",
				static_cast<uint32>(Phase));
		}

		auto AwaitRoute(
			FRendererContactRuntimeSmokeState& State,
			EContactShadowExecutionRoute Expected) -> bool
		{
			const FSceneViewportStatisticsSnapshot Main =
				State.MainViewport->GetRenderStatisticsSnapshot();
			const FSceneViewportStatisticsSnapshot Auxiliary =
				State.AuxiliaryViewport->GetRenderStatisticsSnapshot();
			const bool bNewMain = Main.Revision > State.MainRevision;
			const bool bNewAuxiliary = Auxiliary.Revision > State.AuxiliaryRevision;
			if (bNewMain && bNewAuxiliary
				&& IsExpectedRoute(Main, Expected)
				&& IsExpectedRoute(Auxiliary, Expected))
			{
				State.MainRevision = Main.Revision;
				State.AuxiliaryRevision = Auxiliary.Revision;
				return true;
			}
			++State.PhaseTicks;
			checkf(State.PhaseTicks <= SmokeStepTimeoutTicks,
				"Renderer contact runtime smoke timed out in phase {} "
				"(main revision {}, available {}, route {}; auxiliary revision {}, "
				"available {}, route {}).",
				static_cast<uint32>(State.Phase), Main.Revision,
				Main.bAvailable,
				static_cast<uint32>(Main.Statistics.Shadow.ContactRoute),
				Auxiliary.Revision, Auxiliary.bAvailable,
				static_cast<uint32>(
					Auxiliary.Statistics.Shadow.ContactRoute));
			return false;
		}

	}

	auto BeginRendererContactRuntimeSmoke()
		-> std::shared_ptr<FRendererContactRuntimeSmokeState>
	{
		return std::make_shared<FRendererContactRuntimeSmokeState>();
	}

	auto TickRendererContactRuntimeSmoke(
		const std::shared_ptr<FRendererContactRuntimeSmokeState>& State) -> bool
	{
		check(State);
		if (!State) return true;

		switch (State->Phase)
		{
		case ERendererContactSmokePhase::WaitingForView:
		{
			State->MainViewport = GetMainSceneViewport();
			FViewportClient* MainClient = State->MainViewport
				? State->MainViewport->GetViewportClient() : nullptr;
			if (!MainClient || !GEngine)
			{
				++State->PhaseTicks;
				checkf(State->PhaseTicks <= SmokeStepTimeoutTicks,
					"Renderer contact runtime smoke could not resolve the main "
					"Editor scene viewport.");
				return false;
			}
			State->OriginalSettings = MainClient->GetViewSettings();
			State->AuxiliaryClient =
				std::make_unique<FContactSmokeAuxiliaryViewportClient>(MainClient);
			State->AuxiliaryClient->SetViewSettings(State->OriginalSettings);
			State->AuxiliaryViewport = FSceneViewport::CreateOffscreen(
				State->AuxiliaryClient.get());
			State->AuxiliaryViewport->PrepareDisplay({640.0f, 360.0f});
			GEngine->RegisterAuxiliarySceneViewport(State->AuxiliaryViewport);
			if (Mona::FMonaApplication::IsInitialized())
			{
				const auto& Windows = Mona::FMonaApplication::Get().GetWindows();
				if (!Windows.empty())
				{
					State->MainWindow = Windows.front();
					State->OriginalWindowPosition =
						State->MainWindow->GetScreenPosition();
					State->OriginalWindowSize = State->MainWindow->GetWindowSize();
				}
			}
			ConfigureContact(*MainClient, true,
				EContactShadowRoutePreference::Auto, false);
			ConfigureContact(*State->AuxiliaryClient, true,
				EContactShadowRoutePreference::Auto, false);
			BeginPhase(*State, ERendererContactSmokePhase::AwaitingAuto);
			return false;
		}
		case ERendererContactSmokePhase::AwaitingAuto:
			if (!AwaitRoute(*State, EContactShadowExecutionRoute::Compute))
				return false;
			ConfigureContact(*State->MainViewport->GetViewportClient(), true,
				EContactShadowRoutePreference::Compute, false);
			ConfigureContact(*State->AuxiliaryClient, true,
				EContactShadowRoutePreference::Compute, false);
			BeginPhase(*State, ERendererContactSmokePhase::AwaitingCompute);
			return false;
		case ERendererContactSmokePhase::AwaitingCompute:
			if (!AwaitRoute(*State, EContactShadowExecutionRoute::Compute))
				return false;
			ConfigureContact(*State->MainViewport->GetViewportClient(), true,
				EContactShadowRoutePreference::Fragment, false);
			ConfigureContact(*State->AuxiliaryClient, true,
				EContactShadowRoutePreference::Fragment, false);
			BeginPhase(*State, ERendererContactSmokePhase::AwaitingFragment);
			return false;
		case ERendererContactSmokePhase::AwaitingFragment:
			if (!AwaitRoute(*State, EContactShadowExecutionRoute::Fragment))
				return false;
			ConfigureContact(*State->MainViewport->GetViewportClient(), false,
				EContactShadowRoutePreference::Auto, false);
			ConfigureContact(*State->AuxiliaryClient, false,
				EContactShadowRoutePreference::Auto, false);
			BeginPhase(*State, ERendererContactSmokePhase::AwaitingDisabled);
			return false;
		case ERendererContactSmokePhase::AwaitingDisabled:
			if (!AwaitRoute(*State, EContactShadowExecutionRoute::None))
				return false;
			ConfigureContact(*State->MainViewport->GetViewportClient(), true,
				EContactShadowRoutePreference::Auto, true);
			ConfigureContact(*State->AuxiliaryClient, true,
				EContactShadowRoutePreference::Auto, true);
			BeginPhase(*State, ERendererContactSmokePhase::AwaitingDiagnostic);
			return false;
		case ERendererContactSmokePhase::AwaitingDiagnostic:
			if (!AwaitRoute(*State, EContactShadowExecutionRoute::Compute))
				return false;
		{
			const FConsoleCommandResult Reload =
				FConsoleCommandRegistry::Get().Execute(
					"renderer.reload-shaders all");
			const FConsoleCommandResult Retry =
				FConsoleCommandRegistry::Get().Execute(
					"renderer.retry-resources");
			checkf(Reload.bSuccess && Retry.bSuccess,
				"Renderer contact runtime smoke could not queue reload/retry: {} {}",
				Reload.Message, Retry.Message);
		}
			BeginPhase(*State, ERendererContactSmokePhase::AwaitingReload);
			return false;
		case ERendererContactSmokePhase::AwaitingReload:
			if (!AwaitRoute(*State, EContactShadowExecutionRoute::Compute))
				return false;
			if (State->MainWindow)
				State->MainWindow->ReshapeWindow({140.0f, 120.0f},
					{1000.0f, 650.0f});
			BeginPhase(*State, ERendererContactSmokePhase::AwaitingResize);
			return false;
		case ERendererContactSmokePhase::AwaitingResize:
			if (!AwaitRoute(*State, EContactShadowExecutionRoute::Compute))
				return false;
			BeginPhase(*State, ERendererContactSmokePhase::StableFrames);
			return false;
		case ERendererContactSmokePhase::StableFrames:
			if (!AwaitRoute(*State, EContactShadowExecutionRoute::Compute))
				return false;
			if (++State->StableFrames < SmokeStableFrameCount)
			{
				BeginPhase(*State, ERendererContactSmokePhase::StableFrames);
				return false;
			}
			EndRendererContactRuntimeSmoke(State);
			State->Phase = ERendererContactSmokePhase::Complete;
			DURIN_INFO(
				"Renderer contact runtime smoke passed main and auxiliary offscreen "
				"Auto/Compute/Fragment/Off/diagnostic routes plus application Present, "
				"resize, shader reload, retry, and stable frames.");
			return true;
		case ERendererContactSmokePhase::Complete:
			return true;
		}
		return false;
	}

	auto EndRendererContactRuntimeSmoke(
		const std::shared_ptr<FRendererContactRuntimeSmokeState>& State) -> void
	{
		if (!State || State->bRestored) return;
		State->bRestored = true;
		if (State->MainViewport && State->MainViewport->GetViewportClient())
			State->MainViewport->GetViewportClient()->SetViewSettings(
				State->OriginalSettings);
		if (State->MainWindow && State->OriginalWindowSize.x > 0.0f
			&& State->OriginalWindowSize.y > 0.0f)
		{
			State->MainWindow->ReshapeWindow(
				State->OriginalWindowPosition, State->OriginalWindowSize);
		}
		if (GEngine && State->AuxiliaryViewport)
			GEngine->UnregisterAuxiliarySceneViewport(
				State->AuxiliaryViewport.get());
		if (State->AuxiliaryViewport)
			State->AuxiliaryViewport->ReleaseViewState();
		State->AuxiliaryViewport.reset();
		State->AuxiliaryClient.reset();
		State->MainViewport.reset();
		State->MainWindow.reset();
	}
}
