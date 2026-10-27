#pragma once

#include "CoreMinimal.h"
#include "DObject/AssetPath.h"
#include "SceneView.h"

namespace Durin
{
	class DWorld;
	class DLevel;
	class DClass;
	enum class EMapChangePhase : uint8
	{
		LoadingAssets, PreparingProducts, RegisteringWorld, PreparingView,
		Ready, Committed, Failed, Canceled
	};
	constexpr auto GetMapChangePhaseLabel(EMapChangePhase Phase) -> std::string_view
	{
		switch (Phase)
		{
		case EMapChangePhase::LoadingAssets: return "Loading assets";
		case EMapChangePhase::PreparingProducts: return "Preparing geometry and materials";
		case EMapChangePhase::RegisteringWorld: return "Registering scene";
		case EMapChangePhase::PreparingView: return "Preparing initial view";
		case EMapChangePhase::Ready: return "Ready to commit";
		case EMapChangePhase::Committed: return "Committed";
		case EMapChangePhase::Failed: return "Failed";
		case EMapChangePhase::Canceled: return "Canceled";
		}
		return "Unknown";
	}
	struct FMapChangeProgress
	{
		uint64 Id = 0;
		EMapChangePhase Phase = EMapChangePhase::LoadingAssets;
		double ElapsedSeconds = 0;
		double PhaseSeconds = 0;
		bool bProductsKnown = false;
		uint64 Meshes = 0, Materials = 0, ProgramsInFlight = 0;
		std::string Diagnostic;
	};
	struct FMapChangeRequest
	{
		FObjectPath Path;
		// An engaged null class requests lifecycle-only BeginPlay.
		std::optional<DClass*> GameMode;
		std::function<std::expected<FSceneView, std::string>(const DWorld&)> BuildInitialView;
		// GameThread callbacks run within the host lifecycle/GC guard.
		// Returning false rejects activation and preserves the current World.
		std::function<bool(DLevel&)> BeforeCommit;
		// Delivered once at an ordinary frame boundary, including cancellation.
		// Host shutdown discards callbacks before retiring their owners.
		std::function<void(const FMapChangeProgress&)> OnComplete;
	};
}
