#include "Actors/ProceduralSkyActor.h"
#include "DObject/Archive.h"
#include "Misc/UnifiedError.h"
#include "NativePackageUnloadTestSupport.h"
#include "Components/ProceduralSkyComponent.h"
#include "NativeAssetTestSupport.h"
#include "SkyBoxTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"
#include "Math/Operations.h"
#include "Actors/SkyLightActor.h"
#include "Components/SkyLightComponent.h"
#include "Logging/Logger.h"

TEST(FSkyLightTests, PendingAuthoredCubeDoesNotReportMissingPlatformData)
{
	auto& Logger = Durin::FLogger::Get();
	Durin::FLogSettings LogSettings;
	LogSettings.LogDirectory = (Durin::Testing::GetTestWorkDirectory() / "PendingCubeLogs").string();
	ASSERT_TRUE(Logger.Initialize(LogSettings));
	struct FShutdownLogger
	{
		~FShutdownLogger() { Durin::FLogger::Get().Shutdown(); }
	} ShutdownLogger;
	InitializeDObjectSystem();
	ASSERT_FALSE(Durin::GetAssetRuntimeConfiguration().RequiresCookedPayload());
	Durin::InitRenderingThread();
	FSkyBoxTestEngine Engine;
	Engine.CreateTestScene();
	Durin::GEngine = &Engine;
	auto* World = Durin::NewObject<Durin::DWorld>(&Engine, "PendingCubeWorld");
	ASSERT_NO_THROW(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(Durin::NewObject<Durin::DLevel>(World, "PendingCubeLevel")));
	Engine.SetWorld(World);
	auto* Actor = World->SpawnActor<Durin::ASkyLightActor>("PendingCubeSkyLight");
	auto* Cube = Durin::NewObject<Durin::DTextureCube>(nullptr, "PendingStudioCube");
	ASSERT_FALSE(Cube->HasPlatformData());
	auto* Component = Actor->GetSkyLightComponent();
	Logger.Flush();
	const auto FirstSequence = Logger.ReadRecords(0).NewestAvailableSequence + 1;
	Component->SetSource(Durin::ESkyLightSourceMode::SpecifiedCube, Cube);
	Durin::FlushRenderingCommands();
	EXPECT_FALSE(Cube->HasPlatformData());
	Logger.Flush();
	for (const auto& Record : Logger.ReadRecords(FirstSequence).Records)
	{
		EXPECT_FALSE(Record.Message.contains("platform data has not been built")) << Record.Message;
	}
	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	Engine.SetWorld(nullptr);
	Engine.ResetTestScene();
	Durin::FlushRenderingCommands();
	Durin::GEngine = nullptr;
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::MarkObjectHierarchyAsGarbage(Cube);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	Durin::ShutdownRenderingThread();
}

TEST(FSkyLightTests, DuplicatePreservesSourceIdentityAndIntensityRejectsNonfiniteInput)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::ASkyLightActor>(nullptr, "AuthoredSkyLight");
	auto* Component = Actor->GetSkyLightComponent();
	ASSERT_NE(Component, nullptr);
	Component->SetIntensity(100.0f);
	EXPECT_EQ(Component->GetIntensity(), 16.0f);
	Component->SetIntensity(std::numeric_limits<float>::quiet_NaN());
	EXPECT_EQ(Component->GetIntensity(), 16.0f);
	Component->SetPriority(-2000);
	EXPECT_EQ(Component->GetPriority(), -1000);
	Component->SetSource(Durin::ESkyLightSourceMode::CapturedSky, nullptr);
	const auto Id = Component->GetPersistentId();
	EXPECT_TRUE(Id.IsValid());
	const auto Copy = Durin::DuplicateObject(Actor, nullptr, "CopiedSkyLight");
	ASSERT_TRUE(Copy) << Durin::ToString(Copy.error());
	auto* Loaded = *Copy;
	EXPECT_EQ(Loaded->GetSkyLightComponent()->GetPersistentId(), Id);
	EXPECT_EQ(Loaded->GetSkyLightComponent()->GetSourceMode(), Durin::ESkyLightSourceMode::CapturedSky);
	EXPECT_EQ(Loaded->GetSkyLightComponent()->GetIntensity(), 16.0f);
	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::MarkObjectHierarchyAsGarbage(Loaded);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FSkyLightTests, SelectsDeterministicallyAndRetiresExactSnapshots)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	FSkyBoxTestEngine Engine;
	auto* Scene = Engine.CreateTestScene();
	Durin::GEngine = &Engine;
	auto* World = Durin::NewObject<Durin::DWorld>(&Engine, "SkyLightWorld");
	ASSERT_NO_THROW(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(Durin::NewObject<Durin::DLevel>(World, "SkyLightLevel")));
	Engine.SetWorld(World);
	auto* First = World->SpawnActor<Durin::ASkyLightActor>("First");
	auto* Second = World->SpawnActor<Durin::ASkyLightActor>("Second");
	auto* A = First->GetSkyLightComponent();
	auto* B = Second->GetSkyLightComponent();
	A->SetSource(Durin::ESkyLightSourceMode::CapturedSky, nullptr);
	B->SetSource(Durin::ESkyLightSourceMode::CapturedSky, nullptr);
	auto Observe = [&](bool bRequireSourceReady = true) {
		std::shared_ptr<const Durin::FSkyLightSceneProxy> Result;
		Durin::EnqueueRenderCommand("ObserveSkyBox", [&](Durin::FRHICommandListImmediate&) {
			Result = Scene->GetSkyLight_RenderThread(bRequireSourceReady);
		});
		Durin::FlushRenderingCommands();
		return Result;
	};
	EXPECT_FALSE(Observe());
	EXPECT_TRUE(Observe(false)) << "A missing provider may delay capture, but must not delay shader preparation.";
	auto* Provider = World->SpawnActor<Durin::AProceduralSkyActor>("Provider");
	EXPECT_NE(Provider, nullptr);
	auto Selected = Observe();
	ASSERT_TRUE(Selected);
	EXPECT_EQ(Selected->PersistentId, std::min(A->GetPersistentId(), B->GetPersistentId()));
	B->SetPriority(2);
	Selected = Observe();
	ASSERT_TRUE(Selected);
	EXPECT_EQ(Selected->PersistentId, B->GetPersistentId());
	EXPECT_FALSE(Selected->bMeasureGPUTime);
	B->SetGPUTimeMeasurementEnabled(true);
	Selected = Observe();
	ASSERT_TRUE(Selected);
	EXPECT_TRUE(Selected->bMeasureGPUTime);
	B->SetGPUTimeMeasurementEnabled(false);
	EXPECT_FALSE(Observe()->bMeasureGPUTime);
	const auto Frozen = Selected;
	B->Recapture();
	Selected = Observe();
	ASSERT_TRUE(Selected);
	EXPECT_GT(Selected->RequestSerial, Frozen->RequestSerial);
	EXPECT_EQ(Selected->SourceEpoch, Frozen->SourceEpoch);
	Second->SetActorHiddenInGame(true);
	Selected = Observe();
	ASSERT_TRUE(Selected);
	EXPECT_EQ(Selected->PersistentId, A->GetPersistentId());
	A->SetRefreshPolicy(false, 60.0f);
	Selected = Observe();
	const uint64 BeforeFinalCapture = Selected->RequestSerial;
	auto* Level = World->GetCurrentLevel();
	ASSERT_TRUE(World->SetCurrentLevel(nullptr, false));
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Selected = Observe();
	ASSERT_TRUE(Selected);
	EXPECT_GT(Selected->RequestSerial, BeforeFinalCapture)
		<< "Completing level registration requests final lighting even with automatic refresh disabled.";
	A->SetEnabled(false);
	EXPECT_FALSE(Observe());
	EXPECT_TRUE(Frozen->bEligible);
	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	EXPECT_FALSE(Observe());
	Engine.SetWorld(nullptr);
	Engine.ResetTestScene();
	Durin::FlushRenderingCommands();
	Durin::GEngine = nullptr;
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	Durin::ShutdownRenderingThread();
}

TEST(FSkyBoxTests, ActorDefaultsDuplicateAndRetainCubeReference)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::ASkyBoxActor>(nullptr, "SkyBoxActor");
	Durin::DSkyBoxComponent* Component = Actor->GetSkyBoxComponent();
	auto* Cube = Durin::NewObject<Durin::DTextureCube>(nullptr, "ReferencedCube");
	ASSERT_NE(Component, nullptr);

	Component->SetTextureCube(Cube);
	EXPECT_EQ(Component->CreateSceneProxy(), nullptr);
	Component->SetTint(Durin::FLinearColor(0.25f, 0.5f, 0.75f, 1.0f));
	Component->SetIntensity(-2.0f);
	EXPECT_EQ(Component->GetIntensity(), 0.0f);

	Durin::AddToRoot(Actor);
	const Durin::FObjectKey CubeHandle = Durin::FObjectKey(Cube);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Durin::ResolveObjectKey(CubeHandle), Cube);
	Component->SetTextureCube(nullptr);

	const auto Copy = Durin::DuplicateObject(Actor, nullptr, "CopiedSkyBox");
	ASSERT_TRUE(Copy) << Durin::ToString(Copy.error());
	auto* LoadedActor = *Copy;
	Durin::DSkyBoxComponent* LoadedComponent = LoadedActor->GetSkyBoxComponent();
	ASSERT_NE(LoadedComponent, nullptr);
	EXPECT_EQ(LoadedComponent->GetTextureCube(), nullptr);
	EXPECT_EQ(LoadedComponent->GetTint(), (Durin::FLinearColor(0.25f, 0.5f, 0.75f, 1.0f)));
	EXPECT_EQ(LoadedComponent->GetIntensity(), 0.0f);

	Durin::RemoveFromRoot(Actor);
	Durin::MarkAsGarbage(Actor);
	Durin::MarkAsGarbage(LoadedActor);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Durin::ResolveObjectKey(CubeHandle), nullptr);
}

TEST(FSkyBoxTests, PackageTracksAndReloadsCubeAssetDependency)
{
	InitializeSkyBoxAssetMount();
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> CubeResult = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
		GetSkyBoxConventionFaces(), "/SkyBoxAssetTests/Cube");
	ASSERT_TRUE(CubeResult) << CubeResult.Message;

	Durin::FPackagePath CubePath;
	Durin::FPackagePath ActorPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/Cube", CubePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/Actor", ActorPath));
	Durin::ASkyBoxActor* Actor = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(ActorPath, Actor));
	Actor->GetSkyBoxComponent()->SetTextureCube(CubeResult.Asset);
	ASSERT_TRUE(Durin::SavePackage(Actor->GetPackage()));

	const Durin::FPackageCatalogEntry ActorData = Durin::FindPackageExact(ActorPath);
	ASSERT_NE(ActorData, nullptr);
	EXPECT_NE(std::ranges::find(ActorData->Dependencies, CubePath), ActorData->Dependencies.end());

	ASSERT_TRUE(Durin::UnloadPackage(ActorPath));
	ASSERT_TRUE(Durin::UnloadPackage(CubePath));
	Durin::ASkyBoxActor* LoadedActor = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = Durin::LoadObject<Durin::ASkyBoxActor>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(ActorPath), nullptr, &LoadedValueError);
		LoadedActor = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(LoadedActor, nullptr);
	ASSERT_NE(LoadedActor->GetSkyBoxComponent()->GetTextureCube(), nullptr);
	EXPECT_EQ(LoadedActor->GetSkyBoxComponent()->GetTextureCube()->GetName(), "Cube");

	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(ActorPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(CubePath));
}
