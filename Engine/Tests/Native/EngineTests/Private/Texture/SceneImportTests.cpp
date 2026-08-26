#include "TextureTestSupport.h"

#include "Animation/AnimationClip.h"
#include "AssetForgeBuiltinsProviders.h"
#include "AssetTools.h"
#include "AssetForge/Persistence/ImportRecord.h"
#include "AssetForge/ImportService.h"
#include "Materials/MaterialInstance.h"
#include "AssetForge/Builtins/SceneImport.h"
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
				Durin::AssetForge::Builtins::UnregisterAssetForgeBuiltinsProviders();
		}
	};

	auto InitializeFixture(std::string_view Name,
		std::string_view FixturePath = "StaticModelMaterials/RenderedOpaqueDataUri.gltf")
		-> FSceneFixture
	{
		InitializeDObjectSystem();
		std::string Error;
		EXPECT_TRUE(Durin::AssetForge::Builtins::RegisterAssetForgeBuiltinsProviders(
			Error, GetEngineTestModuleCallbackGate())) << Error;
		const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory()
			/ "SceneAssetForge" / std::string(Name);
		Durin::Testing::RemoveTestWorkDirectory(Root);
		std::filesystem::create_directories(Root / "Engine/Content");
		std::filesystem::create_directories(Root / "Project/Content/Scenes");
		auto Mounts = std::make_unique<Durin::PathUtilities::FScopedMountRegistryFixture>(
			std::vector<Durin::PathUtilities::FMountPoint>{
				{.VirtualRoot = "/Engine/", .Owner = Durin::PathUtilities::EMountOwner::Test,
					.Root = Root / "Engine/Content", .bAutoScan = true,
					.bContentWritable = true},
				{.VirtualRoot = "/SceneImportTests/", .Owner = Durin::PathUtilities::EMountOwner::Test,
					.Root = Root / "Project/Content", .bAutoScan = true,
					.bContentWritable = true, .Dependencies = {"/Engine/"}}});
		EXPECT_TRUE(Mounts->IsValid()) << Mounts->GetError();
		EXPECT_NE(Durin::AssetForge::Builtins::EnsureImportedSurfaceMaterial(Error), nullptr) << Error;
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
		Durin::AssetForge::EImportMode Mode,
		std::optional<Durin::AssetForge::FImportProvenance> Provenance = {})
		-> Durin::AssetForge::FImportRequest
	{
		Durin::AssetForge::FImportRequest Request;
		std::string Error;
		EXPECT_TRUE(Durin::AssetForge::Builtins::MakeSceneImportRequest(
			Fixture.Source, Fixture.DestinationDirectory,
			Durin::FStaticMeshImportSettings::MakeDurin(), Mode,
			{.OwnerId = std::format("SceneImportTests.{}", Fixture.DestinationDirectory.ToString()),
				.ConflictIdentities = {Fixture.DestinationDirectory.ToString()}},
			std::move(Provenance), Request, Error)) << Error;
		return Request;
	}

	auto RunScene(const FSceneFixture& Fixture,
		Durin::AssetForge::EImportMode Mode,
		std::optional<Durin::AssetForge::FImportProvenance> Provenance = {})
		-> Durin::AssetForge::FImportResult
	{
		return Durin::AssetForge::GetImportService().RunImportInline(
			MakeRequest(Fixture, Mode, std::move(Provenance)), "Scene AssetForge test");
	}

	auto FindRecord(const Durin::AssetForge::FImportResult& Result)
		-> Durin::AssetForge::DImportRecord*
	{
		const auto Found = std::ranges::find(Result.Provenance.OutputMappings,
			std::string_view("scene-import-record"),
			&Durin::AssetForge::FOutputMapping::OutputIdentity);
		if (Found == Result.Provenance.OutputMappings.end()) return nullptr;
		Durin::AssetForge::DImportRecord* Record = nullptr;
		(void)Durin::Asset::LoadAsset(Found->AssetPath, Record);
		return Record;
	}
}

TEST(FSceneImportTests, AssetForgePublishesAndReimportsHeterogeneousGraph)
{
	const FSceneFixture Fixture = InitializeFixture("Heterogeneous");
	const auto Imported = RunScene(Fixture, Durin::AssetForge::EImportMode::Import);
	ASSERT_EQ(Imported.Outcome.State, Durin::AssetForge::EImportOperationState::Succeeded)
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

	const auto Reimported = RunScene(Fixture, Durin::AssetForge::EImportMode::Reimport,
		Imported.Provenance);
	ASSERT_EQ(Reimported.Outcome.State, Durin::AssetForge::EImportOperationState::Succeeded)
		<< Reimported.Outcome.Diagnostic;
	EXPECT_EQ(Reimported.Provenance.OutputMappings, Imported.Provenance.OutputMappings);
	EXPECT_EQ(Reimported.Provenance.SourceGraphFingerprint,
		Imported.Provenance.SourceGraphFingerprint);
}

TEST(FSceneImportTests, RecordActionsUsePersistedImportProvenance)
{
	const FSceneFixture Fixture = InitializeFixture("RecordAction");
	const auto Imported = RunScene(Fixture, Durin::AssetForge::EImportMode::Import);
	ASSERT_EQ(Imported.Outcome.State, Durin::AssetForge::EImportOperationState::Succeeded)
		<< Imported.Outcome.Diagnostic;
	auto* Record = FindRecord(Imported);
	ASSERT_NE(Record, nullptr);
	Durin::AssetForge::FImportRequest Request;
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::MakeSceneRecordImportRequest(
		*Record, Durin::AssetForge::EImportRecordAction::Reimport,
		{.OwnerId = "SceneImportTests.RecordReimport"}, Request, Error)) << Error;
	const auto Result = Durin::AssetForge::GetImportService().RunImportInline(
		std::move(Request), "Scene record AssetForge reimport");
	EXPECT_EQ(Result.Outcome.State, Durin::AssetForge::EImportOperationState::Succeeded)
		<< Result.Outcome.Diagnostic;
	EXPECT_EQ(Result.Provenance.OutputMappings, Imported.Provenance.OutputMappings);
}

TEST(FSceneImportTests, AssetForgePublishesSkeletalDependencyGraph)
{
	const FSceneFixture Fixture = InitializeFixture(
		"Skeletal", "Skeletal/ContractExternal.gltf");
	const auto Imported = RunScene(Fixture, Durin::AssetForge::EImportMode::Import);
	ASSERT_EQ(Imported.Outcome.State, Durin::AssetForge::EImportOperationState::Succeeded)
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

TEST(FSceneImportTests, ScheduledImportSurvivesInitiatorLifetimeAndCancelsTerminally)
{
	const FSceneFixture Fixture = InitializeFixture("Scheduled");
	auto Handle = Durin::AssetForge::GetImportService().SubmitImport(
		MakeRequest(Fixture, Durin::AssetForge::EImportMode::Import),
		"Scheduled Scene AssetForge import");
	ASSERT_TRUE(Handle);
	Durin::AssetForge::FImportResult Result;
	for (uint32 Attempt = 0; Attempt < 10'000 && !Handle.TryGetResult(Result); ++Attempt)
	{
		(void)Durin::AssetForge::GetImportService().PumpImportOperations();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_EQ(Result.Outcome.State, Durin::AssetForge::EImportOperationState::Succeeded)
		<< Result.Outcome.Diagnostic;

	auto Canceled = Durin::AssetForge::GetImportService().SubmitImport(
		MakeRequest(Fixture, Durin::AssetForge::EImportMode::Reimport,
			Result.Provenance), "Canceled Scene AssetForge reimport");
	ASSERT_TRUE(Canceled.GetOperationHandle().RequestCancel());
	Durin::AssetForge::GetImportService().CancelAndDrainImportOperation(
		Canceled.GetOperationHandle());
	Durin::AssetForge::FImportResult CanceledResult;
	ASSERT_TRUE(Canceled.TryGetResult(CanceledResult));
	EXPECT_EQ(CanceledResult.Outcome.State,
		Durin::AssetForge::EImportOperationState::Canceled);
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
