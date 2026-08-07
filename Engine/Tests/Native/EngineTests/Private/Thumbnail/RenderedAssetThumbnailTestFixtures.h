#pragma once

#include "AssetSystem.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"

namespace Durin::Tests
{
	// Names and values in this fixture set are versioned inputs to rendered-thumbnail golden tests.
	struct FRenderedAssetThumbnailFixtureSet
	{
		static constexpr uint32 Version = 2;
		static constexpr std::string_view MountPoint = "/RenderedThumbnailFixtures/";
		static constexpr std::string_view MaterialPath = "/RenderedThumbnailFixtures/Materials/M_Deterministic";
		static constexpr std::string_view MaterialInstancePath =
			"/RenderedThumbnailFixtures/Materials/MI_Deterministic";
		static constexpr std::string_view InvalidMaterialInstancePath =
			"/RenderedThumbnailFixtures/Materials/MI_MissingParent";
		static constexpr std::string_view ParentTexturePath =
			"/RenderedThumbnailFixtures/Textures/T_ParentColor";
		static constexpr std::string_view OverrideTexturePath =
			"/RenderedThumbnailFixtures/Textures/T_InstanceColor";
		static constexpr std::string_view DirectionalCubePath =
			"/RenderedThumbnailFixtures/Textures/TC_Directional";
		static constexpr std::string_view StaticMeshPath =
			"/RenderedThumbnailFixtures/Meshes/SM_Deterministic";

		DMaterial* Material = nullptr;
		DMaterialInstance* MaterialInstance = nullptr;
		DMaterialInstance* InvalidMaterialInstance = nullptr;
		DTexture2D* ParentTexture = nullptr;
		DTexture2D* OverrideTexture = nullptr;
		DTextureCube* DirectionalCube = nullptr;
		DStaticMesh* StaticMesh = nullptr;
	};

	inline auto MakeRenderedThumbnailFixturePath(std::string_view Value, FAssetPath& OutPath) -> bool
	{
		return FAssetPath::TryCreate(Value, OutPath);
	}

	inline auto GetRenderedAssetThumbnailFixtureRoot() -> std::filesystem::path
	{
		return Testing::GetTestWorkDirectory()
			/ "RenderedAssetThumbnailFixtures";
	}

	inline auto RegisterRenderedAssetThumbnailFixtureMount() -> std::filesystem::path
	{
		InitializeDObjectSystem();
		const std::filesystem::path Root = GetRenderedAssetThumbnailFixtureRoot();
		PathUtilities::RegisterMountPointForTests(
			FRenderedAssetThumbnailFixtureSet::MountPoint,
			Root.generic_string() + "/");
		return Root;
	}

	inline auto GetRenderedThumbnailDirectionalCubeFaces()
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

	inline auto CreateRenderedAssetThumbnailFixtures(
		FRenderedAssetThumbnailFixtureSet& OutFixtures,
		std::string& OutError
	) -> bool
	{
		InitializeDObjectSystem();
		const std::filesystem::path Root = GetRenderedAssetThumbnailFixtureRoot();
		static std::unordered_map<std::filesystem::path, FRenderedAssetThumbnailFixtureSet> CachedFixtures;
		if (auto It = CachedFixtures.find(Root); It != CachedFixtures.end())
		{
			FAssetPath StaticMeshPath;
			if (!MakeRenderedThumbnailFixturePath(
					FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
					StaticMeshPath))
			{
				OutError = "The cached StaticMesh fixture path is invalid.";
				return false;
			}
			if (Asset::FindLoadedPackage(StaticMeshPath) == nullptr)
			{
				DObject* Loaded = nullptr;
				const Asset::FAssetResult Result = Asset::LoadAsset(StaticMeshPath, Loaded);
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
		RegisterRenderedAssetThumbnailFixtureMount();

		auto Fail = [&OutError](std::string Message) {
			OutError = std::move(Message);
			return false;
		};
		auto MakePath = [&Fail](std::string_view Value, FAssetPath& OutPath) {
			return MakeRenderedThumbnailFixturePath(Value, OutPath)
				? true
				: Fail(std::format("Invalid rendered-thumbnail fixture path: {}.", Value));
		};

		FAssetPath MaterialPath;
		FAssetPath MaterialInstancePath;
		FAssetPath InvalidMaterialInstancePath;
		FAssetPath ParentTexturePath;
		FAssetPath OverrideTexturePath;
		FAssetPath StaticMeshPath;
		if (!MakePath(FRenderedAssetThumbnailFixtureSet::MaterialPath, MaterialPath)
			|| !MakePath(FRenderedAssetThumbnailFixtureSet::MaterialInstancePath, MaterialInstancePath)
			|| !MakePath(FRenderedAssetThumbnailFixtureSet::InvalidMaterialInstancePath, InvalidMaterialInstancePath)
			|| !MakePath(FRenderedAssetThumbnailFixtureSet::ParentTexturePath, ParentTexturePath)
			|| !MakePath(FRenderedAssetThumbnailFixtureSet::OverrideTexturePath, OverrideTexturePath)
			|| !MakePath(FRenderedAssetThumbnailFixtureSet::StaticMeshPath, StaticMeshPath))
		{
			return false;
		}

		const std::filesystem::path DataRoot = std::filesystem::path(DURIN_TEST_DATA_DIR) / "SkyBoxConvention";
		const FTexture2DImportResult ParentTextureResult = DTexture2D::ImportAsset(
			(DataRoot / "PositiveX.png").generic_string(), ParentTexturePath.ToString());
		if (!ParentTextureResult) return Fail(ParentTextureResult.Message);
		OutFixtures.ParentTexture = ParentTextureResult.Asset;

		const FTexture2DImportResult OverrideTextureResult = DTexture2D::ImportAsset(
			(DataRoot / "NegativeX.png").generic_string(), OverrideTexturePath.ToString());
		if (!OverrideTextureResult) return Fail(OverrideTextureResult.Message);
		OutFixtures.OverrideTexture = OverrideTextureResult.Asset;

		Asset::FAssetResult Result = Asset::CreateAsset(MaterialPath, OutFixtures.Material);
		if (!Result) return Fail(Result.Message);
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

		Result = Asset::CreateAsset(MaterialInstancePath, OutFixtures.MaterialInstance);
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

		Result = Asset::CreateAsset(StaticMeshPath, OutFixtures.StaticMesh);
		if (!Result) return Fail(Result.Message);
		FStaticMeshImportedData ImportedMesh;
		ImportedMesh.MaterialSlots.push_back({
			.Name = "Default",
			.SourceMaterialIndex = 0,
			.SourceName = "Default"});
		FStaticMeshImportedMesh& Mesh = ImportedMesh.Meshes.emplace_back();
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
		FStaticMeshSourceImportData SourceImportData = {
			.SourcePath = {.Path =
				"/RenderedThumbnailFixtures/Sources/SM_Deterministic.fixture"},
			.SourceContentHash = "0123456789abcdef0123456789abcdef",
			.ImporterId = "RenderedThumbnailFixture",
			.ImporterVersion = 1,
			.ImportSettings = FStaticMeshImportSettings::MakeDurin()};
		if (!OutFixtures.StaticMesh->InitializeFromImportedData(
				ImportedMesh,
				SourceImportData,
				"Rendered thumbnail StaticMesh fixture",
				OutError)
			|| !OutFixtures.StaticMesh->SetImportedDefaultMaterial(
				0, OutFixtures.Material, OutError))
		{
			return false;
		}
		Result = Asset::SavePackage(OutFixtures.StaticMesh->GetPackage());
		if (!Result) return Fail(Result.Message);

		Result = Asset::CreateAsset(InvalidMaterialInstancePath, OutFixtures.InvalidMaterialInstance);
		if (!Result) return Fail(Result.Message);
		Result = Asset::SavePackage(OutFixtures.InvalidMaterialInstance->GetPackage());
		if (!Result) return Fail(Result.Message);

		const FTextureCubeImportResult CubeResult = DTextureCube::ImportAsset(
			GetRenderedThumbnailDirectionalCubeFaces(),
			FRenderedAssetThumbnailFixtureSet::DirectionalCubePath);
		if (!CubeResult) return Fail(CubeResult.Message);
		OutFixtures.DirectionalCube = CubeResult.Asset;

		CachedFixtures.emplace(Root, OutFixtures);
		OutError.clear();
		return true;
	}
} // namespace Durin::Tests
