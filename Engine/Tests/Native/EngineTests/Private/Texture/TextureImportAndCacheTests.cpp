#include "TextureTestSupport.h"
#include "SourceFingerprintCache.h"

namespace
{
	auto RelocateAssetForTest(
		const Durin::FAssetPath& Source,
		const Durin::FAssetPath& Destination) -> Durin::Asset::FAssetResult
	{
		const Durin::Asset::FAssetRelocationMapping Mapping{Source, Destination};
		Durin::Asset::FAssetRelocationBatchToken Token;
		Durin::Asset::FAssetResult Result =
			Durin::Asset::AnalyzeAssetRelocationBatch(std::span{&Mapping, 1}, Token);
		if (Result) Result = Durin::Asset::ApplyAssetRelocationBatch(Token);
		return Result;
	}
}

TEST(FTexture2DTests, ImportsSourceAndBuildsIndependentPlatformData)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureImportDerivedDataCache");

	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "TextureSource.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), "/TextureImportTests/Transparent");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	const Durin::FTextureSourceData* SourceData = Result.Asset->GetSourceData();
	const Durin::FTexturePlatformData* PlatformData = Result.Asset->GetPlatformData();
	ASSERT_NE(SourceData, nullptr);
	ASSERT_NE(PlatformData, nullptr);
	EXPECT_NE(Result.Asset->GetTextureReferenceRHI(), nullptr);
	EXPECT_EQ(Result.Asset->GetBuildRevision(), 1u);
	EXPECT_TRUE(SourceData->IsValid());
	EXPECT_TRUE(SourceData->bHasTransparency);
	EXPECT_EQ(SourceData->Width, 2u);
	EXPECT_EQ(SourceData->Height, 1u);
	ASSERT_TRUE(PlatformData->IsValid());
	EXPECT_TRUE(Result.Asset->IsSRGB());
	EXPECT_EQ(Result.Asset->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_EQ(PlatformData->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	ASSERT_EQ(PlatformData->Mips.size(), 2u);
	EXPECT_EQ(PlatformData->Mips[0].RowPitch, 16u);
	EXPECT_EQ(PlatformData->Mips[0].Pixels.size(), 16u);
	EXPECT_EQ(PlatformData->Mips[1].Width, 1u);
	EXPECT_EQ(PlatformData->Mips[1].Height, 1u);
	EXPECT_EQ(PlatformData->Mips[1].RowPitch, 16u);
	EXPECT_EQ(PlatformData->Mips[1].Pixels.size(), 16u);
	ExpectPixelNear(DecodeFirstCompressedPixel(PlatformData->PixelFormat, PlatformData->Mips[1].Pixels),
		{188, 0, 0, 128});

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Transparent", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Loaded->GetSourceWidth(), 2u);
	EXPECT_EQ(Loaded->GetSourceHeight(), 1u);
	EXPECT_EQ(Loaded->GetSourceChannelCount(), 4u);
	EXPECT_TRUE(Loaded->SourceHasTransparency());
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	EXPECT_EQ(Loaded->GetBuildRevision(), 1u);
	EXPECT_EQ(Loaded->GetSourceFile(), "/TextureImportTests/Textures/Transparent.png");
	EXPECT_EQ(Loaded->GetSourceImportData().DecoderId, "DurinImage");
	EXPECT_EQ(Loaded->GetSourceImportData().DecoderVersion, 1u);
	EXPECT_TRUE(Loaded->GetSourceImportData().Source.HasContentHash());
	EXPECT_TRUE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::FAssetPath RenamedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Renamed", RenamedPath));
	ASSERT_TRUE(RelocateAssetForTest(AssetPath, RenamedPath));
	const std::filesystem::path ImportRoot = Durin::Testing::GetTestWorkDirectory() / "TextureImports";
	const std::filesystem::path StoredSource =
		ImportRoot / "Content" / "Textures" / "Transparent.png";
	EXPECT_TRUE(std::filesystem::is_regular_file(StoredSource));
	EXPECT_FALSE(std::filesystem::exists(ImportRoot / "Content" / "Transparent.png"));
	ASSERT_TRUE(Durin::Asset::LoadAsset(RenamedPath, Loaded));
	EXPECT_EQ(Loaded->GetSourceFile(), "/TextureImportTests/Textures/Transparent.png");
	EXPECT_EQ(Loaded->InspectSource().Status, Durin::ETextureSourceStatus::Available);
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RenamedPath));
	Durin::Asset::FAssetDeleteAnalysis DeleteAnalysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(RenamedPath, DeleteAnalysis));
	EXPECT_TRUE(DeleteAnalysis.CompanionFiles.empty());
	ASSERT_TRUE(DeleteAssetClosureForTest({AssetPath, RenamedPath}));
	EXPECT_TRUE(std::filesystem::is_regular_file(StoredSource));
}

TEST(FTexture2DTests, DefaultsToFlatSourceRootAndAllowsCustomSourceDestination)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceDestinationCache");

	const std::filesystem::path DefaultInput =
		Durin::Testing::GetTestWorkDirectory() / "FlatDefault.png";
	WriteTextureFixture(DefaultInput);
	Durin::FTexture2DImportResult DefaultResult = Durin::DTexture2D::ImportAsset(
		DefaultInput.generic_string(), "/TextureImportTests/Textures/FlatDefault");
	ASSERT_TRUE(DefaultResult) << DefaultResult.Message;
	ASSERT_NE(DefaultResult.Asset, nullptr);
	EXPECT_EQ(
		DefaultResult.Asset->GetSourceFile(),
		"/TextureImportTests/Textures/FlatDefault.png");
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Durin::Testing::GetTestWorkDirectory() / "TextureImports"
		/ "Content" / "Textures" / "FlatDefault.png"));
	EXPECT_FALSE(std::filesystem::exists(
		Durin::Testing::GetTestWorkDirectory() / "TextureImports"
		/ "Content" / "Textures" / "Textures" / "FlatDefault.png"));

	Durin::FAssetPath DefaultAssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TextureImportTests/Textures/FlatDefault", DefaultAssetPath));
	const Durin::Asset::FAssetData* DefaultAssetData =
		Durin::Asset::GetAssetRegistry().FindAssetExact(DefaultAssetPath);
	ASSERT_NE(DefaultAssetData, nullptr);
	Durin::Asset::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		DefaultAssetData->PhysicalPath, Inspection));
	const Durin::Asset::FAssetPackageField* SourceField =
		Inspection.FindField("SourceImportData");
	ASSERT_NE(SourceField, nullptr);
	Durin::FTexture2DSourceImportData InspectedSource;
	ASSERT_TRUE(SourceField->TryReadStruct(
		Durin::FTexture2DSourceImportData::StaticStruct(), &InspectedSource));
	EXPECT_EQ(
		InspectedSource.Source.SourcePath.Path,
		"/TextureImportTests/Textures/FlatDefault.png");

	const std::filesystem::path CustomInput =
		Durin::Testing::GetTestWorkDirectory() / "CustomInput.png";
	WriteTextureFixture(CustomInput);
	Durin::FTexture2DImportSettings CustomSettings;
	CustomSettings.SourceDestination =
		"ArtistAuthored/CustomCopy.png";
	Durin::FTexture2DImportResult CustomResult = Durin::DTexture2D::ImportAsset(
		CustomInput.generic_string(), "/TextureImportTests/UI/CustomAsset",
		CustomSettings);
	ASSERT_TRUE(CustomResult) << CustomResult.Message;
	ASSERT_NE(CustomResult.Asset, nullptr);
	EXPECT_EQ(
		CustomResult.Asset->GetSourceFile(),
		"/TextureImportTests/ArtistAuthored/CustomCopy.png");
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Durin::Testing::GetTestWorkDirectory() / "TextureImports"
		/ "Content" / "ArtistAuthored" / "CustomCopy.png"));

	Durin::FTexture2DImportResult SharedResult = Durin::DTexture2D::ImportAsset(
		CustomInput.generic_string(), "/TextureImportTests/UI/SharedAsset",
		CustomSettings);
	ASSERT_TRUE(SharedResult) << SharedResult.Message;
	ASSERT_NE(SharedResult.Asset, nullptr);
	std::string RelocateError;
	ASSERT_TRUE(CustomResult.Asset->ChangeSourceLocation(
		"UserLayout/MovedCopy.png", RelocateError)) << RelocateError;
	EXPECT_EQ(
		CustomResult.Asset->GetSourceFile(),
		"/TextureImportTests/UserLayout/MovedCopy.png");
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Durin::Testing::GetTestWorkDirectory() / "TextureImports"
		/ "Content" / "UserLayout" / "MovedCopy.png"));
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Durin::Testing::GetTestWorkDirectory() / "TextureImports"
		/ "Content" / "ArtistAuthored" / "CustomCopy.png"));
	EXPECT_EQ(
		SharedResult.Asset->GetSourceFile(),
		"/TextureImportTests/ArtistAuthored/CustomCopy.png");
	EXPECT_EQ(
		SharedResult.Asset->InspectSource().Status,
		Durin::ETextureSourceStatus::Available);

	Durin::FTexture2DImportSettings InvalidSettings;
	InvalidSettings.SourceDestination = "../Invalid.png";
	EXPECT_FALSE(Durin::DTexture2D::ImportAsset(
		CustomInput.generic_string(), "/TextureImportTests/InvalidRoot",
		InvalidSettings));
	InvalidSettings.SourceDestination =
		"Textures/InvalidExtension.jpg";
	EXPECT_FALSE(Durin::DTexture2D::ImportAsset(
		CustomInput.generic_string(), "/TextureImportTests/InvalidExtension",
		InvalidSettings));

	Durin::FAssetPath CustomAssetPath;
	Durin::FAssetPath SharedAssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TextureImportTests/UI/CustomAsset", CustomAssetPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TextureImportTests/UI/SharedAsset", SharedAssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(DefaultAssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(CustomAssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SharedAssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(DefaultAssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(CustomAssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(SharedAssetPath));
}

TEST(FTexture2DTests, VersionedDerivedDataCacheHitsAndRecoversCorruptPayload)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "TextureDerivedDataMount";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPointForTests(
		"/TextureDerivedDataTests/", Root.generic_string() + "/");
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureDerivedDataCache");

	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "DerivedDataSource.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(
		Source.generic_string(), "/TextureDerivedDataTests/Cached");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	EXPECT_FALSE(Result.Asset->GetSourceContentHash().empty());
	EXPECT_EQ(Result.Asset->GetSourceContentHash().size(), 32u);
	EXPECT_FALSE(Result.Asset->GetDerivedDataKey().empty());
	EXPECT_FALSE(Result.Asset->WasLoadedFromDerivedDataCache());
	EXPECT_TRUE(Result.Asset->GetDerivedDataDiagnostic().bSourceDecoderInvoked);
	const std::filesystem::path CachePath = GetTextureCachePath(*Result.Asset);
	const std::string OriginalKey = Result.Asset->GetDerivedDataKey();
	EXPECT_TRUE(std::filesystem::is_regular_file(CachePath));
	const Durin::FTexturePlatformData ExpectedPlatformData = *Result.Asset->GetPlatformData();

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureDerivedDataTests/Cached", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_FALSE(Loaded->GetDerivedDataDiagnostic().bSourceDecoderInvoked);
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*Loaded->GetPlatformData(), ExpectedPlatformData);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	{
		const std::array<Durin::uint8, 7> CorruptBytes = {0, 1, 2, 3, 4, 5, 6};
		std::ofstream Stream(CachePath, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(CorruptBytes.data()), CorruptBytes.size());
	}
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_FALSE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_TRUE(Loaded->GetDerivedDataDiagnostic().bSourceDecoderInvoked);
	ASSERT_NE(Loaded->GetSourceData(), nullptr);
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*Loaded->GetPlatformData(), ExpectedPlatformData);
	EXPECT_GT(std::filesystem::file_size(CachePath), 7u);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	const std::filesystem::path CopiedSource =
		Durin::Testing::GetTestWorkDirectory() / "TextureDerivedDataMount"
		/ "Textures" / "Cached.png";
	WriteNpotTextureFixture(CopiedSource);
	std::filesystem::last_write_time(CopiedSource,
		std::filesystem::last_write_time(CopiedSource) + std::chrono::seconds(1));
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_FALSE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_NE(Loaded->GetDerivedDataKey(), OriginalKey);
	EXPECT_EQ(Loaded->GetSourceWidth(), 5u);
	EXPECT_EQ(Loaded->GetSourceHeight(), 3u);
	EXPECT_TRUE(Loaded->GetPackage()->IsDirty());
	EXPECT_TRUE(std::filesystem::is_regular_file(GetTextureCachePath(*Loaded)));
	ASSERT_TRUE(Durin::Asset::SavePackage(Loaded->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTexture2DTests, TimestampOnlySourceChangeUsesPersistentFingerprintCacheWithoutDirtying)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceFingerprintCache");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceFingerprint.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(
		Source.generic_string(), "/TextureImportTests/Fingerprint");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Fingerprint", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	const std::filesystem::path StoredSource =
		Durin::Testing::GetTestWorkDirectory() / "TextureImports"
		/ "Content" / "Textures" / "Fingerprint.png";
	ASSERT_TRUE(std::filesystem::is_regular_file(StoredSource));
	std::filesystem::last_write_time(
		StoredSource,
		std::filesystem::last_write_time(StoredSource) + std::chrono::seconds(1));

	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_FALSE(Loaded->GetDerivedDataDiagnostic().bSourceDecoderInvoked);
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	EXPECT_TRUE(std::filesystem::is_regular_file(
		std::filesystem::path(Durin::FPaths::DerivedDataCacheDir())
		/ "SourceFingerprints" / "Index.bin"));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	const std::string FingerprintCacheRoot = Durin::FPaths::DerivedDataCacheDir();
	Durin::FPaths::SetDerivedDataCacheDirForTests(
		(Durin::Testing::GetTestWorkDirectory() / "UnusedSourceFingerprintCache").generic_string());
	std::string IgnoredHash;
	EXPECT_FALSE(Durin::Asset::FindSourceFingerprint(StoredSource, 0, 0, IgnoredHash));
	Durin::FPaths::SetDerivedDataCacheDirForTests(FingerprintCacheRoot);

	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_FALSE(Loaded->GetDerivedDataDiagnostic().bSourceDecoderInvoked);
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTexture2DTests, PortableSourceCanBeRepairedAndRejectsEscapingMetadata)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceRepairCache");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceRepair.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(
		Source.generic_string(), "/TextureImportTests/Repair/Texture");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	const Durin::FTexturePlatformData OriginalPlatformData = *Result.Asset->GetPlatformData();

	auto* SourceImportProperty = Result.Asset->GetClass()->FindPropertyByName("SourceImportData");
	ASSERT_NE(SourceImportProperty, nullptr);
	auto* SourceImportData = static_cast<Durin::FTexture2DSourceImportData*>(
		SourceImportProperty->GetValuePtr(Result.Asset));
	SourceImportData->Source.SourcePath.Path = "../Outside.png";
	EXPECT_EQ(Result.Asset->InspectSource().Status, Durin::ETextureSourceStatus::Invalid);

	const std::filesystem::path Corrupt =
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceRepairCorrupt.png";
	{
		std::ofstream Stream(Corrupt, std::ios::binary | std::ios::trunc);
		Stream << "not an image";
	}
	std::string Error;
	EXPECT_FALSE(Result.Asset->ReimportSource(Corrupt.generic_string(), Error));
	ExpectPlatformDataEqual(*Result.Asset->GetPlatformData(), OriginalPlatformData);
	EXPECT_EQ(Result.Asset->GetSourceImportData().Source.SourcePath.Path, "../Outside.png");

	const std::filesystem::path Replacement =
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceRepairReplacement.tga";
	WriteNpotTextureFixture(Replacement);
	ASSERT_TRUE(Result.Asset->IngestAndChangeSource(
		Replacement.generic_string(),
		"/TextureImportTests/Textures/Texture.tga", Error)) << Error;
	EXPECT_EQ(Result.Asset->GetSourceImportData().Source.SourcePath.Path,
		"/TextureImportTests/Textures/Texture.tga");
	EXPECT_EQ(Result.Asset->InspectSource().Status, Durin::ETextureSourceStatus::Available);
	EXPECT_EQ(Result.Asset->GetSourceWidth(), 5u);
	EXPECT_EQ(Result.Asset->GetSourceHeight(), 3u);
	EXPECT_TRUE(Result.Asset->GetPackage()->IsDirty());
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Durin::Testing::GetTestWorkDirectory() / "TextureImports"
		/ "Content" / "Textures" / "Texture.tga"));
}

TEST(FTexture2DTests, ReportsMountedSourceBytesChangedSinceImport)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureChangedSourceCache");
	const std::filesystem::path Input =
		Durin::Testing::GetTestWorkDirectory() / "TextureChangedSource.png";
	WriteTextureFixture(Input);
	const Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(
		Input.generic_string(), "/TextureImportTests/ChangedSource");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	const Durin::FTextureSourceDiagnostic Available = Result.Asset->InspectSource();
	ASSERT_EQ(Available.Status, Durin::ETextureSourceStatus::Available);

	{
		std::ofstream Stream(
			Available.PhysicalPath, std::ios::binary | std::ios::app);
		const char ExtraByte = '\0';
		Stream.write(&ExtraByte, 1);
	}
	const Durin::FTextureSourceDiagnostic Changed = Result.Asset->InspectSource();
	EXPECT_EQ(Changed.Status, Durin::ETextureSourceStatus::Changed);
	EXPECT_NE(Changed.Message.find("changed"), std::string::npos);
}

TEST(FTexture2DTests, RetiredSourceFileIsNotReflected)
{
	InitializeDObjectSystem();
	EXPECT_EQ(
		Durin::DTexture2D::StaticClass()->FindPropertyByName("SourceFile"),
		nullptr);
}

TEST(FTexture2DTests, DerivedDataKeyCoversSourceContentAndBuildSettings)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureDerivedDataKeyCache");
	const std::filesystem::path FirstSource = Durin::Testing::GetTestWorkDirectory() / "DerivedKeyFirst.png";
	const std::filesystem::path SecondSource = Durin::Testing::GetTestWorkDirectory() / "DerivedKeySecond.tga";
	WriteTextureFixture(FirstSource);
	WriteNpotTextureFixture(SecondSource);

	const Durin::FTexture2DImportResult First = Durin::DTexture2D::ImportAsset(
		FirstSource.generic_string(), "/TextureImportTests/DerivedKeyFirst");
	const Durin::FTexture2DImportResult Second = Durin::DTexture2D::ImportAsset(
		SecondSource.generic_string(), "/TextureImportTests/DerivedKeySecond");
	ASSERT_TRUE(First) << First.Message;
	ASSERT_TRUE(Second) << Second.Message;
	ASSERT_NE(First.Asset, nullptr);
	ASSERT_NE(Second.Asset, nullptr);
	EXPECT_NE(First.Asset->GetSourceContentHash(), Second.Asset->GetSourceContentHash());
	EXPECT_NE(First.Asset->GetDerivedDataKey(), Second.Asset->GetDerivedDataKey());

	Durin::FAssetPath FirstPath;
	Durin::FAssetPath SecondPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/DerivedKeyFirst", FirstPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/DerivedKeySecond", SecondPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(FirstPath));
	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(FirstPath, Loaded));
	ASSERT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	const std::string OriginalKey = Loaded->GetDerivedDataKey();
	std::string Error;
	ASSERT_TRUE(Loaded->SetMaxResolution(1, Error)) << Error;
	EXPECT_NE(Loaded->GetSourceData(), nullptr);
	EXPECT_NE(Loaded->GetDerivedDataKey(), OriginalKey);
	EXPECT_TRUE(std::filesystem::is_regular_file(GetTextureCachePath(*Loaded)));

	ASSERT_TRUE(Durin::Asset::UnloadPackage(FirstPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SecondPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(FirstPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(SecondPath));
}
