#pragma once
#include "NativeDObjectTestSupport.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"

#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "AssetCook.h"
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
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuildOperations.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Thumbnail/StaticMeshThumbnailRenderer.h"
#include "Thumbnail/TextureCubeThumbnailRenderer.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "Texture/TextureCube.h"

namespace Durin::Tests
{
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
			Editor::FThumbnailVisualContract InContract = {})
			: Contract(std::move(InContract))
			, Pool(Contract)
			, View(MakeView(Contract))
		{
		}

		~FAssetThumbnailTestPool()
		{
			Reset();
		}

		auto IsAvailable() const -> bool { return Pool.IsAvailable(); }
		auto GetDiagnostic() const -> std::string { return Pool.GetDiagnostic(); }
		auto SetForceLOD0(bool bForceLOD0) -> void
		{
			View.bForceLOD0 = bForceLOD0;
		}
		auto SetView(std::string& OutError) -> bool
		{
			return Pool.SetView(View, OutError);
		}
		auto SetViewEnvironment(
			const FViewEnvironmentOverride& Environment,
			std::string& OutError) -> bool
		{
			return Pool.SetViewEnvironment(Environment, OutError);
		}

		auto GetSphereMesh() -> DStaticMesh*
		{
			if (!SphereAsset)
			{
				FObjectPath SpherePath;
				std::string Error;
				if (!FObjectPath::TryCreate(
						Editor::FThumbnailVisualContract::SphereAssetPath,
						SpherePath,
						&Error)
					|| !Editor::FAssetRetentionService::Acquire(
						SpherePath, SphereAsset, Error))
					return nullptr;
			}
			return Cast<DStaticMesh>(SphereAsset.Get());
		}

		auto SetMaterial(
			DStaticMesh* Mesh,
			DMaterialInterface* Material,
			const FTransform& Transform,
			std::string& OutError) -> bool
		{
			ResetActor();
			DWorld* World = Pool.GetWorld();
			Actor = World
				? World->SpawnActor<AActor>("ThumbnailTestMaterialActor")
				: nullptr;
			auto* Component = Actor
				? Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(
					DStaticMeshComponent::StaticClass(), "MaterialPreview"))
				: nullptr;
			if (Mesh == nullptr || Material == nullptr || Component == nullptr)
			{
				OutError = "The material test preview is unavailable.";
				ResetActor();
				return false;
			}
			Component->SetStaticMesh(Mesh);
			for (uint32 SlotIndex = 0; SlotIndex < Component->GetNumMaterials(); ++SlotIndex)
				Component->SetMaterial(SlotIndex, Material);
			Component->SetWorldTransform(Transform);
			return Pool.SetView(View, OutError);
		}

		auto SetTextureCube(
			DTextureCube* TextureCube,
			std::string& OutError) -> bool
		{
			ResetActor();
			const FRHITextureReferenceRef TextureReference = TextureCube
				? TextureCube->GetTextureReferenceRHI()
				: FRHITextureReferenceRef{};
			if (TextureReference == nullptr)
			{
				OutError = "The TextureCube test preview is unavailable.";
				return false;
			}
			View.VerticalFieldOfViewDegrees =
				Math::RadiansToDegrees(2.0 * std::atan(1.0 / static_cast<double>(
					1.0f / std::tan(Math::DegreesToRadians(
						Editor::Texture::FTextureCubeThumbnailRendererVisualContract::
							VerticalFieldOfViewDegrees) * 0.5f))));
			return Pool.SetView(View, OutError)
				&& Pool.SetViewEnvironment(
					{.TextureReference = TextureReference}, OutError);
		}

		auto SetStaticMesh(
			DStaticMesh* StaticMesh,
			const Editor::StaticMesh::FStaticMeshThumbnailRendererView& ThumbnailView,
			std::string& OutError) -> bool
		{
			ResetActor();
			DWorld* World = Pool.GetWorld();
			Actor = World
				? World->SpawnActor<AActor>("ThumbnailTestStaticMeshActor")
				: nullptr;
			auto* Component = Actor
				? Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(
					DStaticMeshComponent::StaticClass(), "StaticMeshPreview"))
				: nullptr;
			if (StaticMesh == nullptr || Component == nullptr)
			{
				OutError = "The StaticMesh test preview is unavailable.";
				ResetActor();
				return false;
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
			return Pool.SetView(View, OutError);
		}

		auto BeginCapture(std::string& OutError, bool bOutputOpaque = true) -> bool
		{
			View.ClearRed = bOutputOpaque ? Contract.BackgroundRed : 0.0f;
			View.ClearGreen = bOutputOpaque ? Contract.BackgroundGreen : 0.0f;
			View.ClearBlue = bOutputOpaque ? Contract.BackgroundBlue : 0.0f;
			View.ClearAlpha = bOutputOpaque ? 1.0f : 0.0f;
			return Pool.SetView(View, OutError) && Pool.BeginCapture(OutError);
		}

		auto PollCapture(Durin::FByteArray& OutPixels, std::string& OutError)
			-> Editor::EThumbnailCaptureState
		{
			return Pool.PollCapture(OutPixels, OutError);
		}

		auto Reset() -> void
		{
			ResetActor();
			Pool.Reset();
			View = MakeView(Contract);
		}

	private:
		static auto MakeView(const Editor::FThumbnailVisualContract& Contract)
			-> Editor::FThumbnailPreviewView
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
				if (DWorld* World = Pool.GetWorld()) World->DestroyActor(Actor);
				Actor = nullptr;
			}
		}

		Editor::FThumbnailVisualContract Contract;
		Editor::FThumbnailPreviewScenePool Pool;
		Editor::FThumbnailPreviewView View;
		AActor* Actor = nullptr;
		Editor::FRetainedAsset SphereAsset;
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
			if (Asset::FindResidentPackage(StaticMeshPath) == nullptr)
			{
				DObject* Loaded = nullptr;
				const Asset::FAssetResult Result = Asset::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(StaticMeshPath), Loaded);
				It->second.StaticMesh = Result ? Cast<DStaticMesh>(Loaded) : nullptr;
				if (!Result || It->second.StaticMesh == nullptr)
				{
					OutError = Result.Message.empty()
						? "Could not reload the cached StaticMesh thumbnail fixture."
						: Result.Message;
					return false;
				}
			}
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

		Asset::FAssetResult Result = Asset::CreatePackageLeafAssetForTesting(MaterialPath, OutFixtures.Material);
		if (!Result) return Fail(Result.Message);
		FMaterialProgramValidationResult ProgramValidation;
		if (!OutFixtures.Material->SetMaterialProgram(
			MakeCanonicalMaterialProgram(), ProgramValidation))
			return Fail("Could not assign the expanded material fixture program.");
		if (!OutFixtures.Material->SetVectorParameterValue(
				MaterialParameters::BaseColorName(), FVector3(0.35, 0.55, 0.75))
			|| !OutFixtures.Material->SetScalarParameterValue(
				MaterialParameters::MetallicName(), 0.4f)
			|| !OutFixtures.Material->SetScalarParameterValue(
				MaterialParameters::RoughnessName(), 0.24f)
			|| !OutFixtures.Material->SetTextureParameterValue(
				MaterialParameters::BaseColorTextureName(), OutFixtures.ParentTexture))
		{
			return Fail("Could not assign the deterministic material fixture values.");
		}
		Result = Asset::SavePackage(OutFixtures.Material->GetPackage());
		if (!Result) return Fail(Result.Message);

		Result = Asset::CreatePackageLeafAssetForTesting(MaterialInstancePath, OutFixtures.MaterialInstance);
		if (!Result) return Fail(Result.Message);
		if (!OutFixtures.MaterialInstance->SetParent(OutFixtures.Material)
			|| !OutFixtures.MaterialInstance->SetVectorParameterValue(
				MaterialParameters::BaseColorName(), FVector3(0.8, 0.28, 0.12))
			|| !OutFixtures.MaterialInstance->SetScalarParameterValue(
				MaterialParameters::MetallicName(), 0.7f)
			|| !OutFixtures.MaterialInstance->SetTextureParameterValue(
				MaterialParameters::BaseColorTextureName(), OutFixtures.OverrideTexture))
		{
			return Fail("Could not assign the deterministic material-instance fixture values.");
		}
		Result = Asset::SavePackage(OutFixtures.MaterialInstance->GetPackage());
		if (!Result) return Fail(Result.Message);

		Result = Asset::CreatePackageLeafAssetForTesting(StaticMeshPath, OutFixtures.StaticMesh);
		if (!Result) return Fail(Result.Message);
		Asset::FStaticMeshImportedData ImportedMesh;
		ImportedMesh.MaterialSlots.push_back({
			.Name = "Default",
			.SourceMaterialIndex = 0,
			.SourceName = "Default"});
		Asset::FStaticMeshImportedMesh& Mesh = ImportedMesh.Meshes.emplace_back();
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
		if (!Asset::FStaticMeshBuildOperations::BuildAndPublishImported(
				*OutFixtures.StaticMesh, ImportedMesh,
				"Rendered thumbnail StaticMesh fixture",
				OutError)
			|| !OutFixtures.StaticMesh->SetImportedDefaultMaterial(
				0, OutFixtures.Material, OutError))
		{
			return false;
		}
		Result = Asset::SavePackage(OutFixtures.StaticMesh->GetPackage());
		if (!Result) return Fail(Result.Message);

		Result = Asset::CreatePackageLeafAssetForTesting(InvalidMaterialInstancePath, OutFixtures.InvalidMaterialInstance);
		if (!Result) return Fail(Result.Message);
		Result = Asset::SavePackage(OutFixtures.InvalidMaterialInstance->GetPackage());
		if (!Result) return Fail(Result.Message);

		const Durin::Testing::TFactoryImportResult<Durin::DTextureCube> CubeResult = AssetForge::Builtins::ImportTextureCubeFacesForTest(
			GetThumbnailDirectionalCubeFaces(),
			FAssetThumbnailFixtureSet::DirectionalCubePath);
		if (!CubeResult) return Fail(CubeResult.Message);
		OutFixtures.DirectionalCube = CubeResult.Asset;

		CachedFixtures.emplace(Root, OutFixtures);
		OutError.clear();
		return true;
	}
} // namespace Durin::Tests
