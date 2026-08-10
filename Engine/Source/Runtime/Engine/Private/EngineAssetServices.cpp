#include "EngineAssetServices.h"

#include "AssetSystem.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DBuildCoordinator.h"
#include "Texture/TextureCube.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 NormalFrameTexture2DCompletionBudget = 64;

		// Retains the opt-in asynchronous completion audit across bounded engine ticks.
		struct FEngineAssetServiceLifecycleSmokeState
		{
			uint64 RequestId = 0;
			uint32 CompletionCount = 0;
			ETexture2DBuildPhase Phase = ETexture2DBuildPhase::None;
			bool bRanOnGameThread = false;
			bool bActive = false;
		};

		FEngineAssetServiceLifecycleSmokeState GEngineAssetServiceLifecycleSmoke;
	}

	auto InitializeEngineAssetServices() -> void
	{
		static const bool bInitialized = [] {
			auto PreserveMountedSource = [](
				const Asset::FAssetData&,
				const Asset::FAssetPackageInspection&,
				Asset::FAssetDeleteContribution&
			) -> Asset::FAssetResult {
				// Mounted sources may be shared and require a separate, explicit source operation.
				return {};
			};
			Asset::RegisterAssetDeleteContributor(DTexture2D::StaticClass(), PreserveMountedSource);
			Asset::RegisterAssetDeleteContributor(DTextureCube::StaticClass(), PreserveMountedSource);
			Asset::RegisterAssetDeleteContributor(DStaticMesh::StaticClass(), PreserveMountedSource);
			return true;
		}();
		(void)bInitialized;
		InitializeTexture2DBuildCoordinator();
	}

	auto PumpEngineAssetServiceCompletions() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		PumpTexture2DBuildCompletions(NormalFrameTexture2DCompletionBudget);
	}

	auto BeginEngineAssetServiceLifecycleSmoke() -> void
	{
		CheckGameThread();
		FTexture2DBuildCoordinator* Coordinator = GetTexture2DBuildCoordinator();
		checkf(Coordinator != nullptr,
			"Engine asset-service lifecycle smoke requires an initialized Texture2D coordinator.");
		GEngineAssetServiceLifecycleSmoke = {.bActive = true};
		FTexture2DBuildRequest Request{
			.AssetIdentity = "/EngineAssetServiceLifecycleSmoke",
			.SourcePath = {.Path = "/Engine/Tests/InvalidTexture.png"},
			.EncodedSource = {0, 1, 2, 3, 4, 5, 6, 7},
			.Generation = 1,
			.EstimatedWidth = 1024,
			.EstimatedHeight = 1024,
			.bPersistDerivedData = false};
		GEngineAssetServiceLifecycleSmoke.RequestId = Coordinator->Submit(
			std::move(Request),
			[](FTexture2DBuildResult&& Result) {
				GEngineAssetServiceLifecycleSmoke.bRanOnGameThread = IsInGameThread();
				GEngineAssetServiceLifecycleSmoke.Phase = Result.Phase;
				++GEngineAssetServiceLifecycleSmoke.CompletionCount;
			});
		checkf(GEngineAssetServiceLifecycleSmoke.RequestId != 0,
			"Engine asset-service lifecycle smoke could not submit its asynchronous result.");
	}

	auto ValidateEngineAssetServiceLifecycleSmoke() -> void
	{
		CheckGameThread();
		checkf(GEngineAssetServiceLifecycleSmoke.bActive
			&& GEngineAssetServiceLifecycleSmoke.CompletionCount == 1
			&& GEngineAssetServiceLifecycleSmoke.bRanOnGameThread
			&& GEngineAssetServiceLifecycleSmoke.Phase == ETexture2DBuildPhase::Failed,
			"Engine asset-service lifecycle smoke did not discard one asynchronous result through the normal GameThread frame pump.");
		DURIN_INFO(
			"Engine asset-service lifecycle smoke passed. (request: {}, completions: {})",
			GEngineAssetServiceLifecycleSmoke.RequestId,
			GEngineAssetServiceLifecycleSmoke.CompletionCount);
		GEngineAssetServiceLifecycleSmoke.bActive = false;
	}

	auto ShutdownEngineAssetServices() -> void
	{
		ShutdownTexture2DBuildCoordinator();
	}
}
