#include "NativeAssetTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "TextureTestSupport.h"
#include "Asset/SourceHint.h"
#include "Misc/FileHelper.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/TexturePayloadInspection.h"

namespace
{
	auto FindImportedSource(const Durin::DTexture2D& Texture)
		-> const Durin::FSourceFile*
	{
		const Durin::DAssetImportData* ImportData =
			Texture.GetAssetImportData();
		return ImportData
			? ImportData->GetSourceData().FindByRole("source") : nullptr;
	}

	auto MakeExpectedSourceHint(
		const std::filesystem::path& Source,
		std::string_view AssetPath) -> std::string
	{
		const Durin::FAssetPathResult Resolved =
			Durin::FMountPaths::ResolveAssetPath(
				AssetPath, Durin::EMountPathExistence::AllowMissing);
		if (!Resolved) return {};
		std::filesystem::path PackagePath = Resolved.PhysicalPath;
		PackagePath += ".dasset";
		std::string Hint;
		std::string Error;
		Durin::ESourceHintBase Base;
		return Durin::MakeSourceHint(
			Source.generic_string(), PackagePath.generic_string(), Base, Hint, Error)
			? Hint : std::string{};
	}

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
}

TEST(FTexture2DTests, ImportsSourceAndBuildsIndependentPlatformData)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureImportDerivedDataCache");

	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "TextureSource.png";
	WriteTextureFixture(Source);
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureImportTests/Transparent", AssetPath));
	auto* Factory = Durin::NewObject<Durin::AssetForge::Builtins::DTexture2DFactory>(
		nullptr, "Texture2DFactoryImportTest", Durin::EObjectFlags::Transient);
	const Durin::FAssetToolsResult Created = Durin::IAssetTools::Get().ImportPackageLeafAssetForTesting(
		AssetPath, Durin::DTexture2D::StaticClass(), Source.generic_string(), Factory);
	Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Result{
		Created.Succeeded(), Created.Message, Durin::Cast<Durin::DTexture2D>(Created.Asset)};
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	EXPECT_TRUE(Result.Asset->GetPackage()->IsDirty());
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::FindResidentPackage(AssetPath), Result.Asset->GetPackage());
	ASSERT_TRUE(Durin::SavePackage(Result.Asset->GetPackage()));
	const Durin::FTextureSourceData SourceData =
		Result.Asset->CreateBuildInput().ToSourceData();
	const Durin::FTexturePlatformData* PlatformData = Result.Asset->GetPlatformData();
	ASSERT_TRUE(SourceData.IsValid());
	ASSERT_NE(PlatformData, nullptr);
	EXPECT_NE(Result.Asset->GetTextureReferenceRHI(), nullptr);
	EXPECT_EQ(Result.Asset->GetBuildRevision(), 1u);
	EXPECT_TRUE(SourceData.bHasTransparency);
	EXPECT_EQ(SourceData.Width, 2u);
	EXPECT_EQ(SourceData.Height, 1u);
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

	const Durin::FTexturePayloadInspection LiveInspection =
		Durin::InspectTexturePayloads(*Result.Asset);
	ASSERT_FALSE(LiveInspection.bConstructFree);
	ASSERT_EQ(LiveInspection.Entries.size(), 5u);
	const auto FindLiveStage = [&](Durin::ETexturePayloadStage Stage) {
		return std::ranges::find(LiveInspection.Entries, Stage,
			&Durin::FTexturePayloadInspectionEntry::Stage);
	};
	const auto LiveSource = FindLiveStage(Durin::ETexturePayloadStage::Source);
	const auto LiveDerived = FindLiveStage(Durin::ETexturePayloadStage::DerivedData);
	const auto LiveDecoded = FindLiveStage(Durin::ETexturePayloadStage::Decoded);
	ASSERT_NE(LiveSource, LiveInspection.Entries.end());
	ASSERT_NE(LiveDerived, LiveInspection.Entries.end());
	ASSERT_NE(LiveDecoded, LiveInspection.Entries.end());
	EXPECT_EQ(LiveSource->State, Durin::ETexturePayloadState::Available);
	EXPECT_EQ(LiveSource->LogicalElementCount, 2u);
	EXPECT_EQ(LiveSource->LogicalByteCount, 8u);
	EXPECT_EQ(LiveDerived->State, Durin::ETexturePayloadState::Available);
	EXPECT_EQ(LiveDecoded->State, Durin::ETexturePayloadState::Available);

	const Durin::FAssetCatalogEntry AssetData =
		Durin::FindAssetExact(AssetPath);
	ASSERT_TRUE(AssetData);
	Durin::FAssetPackageInspection PackageInspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(
		AssetData->PhysicalPath, PackageInspection));
	Durin::FTexturePayloadInspection ConstructFreeInspection;
	std::string InspectionError;
	ASSERT_TRUE(Durin::InspectTexturePayloadPackage(
		PackageInspection, ConstructFreeInspection, &InspectionError))
		<< InspectionError;
	ASSERT_TRUE(ConstructFreeInspection.bConstructFree);
	ASSERT_EQ(ConstructFreeInspection.Entries.size(), 3u);
	EXPECT_EQ(ConstructFreeInspection.Entries.front().Domain, "Texture2D");
	EXPECT_EQ(ConstructFreeInspection.Entries.front().LogicalElementCount, 2u);
	EXPECT_EQ(ConstructFreeInspection.Entries.front().Placement, "SourceFile");
	const auto AuthoredCooked = std::ranges::find(
		ConstructFreeInspection.Entries, Durin::ETexturePayloadStage::Cooked,
		&Durin::FTexturePayloadInspectionEntry::Stage);
	ASSERT_NE(AuthoredCooked, ConstructFreeInspection.Entries.end());
	EXPECT_EQ(AuthoredCooked->State, Durin::ETexturePayloadState::NotPresent);
	EXPECT_EQ(AuthoredCooked->Placement, "PackageBulkField");

	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));

	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded));
	EXPECT_TRUE(Loaded->GetSource().IsValid());
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	EXPECT_EQ(Loaded->GetSource().GetWidth(), 2u);
	EXPECT_EQ(Loaded->GetSource().GetHeight(), 1u);
	EXPECT_EQ(Loaded->GetSource().GetSourceChannelCount(), 4u);
	EXPECT_TRUE(Loaded->GetSource().HasTransparency());
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	EXPECT_EQ(Loaded->GetBuildRevision(), 1u);
	std::string ExpectedFilename;
	std::string FilenameError;
	const Durin::FAssetPathResult PhysicalPackage =
		Durin::FMountPaths::ResolveAssetPath(
			AssetPath.GetView(), Durin::EMountPathExistence::AllowMissing);
	ASSERT_TRUE(PhysicalPackage) << PhysicalPackage.Message;
	std::filesystem::path PhysicalPackagePath = PhysicalPackage.PhysicalPath;
	PhysicalPackagePath += ".dasset";
	Durin::ESourceHintBase ExpectedBase;
	ASSERT_TRUE(Durin::MakeSourceHint(
		Source.generic_string(), PhysicalPackagePath.generic_string(),
		ExpectedBase, ExpectedFilename, FilenameError)) << FilenameError;
	const Durin::FSourceFile* LoadedSource = FindImportedSource(*Loaded);
	ASSERT_NE(LoadedSource, nullptr);
	EXPECT_EQ(LoadedSource->Hint, ExpectedFilename);
	EXPECT_EQ(LoadedSource->HintBase, ExpectedBase);
	EXPECT_EQ(LoadedSource->Role, Durin::FName("source"));
	EXPECT_FALSE(LoadedSource->GetContentHash().IsZero());
	EXPECT_TRUE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));

	Durin::FPackagePath RenamedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/Renamed", RenamedPath));
	ASSERT_TRUE(RelocateAssetForTest(AssetPath, RenamedPath));
	Durin::FTopLevelAssetPath RenamedAssetPath;
	Durin::FObjectPath RenamedObjectPath;
	ASSERT_TRUE(Durin::FTopLevelAssetPath::TryCreate(
		RenamedPath, AssetPath.GetPackageName(), RenamedAssetPath));
	ASSERT_TRUE(Durin::FObjectPath::TryCreate(
		RenamedAssetPath, std::span<const std::string>{}, RenamedObjectPath));
	ASSERT_TRUE(Durin::LoadObject(RenamedObjectPath, Loaded));
	LoadedSource = FindImportedSource(*Loaded);
	ASSERT_NE(LoadedSource, nullptr);
	EXPECT_EQ(LoadedSource->Hint, ExpectedFilename);
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	ASSERT_TRUE(Durin::UnloadPackage(RenamedPath));
	Durin::FAssetPackageInspection Inspection;
	const auto Data = Durin::FindAssetExact(RenamedPath);
	ASSERT_TRUE(Data);
	ASSERT_TRUE(Durin::InspectAssetPackage(Data->PhysicalPath, RenamedPath, Inspection));
	std::vector<std::filesystem::path> Companions;
	ASSERT_TRUE(Durin::InspectEditorBulkDataCompanionPaths(Data->PhysicalPath, Inspection, Companions));
	EXPECT_TRUE(Companions.empty());
	ASSERT_TRUE(DeleteAssetClosureForTest({AssetPath, RenamedPath}));
	EXPECT_TRUE(std::filesystem::is_regular_file(Source));
}

TEST(FTexture2DTests, RetainsSourceHintWithoutCopying)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceHintCache");

	const std::filesystem::path DefaultInput =
		Durin::Testing::GetTestWorkDirectory() / "FlatDefault.png";
	WriteTextureFixture(DefaultInput);
	Durin::Testing::TFactoryImportResult<Durin::DTexture2D> DefaultResult = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		DefaultInput.generic_string(), "/TextureImportTests/Textures/FlatDefault");
	ASSERT_TRUE(DefaultResult) << DefaultResult.Message;
	ASSERT_NE(DefaultResult.Asset, nullptr);
	const std::string DefaultFilename = MakeExpectedSourceHint(
		DefaultInput, "/TextureImportTests/Textures/FlatDefault");
	ASSERT_FALSE(DefaultFilename.empty());
	const Durin::FSourceFile* DefaultSource =
		FindImportedSource(*DefaultResult.Asset);
	ASSERT_NE(DefaultSource, nullptr);
	EXPECT_EQ(DefaultSource->Hint, DefaultFilename);

	Durin::FPackagePath DefaultAssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureImportTests/Textures/FlatDefault", DefaultAssetPath));
	const Durin::FAssetCatalogEntry DefaultAssetData =
		Durin::FindAssetExact(DefaultAssetPath);
	ASSERT_NE(DefaultAssetData, nullptr);
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(
		DefaultAssetData->PhysicalPath, Inspection));
	Durin::FAssetImportInfo InspectedImportInfo;
	std::string ImportInfoError;
	ASSERT_TRUE(Durin::InspectAssetImportInfo(
		Inspection, InspectedImportInfo, ImportInfoError)) << ImportInfoError;
	ASSERT_NE(InspectedImportInfo.FindByRole("source"), nullptr);
	EXPECT_EQ(InspectedImportInfo.FindByRole("source")->Hint,
		DefaultSource->Hint);

	const std::filesystem::path CustomInput =
		Durin::Testing::GetTestWorkDirectory() / "CustomInput.png";
	WriteTextureFixture(CustomInput);
	Durin::Testing::TFactoryImportResult<Durin::DTexture2D> CustomResult = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		CustomInput.generic_string(), "/TextureImportTests/UI/CustomAsset");
	ASSERT_TRUE(CustomResult) << CustomResult.Message;
	ASSERT_NE(CustomResult.Asset, nullptr);
	std::string CustomFilename;
	CustomFilename = MakeExpectedSourceHint(
		CustomInput, "/TextureImportTests/UI/CustomAsset");
	ASSERT_FALSE(CustomFilename.empty());
	const Durin::FSourceFile* CustomSource =
		FindImportedSource(*CustomResult.Asset);
	ASSERT_NE(CustomSource, nullptr);
	EXPECT_EQ(CustomSource->Hint, CustomFilename);
	EXPECT_TRUE(std::filesystem::is_regular_file(CustomInput));

	Durin::FPackagePath CustomAssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureImportTests/UI/CustomAsset", CustomAssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(DefaultAssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(
		CustomAssetPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(DefaultAssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(CustomAssetPath));
}

TEST(FTexture2DTests, VersionedDerivedDataCacheHitsAndRecoversCorruptPayload)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "TextureDerivedDataMount";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::Testing::RegisterMountPointForTests(
		"/TextureDerivedDataTests/", Root.generic_string() + "/");
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureDerivedDataCache");

	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "DerivedDataSource.png";
	WriteTextureFixture(Source);
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Source.generic_string(), "/TextureDerivedDataTests/Cached");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	const Durin::FSourceFile* ImportedSource =
		FindImportedSource(*Result.Asset);
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_EQ(ImportedSource->GetContentHash().ToString().size(), 32u);
	EXPECT_FALSE(GetTextureDerivedDataKey(*Result.Asset).empty());
	const std::filesystem::path CachePath = GetTextureCachePath(*Result.Asset);
	const std::string OriginalKey = GetTextureDerivedDataKey(*Result.Asset);
	EXPECT_TRUE(std::filesystem::is_regular_file(CachePath));
	const Durin::FTexturePlatformData ExpectedPlatformData = *Result.Asset->GetPlatformData();

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureDerivedDataTests/Cached", AssetPath));
	const Durin::FAssetCatalogEntry CachedAssetData =
		Durin::FindAssetExact(AssetPath);
	ASSERT_TRUE(CachedAssetData);
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));

	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_TRUE(Loaded->GetSource().IsValid());
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*Loaded->GetPlatformData(), ExpectedPlatformData);
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));

	{
		const std::array<uint8, 7> CorruptBytes = {0, 1, 2, 3, 4, 5, 6};
		std::ofstream Stream(CachePath, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(CorruptBytes.data()), CorruptBytes.size());
	}
	Durin::FByteBuffer PackageBytesBeforeRecovery;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		PackageBytesBeforeRecovery, CachedAssetData->PhysicalPath));
	const auto PackageTimeBeforeRecovery =
		std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
	std::filesystem::last_write_time(
		CachedAssetData->PhysicalPath, PackageTimeBeforeRecovery);
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded));
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Loaded))
		<< Durin::GetTexture2DCompilationDiagnostic(*Loaded).Message;
	EXPECT_EQ(GetTextureDerivedDataKey(*Loaded), OriginalKey);
	ASSERT_TRUE(Loaded->GetSource().IsValid());
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*Loaded->GetPlatformData(), ExpectedPlatformData);
	EXPECT_GT(std::filesystem::file_size(CachePath), 7u);
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	Durin::FByteBuffer PackageBytesAfterRecovery;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		PackageBytesAfterRecovery, CachedAssetData->PhysicalPath));
	EXPECT_EQ(PackageBytesAfterRecovery, PackageBytesBeforeRecovery);
	EXPECT_EQ(std::filesystem::last_write_time(CachedAssetData->PhysicalPath),
		PackageTimeBeforeRecovery);
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));

	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded));
	EXPECT_EQ(GetTextureDerivedDataKey(*Loaded), OriginalKey);
	EXPECT_TRUE(Loaded->GetSource().IsValid());
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));

	const std::filesystem::path CopiedSource = Source;
	WriteNpotTextureFixture(CopiedSource);
	std::filesystem::last_write_time(CopiedSource,
		std::filesystem::last_write_time(CopiedSource) + std::chrono::seconds(1));
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded));
	EXPECT_EQ(GetTextureDerivedDataKey(*Loaded), OriginalKey);
	EXPECT_NE(Loaded->GetSource().GetWidth(), 5u);
	EXPECT_NE(Loaded->GetSource().GetHeight(), 3u);
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	EXPECT_TRUE(std::filesystem::is_regular_file(GetTextureCachePath(*Loaded)));
	ASSERT_TRUE(Durin::SavePackage(Loaded->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded));
	EXPECT_TRUE(Loaded->GetSource().IsValid());
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST(FTexture2DTests, TimestampOnlySourceChangeUsesPersistedIdentityWithoutDirtying)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureTimestampOnlySourceCache");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureTimestampOnlySource.png";
	WriteTextureFixture(Source);
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Source.generic_string(), "/TextureImportTests/Fingerprint");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/Fingerprint", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	const std::filesystem::path StoredSource = Source;
	ASSERT_TRUE(std::filesystem::is_regular_file(StoredSource));
	std::filesystem::last_write_time(
		StoredSource,
		std::filesystem::last_write_time(StoredSource) + std::chrono::seconds(1));

	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));

	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST(FTexture2DTests, SourceFileCanBeReplacedAndRejectsTraversalMetadata)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceRepairCache");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceRepair.png";
	WriteTextureFixture(Source);
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Source.generic_string(), "/TextureImportTests/Repair/Texture");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	const Durin::FTexturePlatformData OriginalPlatformData = *Result.Asset->GetPlatformData();

	auto* ImportData = const_cast<Durin::DAssetImportData*>(
		Result.Asset->GetAssetImportData());
	ASSERT_NE(ImportData, nullptr);
	auto* SourceDataProperty = ImportData->GetClass()->FindPropertyByName("SourceData");
	ASSERT_NE(SourceDataProperty, nullptr);
	auto* ImportInfo = static_cast<Durin::FAssetImportInfo*>(
		SourceDataProperty->GetValuePtr(ImportData));
	ASSERT_NE(ImportInfo, nullptr);
	ASSERT_EQ(ImportInfo->Sources.size(), 1u);
	ImportInfo->Sources.front().Hint = "Invalid/./Outside.png";
	std::string InvalidHintError;
	EXPECT_FALSE(ImportInfo->Sources.front().Validate(InvalidHintError));

	const std::filesystem::path Corrupt =
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceRepairCorrupt.png";
	{
		std::ofstream Stream(Corrupt, std::ios::binary | std::ios::trunc);
		Stream << "not an image";
	}
	std::string Error;
	EXPECT_FALSE(Durin::AssetForge::Builtins::ReimportTexture2DFromFile(
		*Result.Asset, Corrupt.generic_string(), Error));
	ExpectPlatformDataEqual(*Result.Asset->GetPlatformData(), OriginalPlatformData);
	const Durin::FSourceFile* ImportedSource =
		FindImportedSource(*Result.Asset);
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_EQ(ImportedSource->Hint, "Invalid/./Outside.png");

	const std::filesystem::path Replacement =
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceRepairReplacement.tga";
	WriteNpotTextureFixture(Replacement);
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportTexture2DFromFile(
		*Result.Asset, Replacement.generic_string(), Error)) << Error;
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Result.Asset, 10.0));
	const std::string ReplacementFilename = MakeExpectedSourceHint(
		Replacement, "/TextureImportTests/Repair/Texture");
	ASSERT_FALSE(ReplacementFilename.empty());
	ImportedSource = FindImportedSource(*Result.Asset);
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_EQ(ImportedSource->Hint, ReplacementFilename);
	EXPECT_EQ(Result.Asset->GetSource().GetWidth(), 5u);
	EXPECT_EQ(Result.Asset->GetSource().GetHeight(), 3u);
	EXPECT_FALSE(Result.Asset->GetPackage()->IsDirty());
	EXPECT_TRUE(std::filesystem::is_regular_file(Replacement));
}

TEST(FTexture2DTests, SourceChangesRemainUnobservedUntilExplicitReimport)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureChangedSourceCache");
	const std::filesystem::path Input =
		Durin::Testing::GetTestWorkDirectory() / "TextureChangedSource.png";
	WriteTextureFixture(Input);
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Input.generic_string(), "/TextureImportTests/ChangedSource");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	const Durin::FSourceFile* ImportedSource =
		FindImportedSource(*Result.Asset);
	ASSERT_NE(ImportedSource, nullptr);
	const Durin::FXxHash128 ImportedHash = ImportedSource->GetContentHash();
	const Durin::FTexturePlatformData PlatformBefore = *Result.Asset->GetPlatformData();

	{
		std::ofstream Stream(Input, std::ios::binary | std::ios::app);
		const char ExtraByte = '\0';
		Stream.write(&ExtraByte, 1);
	}
	ImportedSource = FindImportedSource(*Result.Asset);
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_EQ(ImportedSource->GetContentHash(), ImportedHash);
	ExpectPlatformDataEqual(*Result.Asset->GetPlatformData(), PlatformBefore);
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

	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> First = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		FirstSource.generic_string(), "/TextureImportTests/DerivedKeyFirst");
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Second = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		SecondSource.generic_string(), "/TextureImportTests/DerivedKeySecond");
	ASSERT_TRUE(First) << First.Message;
	ASSERT_TRUE(Second) << Second.Message;
	ASSERT_NE(First.Asset, nullptr);
	ASSERT_NE(Second.Asset, nullptr);
	const Durin::FSourceFile* FirstImportedSource =
		FindImportedSource(*First.Asset);
	const Durin::FSourceFile* SecondImportedSource =
		FindImportedSource(*Second.Asset);
	ASSERT_NE(FirstImportedSource, nullptr);
	ASSERT_NE(SecondImportedSource, nullptr);
	EXPECT_NE(FirstImportedSource->GetContentHash(), SecondImportedSource->GetContentHash());
	EXPECT_NE(GetTextureDerivedDataKey(*First.Asset),
		GetTextureDerivedDataKey(*Second.Asset));

	Durin::FPackagePath FirstPath;
	Durin::FPackagePath SecondPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/DerivedKeyFirst", FirstPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/DerivedKeySecond", SecondPath));
	ASSERT_TRUE(Durin::UnloadPackage(FirstPath));
	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(FirstPath), Loaded));
	EXPECT_TRUE(Loaded->GetSource().IsValid());
	const std::string OriginalKey = GetTextureDerivedDataKey(*Loaded);
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::SetTexture2DMaxResolution(
		*Loaded, 1, Error)) << Error;
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Loaded, 10.0))
		<< Durin::GetTexture2DCompilationDiagnostic(*Loaded).Message;
	EXPECT_TRUE(Loaded->GetSource().IsValid());
	EXPECT_NE(GetTextureDerivedDataKey(*Loaded), OriginalKey);
	EXPECT_TRUE(std::filesystem::is_regular_file(GetTextureCachePath(*Loaded)));

	ASSERT_TRUE(Durin::UnloadPackage(
		FirstPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::UnloadPackage(SecondPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(FirstPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(SecondPath));
}
