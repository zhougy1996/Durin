#include "AssetSystem.h"
#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeRenderResource.h"

#include <gtest/gtest.h>

namespace
{
	constexpr std::array<std::string_view, Durin::TextureCubeFaceCount> FaceNames = {
		"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};

	struct FObserveCubeReleaseRevisionCommand
	{
		static constexpr auto GetName() -> const char* { return "ObserveCubeReleaseRevision"; }
	};

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
		static const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureCubeImports";
		static const bool bInitialized = [] {
			std::filesystem::remove_all(Root);
			Durin::PathUtilities::RegisterMountPoint("/TextureCubeTests/", Root.generic_string() + "/");
			return true;
		}();
		(void)bInitialized;
		return Root;
	}
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
		EXPECT_EQ(Result.Asset->GetSourceFile(Face), std::format("Convention_{}.png",
			std::array<std::string_view, Durin::TextureCubeFaceCount>{"px", "nx", "py", "ny", "pz", "nz"}[FaceIndex]));
		EXPECT_TRUE(std::filesystem::is_regular_file(Root / Result.Asset->GetSourceFile(Face)));
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
	EXPECT_TRUE(Loaded->GetSourceData()->IsValid());
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	EXPECT_EQ(Loaded->GetSourceFile(Durin::ETextureCubeFace::PositiveX), "Convention_px.png");
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::FAssetPath RenamedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/RenamedCube", RenamedPath));
	ASSERT_TRUE(Durin::Asset::MoveAsset(AssetPath, RenamedPath));
	for (std::string_view Suffix : {"px", "nx", "py", "ny", "pz", "nz"})
	{
		EXPECT_FALSE(std::filesystem::exists(Root / std::format("Convention_{}.png", Suffix)));
		EXPECT_TRUE(std::filesystem::is_regular_file(Root / std::format("RenamedCube_{}.png", Suffix)));
	}
	ASSERT_TRUE(Durin::Asset::LoadAsset(RenamedPath, Loaded));
	EXPECT_EQ(Loaded->GetSourceFile(Durin::ETextureCubeFace::NegativeZ), "RenamedCube_nz.png");
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RenamedPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RenamedPath));
	for (std::string_view Suffix : {"px", "nx", "py", "ny", "pz", "nz"})
		EXPECT_FALSE(std::filesystem::exists(Root / std::format("RenamedCube_{}.png", Suffix)));
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

	const std::filesystem::path Nonsquare = std::filesystem::path(DURIN_TEST_WORK_DIR) / "CubeNonsquare.tga";
	WriteSolidTga(Nonsquare, 4, 2);
	Faces = GetConventionFaces();
	Faces[0] = Nonsquare.generic_string();
	Durin::FTextureCubeImportResult InvalidShape = Durin::DTextureCube::ImportAsset(
		Faces, "/TextureCubeTests/Nonsquare");
	EXPECT_FALSE(InvalidShape);
	EXPECT_NE(InvalidShape.Message.find("square"), std::string::npos);

	const std::filesystem::path DifferentSize = std::filesystem::path(DURIN_TEST_WORK_DIR) / "CubeDifferentSize.tga";
	WriteSolidTga(DifferentSize, 4, 4);
	Faces = GetConventionFaces();
	Faces[0] = DifferentSize.generic_string();
	Durin::FTextureCubeImportResult Mismatch = Durin::DTextureCube::ImportAsset(
		Faces, "/TextureCubeTests/Mismatch");
	EXPECT_FALSE(Mismatch);
	EXPECT_NE(Mismatch.Message.find("identical"), std::string::npos);

	const std::filesystem::path Corrupt = std::filesystem::path(DURIN_TEST_WORK_DIR) / "CubeCorrupt.png";
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
	const std::filesystem::path TransparentFace = std::filesystem::path(DURIN_TEST_WORK_DIR) / "CubeTransparent.tga";
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

TEST(FTextureCubeTests, PostLoadIdentifiesTheMissingFaceAndInvalidatesDerivedData)
{
	const std::filesystem::path Root = InitializeCubeMount();
	const auto Faces = GetConventionFaces();
	Durin::FTextureCubeImportResult Result = Durin::DTextureCube::ImportAsset(
		Faces, "/TextureCubeTests/MissingAfterImport");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTextureCube* Texture = Result.Asset;
	ASSERT_TRUE(std::filesystem::remove(Root / "MissingAfterImport_ny.png"));

	std::string Error;
	EXPECT_FALSE(Texture->PostLoad(Error));
	EXPECT_NE(Error.find("NegativeY"), std::string::npos);
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::MissingSource);
	EXPECT_EQ(Texture->GetSourceData(), nullptr);
	EXPECT_EQ(Texture->GetPlatformData(), nullptr);

	std::filesystem::copy_file(std::filesystem::path(Faces[static_cast<size_t>(Durin::ETextureCubeFace::NegativeY)]),
		Root / "MissingAfterImport_ny.png");
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
	EXPECT_EQ(Result.Asset->GetPanoramaSourceFile(), "Panorama_panorama.tga");
	EXPECT_EQ(Result.Asset->GetSourceFile(Durin::ETextureCubeFace::PositiveX), "");
	EXPECT_EQ(Result.Asset->GetOriginalSourceWidth(), 8u);
	EXPECT_EQ(Result.Asset->GetOriginalSourceHeight(), 4u);
	EXPECT_EQ(Result.Asset->GetBuiltFaceDimension(), 2u);
	EXPECT_EQ(Result.Asset->GetBuiltMipCount(), 2u);
	EXPECT_EQ(Result.Asset->GetBuiltPixelFormat(), Durin::EPixelFormat::BC1_UNORM_SRGB);
	ASSERT_TRUE(std::filesystem::is_regular_file(Root / "Panorama_panorama.tga"));
	const Durin::FTextureCubeSourceData ExpectedSource = *Result.Asset->GetSourceData();

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/Panorama", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	Durin::DTextureCube* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetSourceLayout(), Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_EQ(Loaded->GetPanoramaSourceFile(), "Panorama_panorama.tga");
	for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
		EXPECT_EQ(Loaded->GetSourceData()->Faces[FaceIndex].Pixels, ExpectedSource.Faces[FaceIndex].Pixels);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::FAssetPath RenamedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/RenamedPanorama", RenamedPath));
	ASSERT_TRUE(Durin::Asset::MoveAsset(AssetPath, RenamedPath));
	EXPECT_FALSE(std::filesystem::exists(Root / "Panorama_panorama.tga"));
	EXPECT_TRUE(std::filesystem::is_regular_file(Root / "RenamedPanorama_panorama.tga"));
	ASSERT_TRUE(Durin::Asset::LoadAsset(RenamedPath, Loaded));
	EXPECT_EQ(Loaded->GetPanoramaSourceFile(), "RenamedPanorama_panorama.tga");
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RenamedPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RenamedPath));
	EXPECT_FALSE(std::filesystem::exists(Root / "RenamedPanorama_panorama.tga"));
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
	ASSERT_TRUE(Texture->ReimportPanorama(
		GetPanoramaFixture("AnalyticalHDR.hdr").generic_string(),
		{.FaceDimension = 4, .ExposureEV = 2.0f}, Error)) << Error;
	EXPECT_GT(Texture->GetBuildRevision(), InitialRevision);
	EXPECT_EQ(Texture->GetPanoramaSourceFile(), "ReimportPanorama_panorama.hdr");
	EXPECT_EQ(Texture->GetPanoramaFaceDimension(), 4u);
	EXPECT_FLOAT_EQ(Texture->GetPanoramaExposureEV(), 2.0f);
	EXPECT_EQ(Texture->GetBuiltFaceDimension(), 4u);
	EXPECT_FALSE(std::filesystem::exists(Root / "ReimportPanorama_panorama.tga"));
	EXPECT_TRUE(std::filesystem::is_regular_file(Root / "ReimportPanorama_panorama.hdr"));

	const Durin::uint64 FirstReimportRevision = Texture->GetBuildRevision();
	ASSERT_TRUE(Texture->ReimportPanorama(
		GetPanoramaFixture("AnalyticalHDR.hdr").generic_string(),
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
	EXPECT_NE(Error.find("decode failed"), std::string::npos);
	EXPECT_EQ(Texture->GetBuildRevision(), ValidRevision);
	EXPECT_EQ(Texture->GetPanoramaSourceFile(), "ReimportPanorama_panorama.hdr");
	EXPECT_EQ(Texture->GetPanoramaFaceDimension(), 4u);
	EXPECT_FLOAT_EQ(Texture->GetPanoramaExposureEV(), 1.0f);
	EXPECT_EQ(Texture->GetSourceData()->Faces[0].Pixels, ValidPixels);
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/ReimportPanorama", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	Durin::DTextureCube* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_EQ(Loaded->GetPanoramaSourceFile(), "ReimportPanorama_panorama.hdr");
	EXPECT_EQ(Loaded->GetBuiltFaceDimension(), 4u);
	EXPECT_EQ(Loaded->GetSourceData()->Faces[0].Pixels, ValidPixels);
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
	const std::filesystem::path CopiedSource = Root / "MissingPanorama_panorama.hdr";
	ASSERT_TRUE(std::filesystem::remove(CopiedSource));

	std::string Error;
	EXPECT_FALSE(Texture->PostLoad(Error));
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::MissingSource);
	EXPECT_EQ(Texture->GetSourceData(), nullptr);
	EXPECT_EQ(Texture->GetPlatformData(), nullptr);

	std::filesystem::copy_file(GetPanoramaFixture("AnalyticalHDR.hdr"), CopiedSource);
	ASSERT_TRUE(Texture->PostLoad(Error)) << Error;
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	{
		std::ofstream Stream(CopiedSource, std::ios::binary | std::ios::trunc);
		Stream << "corrupt";
	}
	EXPECT_FALSE(Texture->PostLoad(Error));
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::DecodeFailure);
	EXPECT_NE(Texture->GetLastBuildError().find("decode failed"), std::string::npos);

	std::filesystem::copy_file(GetPanoramaFixture("AnalyticalHDR.hdr"), CopiedSource,
		std::filesystem::copy_options::overwrite_existing);
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureCubeTests/MissingPanorama", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTextureCubeTests, RenderResourceRejectsStaleReleaseRevisions)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	auto Resource = std::make_shared<Durin::FTextureCubeRenderResource>();
	Resource->QueueRelease(2);
	Resource->QueueRelease(3);

	auto ObservedRevision = std::make_shared<std::atomic<Durin::uint64>>(0);
	Durin::EnqueueRenderCommand<FObserveCubeReleaseRevisionCommand>(
		[Resource, ObservedRevision](Durin::FRHICommandListImmediate&) {
			ObservedRevision->store(Resource->GetAppliedRevision_RenderThread(), std::memory_order_release);
		});
	Durin::FlushRenderingCommands();
	EXPECT_EQ(ObservedRevision->load(std::memory_order_acquire), 3u);
	EXPECT_EQ(Resource->GetResourceState(), Durin::ERenderResourceState::Released);

	// Let the final shared owner drain on the rendering thread, matching asset destruction.
	Resource->QueueRelease(4);
	Resource.reset();
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
}
