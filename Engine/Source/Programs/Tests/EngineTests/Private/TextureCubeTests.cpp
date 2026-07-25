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
