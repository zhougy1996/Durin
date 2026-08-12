#include "Engine/FPrimitiveSceneProxy.h"
#include "Engine/LightSceneProxy.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "NativeTestSupport.h"
#include "RenderingThread.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/ForwardLighting.h"
#include "Scene.h"
#include "SkeletalMesh/SkeletalMeshResources.h"
#include "StaticMesh/StaticMeshResources.h"

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

namespace
{
	std::vector<Durin::FViewRenderCounters>* GObservedViewCounterSnapshots = nullptr;

	auto ObserveViewCounterSnapshot(
		const Durin::FViewRenderCounters& Counters) -> void
	{
		if (GObservedViewCounterSnapshots != nullptr)
		{
			GObservedViewCounterSnapshots->push_back(Counters);
		}
	}

	struct FObservedLight
	{
		bool bPresent = false;
		Durin::FDirectionalLightSceneData Data;
	};

	auto ObserveLight(Durin::FScene& Scene) -> FObservedLight
	{
		auto Result = std::make_shared<FObservedLight>();
		struct FObserveLightCommand
		{
			static constexpr auto GetName() -> const char* { return "ObserveLight"; }
		};
		Durin::EnqueueRenderCommand<FObserveLightCommand>(
			[&Scene, Result](Durin::FRHICommandListImmediate&) {
				const auto& Lights = Scene.GetDirectionalLightSceneInfos();
				Result->bPresent = !Lights.empty();
				if (Result->bPresent)
					Result->Data = Lights.front()->GetDirectionalProxy().GetData();
			});
		Durin::FlushRenderingCommands();
		return *Result;
	}

	class FRenderingThreadScope final
	{
	public:
		FRenderingThreadScope()
		{
			if (!Durin::GIsGameThreadIdInitialized)
			{
				Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
				Durin::GIsGameThreadIdInitialized = true;
			}
			Durin::InitRenderingThread();
		}
		~FRenderingThreadScope() { Durin::ShutdownRenderingThread(); }
	};

	auto MakePerspectiveProjection() -> Durin::FMatrix
	{
		Durin::FMatrix Projection(0.0);
		Projection[1][0] = 0.5;
		Projection[2][1] = -1.0;
		Projection[0][2] = 11.0 / 10.0;
		Projection[3][2] = -11.0 / 10.0;
		Projection[0][3] = 1.0;
		return Projection;
	}
}

TEST(FRendererSceneContractTests, CounterSnapshotSeamDeliversOneImmutableValue)
{
	std::vector<Durin::FViewRenderCounters> Snapshots;
	GObservedViewCounterSnapshots = &Snapshots;
	Durin::SetViewRenderCounterSink(ObserveViewCounterSnapshot);
	Durin::FViewRenderCounters Counters;
	Counters.SubmittedPrimitives = 3;
	Counters.StaticMeshAttemptedDraws = 2;
	Durin::EmitViewRenderCounterSnapshot(Counters);
	Counters.SubmittedPrimitives = 9;
	Durin::SetViewRenderCounterSink(nullptr);
	GObservedViewCounterSnapshots = nullptr;
	ASSERT_EQ(Snapshots.size(), 1u);
	EXPECT_EQ(Snapshots.front().SubmittedPrimitives, 3u);
	EXPECT_EQ(Snapshots.front().StaticMeshAttemptedDraws, 2u);
}

TEST(FRendererSceneContractTests, ViewSettingsDefaultToProductionVisibilityAndLOD)
{
	const Durin::FSceneViewSettings Settings;
	EXPECT_EQ(Settings.VisibilityMode, Durin::EViewVisibilityMode::Normal);
	EXPECT_EQ(Settings.LODMode, Durin::EViewLODMode::Automatic);
}

TEST(FRendererSceneContractTests, ViewRenderOptionsDefaultToNoEnvironmentOverride)
{
	const Durin::FSceneViewRenderOptions Options;
	EXPECT_FALSE(Options.Environment.has_value());

	const Durin::FViewEnvironmentOverride Environment;
	EXPECT_EQ(Environment.TextureReference, nullptr);
	EXPECT_EQ(Environment.Rotation, Durin::FQuat(1.0, 0.0, 0.0, 0.0));
	EXPECT_EQ(Environment.Tint, Durin::FVector3f(1.0f));
	EXPECT_EQ(Environment.Intensity, 1.0f);
}

TEST(FRendererSceneContractTests, PrimitiveMembershipOwnsClassificationBoundsAndFifoLifetime)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FStaticMeshRenderData RenderData;
	RenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-1.0, -2.0, -3.0),
		Durin::FVector3(1.0, 2.0, 3.0));
	const Durin::FPrimitiveSceneId Id(41);
	const Durin::FMatrix InitialTransform = glm::translate(
		Durin::FMatrix(1.0), Durin::FVector3(10.0, 20.0, 30.0));

	Scene.AddOrReplacePrimitive(Id,
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, 0),
		InitialTransform);
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Scene.GetPrimitiveSceneInfos().size(), 1u);
	ASSERT_EQ(Scene.GetStaticMeshSceneInfos().size(), 1u);
	const Durin::FPrimitiveSceneInfo* Info = Scene.GetStaticMeshSceneInfos().front();
	EXPECT_EQ(Info->GetId(), Id);
	EXPECT_TRUE(Info->GetLocalBounds().bIsValid);
	EXPECT_EQ(Info->GetWorldBounds().Min, Durin::FVector3(9.0, 18.0, 27.0));
	EXPECT_EQ(Info->GetWorldBounds().Max, Durin::FVector3(11.0, 22.0, 33.0));
	Scene.UpdatePrimitiveVisibility(Id, false);
	Durin::FlushRenderingCommands();
	EXPECT_FALSE(Info->IsVisible());
	Scene.UpdatePrimitiveVisibility(Id, true);
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Info->IsVisible());
	Scene.UpdatePrimitiveTransform(
		Id,
		glm::scale(Durin::FMatrix(1.0), Durin::FVector3(-2.0, 3.0, 0.5)));
	Durin::FlushRenderingCommands();
	EXPECT_EQ(Info->GetWorldBounds().Min, Durin::FVector3(-2.0, -6.0, -1.5));
	EXPECT_EQ(Info->GetWorldBounds().Max, Durin::FVector3(2.0, 6.0, 1.5));

	Scene.AddOrReplacePrimitive(Id, nullptr, Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	EXPECT_EQ(Scene.GetPrimitiveSceneInfos().size(), 1u);

	Scene.RemovePrimitive(Id);
	Scene.UpdatePrimitiveTransform(Id, Durin::FMatrix(2.0));
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Scene.GetPrimitiveSceneInfos().empty());

	Scene.AddOrReplacePrimitive(Id,
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, 0),
		Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	EXPECT_EQ(Scene.GetStaticMeshSceneInfos().size(), 1u);

	Scene.Release();
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Scene.GetPrimitiveSceneInfos().empty());
}

TEST(FRendererSceneContractTests, VisibilityClassifiesOnceAndKeepsFallbacksVisible)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FStaticMeshRenderData ValidRenderData;
	ValidRenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-0.5), Durin::FVector3(0.5));
	Durin::FStaticMeshRenderData InvalidBoundsRenderData;

	auto AddStaticMesh = [&](Durin::uint64 Id, const Durin::FVector3& Location,
		bool bVisible, const Durin::FStaticMeshRenderData* RenderData) {
		Scene.AddOrReplacePrimitive(
			Durin::FPrimitiveSceneId(Id),
			std::make_unique<Durin::FStaticMeshSceneProxy>(
				RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, 0),
			glm::translate(Durin::FMatrix(1.0), Location),
			bVisible);
	};
	AddStaticMesh(1, {3.0, 0.0, 0.0}, true, &ValidRenderData);
	AddStaticMesh(2, {3.0, 0.0, 0.0}, false, &ValidRenderData);
	AddStaticMesh(3, {3.0, 20.0, 0.0}, true, &ValidRenderData);
	AddStaticMesh(4, {3.0, 0.0, 0.0}, true, &InvalidBoundsRenderData);
	Durin::FlushRenderingCommands();

	Durin::FSceneView View;
	View.ProjectionMatrix = MakePerspectiveProjection();
	View.ViewProjectionMatrix = View.ProjectionMatrix;
	Durin::FViewRenderCounters Counters;
	const Durin::FSceneVisibilityResult Visibility =
		Durin::PrepareSceneVisibility(Scene, View, Counters);
	EXPECT_EQ(Visibility.PrimitiveRecords.size(), 4u);
	EXPECT_EQ(Counters.SubmittedPrimitives, 4u);
	EXPECT_EQ(Counters.HiddenPrimitives, 1u);
	EXPECT_EQ(Counters.FrustumCulledPrimitives, 1u);
	EXPECT_EQ(Counters.VisiblePrimitives, 2u);
	EXPECT_EQ(Counters.InvalidBoundsFallbacks, 1u);
	EXPECT_EQ(Counters.InvalidViewFallbacks, 0u);
	EXPECT_EQ(Visibility.StaticMeshSceneInfos.size(), 2u);

	View.Settings.VisibilityMode =
		Durin::EViewVisibilityMode::FrustumCullingDisabled;
	Durin::FViewRenderCounters DisabledCounters;
	const Durin::FSceneVisibilityResult Disabled =
		Durin::PrepareSceneVisibility(Scene, View, DisabledCounters);
	EXPECT_EQ(DisabledCounters.HiddenPrimitives, 1u);
	EXPECT_EQ(DisabledCounters.FrustumCulledPrimitives, 0u);
	EXPECT_EQ(DisabledCounters.VisiblePrimitives, 3u);
	EXPECT_EQ(Disabled.StaticMeshSceneInfos.size(), 3u);

	View.Settings.VisibilityMode = Durin::EViewVisibilityMode::Normal;
	View.ProjectionMatrix[0][0] =
		std::numeric_limits<double>::quiet_NaN();
	Durin::FViewRenderCounters InvalidViewCounters;
	const Durin::FSceneVisibilityResult InvalidView =
		Durin::PrepareSceneVisibility(Scene, View, InvalidViewCounters);
	EXPECT_EQ(InvalidViewCounters.HiddenPrimitives, 1u);
	EXPECT_EQ(InvalidViewCounters.VisiblePrimitives, 3u);
	EXPECT_EQ(InvalidViewCounters.InvalidViewFallbacks, 3u);
	EXPECT_EQ(InvalidView.StaticMeshSceneInfos.size(), 3u);

	Scene.Release();
	Durin::FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, VisibilityPolicyAndSequentialViewsAreIndependent)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FStaticMeshRenderData RenderData;
	RenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-0.5), Durin::FVector3(0.5));
	Scene.AddOrReplacePrimitive(
		Durin::FPrimitiveSceneId(9),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, 0),
		glm::translate(Durin::FMatrix(1.0), Durin::FVector3(3.0, 0.0, 0.0)));
	Durin::FlushRenderingCommands();

	Durin::FSceneView MainView;
	MainView.ProjectionMatrix = MakePerspectiveProjection();
	MainView.ViewProjectionMatrix = MainView.ProjectionMatrix;
	Durin::FSceneView AuxiliaryView = MainView;
	AuxiliaryView.ViewProjectionMatrix = AuxiliaryView.ProjectionMatrix
		* glm::translate(Durin::FMatrix(1.0), Durin::FVector3(0.0, -20.0, 0.0));

	Durin::FViewRenderCounters MainCounters;
	Durin::FViewRenderCounters AuxiliaryCounters;
	EXPECT_EQ(
		Durin::PrepareSceneVisibility(Scene, MainView, MainCounters)
			.StaticMeshSceneInfos.size(),
		1u);
	EXPECT_TRUE(
		Durin::PrepareSceneVisibility(Scene, AuxiliaryView, AuxiliaryCounters)
			.StaticMeshSceneInfos.empty());
	Durin::FViewRenderCounters RepeatedMainCounters;
	EXPECT_EQ(
		Durin::PrepareSceneVisibility(Scene, MainView, RepeatedMainCounters)
			.StaticMeshSceneInfos.size(),
		1u);

	Durin::FSceneView SubmittedView = MainView;
	SubmittedView.Settings.VisibilityMode =
		Durin::EViewVisibilityMode::FrustumCullingDisabled;
	auto ObservedMode = std::make_shared<Durin::EViewVisibilityMode>(
		Durin::EViewVisibilityMode::Normal);
	struct FObserveVisibilityPolicyCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "ObserveVisibilityPolicy";
		}
	};
	Durin::EnqueueRenderCommand<FObserveVisibilityPolicyCommand>(
		[ViewSnapshot = SubmittedView, ObservedMode](
			Durin::FRHICommandListImmediate&) {
			*ObservedMode = ViewSnapshot.Settings.VisibilityMode;
		});
	SubmittedView.Settings.VisibilityMode = Durin::EViewVisibilityMode::Normal;
	Durin::FlushRenderingCommands();
	EXPECT_EQ(
		*ObservedMode,
		Durin::EViewVisibilityMode::FrustumCullingDisabled);

	Scene.Release();
	Durin::FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, DirectionalLightProxyOutlivesPublisherAndUsesFifoReplacement)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	const Durin::FLightSceneId Id(7);
	{
		Durin::FDirectionalLightSceneData Data;
		Data.Intensity = 2.0f;
		Scene.AddOrReplaceLight(Id,
			std::make_unique<Durin::FDirectionalLightSceneProxy>(Data));
	}
	Durin::FlushRenderingCommands();
	FObservedLight Observed = ObserveLight(Scene);
	ASSERT_TRUE(Observed.bPresent);
	EXPECT_EQ(Observed.Data.Intensity, 2.0f);

	Durin::FDirectionalLightSceneData Replacement;
	Replacement.Intensity = 5.0f;
	Scene.AddOrReplaceLight(Id,
		std::make_unique<Durin::FDirectionalLightSceneProxy>(Replacement));
	Scene.RemoveLight(Id);
	Durin::FlushRenderingCommands();
	EXPECT_FALSE(ObserveLight(Scene).bPresent);

	Scene.AddOrReplaceLight(Id,
		std::make_unique<Durin::FDirectionalLightSceneProxy>(Replacement));
	Durin::FlushRenderingCommands();
	Observed = ObserveLight(Scene);
	ASSERT_TRUE(Observed.bPresent);
	EXPECT_EQ(Observed.Data.Intensity, 5.0f);
	Scene.Release();
	Durin::FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, LightFamiliesReplaceTypedMembershipAtomically)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FPointLightSceneData Point;
	Point.Intensity = 1.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(77),
		std::make_unique<Durin::FPointLightSceneProxy>(Point));
	Durin::FSpotLightSceneData Spot;
	Spot.Intensity = 1.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(77),
		std::make_unique<Durin::FSpotLightSceneProxy>(Spot));
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Scene.GetPointLightSceneInfos().empty());
	ASSERT_EQ(Scene.GetSpotLightSceneInfos().size(), 1u);
	EXPECT_EQ(Scene.GetSpotLightSceneInfos().front()->GetId().Value, 77u);
	Scene.Release();
	Durin::FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, PreparedLightsUseStableIdAndSharedLocalBudget)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	for (Durin::uint64 Id : {101u, 100u})
	{
		Durin::FDirectionalLightSceneData Data;
		Data.Intensity = 1.0f;
		Scene.AddOrReplaceLight(Durin::FLightSceneId(Id),
			std::make_unique<Durin::FDirectionalLightSceneProxy>(Data));
	}
	for (Durin::uint64 Id = 10; Id > 0; --Id)
	{
		if ((Id & 1u) == 0)
		{
			Durin::FPointLightSceneData Data;
			Data.Intensity = 1.0f;
			Data.Range = 5.0f;
			Scene.AddOrReplaceLight(Durin::FLightSceneId(Id),
				std::make_unique<Durin::FPointLightSceneProxy>(Data));
		}
		else
		{
			Durin::FSpotLightSceneData Data;
			Data.Intensity = 1.0f;
			Data.Range = 5.0f;
			Scene.AddOrReplaceLight(Durin::FLightSceneId(Id),
				std::make_unique<Durin::FSpotLightSceneProxy>(Data));
		}
	}
	Durin::FlushRenderingCommands();
	struct FObservedPreparation
	{
		Durin::FPreparedLightView Lights;
		Durin::FViewRenderCounters Counters;
	};
	auto Observed = std::make_shared<FObservedPreparation>();
	Durin::FSceneView View;
	View.Settings.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
	struct FPrepareLightsCommand
	{
		static constexpr auto GetName() -> const char* { return "PrepareLights"; }
	};
	Durin::EnqueueRenderCommand<FPrepareLightsCommand>(
		[&Scene, View, Observed](Durin::FRHICommandListImmediate&) {
			Observed->Lights = Durin::PrepareLightView_RenderThread(
				Scene, View, Observed->Counters);
		});
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Observed->Lights.Directional.size(), 1u);
	EXPECT_EQ(Observed->Lights.Directional.front().Id.Value, 100u);
	ASSERT_EQ(Observed->Lights.Local.size(), 4u);
	for (size_t Index = 0; Index < Observed->Lights.Local.size(); ++Index)
		EXPECT_EQ(Observed->Lights.Local[Index].Id.Value, Index + 1);
	EXPECT_EQ(Observed->Counters.OverflowDirectionalLights, 1u);
	EXPECT_EQ(Observed->Counters.SelectedPointLights, 2u);
	EXPECT_EQ(Observed->Counters.SelectedSpotLights, 2u);
	EXPECT_EQ(Observed->Counters.OverflowPointLights, 3u);
	EXPECT_EQ(Observed->Counters.OverflowSpotLights, 3u);
	EXPECT_EQ(Observed->Counters.PackedLightBytes,
		sizeof(Durin::FForwardLightingUniform));
	Scene.Release();
	Durin::FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, PreparedLightsCullOnlyOutsideLocalInfluenceBounds)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	auto AddPoint = [&](Durin::uint64 Id, const Durin::FVector3& Position) {
		Durin::FPointLightSceneData Data;
		Data.Position = Position;
		Data.Intensity = 1.0f;
		Data.Range = 1.0f;
		Scene.AddOrReplaceLight(Durin::FLightSceneId(Id),
			std::make_unique<Durin::FPointLightSceneProxy>(Data));
	};
	AddPoint(1, {3.0, 0.0, 0.0});
	AddPoint(2, {3.0, 20.0, 0.0});
	Durin::FlushRenderingCommands();
	auto Observed = std::make_shared<std::pair<
		Durin::FPreparedLightView, Durin::FViewRenderCounters>>();
	Durin::FSceneView View;
	View.ProjectionMatrix = MakePerspectiveProjection();
	View.ViewProjectionMatrix = View.ProjectionMatrix;
	struct FCullLightsCommand
	{
		static constexpr auto GetName() -> const char* { return "CullLights"; }
	};
	Durin::EnqueueRenderCommand<FCullLightsCommand>(
		[&Scene, View, Observed](Durin::FRHICommandListImmediate&) {
			Observed->first = Durin::PrepareLightView_RenderThread(
				Scene, View, Observed->second);
		});
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Observed->first.Local.size(), 1u);
	EXPECT_EQ(Observed->first.Local.front().Id.Value, 1u);
	EXPECT_EQ(Observed->second.FrustumCulledPointLights, 1u);
	Scene.Release();
	Durin::FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, SkeletalPoseAndBoundsUpdateAtomicallyInTypedMembership)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FSkeletalMeshRenderData RenderData;
	auto FirstPose = std::make_shared<Durin::FSkeletalPosePalette>();
	FirstPose->Revision = 1;
	FirstPose->Matrices = {Durin::FMatrix4f(1.0f)};
	FirstPose->LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 1.0});
	const Durin::FPrimitiveSceneId Id(91);
	Scene.AddOrReplacePrimitive(Id,
		std::make_unique<Durin::FSkeletalMeshSceneProxy>(
			&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, 1, FirstPose),
		glm::translate(Durin::FMatrix(1.0), Durin::FVector3(2.0, 0.0, 0.0)));
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Scene.GetSkeletalMeshSceneInfos().size(), 1u);
	const Durin::FPrimitiveSceneInfo* Info = Scene.GetSkeletalMeshSceneInfos().front();
	EXPECT_EQ(Info->GetSkeletalMeshProxy().GetPose()->Revision, 1u);
	EXPECT_EQ(Info->GetWorldBounds().Min.x, 2.0);

	auto SecondPose = std::make_shared<Durin::FSkeletalPosePalette>(*FirstPose);
	SecondPose->Revision = 2;
	SecondPose->LocalBounds = Durin::FBox({-2.0, -1.0, -1.0}, {3.0, 1.0, 1.0});
	Scene.UpdateSkeletalMeshDynamicData(Id, SecondPose);
	Durin::FlushRenderingCommands();
	Info = Scene.GetSkeletalMeshSceneInfos().front();
	EXPECT_EQ(Info->GetSkeletalMeshProxy().GetPose()->Revision, 2u);
	EXPECT_EQ(Info->GetLocalBounds().Min.x, -2.0);
	EXPECT_EQ(Info->GetWorldBounds().Min.x, 0.0);

	Durin::FViewRenderCounters Counters;
	Durin::FSceneView View;
	View.Settings.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
	const Durin::FSceneVisibilityResult Visibility =
		Durin::PrepareSceneVisibility(Scene, View, Counters);
	EXPECT_EQ(Visibility.SkeletalMeshSceneInfos.size(), 1u);
	EXPECT_EQ(Counters.VisibleSkeletalMeshCandidates, 1u);
	Scene.Release();
	Durin::FlushRenderingCommands();
}
