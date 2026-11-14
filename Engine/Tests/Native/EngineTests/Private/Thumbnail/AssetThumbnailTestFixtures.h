#include "NativeAssetBuildTestSupport.h"
#include "Misc/UnifiedError.h"
#include "StaticMeshTestAccess.h"
#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "../Materials/ExplicitMaterialProgramTestFixture.h"
#include "Asset/AssetCompilingManager.h"
#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "IRendererModule.h"

#include "NativeDObjectTestSupport.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "Asset/AssetRetention.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"
#include "Engine/World.h"
#include "EngineTestSupport.h"
#include "Texture/TextureFactoryTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "Math/Operations.h"
#include "Modules/ModuleManager.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Thumbnail/ThumbnailCapture.h"
#include "RenderingThread.h"
#include "Threading/TaskRuntime.h"
#include "Thumbnail/StaticMeshThumbnailTypes.h"
#include "Thumbnail/TextureCubeThumbnailRenderer.h"
#include "Texture/Texture2D.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "Texture/TextureCube.h"

namespace Durin::Tests
{
	// Preparation shares a per-render-frame admission budget with ordinary draws.
	inline auto BeginThumbnailTestFrame() -> void
	{
		auto* Renderer = GEngine ? GEngine->GetRendererModule() : nullptr;
		EnqueueRenderCommand("AdvanceThumbnailCaptureTestFrame", [Renderer](FRHICommandListImmediate& Commands) {
			++GRenderFrameCounterRenderThread;
			if (Renderer) Renderer->BeginFrame_RenderThread(Commands);
		});
	}
	// Standalone capture must publish cold asset/shader work on the owner thread
	// as well as progress render commands and GPU readback.
	inline auto FinishThumbnailCapture(FThumbnailCapture& Pool,
		FByteBuffer& Pixels, std::string& Error) -> bool
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		for (;;)
		{
			PumpGameThreadDeferredWork();
			FAssetCompilingManager::Get().ProcessAsyncTasks();
			BeginThumbnailTestFrame();
			auto Capture = Pool.PollCapture();
			if (!Capture)
			{
				Error = Capture.error();
				return false;
			}
			if (Capture->has_value())
			{
				Pixels = std::move(**Capture);
				return true;
			}
			if (std::chrono::steady_clock::now() >= Deadline)
			{
				Error = "Timed out waiting for thumbnail readback.";
				return false;
			}
			FlushRenderingCommands();
			std::this_thread::yield();
		}
	}
	// Names and values in this fixture set are versioned inputs to rendered-thumbnail golden tests.
	struct FAssetThumbnailFixtureSet
	{
		static constexpr uint32 Version = 2;
		static constexpr std::string_view MountPoint = "/ThumbnailFixtures/";
		static constexpr std::string_view MaterialPath = "/ThumbnailFixtures/Materials/M_Deterministic";
		static constexpr std::string_view MaterialInstancePath =
			"/ThumbnailFixtures/Materials/MI_Deterministic";
		static constexpr std::string_view InvalidMaterialInstancePath =
			"/ThumbnailFixtures/Materials/MI_MissingParent";
		static constexpr std::string_view ParentTexturePath =
			"/ThumbnailFixtures/Textures/T_ParentColor";
		static constexpr std::string_view OverrideTexturePath =
			"/ThumbnailFixtures/Textures/T_InstanceColor";
		static constexpr std::string_view DirectionalCubePath =
			"/ThumbnailFixtures/Textures/TC_Directional";
		static constexpr std::string_view StaticMeshPath =
			"/ThumbnailFixtures/Meshes/SM_Deterministic";

		DMaterial* Material = nullptr;
		DMaterialInstance* MaterialInstance = nullptr;
		DMaterialInstance* InvalidMaterialInstance = nullptr;
		DTexture2D* ParentTexture = nullptr;
		DTexture2D* OverrideTexture = nullptr;
		DTextureCube* DirectionalCube = nullptr;
		DStaticMesh* StaticMesh = nullptr;
	};

	// Keeps concrete setup helpers in tests after the production preview pool became renderer-neutral.
	class FAssetThumbnailTestPool
	{
	public:
		explicit FAssetThumbnailTestPool(
			FThumbnailVisualContract InContract = {})
			: Contract(std::move(InContract))
			, Scene(Contract)
			, Pool(Contract.Output)
			, View(MakeView(Contract))
		{
		}

		~FAssetThumbnailTestPool()
		{
			Reset();
		}

		auto GetPreviewScene() -> FThumbnailPreviewScene& { return Scene; }
		auto IsAvailable() const -> bool { return Pool.IsAvailable(); }
		auto GetDiagnostic() const -> std::string { return Pool.GetDiagnostic(); }
		auto SetForceLOD0(bool bForceLOD0) -> void
		{
			View.bForceLOD0 = bForceLOD0;
		}
		auto SetView() -> void
		{
			Pool.SetView(View);
		}
		auto SetViewEnvironment(
			const FViewEnvironmentOverride& Environment) -> void
		{
			Pool.SetViewEnvironment(Environment);
		}

		auto GetSphereMesh() -> DStaticMesh*
		{
			if (!SphereAsset)
			{
				FSoftObjectPath SpherePath;
				std::string Error;
				if (!FSoftObjectPath::TryCreate(
						FThumbnailVisualContract::SphereAssetPath,
						SpherePath)
					|| !FAssetRetentionService::Acquire(
						SpherePath, SphereAsset, Error))
					return nullptr;
			}
			return Cast<DStaticMesh>(SphereAsset.Get());
		}

		auto SetMaterial(
			DStaticMesh* Mesh,
			DMaterialInterface* Material,
			const FTransform& Transform) -> std::expected<void, std::string>
		{
			ResetActor();
			DWorld* World = Scene.GetWorld();
			Actor = World
				? World->SpawnActor<AActor>("ThumbnailTestMaterialActor")
				: nullptr;
			auto* Component = Actor
				? Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(
					DStaticMeshComponent::StaticClass(), "MaterialPreview"))
				: nullptr;
			if (Mesh == nullptr || Material == nullptr || Component == nullptr)
			{
				ResetActor();
				return std::unexpected("The material test preview is unavailable.");
			}
			Component->SetStaticMesh(Mesh);
			for (uint32 SlotIndex = 0; SlotIndex < Component->GetNumMaterials(); ++SlotIndex)
				Component->SetMaterial(SlotIndex, Material);
			Component->SetWorldTransform(Transform);
			Pool.SetView(View);
			return {};
		}

		auto SetTextureCube(
			DTextureCube* TextureCube) -> std::expected<void, std::string>
		{
			ResetActor();
			const FRHITextureReferenceRef TextureReference = TextureCube
				? TextureCube->GetTextureReferenceRHI()
				: FRHITextureReferenceRef{};
			if (TextureReference == nullptr)
			{
				return std::unexpected("The TextureCube test preview is unavailable.");
			}
			View.VerticalFieldOfViewDegrees =
				Math::RadiansToDegrees(2.0 * std::atan(1.0 / static_cast<double>(
					1.0f / std::tan(Math::DegreesToRadians(
						TextureEditor::FTextureCubeThumbnailRendererVisualContract::
							VerticalFieldOfViewDegrees) * 0.5f))));
			Pool.SetView(View);
			Pool.SetViewEnvironment({.TextureReference = TextureReference});
			return {};
		}

		auto SetStaticMesh(
			DStaticMesh* StaticMesh,
			const StaticMeshEditor::FStaticMeshThumbnailRendererView& ThumbnailView)
			-> std::expected<void, std::string>
		{
			ResetActor();
			DWorld* World = Scene.GetWorld();
			Actor = World
				? World->SpawnActor<AActor>("ThumbnailTestStaticMeshActor")
				: nullptr;
			auto* Component = Actor
				? Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(
					DStaticMeshComponent::StaticClass(), "StaticMeshPreview"))
				: nullptr;
			if (StaticMesh == nullptr || Component == nullptr)
			{
				ResetActor();
				return std::unexpected("The StaticMesh test preview is unavailable.");
			}
			Component->SetStaticMesh(StaticMesh);
			Component->SetWorldTransform(ThumbnailView.MeshTransform);
			View.CameraPosition = {
				ThumbnailView.CameraPosition.x,
				ThumbnailView.CameraPosition.y,
				ThumbnailView.CameraPosition.z};
			View.CameraForward = {
				ThumbnailView.CameraForward.x,
				ThumbnailView.CameraForward.y,
				ThumbnailView.CameraForward.z};
			View.CameraRight = {
				ThumbnailView.CameraRight.x,
				ThumbnailView.CameraRight.y,
				ThumbnailView.CameraRight.z};
			View.CameraUp = {
				ThumbnailView.CameraUp.x,
				ThumbnailView.CameraUp.y,
				ThumbnailView.CameraUp.z};
			View.NearClipDistance = ThumbnailView.NearClipDistance;
			View.FarClipDistance = ThumbnailView.FarClipDistance;
			Pool.SetView(View);
			return {};
		}

		auto BeginCapture(bool bOutputOpaque = true) -> std::expected<void, std::string>
		{
			View.ClearRed = bOutputOpaque ? Contract.BackgroundRed : 0.0f;
			View.ClearGreen = bOutputOpaque ? Contract.BackgroundGreen : 0.0f;
			View.ClearBlue = bOutputOpaque ? Contract.BackgroundBlue : 0.0f;
			View.ClearAlpha = bOutputOpaque ? 1.0f : 0.0f;
			Pool.SetView(View);
			return Pool.SetScene(Scene).and_then([&] { return Pool.BeginCapture(); });
		}

		auto PollCapture() -> std::expected<std::optional<FByteBuffer>, std::string>
		{
			return Pool.PollCapture();
		}

		auto FinishCapture(Durin::FByteBuffer& OutPixels, std::string& OutError)
			-> bool
		{
			return FinishThumbnailCapture(Pool, OutPixels, OutError);
		}

		auto Reset() -> void
		{
			ResetActor();
			Pool.Reset();
			View = MakeView(Contract);
		}

	private:
		static auto MakeView(const FThumbnailVisualContract& Contract)
			-> FThumbnailPreviewView
		{
			const FVector3 Eye = Math::Normalize(FVector3(
				Contract.CameraDirectionX,
				Contract.CameraDirectionY,
				Contract.CameraDirectionZ)) * static_cast<double>(Contract.CameraDistance);
			const FVector3 Forward = Math::Normalize(-Eye);
			const FVector3 Right = Math::Normalize(
				Math::Cross(FVectorConstants::Up, Forward));
			const FVector3 Up = Math::Normalize(Math::Cross(Forward, Right));
			return {
				.CameraPosition = {Eye.x, Eye.y, Eye.z},
				.CameraForward = {Forward.x, Forward.y, Forward.z},
				.CameraRight = {Right.x, Right.y, Right.z},
				.CameraUp = {Up.x, Up.y, Up.z},
				.VerticalFieldOfViewDegrees = Contract.VerticalFieldOfViewDegrees,
				.NearClipDistance = Contract.NearClipDistance,
				.FarClipDistance = Contract.FarClipDistance,
				.ClearRed = Contract.BackgroundRed,
				.ClearGreen = Contract.BackgroundGreen,
				.ClearBlue = Contract.BackgroundBlue,
				.ClearAlpha = Contract.bOutputOpaque ? 1.0f : Contract.BackgroundAlpha};
		}

		auto ResetActor() -> void
		{
			if (Actor != nullptr)
			{
				if (DWorld* World = Scene.GetWorld()) World->DestroyActor(Actor);
				Actor = nullptr;
			}
		}

		FThumbnailVisualContract Contract;
		FThumbnailPreviewScene Scene;
		FThumbnailCapture Pool;
		FThumbnailPreviewView View;
		AActor* Actor = nullptr;
		FRetainedAsset SphereAsset;
	};

	inline auto MakeThumbnailFixturePath(std::string_view Value, FPackagePath& OutPath) -> bool
	{
		return FPackagePath::TryCreate(Value, OutPath);
	}

	inline auto GetAssetThumbnailFixtureRoot() -> std::filesystem::path
	{
		return Testing::GetTestWorkDirectory()
			/ "AssetThumbnailFixtures";
	}

	inline auto RegisterAssetThumbnailFixtureMount() -> std::filesystem::path
	{
		InitializeDObjectSystem();
		FModuleManager::Get().LoadModuleChecked("TextureCompressor");
		FModuleManager::Get().LoadModuleChecked("MeshBuilder");
		const std::filesystem::path Root = GetAssetThumbnailFixtureRoot();
		Testing::RegisterMountPointForTests(
			FAssetThumbnailFixtureSet::MountPoint,
			Root.generic_string() + "/");
		return Root;
	}

	inline auto GetThumbnailDirectionalCubeFaces()
		-> std::array<std::string, TextureCubeFaceCount>
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
		std::array<std::string, TextureCubeFaceCount> Result;
		for (size_t FaceIndex = 0; FaceIndex < Result.size(); ++FaceIndex)
		{
			Result[FaceIndex] = (std::filesystem::path(DURIN_TEST_DATA_DIR) / "SkyBoxConvention" /
				std::format("{}.png", FaceNames[FaceIndex])).generic_string();
		}
		return Result;
	}

	inline auto CreateAssetThumbnailFixtures(
		FAssetThumbnailFixtureSet& OutFixtures,
		std::string& OutError
	) -> bool
	{
		InitializeDObjectSystem();
		if (!FAssetCompilingManager::Get().IsAcceptingRequests()) InitializeAssetCompilingManager();
		const std::filesystem::path Root = GetAssetThumbnailFixtureRoot();
		static std::unordered_map<std::filesystem::path, FAssetThumbnailFixtureSet> CachedFixtures;
		if (auto It = CachedFixtures.find(Root); It != CachedFixtures.end())
		{
			FPackagePath StaticMeshPath;
			if (!MakeThumbnailFixturePath(
					FAssetThumbnailFixtureSet::StaticMeshPath,
					StaticMeshPath))
			{
				OutError = "The cached StaticMesh fixture path is invalid.";
				return false;
			}
			if (FindResidentPackage(StaticMeshPath) == nullptr)
			{
				std::optional<Durin::UnifiedError::FError> ResultError;
				DObject* Loaded = nullptr;
				const auto Result = LoadObject<DObject>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(StaticMeshPath), nullptr, &ResultError);
				Loaded = Result;
				It->second.StaticMesh = Result ? Cast<DStaticMesh>(Loaded) : nullptr;
				if (It->second.StaticMesh) It->second.StaticMesh->SetFlags(EObjectFlags::Standalone);
				if (!Result || It->second.StaticMesh == nullptr)
				{
					OutError = ResultError ? ResultError->ToString()
						: "Could not reload the cached StaticMesh thumbnail fixture.";
					return false;
				}
			}
			FAssetCompilingManager::Get().FinishCompilationForObject(*It->second.StaticMesh);
			OutFixtures = It->second;
			OutError.clear();
			return true;
		}
		Durin::Testing::RemoveTestWorkDirectory(Root);
		RegisterAssetThumbnailFixtureMount();

		auto Fail = [&OutError](std::string Message) {
			OutError = std::move(Message);
			return false;
		};
		auto MakePath = [&Fail](std::string_view Value, FPackagePath& OutPath) {
			return MakeThumbnailFixturePath(Value, OutPath)
				? true
				: Fail(std::format("Invalid rendered-thumbnail fixture path: {}.", Value));
		};

		FPackagePath MaterialPath;
		FPackagePath MaterialInstancePath;
		FPackagePath InvalidMaterialInstancePath;
		FPackagePath ParentTexturePath;
		FPackagePath OverrideTexturePath;
		FPackagePath StaticMeshPath;
		if (!MakePath(FAssetThumbnailFixtureSet::MaterialPath, MaterialPath)
			|| !MakePath(FAssetThumbnailFixtureSet::MaterialInstancePath, MaterialInstancePath)
			|| !MakePath(FAssetThumbnailFixtureSet::InvalidMaterialInstancePath, InvalidMaterialInstancePath)
			|| !MakePath(FAssetThumbnailFixtureSet::ParentTexturePath, ParentTexturePath)
			|| !MakePath(FAssetThumbnailFixtureSet::OverrideTexturePath, OverrideTexturePath)
			|| !MakePath(FAssetThumbnailFixtureSet::StaticMeshPath, StaticMeshPath))
		{
			return false;
		}

		const std::filesystem::path DataRoot = std::filesystem::path(DURIN_TEST_DATA_DIR) / "SkyBoxConvention";
		const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> ParentTextureResult = AssetForge::Builtins::ImportTexture2DForTest(
			(DataRoot / "PositiveX.png").generic_string(), ParentTexturePath.ToString());
		if (!ParentTextureResult) return Fail(ParentTextureResult.Message);
		OutFixtures.ParentTexture = ParentTextureResult.Asset;

		const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> OverrideTextureResult = AssetForge::Builtins::ImportTexture2DForTest(
			(DataRoot / "NegativeX.png").generic_string(), OverrideTexturePath.ToString());
		if (!OverrideTextureResult) return Fail(OverrideTextureResult.Message);
		OutFixtures.OverrideTexture = OverrideTextureResult.Asset;

		auto Created = CreatePackageLeafAssetForTesting(MaterialPath, OutFixtures.Material);
		if (!Created) return Fail(Created.Message);
		if (!Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*OutFixtures.Material))
			return Fail("Could not assign the expanded material fixture program.");
		if (!OutFixtures.Material->SetVectorParameterValue(
				Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), FVector3(0.35, 0.55, 0.75))
			|| !OutFixtures.Material->SetScalarParameterValue(
				Durin::AssetForge::Builtins::MaterialParameters::MetallicName(), 0.4f)
			|| !OutFixtures.Material->SetScalarParameterValue(
				Durin::AssetForge::Builtins::MaterialParameters::RoughnessName(), 0.24f)
			|| !OutFixtures.Material->SetTextureParameterValue(
				Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), OutFixtures.ParentTexture))
		{
			return Fail("Could not assign the deterministic material fixture values.");
		}
		FAssetWriteResult Result = SavePackage(OutFixtures.Material->GetPackage());
		if (!Result) return Fail(Result.Message);

		Created = CreatePackageLeafAssetForTesting(MaterialInstancePath, OutFixtures.MaterialInstance);
		if (!Created) return Fail(Created.Message);
		if (!OutFixtures.MaterialInstance->SetParent(OutFixtures.Material)
			|| !OutFixtures.MaterialInstance->SetVectorParameterValue(
				Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), FVector3(0.8, 0.28, 0.12))
			|| !OutFixtures.MaterialInstance->SetScalarParameterValue(
				Durin::AssetForge::Builtins::MaterialParameters::MetallicName(), 0.7f)
			|| !OutFixtures.MaterialInstance->SetTextureParameterValue(
				Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), OutFixtures.OverrideTexture))
		{
			return Fail("Could not assign the deterministic material-instance fixture values.");
		}
		Result = SavePackage(OutFixtures.MaterialInstance->GetPackage());
		if (!Result) return Fail(Result.Message);

		Created = CreatePackageLeafAssetForTesting(StaticMeshPath, OutFixtures.StaticMesh);
		if (!Created) return Fail(Created.Message);
		FMeshDescription ImportedMesh;
		ImportedMesh.PolygonGroups.push_back({
			.Name = "Default",
			.SourceMaterialIndex = 0,
			.SourceName = "Default"});
		FMeshDescriptionSection& Mesh = ImportedMesh.Sections.emplace_back();
		Mesh.Name = "ThumbnailTetrahedron";
		Mesh.Positions = {
			FVector3f(-0.6f, -0.5f, -0.4f),
			FVector3f(0.7f, -0.4f, -0.3f),
			FVector3f(0.0f, 0.8f, -0.2f),
			FVector3f(0.1f, 0.0f, 0.9f)};
		Mesh.Indices = {
			0, 2, 1,
			0, 1, 3,
			1, 2, 3,
			2, 0, 3};
		Mesh.SourceMaterialIndex = 0;
		if (const auto Built = Durin::FStaticMeshTestAccess::Build(OutFixtures.StaticMesh, std::move(ImportedMesh)); !Built)
		{
			OutError = Built.error();
			return false;
		}
		OutFixtures.StaticMesh->SetMaterialSlotDefaultMaterial(0, OutFixtures.Material);
		Result = SavePackage(OutFixtures.StaticMesh->GetPackage());
		if (!Result) return Fail(Result.Message);

		Created = CreatePackageLeafAssetForTesting(InvalidMaterialInstancePath, OutFixtures.InvalidMaterialInstance);
		if (!Created) return Fail(Created.Message);
		Result = SavePackage(OutFixtures.InvalidMaterialInstance->GetPackage());
		if (!Result) return Fail(Result.Message);

		const Durin::Testing::TFactoryImportResult<Durin::DTextureCube> CubeResult = AssetForge::Builtins::ImportTextureCubeFacesForTest(
			GetThumbnailDirectionalCubeFaces(),
			FAssetThumbnailFixtureSet::DirectionalCubePath);
		if (!CubeResult) return Fail(CubeResult.Message);
		OutFixtures.DirectionalCube = CubeResult.Asset;

		// Cached editor fixtures must survive collections between toolkit operations.
		for (DObject* Asset : std::array<DObject*, 7>{OutFixtures.Material, OutFixtures.MaterialInstance,
			OutFixtures.InvalidMaterialInstance, OutFixtures.ParentTexture, OutFixtures.OverrideTexture,
			OutFixtures.DirectionalCube, OutFixtures.StaticMesh})
			Asset->SetFlags(EObjectFlags::Standalone);
		CachedFixtures.emplace(Root, OutFixtures);
		OutError.clear();
		return true;
	}
} // namespace Durin::Tests
