#include "Engine/MapChangeHandler.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StreamableManager.h"
#include "Engine/Level.h"
#include "DObject/ObjectLifecycle.h"
#include "IRendererModule.h"
#include "Rendering/SceneInterface.h"
#include "Threading/RunnableThread.h"
#include "Profiling/Profiling.h"

namespace Durin
{
	auto FMapChangeHandler::DescribeException() -> std::string
	{
		try { throw; }
		catch (const std::exception& Error) { return Error.what(); }
		catch (...) { return "Unknown native exception."; }
	}

	auto FMapChangeHandler::SetPhase(EMapChangePhase Next) -> void
	{
		if (Phase == Next) return;
		const auto Now = std::chrono::steady_clock::now();
		if (Profiling::AreStartupDiagnosticsEnabled())
			DURIN_DEBUG("Map change {}: {} -> {} | phase {:.2f} ms", Id,
				GetMapChangePhaseLabel(Phase), GetMapChangePhaseLabel(Next),
				std::chrono::duration<double, std::milli>(Now - PhaseStarted).count());
		Phase = Next; PhaseStarted = Now;
	}
	auto FMapChangeHandler::RetireCandidate() -> void
	{
		if (World) { auto* Retired = World.Get(); Retired->CleanupWorld(); World.Reset(); MarkObjectHierarchyAsGarbage(Retired); }
		if (Scene) { Scene->Release(); Scene.reset(); }
		if (Assets) Assets->CancelHandle();
		Assets.reset();
		Preload.reset();
	}
	auto FMapChangeHandler::Fail(std::string Cause) -> void
	{
		// A callback may cancel and then return/throw an error while unwinding.
		// The Tick guard will finish that cancellation exactly once.
		if (bCancelRequested || IsMapChangeComplete(Phase)) return;
		Error = std::format("Map change {} / {}: {}", Id, GetMapChangePhaseLabel(Phase), Cause);
		SetPhase(EMapChangePhase::Failed);
		RetireCandidate();
	}
	FMapChangeHandler::FMapChangeHandler(DEngine& InHost, FMapChangeRequest Request)
		: Host(InHost), Path(std::move(Request.Path)), GameMode(Request.GameMode),
		  Preload(std::move(Request.Preload)), BeforeCommit(std::move(Request.BeforeCommit)),
		  OnComplete(std::move(Request.OnComplete))
	{
		CheckGameThread();
		static uint64 NextId = 0;
		Id = ++NextId;
		if (Profiling::AreStartupDiagnosticsEnabled())
			DURIN_DEBUG("Map change {}: Loading {}", Id, Path.ToString());
	}
	FMapChangeHandler::~FMapChangeHandler() { Cancel(); }
	auto FMapChangeHandler::GetProgress() const -> FMapChangeProgress
	{
		CheckGameThread();
		const auto Now = std::chrono::steady_clock::now();
		return {Id, Phase, std::chrono::duration<double>(Now - Started).count(),
			std::chrono::duration<double>(Now - PhaseStarted).count(), Error};
	}
	auto FMapChangeHandler::GetLevel() const -> DLevel* { return Assets ? Cast<DLevel>(Assets->GetLoadedAsset()) : nullptr; }
	auto FMapChangeHandler::Cancel() -> void
	{
		CheckGameThread();
		if (IsMapChangeComplete(Phase)) return;
		if (bAdvancing)
		{
			bCancelRequested = true;
			if (World) World->BeginTearingDown();
			return;
		}
		Error = "Map change canceled.";
		SetPhase(EMapChangePhase::Canceled);
		RetireCandidate();
	}
	auto FMapChangeHandler::Tick() -> void
	{
		CheckGameThread();
		if (bAdvancing) return;
		bAdvancing = true;
		struct FTickScope
		{
			FMapChangeHandler& Loading;
			const EMapChangePhase Phase = Loading.Phase;
			const std::chrono::steady_clock::time_point Started = std::chrono::steady_clock::now();
			~FTickScope()
			{
				Loading.bAdvancing = false;
				const double Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
				if (Milliseconds >= 16.0)
					DURIN_DEBUG("Map change {}: {} | tick {:.2f} ms | thread=GameThread", Loading.Id, GetMapChangePhaseLabel(Phase), Milliseconds);
				if (Loading.bCancelRequested) Loading.Cancel();
			}
		} Scope{*this};
		try
		{
			Advance();
			if (Phase == EMapChangePhase::Ready && BeforeCommit)
			{
				auto PrepareCommit = std::move(BeforeCommit);
				if (!PrepareCommit(*GetLevel())) Fail("Map activation was rejected by the caller.");
			}
		}
		catch (...) { Fail(DescribeException()); }
		// Publishing the World is irreversible. Do not turn a publication fault
		// into a recoverable preparation failure and tear down an adopted World.
		if (Phase == EMapChangePhase::Ready)
		{
			if (bCancelRequested || World->IsShutdownRequested()) Cancel();
			else Commit();
		}
	}

	auto FMapChangeHandler::Advance() -> void
	{
		switch (Phase)
		{
		case EMapChangePhase::LoadingAssets:
			if (!Assets) Assets = GetStreamableManager().RequestAsyncLoad(Path, {}, DLevel::StaticClass());
			if (!Assets->IsComplete() || (Preload && !Preload->IsComplete())) return;
			if (Preload && (Preload->HasError() || Preload->WasCanceled())) return Fail("Primary map preload failed or was canceled.");
			if (Assets->HasError()) return Fail(Assets->GetError()->ToString());
			if (!GetLevel()) return Fail("required Level is missing.");
			SetPhase(EMapChangePhase::RegisteringWorld);
			return;
		case EMapChangePhase::RegisteringWorld:
			if (!Host.GetRendererModule()) return Fail("renderer services are unavailable.");
			Scene = Host.GetRendererModule()->CreateScene();
			if (!Scene) return Fail("candidate scene creation failed.");
			if (bCancelRequested) return;
			World = NewObject<DWorld>(&Host, FName(std::format("SceneCandidate{}", Id)));
			World->SetWorldType(Host.GetInitialWorldType());
			World->SetRenderScene(Scene.get());
			World->InitializeSubsystems(&Host);
			if (bCancelRequested) return;
			if (World->IsShutdownRequested()) return Fail("Candidate World retired during initialization.");
			if (!World->SetCurrentLevel(GetLevel())) return Fail("Level registration was rejected; the previous document remains active.");
			if (bCancelRequested) return;
			if (GameMode.has_value())
			{
				World->BeginPlay({.GameModeClass = *GameMode});
				if (World->GetPlayState() != EWorldPlayState::Playing) return Fail("The loaded World could not begin play. See World diagnostics for details.");
				if (bCancelRequested) return;
			}
			SetPhase(EMapChangePhase::Ready);
			return;
		default: return;
		}
	}
	auto FMapChangeHandler::Commit() -> void
	{
		World->LevelResidency = std::move(Assets);
		Preload.reset();
		Host.AdoptSceneWorld(World.Get(), std::move(Scene), Id);
		World.Reset();
		SetPhase(EMapChangePhase::Committed);
	}
}
