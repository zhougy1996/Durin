#include "AssetSystem.h"
#include "CookedAsset.h"
#include "DObject/Property.h"
#include "EngineTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "RenderingThread.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/TextureDerivedData.h"

#include <gtest/gtest.h>
#include <unordered_set>

namespace
{
	constexpr std::array<std::string_view, Durin::TextureCubeFaceCount> FaceNames = {
		"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};

	auto GetConventionFaces() -> std::array<std::string, Durin::TextureCubeFaceCount>
	{
		std::array<std::string, Durin::TextureCubeFaceCount> Result;
		for (size_t FaceIndex = 0; FaceIndex < Result.size(); ++FaceIndex)
		{
			Result[FaceIndex] = (std::filesystem::path(DURIN_TEST_DATA_DIR) / "SkyBoxConvention" /
				std::format("{}.png", FaceNames[FaceIndex])).generic_string();
		}
		return Result;
	}

	auto GetPanoramaFixture(std::string_view FileName) -> std::filesystem::path
	{
		return std::filesystem::path(DURIN_TEST_DATA_DIR) /
			"EquirectangularPanorama" / FileName;
	}

	auto WriteSolidTga(const std::filesystem::path& Path, Durin::uint16 Width, Durin::uint16 Height,
		Durin::uint8 Alpha = 255) -> void
	{
		std::array<Durin::uint8, 18> Header{};
		Header[2] = 2;
		Header[12] = static_cast<Durin::uint8>(Width & 0xff);
		Header[13] = static_cast<Durin::uint8>(Width >> 8);
		Header[14] = static_cast<Durin::uint8>(Height & 0xff);
		Header[15] = static_cast<Durin::uint8>(Height >> 8);
		Header[16] = 32;
		Header[17] = 0x28;
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(Header.data()), Header.size());
		const std::array<Durin::uint8, 4> Pixel = {32, 64, 128, Alpha};
		for (Durin::uint32 PixelIndex = 0; PixelIndex < static_cast<Durin::uint32>(Width) * Height; ++PixelIndex)
			Stream.write(reinterpret_cast<const char*>(Pixel.data()), Pixel.size());
	}

	auto InitializeCubeMount() -> std::filesystem::path
	{
		InitializeDObjectSystem();
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "TextureCubeImports";
		static std::unordered_set<std::filesystem::path> InitializedRoots;
		if (InitializedRoots.insert(Root).second)
		{
			Durin::Testing::RemoveTestWorkDirectory(Root);
			Durin::PathUtilities::RegisterMountPoint("/TextureCubeTests/", Root.generic_string() + "/");
		}
		return Root;
	}

	auto RestartAssetManager() -> void
	{
		Durin::Asset::ShutdownAssetManager();
		Durin::CollectGarbage();
		Durin::Asset::FAssetManager::Get().Initialize();
	}
}

TEST(FTextureCubeTests, RetiredSourceStringsAreNotReflected)
{
	InitializeCubeMount();
	for (std::string_view RetiredField : {
		"PositiveXSourceFile",
		"NegativeXSourceFile",
		"PositiveYSourceFile",
		"NegativeYSourceFile",
		"PositiveZSourceFile",
		"NegativeZSourceFile",
		"PanoramaSourceFile"})
		EXPECT_EQ(
			Durin::DTextureCube::StaticClass()->FindPropertyByName(RetiredField),
			nullptr);
}

TEST(FTextureCubeTests, ImportsReloadsMovesAndDeletesSixFaceAsset)
{
	const std::filesystem::path Root = InitializeCubeMount();
	const auto Faces = GetConventionFaces();
	Durin::FTextureCubeImportResult Result = Durin::DTextureCube::ImportAsset(
		Faces, "/TextureCubeTests/Convention");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	ASSERT_NE(Result.Asset->GetSourceData(), nullptr);
	ASSERT_NE(Result.Asset->GetPlatformData(), nullptr);
	EXPECT_TRUE(Result.Asset->GetSourceData()->IsValid());
	EXPECT_TRUE(Result.Asset->GetPlatformData()->IsValid());
	EXPECT_EQ(Result.Asset->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_EQ(Result.Asset->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC1_UNORM_SRGB);
	EXPECT_EQ(Result.Asset->GetBuildRevision(), 1u);
	for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
	{
		const auto Face = static_cast<Durin::ETextureCubeFace>(FaceIndex);
		EXPECT_EQ(Result.Asset->GetSourceFile(Face), std::format("/TextureCubeTests/Textures/Convention_{}.png",
			std::array<std::string_view, Durin::TextureCubeFaceCount>{"px", "nx", "py", "ny", "pz", "nz"}[FaceIndex]));
		EXPECT_TRUE(std::filesystem::is_regular_file(Root / std::format(
			"SourceAssets/Textures/Convention_{}.png",
			std::array<std::string_view, Durin::TextureCubeFaceCount>{"px", "nx", "py", "ny", "pz", "nz"}[FaceIndex])));
	}

	const Durin::FTextureCubePlatformData* PlatformData = Result.Asset->GetPlatformData();
	ASSERT_EQ(PlatformData->Faces[0].Mips.size(), 8u);
	for (const Durin::FTexturePlatformData& Face : PlatformData->Faces)
	{
		ASSERT_EQ(Face.Mips.size(), PlatformData->Faces[0].Mips.size());
		EXPECT_EQ(Face.Mips.front().Width, 128u);
		EXPECT_EQ(Face.Mips.front().Height, 128u);
		EXPECT_EQ(Face.Mips.back().Width, 1u);
		EXPECT_EQ(Face.Mips.back().Height, 1u);
	}

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/Convention", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	Durin::DTextureCube* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_FALSE(Loaded->GetDerivedDataDiagnostic().bSourceDecoderInvoked);
	EXPECT_EQ(Loaded->GetSourceFile(Durin::ETextureCubeFace::PositiveX),
		"/TextureCubeTests/Textures/Convention_px.png");
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::FAssetPath RenamedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/RenamedCube", RenamedPath));
	ASSERT_TRUE(Durin::Asset::MoveAsset(AssetPath, RenamedPath));
	for (std::string_view Suffix : {"px", "nx", "py", "ny", "pz", "nz"})
	{
		EXPECT_TRUE(std::filesystem::is_regular_file(
			Root / std::format("SourceAssets/Textures/Convention_{}.png", Suffix)));
		EXPECT_FALSE(std::filesystem::exists(
			Root / std::format("SourceAssets/Textures/RenamedCube_{}.png", Suffix)));
	}
	ASSERT_TRUE(Durin::Asset::LoadAsset(RenamedPath, Loaded));
	EXPECT_EQ(Loaded->GetSourceFile(Durin::ETextureCubeFace::NegativeZ),
		"/TextureCubeTests/Textures/Convention_nz.png");
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RenamedPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RenamedPath));
	for (std::string_view Suffix : {"px", "nx", "py", "ny", "pz", "nz"})
		EXPECT_TRUE(std::filesystem::is_regular_file(
			Root / std::format("SourceAssets/Textures/Convention_{}.png", Suffix)));
}

TEST(FTextureCubeTests, RejectsMissingNonsquareAndMismatchedFacesWithoutArtifacts)
{
	const std::filesystem::path Root = InitializeCubeMount();
	auto Faces = GetConventionFaces();
	Faces[static_cast<size_t>(Durin::ETextureCubeFace::PositiveY)].clear();
	Durin::FTextureCubeImportResult Missing = Durin::DTextureCube::ImportAsset(
		Faces, "/TextureCubeTests/MissingFace");
	EXPECT_FALSE(Missing);
	EXPECT_NE(Missing.Message.find("PositiveY"), std::string::npos);

	const std::filesystem::path Nonsquare = Durin::Testing::GetTestWorkDirectory() / "CubeNonsquare.tga";
	WriteSolidTga(Nonsquare, 4, 2);
	Faces = GetConventionFaces();
	Faces[0] = Nonsquare.generic_string();
	Durin::FTextureCubeImportResult InvalidShape = Durin::DTextureCube::ImportAsset(
		Faces, "/TextureCubeTests/Nonsquare");
	EXPECT_FALSE(InvalidShape);
	EXPECT_NE(InvalidShape.Message.find("square"), std::string::npos);

	const std::filesystem::path DifferentSize = Durin::Testing::GetTestWorkDirectory() / "CubeDifferentSize.tga";
	WriteSolidTga(DifferentSize, 4, 4);
	Faces = GetConventionFaces();
	Faces[0] = DifferentSize.generic_string();
	Durin::FTextureCubeImportResult Mismatch = Durin::DTextureCube::ImportAsset(
		Faces, "/TextureCubeTests/Mismatch");
	EXPECT_FALSE(Mismatch);
	EXPECT_NE(Mismatch.Message.find("identical"), std::string::npos);

	const std::filesystem::path Corrupt = Durin::Testing::GetTestWorkDirectory() / "CubeCorrupt.png";
	{
		std::ofstream Stream(Corrupt, std::ios::binary | std::ios::trunc);
		Stream << "not an image";
	}
	Faces = GetConventionFaces();
	Faces[static_cast<size_t>(Durin::ETextureCubeFace::NegativeZ)] = Corrupt.generic_string();
	const Durin::FTextureCubeImportValidation CorruptValidation =
		Durin::DTextureCube::ValidateImportSources(Faces);
	EXPECT_FALSE(CorruptValidation);
	EXPECT_NE(CorruptValidation.Message.find("NegativeZ"), std::string::npos);
	EXPECT_NE(CorruptValidation.Message.find("decode failed"), std::string::npos);

	for (std::string_view AssetName : {"MissingFace", "Nonsquare", "Mismatch"})
	{
		Durin::FAssetPath AssetPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(std::format("/TextureCubeTests/{}", AssetName), AssetPath));
		EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(AssetPath), nullptr);
		EXPECT_EQ(Durin::Asset::FindLoadedPackage(AssetPath), nullptr);
	}
	EXPECT_FALSE(std::filesystem::exists(Root / "MissingFace_px.png"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Nonsquare_px.tga"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Mismatch_px.tga"));
}

TEST(FTextureCubeTests, UsesOneCompressedFormatWhenOnlyOneFaceHasTransparency)
{
	const std::filesystem::path Root = InitializeCubeMount();
	const std::filesystem::path TransparentFace = Durin::Testing::GetTestWorkDirectory() / "CubeTransparent.tga";
	WriteSolidTga(TransparentFace, 128, 128, 128);
	auto Faces = GetConventionFaces();
	Faces[static_cast<size_t>(Durin::ETextureCubeFace::NegativeZ)] = TransparentFace.generic_string();

	Durin::FTextureCubeImportResult Result = Durin::DTextureCube::ImportAsset(
		Faces, "/TextureCubeTests/Transparent");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset->GetPlatformData(), nullptr);
	EXPECT_EQ(Result.Asset->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	for (const Durin::FTexturePlatformData& Face : Result.Asset->GetPlatformData()->Faces)
		EXPECT_EQ(Face.PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/Transparent", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
	EXPECT_FALSE(std::filesystem::exists(Root / "Transparent_nz.tga"));
}

TEST(FTextureCubeTests, ReimportsSixFacesTransactionally)
{
	const std::filesystem::path Root = InitializeCubeMount();
	auto Faces = GetConventionFaces();
	Durin::FTextureCubeImportResult Result = Durin::DTextureCube::ImportAsset(
		Faces, "/TextureCubeTests/ReimportFaces");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTextureCube* Texture = Result.Asset;
	const std::string InitialKey = Texture->GetDerivedDataKey();
	const Durin::uint64 InitialRevision = Texture->GetBuildRevision();
	const std::filesystem::path Transparent =
		Durin::Testing::GetTestWorkDirectory() / "ReimportFaceTransparent.tga";
	WriteSolidTga(Transparent, 128, 128, 128);
	constexpr std::array<std::string_view, Durin::TextureCubeFaceCount> Suffixes{
		"px", "nx", "py", "ny", "pz", "nz"};
	for (size_t FaceIndex = 0; FaceIndex < Faces.size(); ++FaceIndex)
		Faces[FaceIndex] = (Root / std::format(
			"SourceAssets/Textures/ReimportFaces_{}.png", Suffixes[FaceIndex])).generic_string();
	std::filesystem::copy_file(
		Transparent,
		Faces[static_cast<size_t>(Durin::ETextureCubeFace::NegativeZ)],
		std::filesystem::copy_options::overwrite_existing);
	std::string Error;
	ASSERT_TRUE(Texture->ReimportSources(Faces, {.bSRGB = true}, Error)) << Error;
	EXPECT_NE(Texture->GetDerivedDataKey(), InitialKey);
	EXPECT_GT(Texture->GetBuildRevision(), InitialRevision);
	EXPECT_EQ(Texture->GetBuiltPixelFormat(), Durin::EPixelFormat::BC3_UNORM_SRGB);
	EXPECT_EQ(Texture->GetSourceFile(Durin::ETextureCubeFace::NegativeZ),
		"/TextureCubeTests/Textures/ReimportFaces_nz.png");
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Root / "SourceAssets/Textures/ReimportFaces_nz.png"));

	const std::string ValidKey = Texture->GetDerivedDataKey();
	const Durin::uint64 ValidRevision = Texture->GetBuildRevision();
	const std::filesystem::path Corrupt =
		Durin::Testing::GetTestWorkDirectory() / "ReimportFaceCorrupt.png";
	{
		std::ofstream Stream(Corrupt, std::ios::binary | std::ios::trunc);
		Stream << "not an image";
	}
	std::filesystem::copy_file(
		Corrupt,
		Faces[static_cast<size_t>(Durin::ETextureCubeFace::PositiveY)],
		std::filesystem::copy_options::overwrite_existing);
	EXPECT_FALSE(Texture->ReimportSources(Faces, {.bSRGB = false}, Error));
	EXPECT_EQ(Texture->GetDerivedDataKey(), ValidKey);
	EXPECT_EQ(Texture->GetBuildRevision(), ValidRevision);
	EXPECT_TRUE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetBuiltPixelFormat(), Durin::EPixelFormat::BC3_UNORM_SRGB);
}

TEST(FTextureCubeTests, PostLoadIdentifiesTheMissingFaceAndInvalidatesDerivedData)
{
	const std::filesystem::path Root = InitializeCubeMount();
	const auto Faces = GetConventionFaces();
	Durin::FTextureCubeImportResult Result = Durin::DTextureCube::ImportAsset(
		Faces, "/TextureCubeTests/MissingAfterImport");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTextureCube* Texture = Result.Asset;
	ASSERT_TRUE(std::filesystem::remove(
		Root / "SourceAssets/Textures/MissingAfterImport_ny.png"));

	std::string Error;
	EXPECT_TRUE(Texture->PostLoad(Error)) << Error;
	EXPECT_TRUE(Texture->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_EQ(Texture->GetSourceData(), nullptr);
	EXPECT_NE(Texture->GetPlatformData(), nullptr);

	std::filesystem::copy_file(std::filesystem::path(Faces[static_cast<size_t>(Durin::ETextureCubeFace::NegativeY)]),
		Root / "SourceAssets/Textures/MissingAfterImport_ny.png");
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/MissingAfterImport", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTextureCubeTests, ImportsReloadsMovesAndDeletesPanoramaAsset)
{
	const std::filesystem::path Root = InitializeCubeMount();
	const std::filesystem::path Panorama = GetPanoramaFixture("AnalyticalLDR.tga");
	const Durin::FTextureCubeImportValidation Validation =
		Durin::DTextureCube::ValidatePanoramaImportSource(Panorama.generic_string());
	ASSERT_TRUE(Validation) << Validation.Message;
	EXPECT_EQ(Validation.SourceLayout, Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_EQ(Validation.SourceWidth, 8u);
	EXPECT_EQ(Validation.SourceHeight, 4u);
	EXPECT_EQ(Validation.Dimension, 2u);
	EXPECT_EQ(Validation.MipCount, 2u);
	EXPECT_FALSE(Validation.bHDR);

	Durin::FTextureCubeImportResult Result = Durin::DTextureCube::ImportPanoramaAsset(
		Panorama.generic_string(), "/TextureCubeTests/Panorama");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	EXPECT_EQ(Result.Asset->GetSourceLayout(), Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_EQ(Result.Asset->GetPanoramaSourceFile(),
		"/TextureCubeTests/Textures/Panorama_panorama.tga");
	EXPECT_EQ(Result.Asset->GetSourceFile(Durin::ETextureCubeFace::PositiveX), "");
	EXPECT_EQ(Result.Asset->GetOriginalSourceWidth(), 8u);
	EXPECT_EQ(Result.Asset->GetOriginalSourceHeight(), 4u);
	EXPECT_EQ(Result.Asset->GetBuiltFaceDimension(), 2u);
	EXPECT_EQ(Result.Asset->GetBuiltMipCount(), 2u);
	EXPECT_EQ(Result.Asset->GetBuiltPixelFormat(), Durin::EPixelFormat::BC1_UNORM_SRGB);
	ASSERT_TRUE(std::filesystem::is_regular_file(
		Root / "SourceAssets/Textures/Panorama_panorama.tga"));
	const Durin::FTextureCubePlatformData ExpectedPlatform = *Result.Asset->GetPlatformData();

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/Panorama", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	Durin::DTextureCube* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetSourceLayout(), Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_EQ(Loaded->GetPanoramaSourceFile(),
		"/TextureCubeTests/Textures/Panorama_panorama.tga");
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
		EXPECT_EQ(Loaded->GetPlatformData()->Faces[FaceIndex].Mips[0].Pixels,
			ExpectedPlatform.Faces[FaceIndex].Mips[0].Pixels);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::FAssetPath RenamedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/RenamedPanorama", RenamedPath));
	ASSERT_TRUE(Durin::Asset::MoveAsset(AssetPath, RenamedPath));
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Root / "SourceAssets/Textures/Panorama_panorama.tga"));
	EXPECT_FALSE(std::filesystem::exists(
		Root / "SourceAssets/Textures/RenamedPanorama_panorama.tga"));
	ASSERT_TRUE(Durin::Asset::LoadAsset(RenamedPath, Loaded));
	EXPECT_EQ(Loaded->GetPanoramaSourceFile(),
		"/TextureCubeTests/Textures/Panorama_panorama.tga");
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RenamedPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RenamedPath));
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Root / "SourceAssets/Textures/Panorama_panorama.tga"));
}

TEST(FTextureCubeTests, SourceLayoutReflectionRetainsSixFaceCompatibilityValue)
{
	InitializeDObjectSystem();
	EXPECT_EQ(static_cast<Durin::uint8>(Durin::ETextureCubeSourceLayout::SixFaces), 0u);
	EXPECT_EQ(static_cast<Durin::uint8>(
		Durin::ETextureCubeSourceLayout::EquirectangularPanorama), 1u);
	Durin::DEnum* SourceLayoutEnum =
		Durin::FindEnumByQualifiedName("Durin::ETextureCubeSourceLayout");
	ASSERT_NE(SourceLayoutEnum, nullptr);
	EXPECT_NE(SourceLayoutEnum->FindValueRecordByValue(0), nullptr);
	EXPECT_NE(SourceLayoutEnum->FindValueRecordByValue(1), nullptr);
	EXPECT_EQ(SourceLayoutEnum->FindValueRecordByValue(2), nullptr);
	auto* CompatibilityAsset = Durin::NewObject<Durin::DTextureCube>(
		nullptr, "PreSourceLayoutCompatibility");
	EXPECT_EQ(CompatibilityAsset->GetSourceLayout(), Durin::ETextureCubeSourceLayout::SixFaces);
}

TEST(FTextureCubeTests, RejectsInvalidPanoramaImportsWithoutArtifacts)
{
	const std::filesystem::path Root = InitializeCubeMount();
	const std::filesystem::path WrongAspect = Root / "WrongAspect.tga";
	WriteSolidTga(WrongAspect, 4, 4);
	Durin::FTextureCubeImportResult Result = Durin::DTextureCube::ImportPanoramaAsset(
		WrongAspect.generic_string(), "/TextureCubeTests/InvalidPanorama");
	EXPECT_FALSE(Result);
	EXPECT_NE(Result.Message.find("2:1"), std::string::npos);

	const std::filesystem::path Corrupt = Root / "CorruptPanorama.hdr";
	{
		std::ofstream Stream(Corrupt, std::ios::binary | std::ios::trunc);
		Stream << "not radiance";
	}
	Result = Durin::DTextureCube::ImportPanoramaAsset(
		Corrupt.generic_string(), "/TextureCubeTests/InvalidPanorama");
	EXPECT_FALSE(Result);
	EXPECT_NE(Result.Message.find("decode failed"), std::string::npos);
	Result = Durin::DTextureCube::ImportPanoramaAsset(
		GetPanoramaFixture("AnalyticalLDR.tga").generic_string(),
		"/TextureCubeTests/InvalidPanorama", {.FaceDimension = 4097});
	EXPECT_FALSE(Result);
	EXPECT_NE(Result.Message.find("4096"), std::string::npos);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/InvalidPanorama", AssetPath));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(AssetPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(AssetPath), nullptr);
	EXPECT_FALSE(std::filesystem::exists(Root / "InvalidPanorama_panorama.tga"));
	EXPECT_FALSE(std::filesystem::exists(Root / "InvalidPanorama_panorama.hdr"));
}

TEST(FTextureCubeTests, ReimportsPanoramaAtomicallyAndPreservesValidDataOnFailure)
{
	const std::filesystem::path Root = InitializeCubeMount();
	Durin::FTextureCubeImportResult Result = Durin::DTextureCube::ImportPanoramaAsset(
		GetPanoramaFixture("AnalyticalLDR.tga").generic_string(), "/TextureCubeTests/ReimportPanorama");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTextureCube* Texture = Result.Asset;
	const Durin::uint64 InitialRevision = Texture->GetBuildRevision();

	std::string Error;
	ASSERT_TRUE(Texture->IngestAndChangePanoramaSource(
		GetPanoramaFixture("AnalyticalHDR.hdr").generic_string(),
		"/TextureCubeTests/Textures/ReimportPanorama_panorama.hdr",
		{.FaceDimension = 4, .ExposureEV = 2.0f}, Error)) << Error;
	EXPECT_GT(Texture->GetBuildRevision(), InitialRevision);
	EXPECT_EQ(Texture->GetPanoramaSourceFile(),
		"/TextureCubeTests/Textures/ReimportPanorama_panorama.hdr");
	EXPECT_EQ(Texture->GetPanoramaFaceDimension(), 4u);
	EXPECT_FLOAT_EQ(Texture->GetPanoramaExposureEV(), 2.0f);
	EXPECT_EQ(Texture->GetBuiltFaceDimension(), 4u);
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Root / "SourceAssets/Textures/ReimportPanorama_panorama.tga"));
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Root / "SourceAssets/Textures/ReimportPanorama_panorama.hdr"));

	const Durin::uint64 FirstReimportRevision = Texture->GetBuildRevision();
	ASSERT_TRUE(Texture->ReimportPanorama(
		(Root / "SourceAssets/Textures/ReimportPanorama_panorama.hdr").generic_string(),
		{.FaceDimension = 4, .ExposureEV = 1.0f}, Error)) << Error;
	EXPECT_GT(Texture->GetBuildRevision(), FirstReimportRevision);
	EXPECT_FLOAT_EQ(Texture->GetPanoramaExposureEV(), 1.0f);

	const Durin::uint64 ValidRevision = Texture->GetBuildRevision();
	const std::vector<Durin::uint8> ValidPixels = Texture->GetSourceData()->Faces[0].Pixels;
	const std::filesystem::path Corrupt = Root / "CorruptReplacement.hdr";
	{
		std::ofstream Stream(Corrupt, std::ios::binary | std::ios::trunc);
		Stream << "not radiance";
	}
	EXPECT_FALSE(Texture->ReimportPanorama(Corrupt.generic_string(),
		{.FaceDimension = 8, .ExposureEV = -1.0f}, Error));
	EXPECT_NE(Error.find("read-only"), std::string::npos);
	EXPECT_EQ(Texture->GetBuildRevision(), ValidRevision);
	EXPECT_EQ(Texture->GetPanoramaSourceFile(),
		"/TextureCubeTests/Textures/ReimportPanorama_panorama.hdr");
	EXPECT_EQ(Texture->GetPanoramaFaceDimension(), 4u);
	EXPECT_FLOAT_EQ(Texture->GetPanoramaExposureEV(), 1.0f);
	EXPECT_EQ(Texture->GetSourceData()->Faces[0].Pixels, ValidPixels);
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/ReimportPanorama", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	Durin::DTextureCube* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_EQ(Loaded->GetPanoramaSourceFile(),
		"/TextureCubeTests/Textures/ReimportPanorama_panorama.hdr");
	EXPECT_EQ(Loaded->GetBuiltFaceDimension(), 4u);
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTextureCubeTests, PanoramaPostLoadReportsMissingAndCorruptAuthoritativeSource)
{
	const std::filesystem::path Root = InitializeCubeMount();
	Durin::FTextureCubeImportResult Result = Durin::DTextureCube::ImportPanoramaAsset(
		GetPanoramaFixture("AnalyticalHDR.hdr").generic_string(), "/TextureCubeTests/MissingPanorama");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTextureCube* Texture = Result.Asset;
	const std::filesystem::path CopiedSource =
		Root / "SourceAssets/Textures/MissingPanorama_panorama.hdr";
	ASSERT_TRUE(std::filesystem::remove(CopiedSource));

	std::string Error;
	EXPECT_TRUE(Texture->PostLoad(Error)) << Error;
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_EQ(Texture->GetSourceData(), nullptr);
	EXPECT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_TRUE(Texture->WasLoadedFromDerivedDataCache());

	std::filesystem::copy_file(GetPanoramaFixture("AnalyticalHDR.hdr"), CopiedSource);
	ASSERT_TRUE(Texture->PostLoad(Error)) << Error;
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	{
		std::ofstream Stream(CopiedSource, std::ios::binary | std::ios::trunc);
		Stream << "corrupt";
	}
	EXPECT_TRUE(Texture->PostLoad(Error)) << Error;
	EXPECT_TRUE(Texture->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);

	std::filesystem::copy_file(GetPanoramaFixture("AnalyticalHDR.hdr"), CopiedSource,
		std::filesystem::copy_options::overwrite_existing);
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/MissingPanorama", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTextureCubeTests, CookIsDeterministicAndRuntimeLoadsWithoutSources)
{
	const std::filesystem::path Root = InitializeCubeMount();
	const auto Faces = GetConventionFaces();
	const Durin::FTextureCubeImportResult Import = Durin::DTextureCube::ImportAsset(
		Faces, "/TextureCubeTests/CookedCube");
	ASSERT_TRUE(Import) << Import.Message;
	ASSERT_NE(Import.Asset, nullptr);
	const Durin::FTextureCubePlatformData Expected = *Import.Asset->GetPlatformData();
	const std::filesystem::path FirstRoot = std::filesystem::absolute(Root / "CookFirst");
	const std::filesystem::path SecondRoot = std::filesystem::absolute(Root / "CookSecond");
	std::string Error;
	Durin::Asset::FCookContext First(
		FirstRoot, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(Import.Asset->AddToCook(First, "/Game/CookedCube", Error)) << Error;
	ASSERT_TRUE(First.Publish(&Error)) << Error;
	Durin::Asset::FCookContext Second(
		SecondRoot, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(Import.Asset->AddToCook(Second, "/Game/CookedCube", Error)) << Error;
	ASSERT_TRUE(Second.Publish(&Error)) << Error;

	std::vector<Durin::uint8> FirstPackage;
	std::vector<Durin::uint8> SecondPackage;
	std::vector<Durin::uint8> FirstBulk;
	std::vector<Durin::uint8> SecondBulk;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstPackage, (FirstRoot / "Game/CookedCube.dasset").generic_string()));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondPackage, (SecondRoot / "Game/CookedCube.dasset").generic_string()));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstBulk, (FirstRoot / "Game/CookedCube.dbulk").generic_string()));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondBulk, (SecondRoot / "Game/CookedCube.dbulk").generic_string()));
	EXPECT_EQ(FirstPackage, SecondPackage);
	EXPECT_EQ(FirstBulk, SecondBulk);

	Durin::Asset::FCookedBulkContainer Container;
	ASSERT_TRUE(Durin::Asset::DecodeCookedBulk(
		FirstBulk, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Container, &Error)) << Error;
	ASSERT_EQ(Container.Entries.size(), 1u);
	EXPECT_EQ(Container.Entries[0].PayloadId, Durin::TextureCubePrimaryCookedPayloadId);
	std::unique_ptr<Durin::FTextureCubePlatformData> Decoded;
	const Durin::FPayloadDecodeResult DecodeResult = Durin::DecodeTextureCubePayload(
		Container.Payloads[0], Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Decoded);
	ASSERT_TRUE(DecodeResult) << DecodeResult.Message;
	ASSERT_NE(Decoded, nullptr);
	for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
		EXPECT_EQ(Decoded->Faces[FaceIndex].Mips[0].Pixels,
			Expected.Faces[FaceIndex].Mips[0].Pixels);

	for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
	{
		const Durin::PathUtilities::FSourcePathResult Resolved =
			Durin::PathUtilities::ResolveSourcePath(
				Import.Asset->GetSourceFile(static_cast<Durin::ETextureCubeFace>(FaceIndex)));
		ASSERT_TRUE(Resolved) << Resolved.Message;
		ASSERT_TRUE(std::filesystem::remove(Resolved.PhysicalPath));
	}
	Durin::FAssetPath AuthoredPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/CookedCube", AuthoredPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AuthoredPath));
	RestartAssetManager();
	ASSERT_TRUE(Durin::Asset::ConfigurePackageLoadContext({
		Durin::Asset::EPackageLoadMode::CookedRuntime, FirstRoot}));
	Durin::PathUtilities::RegisterMountPoint(
		"/Game/", (FirstRoot / "Game").generic_string() + "/");
	Durin::FAssetPath CookedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/CookedCube", CookedPath));
	Durin::DTextureCube* Cooked = nullptr;
	const Durin::Asset::FAssetResult Load = Durin::Asset::LoadAsset(CookedPath, Cooked);
	ASSERT_TRUE(Load) << Load.Message;
	ASSERT_NE(Cooked, nullptr);
	ASSERT_NE(Cooked->GetPlatformData(), nullptr);
	EXPECT_FALSE(Cooked->GetSourceImportData().HasSource());
	EXPECT_TRUE(Cooked->GetDerivedDataKey().empty());
	EXPECT_EQ(Cooked->GetDerivedDataDiagnostic().Status,
		Durin::ETextureDerivedDataStatus::CookedLoaded);
	EXPECT_EQ(Cooked->GetCookedPayloadDescriptor().PayloadId,
		Durin::TextureCubePrimaryCookedPayloadId);
	for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
		EXPECT_EQ(Cooked->GetPlatformData()->Faces[FaceIndex].Mips[0].Pixels,
			Expected.Faces[FaceIndex].Mips[0].Pixels);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(CookedPath));
	RestartAssetManager();
	ASSERT_TRUE(Durin::Asset::ConfigurePackageLoadContext({}));
}
