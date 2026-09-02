#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "EditorReimportHandler.h"
#include "Asset/SourceHint.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "AssetForge/Builtins/TextureCubeFactory.h"
#include "Texture/TextureCubeFactoryTestSupport.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "EngineTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "RenderingThread.h"
#include "Serialization/Archive.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeBuildProvider.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/TextureDerivedData.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"
#include <unordered_set>

TEST(FTextureCubeFactoryTests, ClassLookupDisambiguatesOverlappingExtensions)
{
	InitializeDObjectSystem();
	const std::vector<const Durin::DFactory*> CubeCandidates =
		Durin::DFactory::FindFactories(Durin::DTextureCube::StaticClass(), ".PNG");
	ASSERT_EQ(CubeCandidates.size(), 1u);
	EXPECT_TRUE(CubeCandidates.front()->IsA(
		Durin::AssetForge::Builtins::DTextureCubeFactory::StaticClass()));
	EXPECT_GT(Durin::DFactory::FindFactoriesByExtension("png").size(), 1u);
	EXPECT_EQ(Durin::DFactory::FindFactoryByExtension(".png"), nullptr);
}

namespace
{
	auto RelocateAssetForTest(
		const Durin::FPackagePath& Source,
		const Durin::FPackagePath& Destination) -> Durin::FAssetResult
	{
		const Durin::FAssetRelocationMapping Mapping{Source, Destination};
		Durin::FAssetRelocationSummary Summary;
		Durin::FAssetMutationJob Transaction;
		Durin::FAssetResult Result =
			Durin::PrepareAssetRelocationJob(
				std::span{&Mapping, 1}, Summary, Transaction);
		if (Result) Result = Transaction.ResumeForward();
		return Result;
	}

	constexpr std::array<std::string_view, Durin::TextureCubeFaceCount> FaceNames = {
		"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
	constexpr std::array<std::string_view, Durin::TextureCubeFaceCount> FaceRoles = {
		"positive-x", "negative-x", "positive-y", "negative-y", "positive-z", "negative-z"};

	auto GetSourceHint(const Durin::DTextureCube& Texture, std::string_view Role)
		-> std::string_view
	{
		const Durin::DAssetImportData* ImportData =
			Texture.GetAssetImportData();
		const Durin::FSourceFile* Source = ImportData
			? ImportData->GetSourceData().FindByRole(Role) : nullptr;
		return Source ? std::string_view(Source->Hint) : std::string_view{};
	}

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

	auto ResolveCubeSourceHint(const Durin::DTextureCube& Texture,
		std::string_view Filename, std::filesystem::path& OutPath,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "TextureCube has no owning package.";
			return false;
		}
		const auto Resolved = Durin::FMountPaths::ResolveAssetPath(
			Texture.GetPackage()->GetPackagePath(),
			Durin::EMountPathExistence::AllowMissing);
		if (!Resolved) { OutError = Resolved.Message; return false; }
		std::filesystem::path PackagePath = Resolved.PhysicalPath;
		PackagePath += ".dasset";
		const Durin::FSourceFile* Source = nullptr;
		if (const auto* ImportData = Texture.GetAssetImportData())
		{
			const auto It = std::ranges::find(
				ImportData->GetSourceData().Sources, Filename,
				&Durin::FSourceFile::Hint);
			if (It != ImportData->GetSourceData().Sources.end()) Source = &*It;
		}
		if (!Source)
		{
			OutError = "TextureCube source hint is not present in import data.";
			return false;
		}
		std::string PhysicalPath;
		if (!Durin::ResolveSourceHint(Source->HintBase, Filename,
			PackagePath.generic_string(), PhysicalPath, OutError)) return false;
		OutPath = std::filesystem::absolute(PhysicalPath).lexically_normal();
		return true;
	}

	void ExpectCubeSourcePath(const Durin::DTextureCube& Texture,
		std::string_view Filename, const std::filesystem::path& Expected)
	{
		std::filesystem::path Actual;
		std::string Error;
		ASSERT_TRUE(ResolveCubeSourceHint(Texture, Filename, Actual, Error)) << Error;
		EXPECT_EQ(Actual, std::filesystem::absolute(Expected).lexically_normal());
	}

	auto CopyConventionFaces(std::string_view Name)
		-> std::array<std::string, Durin::TextureCubeFaceCount>
	{
		const auto Fixtures = GetConventionFaces();
		std::array<std::string, Durin::TextureCubeFaceCount> Result;
		const std::filesystem::path Directory =
			Durin::Testing::GetTestWorkDirectory() / std::string(Name);
		std::filesystem::create_directories(Directory);
		for (size_t Index = 0; Index < Result.size(); ++Index)
		{
			const std::filesystem::path Destination = Directory /
				std::format("{}.png", FaceNames[Index]);
			std::filesystem::copy_file(Fixtures[Index], Destination,
				std::filesystem::copy_options::overwrite_existing);
			Result[Index] = Destination.generic_string();
		}
		return Result;
	}

	auto CopyPanorama(std::string_view Fixture, std::string_view Name)
		-> std::filesystem::path
	{
		const std::filesystem::path Destination =
			Durin::Testing::GetTestWorkDirectory() / std::string(Name);
		std::filesystem::copy_file(GetPanoramaFixture(Fixture), Destination,
			std::filesystem::copy_options::overwrite_existing);
		return Destination;
	}

	auto WriteSolidTga(const std::filesystem::path& Path, uint16 Width, uint16 Height,
		uint8 Alpha = 255) -> void
	{
		std::array<uint8, 18> Header{};
		Header[2] = 2;
		Header[12] = static_cast<uint8>(Width & 0xff);
		Header[13] = static_cast<uint8>(Width >> 8);
		Header[14] = static_cast<uint8>(Height & 0xff);
		Header[15] = static_cast<uint8>(Height >> 8);
		Header[16] = 32;
		Header[17] = 0x28;
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(Header.data()), Header.size());
		const std::array<uint8, 4> Pixel = {32, 64, 128, Alpha};
		for (uint32 PixelIndex = 0; PixelIndex < static_cast<uint32>(Width) * Height; ++PixelIndex)
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
			Durin::Testing::RegisterMountPointForTests("/TextureCubeTests/", Root.generic_string() + "/");
		}
		return Root;
	}

	auto RestartAssetManager(const std::filesystem::path& CookRoot = {}) -> void
	{
		Durin::ShutdownAssetManager();
		Durin::CollectGarbage();
		if (CookRoot.empty())
		{
			ASSERT_TRUE(Durin::InitializeAssetManager());
			return;
		}
		auto Configuration = Durin::FAssetRuntimeConfiguration::Authored();
		ASSERT_TRUE(Durin::FAssetRuntimeConfiguration::Cooked(
			CookRoot, Configuration));
		ASSERT_TRUE(Durin::InitializeAssetManager(std::move(Configuration)));
	}
}

TEST(FTextureCubeTests, ImportsReloadsMovesAndDeletesSixFaceAsset)
{
	const std::filesystem::path Root = InitializeCubeMount();
	const auto Faces = GetConventionFaces();
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Result = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
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
		ExpectCubeSourcePath(*Result.Asset, GetSourceHint(*Result.Asset, FaceRoles[FaceIndex]),
			Faces[FaceIndex]);
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

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureCubeTests/Convention", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	Durin::DTextureCube* Loaded = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_FALSE(Loaded->GetDerivedDataDiagnostic().bSourceDecoderInvoked);
	ExpectCubeSourcePath(*Loaded,
		GetSourceHint(*Loaded, FaceRoles[0]), Faces[0]);
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));

	Durin::FPackagePath RenamedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureCubeTests/RenamedCube", RenamedPath));
	ASSERT_TRUE(RelocateAssetForTest(AssetPath, RenamedPath));
	ASSERT_TRUE(Durin::LoadObject(
		Durin::Testing::MakeTopLevelAssetObjectPathForTests(
			RenamedPath, AssetPath.GetPackageName()), Loaded));
	ExpectCubeSourcePath(*Loaded,
		GetSourceHint(*Loaded, FaceRoles[5]), Faces[5]);
	ASSERT_TRUE(Durin::UnloadPackage(RenamedPath));
	ASSERT_TRUE(DeleteAssetClosureForTest({AssetPath, RenamedPath}));
	for (const std::string& Face : Faces)
		EXPECT_TRUE(std::filesystem::is_regular_file(Face));
}

TEST(FTextureCubeTests, RejectsMissingNonsquareAndMismatchedFacesWithoutArtifacts)
{
	const std::filesystem::path Root = InitializeCubeMount();
	auto Faces = GetConventionFaces();
	Faces[static_cast<size_t>(Durin::ETextureCubeFace::PositiveY)].clear();
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Missing = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
		Faces, "/TextureCubeTests/MissingFace");
	EXPECT_FALSE(Missing);
	EXPECT_NE(Missing.Message.find("PositiveY"), std::string::npos);

	const std::filesystem::path Nonsquare = Durin::Testing::GetTestWorkDirectory() / "CubeNonsquare.tga";
	WriteSolidTga(Nonsquare, 4, 2);
	Faces = GetConventionFaces();
	Faces[0] = Nonsquare.generic_string();
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> InvalidShape = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
		Faces, "/TextureCubeTests/Nonsquare");
	EXPECT_FALSE(InvalidShape);
	EXPECT_NE(InvalidShape.Message.find("invalid"), std::string::npos)
		<< InvalidShape.Message;

	const std::filesystem::path DifferentSize = Durin::Testing::GetTestWorkDirectory() / "CubeDifferentSize.tga";
	WriteSolidTga(DifferentSize, 4, 4);
	Faces = GetConventionFaces();
	Faces[0] = DifferentSize.generic_string();
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Mismatch = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
		Faces, "/TextureCubeTests/Mismatch");
	EXPECT_FALSE(Mismatch);
	EXPECT_NE(Mismatch.Message.find("invalid"), std::string::npos)
		<< Mismatch.Message;

	const std::filesystem::path Corrupt = Durin::Testing::GetTestWorkDirectory() / "CubeCorrupt.png";
	{
		std::ofstream Stream(Corrupt, std::ios::binary | std::ios::trunc);
		Stream << "not an image";
	}
	Faces = GetConventionFaces();
	Faces[static_cast<size_t>(Durin::ETextureCubeFace::NegativeZ)] = Corrupt.generic_string();
	const Durin::AssetForge::Builtins::FTextureCubeImportValidation CorruptValidation =
		Durin::AssetForge::Builtins::ValidateTextureCubeFaces(Faces);
	EXPECT_FALSE(CorruptValidation);
	EXPECT_NE(CorruptValidation.Message.find("NegativeZ"), std::string::npos);
	EXPECT_NE(CorruptValidation.Message.find("decode failed"), std::string::npos);

	for (std::string_view AssetName : {"MissingFace", "Nonsquare", "Mismatch"})
	{
		Durin::FPackagePath AssetPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(std::format("/TextureCubeTests/{}", AssetName), AssetPath));
		EXPECT_EQ(Durin::FindAssetExact(AssetPath), nullptr);
		EXPECT_EQ(Durin::FindResidentPackage(AssetPath), nullptr);
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

	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Result = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
		Faces, "/TextureCubeTests/Transparent");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset->GetPlatformData(), nullptr);
	EXPECT_EQ(Result.Asset->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	for (const Durin::FTexturePlatformData& Face : Result.Asset->GetPlatformData()->Faces)
		EXPECT_EQ(Face.PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureCubeTests/Transparent", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::DeleteAssetForTesting(AssetPath));
	EXPECT_FALSE(std::filesystem::exists(Root / "Transparent_nz.tga"));
}

TEST(FTextureCubeTests, ReimportsSixFacesTransactionally)
{
	const std::filesystem::path Root = InitializeCubeMount();
	auto Faces = GetConventionFaces();
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Result = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
		Faces, "/TextureCubeTests/ReimportFaces");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTextureCube* Texture = Result.Asset;
	const std::string InitialKey = Texture->GetDerivedDataKey();
	const uint64 InitialRevision = Texture->GetBuildRevision();
	const std::filesystem::path Transparent =
		Durin::Testing::GetTestWorkDirectory() / "ReimportFaceTransparent.tga";
	WriteSolidTga(Transparent, 128, 128, 128);
	Faces[static_cast<size_t>(Durin::ETextureCubeFace::NegativeZ)] =
		Transparent.generic_string();
	Durin::FReimportResult Reimported;
	Durin::FReimportManager::ReimportFromFiles(*Texture, Faces, {},
		[&](Durin::FReimportResult ResultValue) {
			Reimported = std::move(ResultValue);
		});
	ASSERT_TRUE(Reimported) << Reimported.Message;
	EXPECT_NE(Texture->GetDerivedDataKey(), InitialKey);
	EXPECT_GT(Texture->GetBuildRevision(), InitialRevision);
	EXPECT_EQ(Texture->GetBuiltPixelFormat(), Durin::EPixelFormat::BC3_UNORM_SRGB);
	ExpectCubeSourcePath(*Texture,
		GetSourceHint(*Texture, FaceRoles[5]), Transparent);
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Transparent));

	const std::string ValidKey = Texture->GetDerivedDataKey();
	const uint64 ValidRevision = Texture->GetBuildRevision();
	const std::filesystem::path Corrupt =
		Durin::Testing::GetTestWorkDirectory() / "ReimportFaceCorrupt.png";
	{
		std::ofstream Stream(Corrupt, std::ios::binary | std::ios::trunc);
		Stream << "not an image";
	}
	Faces[static_cast<size_t>(Durin::ETextureCubeFace::PositiveY)] =
		Corrupt.generic_string();
	Durin::FReimportManager::ReimportFromFiles(*Texture, Faces, {},
		[&](Durin::FReimportResult ResultValue) {
			Reimported = std::move(ResultValue);
		});
	EXPECT_EQ(Reimported.Status, Durin::EReimportStatus::SourceOrBuildFailure);
	EXPECT_EQ(Texture->GetDerivedDataKey(), ValidKey);
	EXPECT_EQ(Texture->GetBuildRevision(), ValidRevision);
	EXPECT_TRUE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetBuiltPixelFormat(), Durin::EPixelFormat::BC3_UNORM_SRGB);
}

TEST(FTextureCubeTests, PostLoadIdentifiesTheMissingFaceAndInvalidatesDerivedData)
{
	const std::filesystem::path Root = InitializeCubeMount();
	const auto Faces = CopyConventionFaces("MissingAfterImportSources");
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Result = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
		Faces, "/TextureCubeTests/MissingAfterImport");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTextureCube* Texture = Result.Asset;
	ASSERT_TRUE(std::filesystem::remove(
		Faces[static_cast<size_t>(Durin::ETextureCubeFace::NegativeY)]));

	std::string Error;
	EXPECT_TRUE(Texture->PostLoad(Error)) << Error;
	EXPECT_TRUE(Texture->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_EQ(Texture->GetSourceData(), nullptr);
	EXPECT_NE(Texture->GetPlatformData(), nullptr);

	std::filesystem::copy_file(GetConventionFaces()[static_cast<size_t>(
		Durin::ETextureCubeFace::NegativeY)],
		Faces[static_cast<size_t>(Durin::ETextureCubeFace::NegativeY)]);
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureCubeTests/MissingAfterImport", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::DeleteAssetForTesting(AssetPath));
}

TEST(FTextureCubeTests, ImportsReloadsMovesAndDeletesPanoramaAsset)
{
	const std::filesystem::path Root = InitializeCubeMount();
	const std::filesystem::path Panorama = GetPanoramaFixture("AnalyticalLDR.tga");
	const Durin::AssetForge::Builtins::FTextureCubeImportValidation Validation =
		Durin::AssetForge::Builtins::ValidateTextureCubePanorama(Panorama.generic_string());
	ASSERT_TRUE(Validation) << Validation.Message;
	EXPECT_EQ(Validation.SourceLayout, Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_EQ(Validation.SourceWidth, 8u);
	EXPECT_EQ(Validation.SourceHeight, 4u);
	EXPECT_EQ(Validation.Dimension, 2u);
	EXPECT_EQ(Validation.MipCount, 2u);
	EXPECT_FALSE(Validation.bHDR);

	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Result = Durin::AssetForge::Builtins::ImportTextureCubePanoramaForTest(
		Panorama.generic_string(), "/TextureCubeTests/Panorama");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	EXPECT_EQ(Result.Asset->GetSourceLayout(), Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	ExpectCubeSourcePath(*Result.Asset, GetSourceHint(*Result.Asset, "panorama"), Panorama);
	EXPECT_TRUE(GetSourceHint(*Result.Asset, FaceRoles[0]).empty());
	EXPECT_EQ(Result.Asset->GetOriginalSourceWidth(), 8u);
	EXPECT_EQ(Result.Asset->GetOriginalSourceHeight(), 4u);
	EXPECT_EQ(Result.Asset->GetBuiltFaceDimension(), 2u);
	EXPECT_EQ(Result.Asset->GetBuiltMipCount(), 2u);
	EXPECT_EQ(Result.Asset->GetBuiltPixelFormat(), Durin::EPixelFormat::BC1_UNORM_SRGB);
	ASSERT_TRUE(std::filesystem::is_regular_file(Panorama));
	const Durin::FTextureCubePlatformData ExpectedPlatform = *Result.Asset->GetPlatformData();

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureCubeTests/Panorama", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	Durin::DTextureCube* Loaded = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetSourceLayout(), Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	ExpectCubeSourcePath(*Loaded, GetSourceHint(*Loaded, "panorama"), Panorama);
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
		EXPECT_EQ(Loaded->GetPlatformData()->Faces[FaceIndex].Mips[0].Pixels,
			ExpectedPlatform.Faces[FaceIndex].Mips[0].Pixels);
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));

	Durin::FPackagePath RenamedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureCubeTests/RenamedPanorama", RenamedPath));
	ASSERT_TRUE(RelocateAssetForTest(AssetPath, RenamedPath));
	ASSERT_TRUE(Durin::LoadObject(
		Durin::Testing::MakeTopLevelAssetObjectPathForTests(
			RenamedPath, AssetPath.GetPackageName()), Loaded));
	ExpectCubeSourcePath(*Loaded, GetSourceHint(*Loaded, "panorama"), Panorama);
	ASSERT_TRUE(Durin::UnloadPackage(RenamedPath));
	ASSERT_TRUE(DeleteAssetClosureForTest({AssetPath, RenamedPath}));
	EXPECT_TRUE(std::filesystem::is_regular_file(Panorama));
}

TEST(FTextureCubeTests, PanoramaBuildRequiresCanonicalPixelsBeforeDdcLookup)
{
	InitializeCubeMount();
	Durin::FTextureCubePanoramaImage Panorama{
		.Pixels = Durin::FByteArray(4u * 2u * 4u, std::byte{127}),
		.Width = 4,
		.Height = 2,
		.SourceChannelCount = 4};
	const Durin::FTextureCubePanoramaBuildSettings Settings{
		.FaceDimension = 2};
	Durin::FTextureCubeCanonicalBuildInput InitialCanonical;
	Durin::FTextureCubeBuildProduct Initial;
	std::string Error;
	ASSERT_TRUE(Durin::InvokeTextureCubeBuildProvider({.Input = Durin::FTextureCubePanoramaBuildInput{
		.Image = Panorama, .Settings = Settings}}, InitialCanonical, Initial, Error)) << Error;
	ASSERT_NE(Initial.PlatformData, nullptr);

	Durin::FTextureCubeCanonicalBuildInput CachedCanonical;
	Durin::FTextureCubeBuildProduct Cached;
	ASSERT_TRUE(Durin::InvokeTextureCubeBuildProvider({.Input = Durin::FTextureCubePanoramaBuildInput{
		.Image = Panorama, .Settings = Settings}}, CachedCanonical, Cached, Error)) << Error;
	EXPECT_EQ(Cached.Origin, Durin::ETextureCubeBuildProductOrigin::CacheHit);
	EXPECT_TRUE(CachedCanonical.ImportedData.IsValid());
	ASSERT_NE(Cached.PlatformData, nullptr);
	EXPECT_TRUE(Cached.PlatformData->IsValid());

	Panorama.Pixels.clear();
	Durin::FTextureCubeCanonicalBuildInput InvalidCanonical;
	Durin::FTextureCubeBuildProduct Invalid;
	EXPECT_FALSE(Durin::InvokeTextureCubeBuildProvider({.Input = Durin::FTextureCubePanoramaBuildInput{
		.Image = std::move(Panorama), .Settings = Settings}},
		InvalidCanonical, Invalid, Error));
	EXPECT_NE(Error.find("pixel storage"), std::string::npos);
}

TEST(FTextureCubeTests, SourceLayoutReflectionRetainsSixFaceCompatibilityValue)
{
	InitializeDObjectSystem();
	EXPECT_EQ(static_cast<uint8>(Durin::ETextureCubeSourceLayout::SixFaces), 0u);
	EXPECT_EQ(static_cast<uint8>(
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
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Result = Durin::AssetForge::Builtins::ImportTextureCubePanoramaForTest(
		WrongAspect.generic_string(), "/TextureCubeTests/InvalidPanorama");
	EXPECT_FALSE(Result);
	EXPECT_NE(Result.Message.find("2:1"), std::string::npos);

	const std::filesystem::path Corrupt = Root / "CorruptPanorama.hdr";
	{
		std::ofstream Stream(Corrupt, std::ios::binary | std::ios::trunc);
		Stream << "not radiance";
	}
	Result = Durin::AssetForge::Builtins::ImportTextureCubePanoramaForTest(
		Corrupt.generic_string(), "/TextureCubeTests/InvalidPanorama");
	EXPECT_FALSE(Result);
	EXPECT_NE(Result.Message.find("decode failed"), std::string::npos);
	Result = Durin::AssetForge::Builtins::ImportTextureCubePanoramaForTest(
		GetPanoramaFixture("AnalyticalLDR.tga").generic_string(),
		"/TextureCubeTests/InvalidPanorama", {.FaceDimension = 4097});
	EXPECT_FALSE(Result);
	EXPECT_NE(Result.Message.find("4096"), std::string::npos);

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureCubeTests/InvalidPanorama", AssetPath));
	EXPECT_EQ(Durin::FindAssetExact(AssetPath), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(AssetPath), nullptr);
	EXPECT_FALSE(std::filesystem::exists(Root / "InvalidPanorama_panorama.tga"));
	EXPECT_FALSE(std::filesystem::exists(Root / "InvalidPanorama_panorama.hdr"));
}

TEST(FTextureCubeTests, ReimportsPanoramaAtomicallyAndPreservesValidDataOnFailure)
{
	const std::filesystem::path Root = InitializeCubeMount();
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Result = Durin::AssetForge::Builtins::ImportTextureCubePanoramaForTest(
		GetPanoramaFixture("AnalyticalLDR.tga").generic_string(), "/TextureCubeTests/ReimportPanorama");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTextureCube* Texture = Result.Asset;
	const uint64 InitialRevision = Texture->GetBuildRevision();

	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportTextureCubePanoramaFromFile(*Texture,
		GetPanoramaFixture("AnalyticalHDR.hdr").generic_string(),
		{.FaceDimension = 4, .ExposureEV = 2.0f}, Error)) << Error;
	EXPECT_GT(Texture->GetBuildRevision(), InitialRevision);
	ExpectCubeSourcePath(*Texture, GetSourceHint(*Texture, "panorama"),
		GetPanoramaFixture("AnalyticalHDR.hdr"));
	EXPECT_EQ(Texture->GetPanoramaFaceDimension(), 4u);
	EXPECT_FLOAT_EQ(Texture->GetPanoramaExposureEV(), 2.0f);
	EXPECT_EQ(Texture->GetBuiltFaceDimension(), 4u);
	EXPECT_TRUE(std::filesystem::is_regular_file(
		GetPanoramaFixture("AnalyticalHDR.hdr")));

	const uint64 FirstReimportRevision = Texture->GetBuildRevision();
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportTextureCubePanorama(*Texture,
		{.FaceDimension = 4, .ExposureEV = 1.0f}, Error)) << Error;
	EXPECT_GT(Texture->GetBuildRevision(), FirstReimportRevision);
	EXPECT_FLOAT_EQ(Texture->GetPanoramaExposureEV(), 1.0f);

	const uint64 ValidRevision = Texture->GetBuildRevision();
	const Durin::FByteArray ValidPixels =
		Texture->GetPlatformData()->Faces[0].Mips[0].Pixels;
	const std::filesystem::path Corrupt = Root / "CorruptReplacement.hdr";
	{
		std::ofstream Stream(Corrupt, std::ios::binary | std::ios::trunc);
		Stream << "not radiance";
	}
	EXPECT_FALSE(Durin::AssetForge::Builtins::ReimportTextureCubePanoramaFromFile(
		*Texture, Corrupt.generic_string(),
		{.FaceDimension = 8, .ExposureEV = -1.0f}, Error));
	EXPECT_NE(Error.find("decode"), std::string::npos);
	EXPECT_EQ(Texture->GetBuildRevision(), ValidRevision);
	ExpectCubeSourcePath(*Texture, GetSourceHint(*Texture, "panorama"),
		GetPanoramaFixture("AnalyticalHDR.hdr"));
	EXPECT_EQ(Texture->GetPanoramaFaceDimension(), 4u);
	EXPECT_FLOAT_EQ(Texture->GetPanoramaExposureEV(), 1.0f);
	EXPECT_EQ(Texture->GetPlatformData()->Faces[0].Mips[0].Pixels, ValidPixels);
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureCubeTests/ReimportPanorama", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	Durin::DTextureCube* Loaded = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded));
	ExpectCubeSourcePath(*Loaded, GetSourceHint(*Loaded, "panorama"),
		GetPanoramaFixture("AnalyticalHDR.hdr"));
	EXPECT_EQ(Loaded->GetBuiltFaceDimension(), 4u);
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::DeleteAssetForTesting(AssetPath));
}

TEST(FTextureCubeTests, PanoramaPostLoadReportsMissingAndCorruptAuthoritativeSource)
{
	const std::filesystem::path Root = InitializeCubeMount();
	const std::filesystem::path CopiedSource =
		CopyPanorama("AnalyticalHDR.hdr", "MissingPanorama.hdr");
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Result = Durin::AssetForge::Builtins::ImportTextureCubePanoramaForTest(
		CopiedSource.generic_string(), "/TextureCubeTests/MissingPanorama");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTextureCube* Texture = Result.Asset;
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
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureCubeTests/MissingPanorama", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::DeleteAssetForTesting(AssetPath));
}

TEST(FTextureCubeTests, CookIsDeterministicAndRuntimeLoadsWithoutSources)
{
	const std::filesystem::path Root = InitializeCubeMount();
	const auto Faces = CopyConventionFaces("CookedCubeSources");
	const Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Import = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
		Faces, "/TextureCubeTests/CookedCube");
	ASSERT_TRUE(Import) << Import.Message;
	ASSERT_NE(Import.Asset, nullptr);
	const Durin::FTextureCubePlatformData Expected = *Import.Asset->GetPlatformData();
	const std::filesystem::path FirstRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "TextureCubeCookFirst");
	const std::filesystem::path SecondRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "TextureCubeCookSecond");
	Durin::Testing::RemoveTestWorkDirectory(FirstRoot);
	Durin::Testing::RemoveTestWorkDirectory(SecondRoot);
	std::string Error;
	Durin::FCookContext First(
		FirstRoot, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Import.Asset, "/Game/CookedCube", First, Error)) << Error;
	ASSERT_TRUE(First.Publish(&Error)) << Error;
	Durin::FCookContext Second(
		SecondRoot, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Import.Asset, "/Game/CookedCube", Second, Error)) << Error;
	ASSERT_TRUE(Second.Publish(&Error)) << Error;

	Durin::FByteArray FirstPackage;
	Durin::FByteArray SecondPackage;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstPackage, (FirstRoot / "Game/CookedCube.dasset")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondPackage, (SecondRoot / "Game/CookedCube.dasset")));
	EXPECT_EQ(FirstPackage, SecondPackage);
	EXPECT_FALSE(std::filesystem::exists(FirstRoot / "Game/CookedCube.dbulk"));
	EXPECT_FALSE(std::filesystem::exists(SecondRoot / "Game/CookedCube.dbulk"));
	Durin::FAssetPackageInspection CookedInspection;
	Durin::FPackagePath CookedInspectionPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
		"/Game/CookedCube", CookedInspectionPath));
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(FirstRoot / "Game/CookedCube.dasset").generic_string(),
		CookedInspectionPath, CookedInspection));
	EXPECT_NE(CookedInspection.FindField("PlatformData"), nullptr);

	for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
	{
		std::filesystem::path PhysicalPath;
		ASSERT_TRUE(ResolveCubeSourceHint(*Import.Asset,
			GetSourceHint(*Import.Asset, FaceRoles[FaceIndex]),
			PhysicalPath, Error)) << Error;
		ASSERT_TRUE(std::filesystem::remove(PhysicalPath));
	}
	Durin::FPackagePath AuthoredPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureCubeTests/CookedCube", AuthoredPath));
	ASSERT_TRUE(Durin::UnloadPackage(AuthoredPath));
	Durin::Testing::RemoveTestWorkDirectory(SecondRoot);
	RestartAssetManager(FirstRoot);
	Durin::Testing::RegisterMountPointForTests(
		"/Game/", (FirstRoot / "Game").generic_string() + "/");
	const Durin::FAssetCatalogRefreshResult Refresh =
		Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(Refresh) << (Refresh.Errors.empty()
		? "asset catalog refresh failed without a diagnostic"
		: Refresh.Errors.front().Message);
	Durin::FPackagePath CookedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/CookedCube", CookedPath));
	Durin::DTextureCube* Cooked = nullptr;
	const Durin::FAssetResult Load = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(CookedPath), Cooked);
	ASSERT_TRUE(Load) << Load.Message;
	ASSERT_NE(Cooked, nullptr);
	ASSERT_NE(Cooked->GetPlatformData(), nullptr);
	EXPECT_EQ(Cooked->GetAssetImportData(), nullptr);
	EXPECT_TRUE(Cooked->GetDerivedDataKey().empty());
	EXPECT_EQ(Cooked->GetDerivedDataDiagnostic().Status,
		Durin::ETextureDerivedDataStatus::CookedLoaded);
	EXPECT_NE(Cooked->GetCookedPlatformData().GetMetadata().LogicalSize, 0u);
	for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
		EXPECT_EQ(Cooked->GetPlatformData()->Faces[FaceIndex].Mips[0].Pixels,
			Expected.Faces[FaceIndex].Mips[0].Pixels);
	ASSERT_TRUE(Durin::UnloadPackage(CookedPath));
	RestartAssetManager();
}
