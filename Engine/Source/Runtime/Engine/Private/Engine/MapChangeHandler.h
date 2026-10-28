#pragma once

#include "CoreMinimal.h"
#include "Engine/MapChange.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "SceneOwnership.h"
#include "Materials/MaterialProgramPreparation.h"

namespace Durin
{
	class DEngine;
	class DWorld;
	class FAsyncLoadHandle;
	// Engine-owned resource and World preparation.
	class FMapChangeHandler
	{
	public:
		~FMapChangeHandler();
		FMapChangeHandler(const FMapChangeHandler&) = delete;
	private:
		friend class DEngine;
		FMapChangeHandler(DEngine& Host, FMapChangeRequest Request);
		auto Cancel() -> void;
		auto Tick() -> void;
		auto Advance() -> void;
		auto Commit() -> void;
		auto GetLevel() const -> DLevel*;
		auto GetProgress() const -> FMapChangeProgress;
		auto SetPhase(EMapChangePhase Next) -> void;
		auto RetireCandidate() -> void;
		auto Fail(std::string Cause) -> void;
		auto PrepareProducts(DLevel& Level) -> std::expected<bool, std::string>;
		// Only called from an active exception handler, at owner boundaries.
		static auto DescribeException() -> std::string;

		DEngine& Host;
		FObjectPath Path;
		uint64 Id = 0;
		EMapChangePhase Phase = EMapChangePhase::LoadingAssets;
		std::chrono::steady_clock::time_point Started = std::chrono::steady_clock::now(), PhaseStarted = Started;
		std::string Error;
		std::shared_ptr<FAsyncLoadHandle> Assets;
		TStrongObjectPtr<DWorld> World;
		FScenePtr Scene;
		std::vector<FWeakObjectPtr> Meshes, Materials, MaterialBindings;
		std::vector<FMaterialProgramPreparation> Programs;
		size_t NextMaterial = 0;
		bool bCollected = false;
		bool bAdvancing = false, bCancelRequested = false, bCommitRequested = false;
		std::optional<DClass*> GameMode;
		std::function<bool(DLevel&)> BeforeCommit;
		std::function<void(const FMapChangeProgress&)> OnComplete;
	};
}
