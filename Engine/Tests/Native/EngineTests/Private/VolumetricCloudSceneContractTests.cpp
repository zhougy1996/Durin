#include "Actors/VolumetricCloudActor.h"
#include "Components/VolumetricCloudComponent.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Rendering/VolumetricCloudSceneProxy.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "RendererModule.h"
#include "RenderResource.h"
#include "Renderers/VolumetricCloudScenePreparation.h"
#include "RenderingThread.h"
#include "SceneTestAccess.h"

#include <gtest/gtest.h>

namespace
{
	class FCloudContractEngine final : public Durin::DEngine
	{
	public:
		FCloudContractEngine()
			: DEngine(Durin::FObjectInitializer::Get())
		{
		}
		auto InstallScene(Durin::FScenePtr Scene) -> Durin::FScene*
		{
			MainScene = std::move(Scene);
			return static_cast<Durin::FScene*>(MainScene.get());
		}
		auto ResetScene() -> void { Durin::FSceneInterfaceTestAccess::ReleaseScene(MainScene); }
	};
	struct FObserveVolumetricCloud
	{
		static constexpr auto GetName() -> const char*
		{
			return "ObserveVolumetricCloud";
		}
	};
	struct FMakeCloudTexturesReady
	{
		static constexpr auto GetName() -> const char*
		{
			return "MakeCloudTexturesReady";
		}
	};

	struct FCloudObservation
	{
		bool bHasActive = false;
		Durin::FVolumetricCloudSceneSnapshot Active;
		size_t Count = 0;
	};

	struct FCloudCandidate
	{
		uint64 HistoryKey = 0;
		Durin::FGuid PersistentId;
		std::string SelectionKey;
		Durin::FVolumetricCloudSceneData Data;

		operator Durin::FVolumetricCloudSceneData() &&
		{
			return std::move(Data);
		}
	};

	class FDeferredReadyCloudTexture final : public Durin::FRHITexture
	{
	protected:
		~FDeferredReadyCloudTexture() override = default;
	};

	auto ObserveClouds(const Durin::FSceneInterface& SceneInterface)
		-> FCloudObservation
	{
		const auto& Scene = static_cast<const Durin::FScene&>(SceneInterface);
		auto Result = std::make_shared<FCloudObservation>();
		Durin::EnqueueRenderCommand<FObserveVolumetricCloud>(
			[&Scene, Result](Durin::FRHICommandListImmediate&) {
				Result->bHasActive =
					Scene.GetActiveVolumetricCloud_RenderThread(Result->Active);
				Result->Count = Scene.GetVolumetricCloudCount_RenderThread();
			}
		);
		Durin::FlushRenderingCommands();
		return *Result;
	}

	auto Publish(Durin::FScene& Scene, FCloudCandidate Candidate)
		-> Durin::FVolumetricCloudSceneProxy*
	{
		auto Proxy = std::make_unique<Durin::FVolumetricCloudSceneProxy>(
			Durin::FVolumetricCloudSceneProxyDesc{
				.PersistentId = Candidate.PersistentId,
				.SelectionKey = std::move(Candidate.SelectionKey),
				.HistoryKey = Candidate.HistoryKey,
				.Data = std::move(Candidate.Data)});
		Durin::FVolumetricCloudSceneProxy* Token = Proxy.get();
		return Durin::FSceneInterfaceTestAccess::TryAddVolumetricCloudProxy(Scene, std::move(Proxy)) ? Token : nullptr;
	}

	auto MakeCandidate(uint64 HistoryKey, int32 Priority,
		Durin::FGuid PersistentId, std::string SelectionKey) -> FCloudCandidate
	{
		FCloudCandidate Candidate;
		Candidate.HistoryKey = HistoryKey;
		Candidate.PersistentId = PersistentId;
		Candidate.SelectionKey = std::move(SelectionKey);
		Candidate.Data.Priority = Priority;
		Candidate.Data.bEligible = true;
		return Candidate;
	}
} // namespace

TEST(FVolumetricCloudSceneContractTests, ReflectedDefaultsAndPersistentIdentityAreStable)
{
	InitializeDObjectSystem();
	const auto* Default = static_cast<const Durin::DVolumetricCloudComponent*>(
		Durin::DVolumetricCloudComponent::StaticClass()->GetDefaultObject()
	);
	ASSERT_NE(Default, nullptr);
	EXPECT_FALSE(Default->GetVolumetricCloudSceneId().IsValid());
	EXPECT_TRUE(Default->IsEnabled());
	EXPECT_EQ(Default->GetPriority(), 0);
	EXPECT_DOUBLE_EQ(Default->GetMinimumZ(), 1500.0);
	EXPECT_DOUBLE_EQ(Default->GetMaximumZ(), 3500.0);
	EXPECT_DOUBLE_EQ(Default->GetMaximumDistance(), 100000.0);
	EXPECT_EQ(Default->GetBaseFrequency(), Durin::FVector3f(0.00008f));
	EXPECT_EQ(Default->GetDetailFrequency(), Durin::FVector3f(0.00032f));
	EXPECT_EQ(Default->GetWindOffset(), Durin::FVector3f(0.0f));
	EXPECT_EQ(Default->GetWeatherFrequency(), Durin::FVector2f(0.00004f));
	EXPECT_EQ(Default->GetWeatherOffset(), Durin::FVector2f(0.0f));
	EXPECT_FLOAT_EQ(Default->GetCoverage(), 0.55f);
	EXPECT_FLOAT_EQ(Default->GetDetailErosion(), 0.30f);
	EXPECT_FLOAT_EQ(Default->GetExtinction(), 0.0015f);
	EXPECT_FLOAT_EQ(Default->GetLightExtinction(), 0.0020f);
	EXPECT_FLOAT_EQ(Default->GetAmbient(), 0.12f);
	auto* BaseFrequencyProperty = static_cast<Durin::FStructProperty*>(
		Durin::DVolumetricCloudComponent::StaticClass()->FindPropertyByName(
			"BaseFrequency"
		)
	);
	auto* WeatherFrequencyProperty = static_cast<Durin::FStructProperty*>(
		Durin::DVolumetricCloudComponent::StaticClass()->FindPropertyByName(
			"WeatherFrequency"
		)
	);
	ASSERT_NE(BaseFrequencyProperty, nullptr);
	ASSERT_NE(WeatherFrequencyProperty, nullptr);
	EXPECT_EQ(BaseFrequencyProperty->GetStruct()->GetQualifiedName().ToString(), "Durin::FVector3f");
	EXPECT_EQ(WeatherFrequencyProperty->GetStruct()->GetQualifiedName().ToString(), "Durin::FVector2f");

	auto* Runtime = Durin::NewObject<Durin::DVolumetricCloudComponent>(
		nullptr, "RuntimeCloud"
	);
	ASSERT_NE(Runtime, nullptr);
	EXPECT_TRUE(Runtime->GetVolumetricCloudSceneId().IsValid());
	Runtime->SetPriority(5000);
	EXPECT_EQ(Runtime->GetPriority(), 1000);
	Runtime->SetLayer(-20'000'000.0, 20'000'000.0, 0.0);
	EXPECT_DOUBLE_EQ(Runtime->GetMinimumZ(), -10'000'000.0);
	EXPECT_DOUBLE_EQ(Runtime->GetMaximumZ(), 10'000'000.0);
	EXPECT_DOUBLE_EQ(Runtime->GetMaximumDistance(), 1.0);
	Runtime->SetOpticalProperties(2.0f, -1.0f, 0.5f, 2.0f, -2.0f);
	EXPECT_FLOAT_EQ(Runtime->GetCoverage(), 1.0f);
	EXPECT_FLOAT_EQ(Runtime->GetDetailErosion(), 0.0f);
	EXPECT_FLOAT_EQ(Runtime->GetExtinction(), 0.5f);
	EXPECT_FLOAT_EQ(Runtime->GetLightExtinction(), 1.0f);
	EXPECT_FLOAT_EQ(Runtime->GetAmbient(), 0.0f);
	Runtime->SetDensityMapping(
		Durin::FVector3f(0.0f, 0.5f, 2.0f),
		Durin::FVector3f(0.25f),
		Durin::FVector3f(-2'000'000.0f, 0.0f, 2'000'000.0f),
		Durin::FVector2f(0.0f, 2.0f),
		Durin::FVector2f(-2'000'000.0f, 2'000'000.0f)
	);
	EXPECT_EQ(Runtime->GetBaseFrequency(), Durin::FVector3f(0.00000001f, 0.5f, 1.0f));
	EXPECT_EQ(Runtime->GetDetailFrequency(), Durin::FVector3f(0.25f));
	EXPECT_EQ(Runtime->GetWindOffset(), Durin::FVector3f(-1'000'000.0f, 0.0f, 1'000'000.0f));
	EXPECT_EQ(Runtime->GetWeatherFrequency(), Durin::FVector2f(0.00000001f, 1.0f));
	EXPECT_EQ(Runtime->GetWeatherOffset(), Durin::FVector2f(-1'000'000.0f, 1'000'000.0f));
	Durin::MarkAsGarbage(Runtime);
	Durin::CollectGarbage();
}

TEST(FVolumetricCloudSceneContractTests, ValidationAndP1TranslationPreserveTheFrozenBoundary)
{
	Durin::FVolumetricCloudSceneData Data = MakeCandidate(
		3, 2, Durin::FGuid(1, 2, 3, 4), "Cloud"
	);
	EXPECT_TRUE(Durin::AreVolumetricCloudParametersValid(Data));
	EXPECT_FALSE(Durin::IsVolumetricCloudCandidateEligible(Data));
	Data.MaximumZ = Data.MinimumZ;
	EXPECT_FALSE(Durin::AreVolumetricCloudParametersValid(Data));
	Data.MaximumZ = 4200.0;
	Data.MinimumZ = 1200.0;
	Data.MaximumDistance = 85000.0;
	Data.BaseFrequency = {0.1f, 0.2f, 0.3f};
	Data.DetailFrequency = {0.4f, 0.5f, 0.6f};
	Data.WindOffset = {0.7f, 0.8f, 0.9f};
	Data.WeatherFrequency = {0.01f, 0.02f};
	Data.WeatherOffset = {0.03f, 0.04f};
	Data.Coverage = 0.65f;
	Data.DetailErosion = 0.25f;
	Data.Extinction = 0.005f;
	Data.LightExtinction = 0.006f;
	Data.Ambient = 0.2f;
	Durin::FPreparedLightView Lights;
	Durin::FDirectionalLightSceneData Light;
	Light.Direction = {0.0, 0.0, -1.0};
	Light.Color = {0.5f, 0.25f, 1.0f};
	Light.Intensity = 2.0f;
	Light.AmbientIntensity = 0.4f;
	Lights.Directional.push_back({Durin::FLightSceneId(9), Light});

	const auto Parameters = Durin::BuildVolumetricCloudParameters(Data, Lights);
	EXPECT_DOUBLE_EQ(Parameters.MinimumZ, Data.MinimumZ);
	EXPECT_DOUBLE_EQ(Parameters.MaximumZ, Data.MaximumZ);
	EXPECT_EQ(Parameters.BaseFrequency, Data.BaseFrequency);
	EXPECT_EQ(Parameters.WindOffset, Data.WindOffset);
	EXPECT_EQ(Parameters.LightDirection, Durin::FVector3f(0.0f, 0.0f, 1.0f));
	EXPECT_EQ(Parameters.LightColor, Durin::FVector3f(1.0f, 0.5f, 2.0f));
	EXPECT_EQ(Parameters.AmbientColor, Durin::FVector3f(0.2f, 0.1f, 0.4f));
	EXPECT_EQ(Parameters.PrimarySampleCount, 32u);
	EXPECT_EQ(Parameters.LightSampleCount, 4u);
	EXPECT_FLOAT_EQ(Parameters.TransmittanceCutoff, 0.01f);
	const uint64 LightingKey =
		Durin::CalculateVolumetricCloudLightingKey(Lights);
	EXPECT_NE(LightingKey, 0u);
	Lights.Directional.front().Data.Intensity = 3.0f;
	EXPECT_NE(Durin::CalculateVolumetricCloudLightingKey(Lights), LightingKey);
	Lights.Directional.clear();
	EXPECT_EQ(Durin::CalculateVolumetricCloudLightingKey(Lights), 0u);
}

TEST(FVolumetricCloudSceneContractTests, EligibilityDiagnosticsUseOneStableFirstFailureOrder)
{
	Durin::FVolumetricCloudSceneData Data = MakeCandidate(
		3, 2, Durin::FGuid(1, 2, 3, 4), "Cloud"
	);
	Durin::FVolumetricCloudEligibilityContext Context;
	auto ExpectReason = [&](Durin::EVolumetricCloudEligibilityReason Reason) {
		const auto Diagnostic = Durin::DiagnoseVolumetricCloudEligibility(Data, Context);
		EXPECT_EQ(Diagnostic.Reason, Reason);
		EXPECT_EQ(Diagnostic.bEligible, Reason == Durin::EVolumetricCloudEligibilityReason::Ready);
		EXPECT_FALSE(Diagnostic.Message.empty());
	};

	Data.bEnabled = false;
	ExpectReason(Durin::EVolumetricCloudEligibilityReason::Disabled);
	Data.bEnabled = true;
	Context.bOwnerHidden = true;
	ExpectReason(Durin::EVolumetricCloudEligibilityReason::OwnerHidden);
	Context.bOwnerHidden = false;
	ExpectReason(Durin::EVolumetricCloudEligibilityReason::MissingBaseDensityTexture);
	Context.bBaseDensityTextureAssigned = true;
	ExpectReason(Durin::EVolumetricCloudEligibilityReason::InvalidBaseDensityTexture);
	Context.bBaseDensityTextureReady = true;
	ExpectReason(Durin::EVolumetricCloudEligibilityReason::MissingDetailDensityTexture);
	Context.bDetailDensityTextureAssigned = true;
	ExpectReason(Durin::EVolumetricCloudEligibilityReason::InvalidDetailDensityTexture);
	Context.bDetailDensityTextureReady = true;
	Data.MaximumZ = Data.MinimumZ;
	ExpectReason(Durin::EVolumetricCloudEligibilityReason::InvalidLayer);
	Data.MaximumZ = Data.MinimumZ + 1.0;
	Data.MaximumDistance = 0.0;
	ExpectReason(Durin::EVolumetricCloudEligibilityReason::InvalidMaximumDistance);
	Data.MaximumDistance = 1.0;
	Data.BaseFrequency.x = 0.0f;
	ExpectReason(Durin::EVolumetricCloudEligibilityReason::InvalidDensityMapping);
	Data.BaseFrequency.x = 0.00008f;
	Data.Extinction = 0.0f;
	ExpectReason(Durin::EVolumetricCloudEligibilityReason::InvalidOpticalParameters);
	Data.Extinction = 0.0015f;
	ExpectReason(Durin::EVolumetricCloudEligibilityReason::Ready);
	EXPECT_TRUE(Durin::AreVolumetricCloudParametersValid(Data));

	InitializeDObjectSystem();
	auto* StatusProperty = Durin::DVolumetricCloudComponent::StaticClass()
							   ->FindPropertyByName("EligibilityStatus");
	ASSERT_NE(StatusProperty, nullptr);
	EXPECT_TRUE(StatusProperty->HasAnyPropertyFlags(Durin::EPropertyFlags::Edit));
	EXPECT_TRUE(StatusProperty->HasAnyPropertyFlags(Durin::EPropertyFlags::ReadOnly));
	EXPECT_TRUE(StatusProperty->HasAnyPropertyFlags(Durin::EPropertyFlags::Transient));
}

TEST(FVolumetricCloudSceneContractTests, SceneSelectsPriorityAndStableIdentityAcrossOrderedMutations)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::FRendererModule Factory;
	Durin::FScenePtr Owner = Factory.CreateScene();
	auto& Scene = static_cast<Durin::FScene&>(*Owner);

	auto* Token3 = Publish(Scene,
		MakeCandidate(1, 2, Durin::FGuid(3, 0, 0, 0), "C"));
	auto* Token2 = Publish(Scene,
		MakeCandidate(1, 5, Durin::FGuid(2, 0, 0, 0), "B"));
	auto* Token1 = Publish(Scene,
		MakeCandidate(1, 5, Durin::FGuid(1, 0, 0, 0), "A"));
	auto Observation = ObserveClouds(Scene);
	ASSERT_TRUE(Observation.bHasActive);
	EXPECT_EQ(Observation.Count, 3u);
	EXPECT_EQ(Observation.Active.Desc.PersistentId, Durin::FGuid(1, 0, 0, 0));
	const uint64 FirstHistoryKey = Observation.Active.Desc.HistoryKey;

	Durin::FSceneInterfaceTestAccess::TryRemoveVolumetricCloudProxy(Scene, Token1);
	Token1 = Publish(Scene,
		MakeCandidate(2, 5, Durin::FGuid(1, 0, 0, 0), "A2"));
	Observation = ObserveClouds(Scene);
	ASSERT_TRUE(Observation.bHasActive);
	EXPECT_GT(Observation.Active.Desc.HistoryKey, FirstHistoryKey);
	EXPECT_EQ(Observation.Active.Desc.SelectionKey, "A2");

	auto Ineligible = MakeCandidate(3, 5, Durin::FGuid(1, 0, 0, 0), "A3");
	Ineligible.Data.bEligible = false;
	Durin::FSceneInterfaceTestAccess::TryRemoveVolumetricCloudProxy(Scene, Token1);
	Token1 = Publish(Scene, std::move(Ineligible));
	Observation = ObserveClouds(Scene);
	ASSERT_TRUE(Observation.bHasActive);
	EXPECT_EQ(Observation.Active.Desc.PersistentId, Durin::FGuid(2, 0, 0, 0));
	Durin::FSceneInterfaceTestAccess::TryRemoveVolumetricCloudProxy(Scene, Token1);
	Durin::FSceneInterfaceTestAccess::TryRemoveVolumetricCloudProxy(Scene, Token2);
	Durin::FSceneInterfaceTestAccess::TryRemoveVolumetricCloudProxy(Scene, Token3);
	EXPECT_FALSE(ObserveClouds(Scene).bHasActive);

	Durin::FSceneInterfaceTestAccess::ReleaseScene(Owner);
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
}

TEST(FVolumetricCloudSceneContractTests,
	SceneSelectsCloudWhenStableTextureReferencesBecomeReady)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::FRendererModule Factory;
	Durin::FScenePtr Owner = Factory.CreateScene();
	auto& Scene = static_cast<Durin::FScene&>(*Owner);
	Durin::FTextureReference BaseReference;
	Durin::FTextureReference DetailReference;
	BaseReference.BeginInit_GameThread();
	DetailReference.BeginInit_GameThread();
	Durin::FlushRenderingCommands();

	auto Candidate = MakeCandidate(
		1, 0, Durin::FGuid(1, 0, 0, 0), "DeferredReadyCloud");
	Candidate.Data.BaseDensityTexture = BaseReference.GetTextureReferenceRHI();
	Candidate.Data.DetailDensityTexture = DetailReference.GetTextureReferenceRHI();
	auto* Token = Publish(Scene, std::move(Candidate));
	EXPECT_FALSE(ObserveClouds(Scene).bHasActive);

	Durin::FTextureRHIRef BaseTexture(new FDeferredReadyCloudTexture());
	Durin::FTextureRHIRef DetailTexture(new FDeferredReadyCloudTexture());
	Durin::EnqueueRenderCommand<FMakeCloudTexturesReady>(
		[&BaseReference, &DetailReference, BaseTexture, DetailTexture](
			Durin::FRHICommandListImmediate&) {
			BaseReference.SetReferencedTexture_RenderThread(BaseTexture);
			DetailReference.SetReferencedTexture_RenderThread(DetailTexture);
		});
	Durin::FlushRenderingCommands();

	const FCloudObservation Observation = ObserveClouds(Scene);
	ASSERT_TRUE(Observation.bHasActive);
	EXPECT_EQ(Observation.Active.Desc.PersistentId, Durin::FGuid(1, 0, 0, 0));

	Durin::FSceneInterfaceTestAccess::TryRemoveVolumetricCloudProxy(Scene, Token);
	BaseReference.BeginRelease_GameThread();
	DetailReference.BeginRelease_GameThread();
	Durin::FSceneInterfaceTestAccess::ReleaseScene(Owner);
	Durin::FlushRenderingCommands();
	BaseTexture = nullptr;
	DetailTexture = nullptr;
	Durin::ShutdownRenderingThread();
}

TEST(FVolumetricCloudSceneContractTests, ActorGraphRoundTripsAuthoredIntentAndPersistentIdentity)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::AVolumetricCloudActor>(nullptr, "CloudActor");
	auto* Component = Actor->GetVolumetricCloudComponent();
	ASSERT_NE(Component, nullptr);
	Component->SetEnabled(false);
	Component->SetPriority(42);
	Component->SetLayer(900.0, 4100.0, 75000.0);
	Component->SetOpticalProperties(0.7f, 0.2f, 0.004f, 0.006f, 0.3f);
	const Durin::FGuid PersistentId = Component->GetVolumetricCloudSceneId();
	Durin::FByteArray Bytes;
	ASSERT_TRUE(Durin::SaveObjectGraphToMemory(Actor, Bytes));
	auto* Loaded = Durin::Cast<Durin::AVolumetricCloudActor>(
		Durin::LoadObjectGraphFromMemory(Bytes)
	);
	ASSERT_NE(Loaded, nullptr);
	auto* LoadedComponent = Loaded->GetVolumetricCloudComponent();
	ASSERT_NE(LoadedComponent, nullptr);
	EXPECT_FALSE(LoadedComponent->IsEnabled());
	EXPECT_EQ(LoadedComponent->GetPriority(), 42);
	EXPECT_EQ(LoadedComponent->GetVolumetricCloudSceneId(), PersistentId);
	EXPECT_DOUBLE_EQ(LoadedComponent->GetMinimumZ(), 900.0);
	EXPECT_FLOAT_EQ(LoadedComponent->GetCoverage(), 0.7f);
	Durin::MarkAsGarbage(Actor);
	Durin::MarkAsGarbage(Loaded);
	Durin::CollectGarbage();
}

TEST(FVolumetricCloudSceneContractTests, ComponentRegistrationPublishesCompleteIneligibleReplacementAndRemoval)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::FRendererModule Factory;
	FCloudContractEngine Engine;
	Durin::FScene* Scene = Engine.InstallScene(Factory.CreateScene());
	Durin::GEngine = &Engine;
	auto* World = Durin::NewObject<Durin::DWorld>(&Engine, "CloudContractWorld");
	ASSERT_TRUE(World->SetCurrentLevel(
		Durin::NewObject<Durin::DLevel>(World, "CloudContractLevel")
	));
	Engine.SetWorld(World);
	auto* Actor = World->SpawnActor<Durin::AVolumetricCloudActor>("Cloud");
	auto* Component = Actor->GetVolumetricCloudComponent();
	Component->RegisterComponent();
	auto Observation = ObserveClouds(*Scene);
	EXPECT_EQ(Observation.Count, 1u);
	EXPECT_FALSE(Observation.bHasActive);
	Component->SetPriority(50);
	Observation = ObserveClouds(*Scene);
	EXPECT_EQ(Observation.Count, 1u);
	Actor->SetHidden(true);
	EXPECT_EQ(ObserveClouds(*Scene).Count, 1u);
	Component->UnregisterComponent();
	EXPECT_EQ(ObserveClouds(*Scene).Count, 0u);

	Engine.SetWorld(nullptr);
	Engine.ResetScene();
	Durin::FlushRenderingCommands();
	Durin::GEngine = nullptr;
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
	Durin::ShutdownRenderingThread();
}
