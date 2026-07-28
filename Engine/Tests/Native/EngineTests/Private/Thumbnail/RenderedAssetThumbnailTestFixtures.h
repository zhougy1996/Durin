#pragma once

#include "AssetSystem.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"

namespace Durin::Tests
{
	// Names and values in this fixture set are versioned inputs to rendered-thumbnail golden tests.
	struct FRenderedAssetThumbnailFixtureSet
	{
		static constexpr uint32 Version = 1;
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

		DMaterial* Material = nullptr;
		DMaterialInstance* MaterialInstance = nullptr;
		DMaterialInstance* InvalidMaterialInstance = nullptr;
		DTexture2D* ParentTexture = nullptr;
		DTexture2D* OverrideTexture = nullptr;
		DTextureCube* DirectionalCube = nullptr;
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
		PathUtilities::RegisterMountPoint(
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
		if (const auto It = CachedFixtures.find(Root); It != CachedFixtures.end())
		{
			OutFixtures = It->second;
			OutError.clear();
			return true;
		}
		std::filesystem::remove_all(Root);
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
		if (!MakePath(FRenderedAssetThumbnailFixtureSet::MaterialPath, MaterialPath)
			|| !MakePath(FRenderedAssetThumbnailFixtureSet::MaterialInstancePath, MaterialInstancePath)
			|| !MakePath(FRenderedAssetThumbnailFixtureSet::InvalidMaterialInstancePath, InvalidMaterialInstancePath)
			|| !MakePath(FRenderedAssetThumbnailFixtureSet::ParentTexturePath, ParentTexturePath)
			|| !MakePath(FRenderedAssetThumbnailFixtureSet::OverrideTexturePath, OverrideTexturePath))
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
				MaterialParameters::SpecularStrengthName(), 0.4f)
			|| !OutFixtures.Material->SetScalarParameterValue(
				MaterialParameters::ShininessName(), 24.0f)
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
				MaterialParameters::SpecularStrengthName(), 0.7f)
			|| !OutFixtures.MaterialInstance->SetTextureParameterValue(
				MaterialParameters::BaseColorTextureName(), OutFixtures.OverrideTexture))
		{
			return Fail("Could not assign the deterministic material-instance fixture values.");
		}
		Result = Asset::SavePackage(OutFixtures.MaterialInstance->GetPackage());
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
