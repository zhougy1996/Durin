#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	enum class ESceneLoadPhase : uint8
	{
		LoadingAssets, PreparingProducts, RegisteringWorld, PreparingView,
		Ready, Committed, Failed, Canceled
	};
	constexpr auto GetSceneLoadPhaseLabel(ESceneLoadPhase Phase) -> std::string_view
	{
		switch (Phase)
		{
		case ESceneLoadPhase::LoadingAssets: return "Loading assets";
		case ESceneLoadPhase::PreparingProducts: return "Preparing geometry and materials";
		case ESceneLoadPhase::RegisteringWorld: return "Registering scene";
		case ESceneLoadPhase::PreparingView: return "Preparing initial view";
		case ESceneLoadPhase::Ready: return "Ready to commit";
		case ESceneLoadPhase::Committed: return "Committed";
		case ESceneLoadPhase::Failed: return "Failed";
		case ESceneLoadPhase::Canceled: return "Canceled";
		}
		return "Unknown";
	}
	struct FSceneLoadProgress
	{
		uint64 Id = 0;
		ESceneLoadPhase Phase = ESceneLoadPhase::LoadingAssets;
		double ElapsedSeconds = 0;
		double PhaseSeconds = 0;
		bool bProductsKnown = false;
		uint64 Meshes = 0, Materials = 0, ProgramsInFlight = 0;
		std::string Diagnostic;
	};
}
