#include "TextureTestSupport.h"

#include "Animation/AnimationClip.h"
#include "AssetForgeProviders.h"
#include "AssetTools.h"
#include "ImportRecord.h"
#include "ImportService.h"
#include "Materials/MaterialInstance.h"
#include "SceneImport.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "StaticMesh/StaticMesh.h"

namespace
{
	auto MakeAssetPath(std::string_view Value) -> Durin::FAssetPath
	{
		Durin::FAssetPath Result;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Value, Result));
		return Result;
	}

	struct FSceneFixture
	{
		std::unique_ptr<Durin::PathUtilities::FScopedMountRegistryFixture> Mounts;
		Durin::FSourcePath Source;
		Durin::FAssetPath DestinationDirectory;
		bool bOwnsRegistration = true;

		FSceneFixture() = default;
		FSceneFixture(FSceneFixture&& Other) noexcept
			: Mounts(std::move(Other.Mounts)), Source(std::move(Other.Source)),
			DestinationDirectory(std::move(Other.DestinationDirectory)),
			bOwnsRegistration(std::exchange(Other.bOwnsRegistration, false)) {}
		~FSceneFixture()
		{
			if (bOwnsRegistration)
				Durin::Asset::Forge::UnregisterAssetForgeProviders();
		}
	};

	auto InitializeFixture(std::string_view Name,
		std::string_view FixturePath = "StaticModelMaterials/RenderedOpaqueDataUri.gltf")
		-> FSceneFixture
	{
		InitializeDObjectSystem();
		std::string Error;
		EXPECT_TRUE(Durin::Asset::Forge::RegisterAssetForgeProviders(
			Error, GetEngineTestModuleCallbackGate())) << Error;
		const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory()
			/ "SceneInterchange" / std::string(Name);
		Durin::Testing::RemoveTestWorkDirectory(Root);
		std::filesystem::create_directories(Root / "Engine/Content");
		std::filesystem::create_directories(Root / "Project/Content/Scenes");
		auto Mounts = std::make_unique<Durin::PathUtilities::FScopedMountRegistryFixture>(
			std::vector<Durin::PathUtilities::FMountPoint>{
				{.VirtualRoot = "/Engine/", .Owner = Durin::PathUtilities::EMountOwner::Test,
					.Root = Root / "Engine/Content", .bAutoScan = true,
					.bAuthoringWritable = true},
				{.VirtualRoot = "/SceneImportTests/", .Owner = Durin::PathUtilities::EMountOwner::Test,
					.Root = Root / "Project/Content", .bAutoScan = true,
					.bAuthoringWritable = true, .Dependencies = {"/Engine/"}}});
		EXPECT_TRUE(Mounts->IsValid()) << Mounts->GetError();
		EXPECT_NE(Durin::Asset::Forge::EnsureImportedSurfaceMaterial(Error), nullptr) << Error;
		const std::filesystem::path Input = std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ FixturePath;
		const std::string Extension = Input.extension().generic_string();
		std::filesystem::copy_file(Input,
			Root / "Project/Content/Scenes" / (std::string(Name) + Extension),
			std::filesystem::copy_options::overwrite_existing);
		if (FixturePath == "Skeletal/ContractExternal.gltf")
			std::filesystem::copy_file(
				std::filesystem::path(DURIN_TEST_DATA_DIR) / "Skeletal/Contract.bin",
				Root / "Project/Content/Scenes/Contract.bin",
				std::filesystem::copy_options::overwrite_existing);
		FSceneFixture Fixture;
		Fixture.Mounts = std::move(Mounts);
		Fixture.Source = {.Path = std::format(
			"/SceneImportTests/Scenes/{}{}", Name, Extension)};
		Fixture.DestinationDirectory = MakeAssetPath(std::format(
			"/SceneImportTests/SceneImport/{}", Name));
		return Fixture;
	}

	auto MakeRequest(const FSceneFixture& Fixture,
		Durin::Asset::EInterchangeImportMode Mode,
		std::optional<Durin::Asset::FInterchangeProvenance> Provenance = {})
		-> Durin::Asset::FInterchangeImportRequest
	{
		Durin::Asset::FInterchangeImportRequest Request;
		std::string Error;
		EXPECT_TRUE(Durin::Asset::Forge::MakeSceneInterchangeRequest(
			Fixture.Source, Fixture.DestinationDirectory,
			Durin::FStaticMeshImportSettings::MakeDurin(), Mode,
			{.OwnerId = std::format("SceneImportTests.{}", Fixture.DestinationDirectory.ToString()),
				.ConflictIdentities = {Fixture.DestinationDirectory.ToString()}},
			std::move(Provenance), Request, Error)) << Error;
		return Request;
	}

	auto RunScene(const FSceneFixture& Fixture,
		Durin::Asset::EInterchangeImportMode Mode,
		std::optional<Durin::Asset::FInterchangeProvenance> Provenance = {})
		-> Durin::Asset::FInterchangeImportResult
	{
		return Durin::Asset::GetImportService().RunInterchangeImportInline(
			MakeRequest(Fixture, Mode, std::move(Provenance)), "Scene Interchange test");
	}

	auto FindRecord(const Durin::Asset::FInterchangeImportResult& Result)
		-> Durin::Asset::DImportRecord*
	{
		const auto Found = std::ranges::find(Result.Provenance.OutputMappings,
			std::string_view("scene-import-record"),
			&Durin::Asset::FInterchangeOutputMapping::OutputIdentity);
		if (Found == Result.Provenance.OutputMappings.end()) return nullptr;
		Durin::Asset::DImportRecord* Record = nullptr;
		(void)Durin::Asset::LoadAsset(Found->AssetPath, Record);
		return Record;
	}
}

TEST(FSceneImportTests, InterchangePublishesAndReimportsHeterogeneousGraph)
{
	const FSceneFixture Fixture = InitializeFixture("Heterogeneous");
	const auto Imported = RunScene(Fixture, Durin::Asset::EInterchangeImportMode::Import);
	ASSERT_EQ(Imported.Outcome.State, Durin::Asset::EImportOperationState::Succeeded)
		<< Imported.Outcome.Diagnostic;
	ASSERT_EQ(Imported.Inspection.Outputs.size(), 4u);
	ASSERT_EQ(Imported.Provenance.OutputMappings.size(), 4u);
	for (const auto& Output : Imported.Inspection.Outputs)
	{
		Durin::DObject* Object = nullptr;
		EXPECT_TRUE(Durin::Asset::LoadAsset(Output.AssetPath, Object));
		EXPECT_NE(Object, nullptr);
	}
	auto* Record = FindRecord(Imported);
	ASSERT_NE(Record, nullptr);
	EXPECT_EQ(Record->GetProviderId(), "Durin.SceneGraph");
	EXPECT_EQ(Record->GetOutputs().size(), 3u);

	const auto Reimported = RunScene(Fixture, Durin::Asset::EInterchangeImportMode::Reimport,
		Imported.Provenance);
	ASSERT_EQ(Reimported.Outcome.State, Durin::Asset::EImportOperationState::Succeeded)
		<< Reimported.Outcome.Diagnostic;
	EXPECT_EQ(Reimported.Provenance.OutputMappings, Imported.Provenance.OutputMappings);
	EXPECT_EQ(Reimported.Provenance.TranslatedGraphFingerprint,
		Imported.Provenance.TranslatedGraphFingerprint);
}

TEST(FSceneImportTests, RecordActionsUsePersistedInterchangeProvenance)
{
	const FSceneFixture Fixture = InitializeFixture("RecordAction");
	const auto Imported = RunScene(Fixture, Durin::Asset::EInterchangeImportMode::Import);
	ASSERT_EQ(Imported.Outcome.State, Durin::Asset::EImportOperationState::Succeeded)
		<< Imported.Outcome.Diagnostic;
	auto* Record = FindRecord(Imported);
	ASSERT_NE(Record, nullptr);
	Durin::Asset::FInterchangeImportRequest Request;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Forge::MakeSceneRecordInterchangeRequest(
		*Record, Durin::Asset::EImportRecordAction::Reimport,
		{.OwnerId = "SceneImportTests.RecordReimport"}, Request, Error)) << Error;
	const auto Result = Durin::Asset::GetImportService().RunInterchangeImportInline(
		std::move(Request), "Scene record Interchange reimport");
	EXPECT_EQ(Result.Outcome.State, Durin::Asset::EImportOperationState::Succeeded)
		<< Result.Outcome.Diagnostic;
	EXPECT_EQ(Result.Provenance.OutputMappings, Imported.Provenance.OutputMappings);
}

TEST(FSceneImportTests, InterchangePublishesSkeletalDependencyGraph)
{
	const FSceneFixture Fixture = InitializeFixture(
		"Skeletal", "Skeletal/ContractExternal.gltf");
	const auto Imported = RunScene(Fixture, Durin::Asset::EInterchangeImportMode::Import);
	ASSERT_EQ(Imported.Outcome.State, Durin::Asset::EImportOperationState::Succeeded)
		<< Imported.Outcome.Diagnostic;
	ASSERT_EQ(Imported.Inspection.Outputs.size(), 12u);
	EXPECT_EQ(Imported.Provenance.OutputMappings.size(), 12u);
	size_t Skeletons = 0;
	size_t SkeletalMeshes = 0;
	size_t Animations = 0;
	for (const auto& Output : Imported.Inspection.Outputs)
	{
		Durin::DObject* Object = nullptr;
		ASSERT_TRUE(Durin::Asset::LoadAsset(Output.AssetPath, Object));
		Skeletons += Durin::Cast<Durin::DSkeleton>(Object) != nullptr;
		SkeletalMeshes += Durin::Cast<Durin::DSkeletalMesh>(Object) != nullptr;
		Animations += Durin::Cast<Durin::DAnimationClip>(Object) != nullptr;
	}
	EXPECT_EQ(Skeletons, 2u);
	EXPECT_EQ(SkeletalMeshes, 2u);
	EXPECT_EQ(Animations, 4u);
}

TEST(FSceneImportTests, ScheduledInterchangeSurvivesInitiatorLifetimeAndCancelsTerminally)
{
	const FSceneFixture Fixture = InitializeFixture("Scheduled");
	auto Handle = Durin::Asset::GetImportService().SubmitInterchangeImport(
		MakeRequest(Fixture, Durin::Asset::EInterchangeImportMode::Import),
		"Scheduled Scene Interchange import");
	ASSERT_TRUE(Handle);
	Durin::Asset::FInterchangeImportResult Result;
	for (uint32 Attempt = 0; Attempt < 10'000 && !Handle.TryGetResult(Result); ++Attempt)
	{
		(void)Durin::Asset::GetImportService().PumpImportOperations();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_EQ(Result.Outcome.State, Durin::Asset::EImportOperationState::Succeeded)
		<< Result.Outcome.Diagnostic;

	auto Canceled = Durin::Asset::GetImportService().SubmitInterchangeImport(
		MakeRequest(Fixture, Durin::Asset::EInterchangeImportMode::Reimport,
			Result.Provenance), "Canceled Scene Interchange reimport");
	ASSERT_TRUE(Canceled.GetOperationHandle().RequestCancel());
	Durin::Asset::GetImportService().CancelAndDrainImportOperation(
		Canceled.GetOperationHandle());
	Durin::Asset::FInterchangeImportResult CanceledResult;
	ASSERT_TRUE(Canceled.TryGetResult(CanceledResult));
	EXPECT_EQ(CanceledResult.Outcome.State,
		Durin::Asset::EImportOperationState::Canceled);
}

TEST(FSceneImportTests, RuntimeOutputsDoNotReflectSceneOwnershipState)
{
	InitializeDObjectSystem();
	for (Durin::DClass* Class : {Durin::DStaticMesh::StaticClass(),
		Durin::DMaterialInstance::StaticClass(), Durin::DTexture2D::StaticClass(),
		Durin::DSkeleton::StaticClass(), Durin::DSkeletalMesh::StaticClass(),
		Durin::DAnimationClip::StaticClass()})
		EXPECT_EQ(Class->FindPropertyByName("ImportOwnership"), nullptr);
}
