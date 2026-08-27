#include "TextureTestSupport.h"
#include "Asset/SourceHint.h"
#include "Misc/FileHelper.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/TexturePayloadInspection.h"

namespace
{
	auto MakeExpectedSourceHint(
		const std::filesystem::path& Source,
		std::string_view AssetPath) -> std::string
	{
		const Durin::PathUtilities::FAssetPathResult Resolved =
			Durin::PathUtilities::ResolveAssetPath(
				AssetPath, Durin::PathUtilities::EPathExistence::AllowMissing);
		if (!Resolved) return {};
		std::filesystem::path PackagePath = Resolved.PhysicalPath;
		PackagePath += ".dasset";
		std::string Hint;
		std::string Error;
		Durin::AssetImport::ESourceHintBase Base;
		return Durin::AssetImport::MakeSourceHint(
			Source.generic_string(), PackagePath.generic_string(), Base, Hint, Error)
			? Hint : std::string{};
	}

	auto RelocateAssetForTest(
		const Durin::FAssetPath& Source,
		const Durin::FAssetPath& Destination) -> Durin::Asset::FAssetResult
	{
		const Durin::Asset::FAssetRelocationMapping Mapping{Source, Destination};
		Durin::Asset::FAssetMutationSummary Summary;
		Durin::Asset::FAssetMutationTransaction Transaction;
		Durin::Asset::FAssetResult Result =
			Durin::Asset::PrepareAssetRelocationTransaction(
				std::span{&Mapping, 1}, Summary, Transaction);
		if (Result) Result = Transaction.Commit();
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
	Durin::FTexture2DImportResult Result = Durin::AssetForge::Builtins::ImportTexture2DAsset(Source.generic_string(), "/TextureImportTests/Transparent");
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
	EXPECT_EQ(LiveSource->LogicalByteCount, 74u);
	EXPECT_EQ(LiveDerived->State, Durin::ETexturePayloadState::Available);
	EXPECT_EQ(LiveDecoded->State, Durin::ETexturePayloadState::Available);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Transparent", AssetPath));
	const Durin::Asset::FAssetCatalogEntry AssetData =
		Durin::Asset::FindAssetExact(AssetPath);
	ASSERT_TRUE(AssetData);
	Durin::Asset::FAssetPackageInspection PackageInspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
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
	Durin::FProperty* CookedProperty = Result.Asset->GetClass()->FindPropertyByName(
		"CookedPayload");
	ASSERT_NE(CookedProperty, nullptr);
	auto* CookedDescriptor = static_cast<Durin::Asset::FCookedPayloadDescriptor*>(
		CookedProperty->GetValuePtr(Result.Asset));
	ASSERT_NE(CookedDescriptor, nullptr);
	const Durin::Asset::FCookedPayloadDescriptor SavedCookedDescriptor =
		*CookedDescriptor;
	*CookedDescriptor = {
		.PayloadId = Durin::Texture2DPrimaryCookedPayloadId,
		.LocationKind = static_cast<uint32>(
			Durin::Asset::ECookedPayloadLocationKind::PackageCompanion),
		.StoredSize = 16,
		.UncompressedSize = 16,
		.PayloadSchemaVersion = Durin::TexturePayloadSchemaVersion + 1};
	ASSERT_TRUE(Durin::Asset::SavePackage(Result.Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		AssetData->PhysicalPath, PackageInspection));
	ASSERT_TRUE(Durin::InspectTexturePayloadPackage(
		PackageInspection, ConstructFreeInspection, &InspectionError))
		<< InspectionError;
	const auto UnsupportedCooked = std::ranges::find(
		ConstructFreeInspection.Entries, Durin::ETexturePayloadStage::Cooked,
		&Durin::FTexturePayloadInspectionEntry::Stage);
	ASSERT_NE(UnsupportedCooked, ConstructFreeInspection.Entries.end());
	EXPECT_EQ(UnsupportedCooked->State, Durin::ETexturePayloadState::Unsupported);
	EXPECT_EQ(UnsupportedCooked->Repair,
		Durin::ETexturePayloadRepairAction::UpgradeOrResave);
	*CookedDescriptor = SavedCookedDescriptor;
	ASSERT_TRUE(Durin::Asset::SavePackage(Result.Asset->GetPackage()));

	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache())
		<< Loaded->GetDerivedDataDiagnostic().Message;
	EXPECT_EQ(Loaded->GetSourceWidth(), 2u);
	EXPECT_EQ(Loaded->GetSourceHeight(), 1u);
	EXPECT_EQ(Loaded->GetSourceChannelCount(), 4u);
	EXPECT_TRUE(Loaded->SourceHasTransparency());
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	EXPECT_EQ(Loaded->GetBuildRevision(), 1u);
	std::string ExpectedFilename;
	std::string FilenameError;
	const Durin::PathUtilities::FAssetPathResult PhysicalPackage =
		Durin::PathUtilities::ResolveAssetPath(
			AssetPath.GetView(), Durin::PathUtilities::EPathExistence::AllowMissing);
	ASSERT_TRUE(PhysicalPackage) << PhysicalPackage.Message;
	std::filesystem::path PhysicalPackagePath = PhysicalPackage.PhysicalPath;
	PhysicalPackagePath += ".dasset";
	Durin::AssetImport::ESourceHintBase ExpectedBase;
	ASSERT_TRUE(Durin::AssetImport::MakeSourceHint(
		Source.generic_string(), PhysicalPackagePath.generic_string(),
		ExpectedBase, ExpectedFilename, FilenameError)) << FilenameError;
	EXPECT_EQ(Loaded->GetSourceFile(), ExpectedFilename);
	EXPECT_EQ(Loaded->GetSourceHintBase(), ExpectedBase);
	ASSERT_NE(Loaded->GetImportedSource(), nullptr);
	EXPECT_EQ(Loaded->GetImportedSource()->StableIdentity, "root");
	EXPECT_EQ(Loaded->GetImportedSource()->Role, "source");
	EXPECT_FALSE(Loaded->GetImportedSource()->GetContentHash().IsZero());
	EXPECT_TRUE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::FAssetPath RenamedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Renamed", RenamedPath));
	ASSERT_TRUE(RelocateAssetForTest(AssetPath, RenamedPath));
	ASSERT_TRUE(Durin::Asset::LoadAsset(RenamedPath, Loaded));
	EXPECT_EQ(Loaded->GetSourceFile(), ExpectedFilename);
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RenamedPath));
	Durin::Asset::FAssetDeleteAnalysis DeleteAnalysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(RenamedPath, DeleteAnalysis));
	EXPECT_TRUE(DeleteAnalysis.CompanionFiles.empty());
	ASSERT_TRUE(DeleteAssetClosureForTest({AssetPath, RenamedPath}));
	EXPECT_TRUE(std::filesystem::is_regular_file(Source));
}

TEST(FTexture2DTests, RetainsSourceHintWithoutCopying)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceDestinationCache");

	const std::filesystem::path DefaultInput =
		Durin::Testing::GetTestWorkDirectory() / "FlatDefault.png";
	WriteTextureFixture(DefaultInput);
	Durin::FTexture2DImportResult DefaultResult = Durin::AssetForge::Builtins::ImportTexture2DAsset(
		DefaultInput.generic_string(), "/TextureImportTests/Textures/FlatDefault");
	ASSERT_TRUE(DefaultResult) << DefaultResult.Message;
	ASSERT_NE(DefaultResult.Asset, nullptr);
	const std::string DefaultFilename = MakeExpectedSourceHint(
		DefaultInput, "/TextureImportTests/Textures/FlatDefault");
	ASSERT_FALSE(DefaultFilename.empty());
	EXPECT_EQ(DefaultResult.Asset->GetSourceFile(), DefaultFilename);

	Durin::FAssetPath DefaultAssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TextureImportTests/Textures/FlatDefault", DefaultAssetPath));
	const Durin::Asset::FAssetCatalogEntry DefaultAssetData =
		Durin::Asset::FindAssetExact(DefaultAssetPath);
	ASSERT_NE(DefaultAssetData, nullptr);
	Durin::Asset::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		DefaultAssetData->PhysicalPath, Inspection));
	Durin::AssetImport::FAssetImportInfo InspectedImportInfo;
	std::string ImportInfoError;
	ASSERT_TRUE(Durin::AssetImport::InspectAssetImportInfo(
		Inspection, InspectedImportInfo, ImportInfoError)) << ImportInfoError;
	ASSERT_NE(InspectedImportInfo.FindByRole("source"), nullptr);
	EXPECT_EQ(InspectedImportInfo.FindByRole("source")->Hint,
		DefaultResult.Asset->GetSourceFile());

	const std::filesystem::path CustomInput =
		Durin::Testing::GetTestWorkDirectory() / "CustomInput.png";
	WriteTextureFixture(CustomInput);
	Durin::FTexture2DImportResult CustomResult = Durin::AssetForge::Builtins::ImportTexture2DAsset(
		CustomInput.generic_string(), "/TextureImportTests/UI/CustomAsset");
	ASSERT_TRUE(CustomResult) << CustomResult.Message;
	ASSERT_NE(CustomResult.Asset, nullptr);
	std::string CustomFilename;
	CustomFilename = MakeExpectedSourceHint(
		CustomInput, "/TextureImportTests/UI/CustomAsset");
	ASSERT_FALSE(CustomFilename.empty());
	EXPECT_EQ(CustomResult.Asset->GetSourceFile(), CustomFilename);
	EXPECT_TRUE(std::filesystem::is_regular_file(CustomInput));

	Durin::FAssetPath CustomAssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TextureImportTests/UI/CustomAsset", CustomAssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(DefaultAssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		CustomAssetPath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(DefaultAssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(CustomAssetPath));
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
	const Durin::FTexture2DImportResult Result = Durin::AssetForge::Builtins::ImportTexture2DAsset(
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
	const Durin::Asset::FAssetCatalogEntry CachedAssetData =
		Durin::Asset::FindAssetExact(AssetPath);
	ASSERT_TRUE(CachedAssetData);
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
		const std::array<uint8, 7> CorruptBytes = {0, 1, 2, 3, 4, 5, 6};
		std::ofstream Stream(CachePath, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(CorruptBytes.data()), CorruptBytes.size());
	}
	std::vector<std::byte> PackageBytesBeforeRecovery;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		PackageBytesBeforeRecovery, CachedAssetData->PhysicalPath));
	const auto PackageTimeBeforeRecovery =
		std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
	std::filesystem::last_write_time(
		CachedAssetData->PhysicalPath, PackageTimeBeforeRecovery);
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_TRUE(Durin::Asset::WaitForTexture2DCompilation(*Loaded))
		<< Loaded->GetLastBuildError();
	EXPECT_FALSE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_FALSE(Loaded->GetDerivedDataDiagnostic().bSourceDecoderInvoked);
	EXPECT_EQ(Loaded->GetDerivedDataKey(), OriginalKey);
	ASSERT_NE(Loaded->GetSourceData(), nullptr);
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*Loaded->GetPlatformData(), ExpectedPlatformData);
	EXPECT_GT(std::filesystem::file_size(CachePath), 7u);
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	std::vector<std::byte> PackageBytesAfterRecovery;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		PackageBytesAfterRecovery, CachedAssetData->PhysicalPath));
	EXPECT_EQ(PackageBytesAfterRecovery, PackageBytesBeforeRecovery);
	EXPECT_EQ(std::filesystem::last_write_time(CachedAssetData->PhysicalPath),
		PackageTimeBeforeRecovery);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_EQ(Loaded->GetDerivedDataKey(), OriginalKey);
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache())
		<< Loaded->GetDerivedDataDiagnostic().Message;
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	const std::filesystem::path CopiedSource = Source;
	WriteNpotTextureFixture(CopiedSource);
	std::filesystem::last_write_time(CopiedSource,
		std::filesystem::last_write_time(CopiedSource) + std::chrono::seconds(1));
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Loaded->GetDerivedDataKey(), OriginalKey);
	EXPECT_NE(Loaded->GetSourceWidth(), 5u);
	EXPECT_NE(Loaded->GetSourceHeight(), 3u);
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	EXPECT_TRUE(std::filesystem::is_regular_file(GetTextureCachePath(*Loaded)));
	ASSERT_TRUE(Durin::Asset::SavePackage(Loaded->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
}

TEST(FTexture2DTests, TimestampOnlySourceChangeUsesPersistedIdentityWithoutDirtying)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureTimestampOnlySourceCache");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureTimestampOnlySource.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Result = Durin::AssetForge::Builtins::ImportTexture2DAsset(
		Source.generic_string(), "/TextureImportTests/Fingerprint");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Fingerprint", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	const std::filesystem::path StoredSource = Source;
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
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_FALSE(Loaded->GetDerivedDataDiagnostic().bSourceDecoderInvoked);
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
}

TEST(FTexture2DTests, SourceFileCanBeReplacedAndRejectsTraversalMetadata)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceRepairCache");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceRepair.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Result = Durin::AssetForge::Builtins::ImportTexture2DAsset(
		Source.generic_string(), "/TextureImportTests/Repair/Texture");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	const Durin::FTexturePlatformData OriginalPlatformData = *Result.Asset->GetPlatformData();

	auto* ImportData = const_cast<Durin::AssetImport::DAssetImportData*>(
		Result.Asset->GetAssetImportData());
	ASSERT_NE(ImportData, nullptr);
	auto* SourceDataProperty = ImportData->GetClass()->FindPropertyByName("SourceData");
	ASSERT_NE(SourceDataProperty, nullptr);
	auto* ImportInfo = static_cast<Durin::AssetImport::FAssetImportInfo*>(
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
	EXPECT_EQ(Result.Asset->GetSourceFile(), "Invalid/./Outside.png");

	const std::filesystem::path Replacement =
		Durin::Testing::GetTestWorkDirectory() / "TextureSourceRepairReplacement.tga";
	WriteNpotTextureFixture(Replacement);
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportTexture2DFromFile(
		*Result.Asset, Replacement.generic_string(), Error)) << Error;
	ASSERT_TRUE(Durin::Asset::WaitForTexture2DCompilation(*Result.Asset, 10.0));
	const std::string ReplacementFilename = MakeExpectedSourceHint(
		Replacement, "/TextureImportTests/Repair/Texture");
	ASSERT_FALSE(ReplacementFilename.empty());
	EXPECT_EQ(Result.Asset->GetSourceFile(), ReplacementFilename);
	EXPECT_EQ(Result.Asset->GetSourceWidth(), 5u);
	EXPECT_EQ(Result.Asset->GetSourceHeight(), 3u);
	EXPECT_FALSE(Result.Asset->GetPackage()->IsDirty());
	ASSERT_NE(Result.Asset->GetImportedSource(), nullptr);
	EXPECT_EQ(Result.Asset->GetImportedSource()->Hint, ReplacementFilename);
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
	const Durin::FTexture2DImportResult Result = Durin::AssetForge::Builtins::ImportTexture2DAsset(
		Input.generic_string(), "/TextureImportTests/ChangedSource");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	const Durin::FXxHash128 ImportedHash =
		Result.Asset->GetImportedSource()->GetContentHash();
	const Durin::FTexturePlatformData PlatformBefore = *Result.Asset->GetPlatformData();

	{
		std::ofstream Stream(Input, std::ios::binary | std::ios::app);
		const char ExtraByte = '\0';
		Stream.write(&ExtraByte, 1);
	}
	EXPECT_EQ(Result.Asset->GetImportedSource()->GetContentHash(), ImportedHash);
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

	const Durin::FTexture2DImportResult First = Durin::AssetForge::Builtins::ImportTexture2DAsset(
		FirstSource.generic_string(), "/TextureImportTests/DerivedKeyFirst");
	const Durin::FTexture2DImportResult Second = Durin::AssetForge::Builtins::ImportTexture2DAsset(
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
	ASSERT_TRUE(Durin::AssetForge::Builtins::SetTexture2DMaxResolution(
		*Loaded, 1, Error)) << Error;
	ASSERT_TRUE(Durin::Asset::WaitForTexture2DCompilation(*Loaded, 10.0))
		<< Durin::Asset::GetTexture2DCompilationDiagnostic(*Loaded).Message;
	EXPECT_NE(Loaded->GetSourceData(), nullptr);
	EXPECT_NE(Loaded->GetDerivedDataKey(), OriginalKey);
	EXPECT_TRUE(std::filesystem::is_regular_file(GetTextureCachePath(*Loaded)));

	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		FirstPath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SecondPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(FirstPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(SecondPath));
}
