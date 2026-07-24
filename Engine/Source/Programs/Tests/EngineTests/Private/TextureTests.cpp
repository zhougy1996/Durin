#include "AssetSystem.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Package.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/EditorTransaction.h"
#include "Editor/ReflectedPropertyView.h"
#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "Texture/Texture2D.h"

#include <gtest/gtest.h>

namespace
{
	constexpr Durin::uint8 TransparentPngBytes[] = {
		137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
		0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
		0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};

	auto WriteTextureFixture(const std::filesystem::path& Path) -> void
	{
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(TransparentPngBytes), sizeof(TransparentPngBytes));
	}

	auto WriteNpotTextureFixture(const std::filesystem::path& Path) -> void
	{
		constexpr Durin::uint16 Width = 5;
		constexpr Durin::uint16 Height = 3;
		std::array<Durin::uint8, 18> Header{};
		Header[2] = 2;
		Header[12] = static_cast<Durin::uint8>(Width);
		Header[14] = static_cast<Durin::uint8>(Height);
		Header[16] = 32;
		Header[17] = 0x28;
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(Header.data()), Header.size());
		for (Durin::uint16 Y = 0; Y < Height; ++Y)
		{
			for (Durin::uint16 X = 0; X < Width; ++X)
			{
				const Durin::uint8 Value = X == Width - 1 ? 255 : 0;
				const std::array<Durin::uint8, 4> Pixel = {Value, Value, Value, 255};
				Stream.write(reinterpret_cast<const char*>(Pixel.data()), Pixel.size());
			}
		}
	}
}

TEST(FTexture2DTests, ImportsSourceAndBuildsIndependentPlatformData)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureImports";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/TextureImportTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureSource.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), "/TextureImportTests/Transparent");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	const Durin::FTextureSourceData* SourceData = Result.Asset->GetSourceData();
	const Durin::FTexturePlatformData* PlatformData = Result.Asset->GetPlatformData();
	ASSERT_NE(SourceData, nullptr);
	ASSERT_NE(PlatformData, nullptr);
	EXPECT_NE(Result.Asset->GetRenderResource(), nullptr);
	EXPECT_EQ(Result.Asset->GetBuildRevision(), 1u);
	EXPECT_TRUE(SourceData->IsValid());
	EXPECT_TRUE(SourceData->bHasTransparency);
	EXPECT_EQ(SourceData->Width, 2u);
	EXPECT_EQ(SourceData->Height, 1u);
	ASSERT_TRUE(PlatformData->IsValid());
	EXPECT_TRUE(Result.Asset->IsSRGB());
	EXPECT_EQ(Result.Asset->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_EQ(PlatformData->PixelFormat, Durin::EPixelFormat::SRGBA8_UNORM);
	ASSERT_EQ(PlatformData->Mips.size(), 2u);
	EXPECT_EQ(PlatformData->Mips[0].Pixels, SourceData->Pixels);
	EXPECT_NE(PlatformData->Mips[0].Pixels.data(), SourceData->Pixels.data());
	EXPECT_EQ(PlatformData->Mips[1].Width, 1u);
	EXPECT_EQ(PlatformData->Mips[1].Height, 1u);
	EXPECT_EQ(PlatformData->Mips[1].RowPitch, 4u);
	EXPECT_EQ(PlatformData->Mips[1].Pixels, (std::vector<Durin::uint8>{188, 0, 0, 128}));

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Transparent", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded->GetSourceData(), nullptr);
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	EXPECT_TRUE(Loaded->GetSourceData()->IsValid());
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	EXPECT_EQ(Loaded->GetBuildRevision(), 1u);
	EXPECT_EQ(Loaded->GetSourceFile(), "Transparent.png");
	EXPECT_TRUE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::SRGBA8_UNORM);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::FAssetPath RenamedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Renamed", RenamedPath));
	ASSERT_TRUE(Durin::Asset::MoveAsset(AssetPath, RenamedPath));
	const std::filesystem::path ImportRoot = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureImports";
	EXPECT_FALSE(std::filesystem::exists(ImportRoot / "Transparent.png"));
	EXPECT_TRUE(std::filesystem::is_regular_file(ImportRoot / "Renamed.png"));
	ASSERT_TRUE(Durin::Asset::LoadAsset(RenamedPath, Loaded));
	EXPECT_EQ(Loaded->GetSourceFile(), "Renamed.png");
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RenamedPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RenamedPath));
	EXPECT_FALSE(std::filesystem::exists(ImportRoot / "Renamed.png"));
}

TEST(FTexture2DTests, UsagePresetsChooseColorSpaceAndMipFilter)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "UsagePresetSource.png";
	WriteTextureFixture(Source);

	struct FExpectedPreset
	{
		Durin::ETextureUsage Usage;
		std::string_view AssetName;
		Durin::EPixelFormat PixelFormat;
		std::array<Durin::uint8, 4> LastMip;
	};
	const std::array Presets = {
		FExpectedPreset{Durin::ETextureUsage::Color, "PresetColor", Durin::EPixelFormat::SRGBA8_UNORM, {188, 0, 0, 128}},
		FExpectedPreset{Durin::ETextureUsage::Normal, "PresetNormal", Durin::EPixelFormat::RGBA8_UNORM, {128, 37, 37, 128}},
		FExpectedPreset{Durin::ETextureUsage::DataMask, "PresetDataMask", Durin::EPixelFormat::RGBA8_UNORM, {128, 0, 0, 128}}
	};

	for (const FExpectedPreset& Preset : Presets)
	{
		Durin::FTexture2DImportSettings Settings;
		Settings.Usage = Preset.Usage;
		const std::string AssetPathString = std::format("/TextureImportTests/{}", Preset.AssetName);
		Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), AssetPathString, Settings);
		ASSERT_TRUE(Result) << Result.Message;
		ASSERT_NE(Result.Asset, nullptr);
		EXPECT_EQ(Result.Asset->GetUsage(), Preset.Usage);
		EXPECT_EQ(Result.Asset->IsSRGB(), Preset.Usage == Durin::ETextureUsage::Color);
		ASSERT_NE(Result.Asset->GetPlatformData(), nullptr);
		EXPECT_EQ(Result.Asset->GetPlatformData()->PixelFormat, Preset.PixelFormat);
		ASSERT_EQ(Result.Asset->GetPlatformData()->Mips.size(), 2u);
		EXPECT_EQ(Result.Asset->GetPlatformData()->Mips.back().Pixels,
			(std::vector<Durin::uint8>(Preset.LastMip.begin(), Preset.LastMip.end())));

		Durin::FAssetPath AssetPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(AssetPathString, AssetPath));
		ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
		Durin::DTexture2D* Loaded = nullptr;
		ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
		ASSERT_NE(Loaded, nullptr);
		EXPECT_EQ(Loaded->GetUsage(), Preset.Usage);
		EXPECT_EQ(Loaded->GetPlatformData()->Mips.back().Pixels,
			(std::vector<Durin::uint8>(Preset.LastMip.begin(), Preset.LastMip.end())));
		ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
		ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
	}
}

TEST(FTexture2DTests, BuildsCompleteNpotMipChainWithoutDroppingEdges)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "NpotTextureSource.tga";
	WriteNpotTextureFixture(Source);
	Durin::FTexture2DImportSettings Settings;
	Settings.Usage = Durin::ETextureUsage::DataMask;
	Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), "/TextureImportTests/Npot", Settings);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	const Durin::FTexturePlatformData* PlatformData = Result.Asset->GetPlatformData();
	ASSERT_NE(PlatformData, nullptr);
	ASSERT_EQ(PlatformData->Mips.size(), 3u);
	EXPECT_EQ(std::pair(PlatformData->Mips[0].Width, PlatformData->Mips[0].Height), std::pair(5u, 3u));
	EXPECT_EQ(std::pair(PlatformData->Mips[1].Width, PlatformData->Mips[1].Height), std::pair(2u, 1u));
	EXPECT_EQ(std::pair(PlatformData->Mips[2].Width, PlatformData->Mips[2].Height), std::pair(1u, 1u));
	EXPECT_EQ(PlatformData->Mips[2].Pixels, (std::vector<Durin::uint8>{43, 43, 43, 255}));

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Npot", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTexture2DTests, PreservesLinearBuildSettingAndRebuildsColorSpace)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "LinearTextureSource.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportSettings Settings;
	Settings.bSRGB = false;
	Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), "/TextureImportTests/Linear", Settings);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	EXPECT_FALSE(Result.Asset->IsSRGB());
	EXPECT_EQ(Result.Asset->GetUsage(), Durin::ETextureUsage::Color);
	ASSERT_NE(Result.Asset->GetPlatformData(), nullptr);
	EXPECT_EQ(Result.Asset->GetPlatformData()->PixelFormat, Durin::EPixelFormat::RGBA8_UNORM);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Linear", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_FALSE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::RGBA8_UNORM);
	EXPECT_EQ(Loaded->GetPlatformData()->Mips.back().Pixels, (std::vector<Durin::uint8>{128, 0, 0, 128}));

	const std::vector<Durin::uint8> SourcePixels = Loaded->GetSourceData()->Pixels;
	std::string Error;
	ASSERT_TRUE(Loaded->SetSRGB(true, Error)) << Error;
	EXPECT_TRUE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::SRGBA8_UNORM);
	EXPECT_EQ(Loaded->GetPlatformData()->Mips[0].Pixels, SourcePixels);
	EXPECT_EQ(Loaded->GetPlatformData()->Mips.back().Pixels, (std::vector<Durin::uint8>{188, 0, 0, 128}));
	ASSERT_TRUE(Loaded->SetUsage(Durin::ETextureUsage::Normal, Error)) << Error;
	EXPECT_EQ(Loaded->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->Mips.back().Pixels, (std::vector<Durin::uint8>{128, 37, 37, 128}));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTexture2DTests, ReflectedBuildSettingsRebuildTransactionallyAndSupportUndoRedo)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TransactionalTextureSource.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(
		Source.generic_string(), "/TextureImportTests/Transactional");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTexture2D* Texture = Result.Asset;
	ASSERT_NE(Texture, nullptr);
	ASSERT_NE(Texture->GetPackage(), nullptr);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());

	Durin::FProperty* UsageProperty = Texture->GetClass()->FindPropertyByName("Usage");
	Durin::FProperty* SRGBProperty = Texture->GetClass()->FindPropertyByName("bSRGB");
	ASSERT_NE(UsageProperty, nullptr);
	ASSERT_NE(SRGBProperty, nullptr);
	Durin::FReflectedPropertyView PropertyView;
	Durin::FEditorTransactionManager Transactions;
	std::string Error;
	const Durin::FReflectedPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};

	const auto SubmitUsage = [&](Durin::ETextureUsage Usage) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::FReflectedPropertyEditTarget::ForMember(Texture, UsageProperty),
			[Usage](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				static_cast<Durin::FEnumProperty*>(Property)->SetValueFromUInt64(
					Container, static_cast<Durin::uint64>(Usage), ArrayIndex);
			}, false);
	};
	const auto SubmitSRGB = [&](bool bSRGB) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::FReflectedPropertyEditTarget::ForMember(Texture, SRGBProperty),
			[bSRGB](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				*Property->ContainerPtrToValuePtr<bool>(Container, ArrayIndex) = bSRGB;
			}, false);
	};

	const Durin::uint64 InitialRevision = Texture->GetBuildRevision();
	ASSERT_TRUE(SubmitUsage(Durin::ETextureUsage::Normal)) << Error;
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Texture->IsSRGB());
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::RGBA8_UNORM);
	EXPECT_EQ(Texture->GetPlatformData()->Mips.back().Pixels, (std::vector<Durin::uint8>{128, 37, 37, 128}));
	EXPECT_GT(Texture->GetBuildRevision(), InitialRevision);
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_TRUE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::SRGBA8_UNORM);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Texture->IsSRGB());

	ASSERT_TRUE(SubmitSRGB(true)) << Error;
	EXPECT_TRUE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::SRGBA8_UNORM);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FALSE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::RGBA8_UNORM);

	Error.clear();
	EXPECT_FALSE(SubmitUsage(static_cast<Durin::ETextureUsage>(255)));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::RGBA8_UNORM);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Transactional", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTexture2DTests, RejectsUnsupportedSourceWithoutCreatingAsset)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "UnsupportedTexture.gif";
	std::ofstream(Source, std::ios::binary | std::ios::trunc) << "not an image";
	Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), "/TextureImportTests/Unsupported");
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Asset, nullptr);
	EXPECT_FALSE(Result.Message.empty());

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Unsupported", AssetPath));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(AssetPath), nullptr);
}

TEST(FTexture2DTests, FailureState_RecordsMissingSourceOnPostLoad)
{
	InitializeDObjectSystem();
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/FailureTestMissing", AssetPath));
	Durin::DTexture2D* Texture = nullptr;
	Durin::Asset::FAssetResult CreateResult = Durin::Asset::CreateAsset(AssetPath, Texture);
	ASSERT_TRUE(CreateResult) << CreateResult.Message;
	ASSERT_NE(Texture, nullptr);
	// At creation time, the build has not run.
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Unbuilt);
	// PostLoad with an empty source file.
	std::string Error;
	EXPECT_FALSE(Texture->PostLoad(Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::MissingSource);
	EXPECT_FALSE(Texture->GetLastBuildError().empty());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
}

TEST(FTexture2DTests, FailureState_ReadyAfterSuccessfulPostLoad)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureFailureMount";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/TextureFailureTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "FailureReadySource.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), "/TextureFailureTests/Ready");
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Result.Asset->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_TRUE(Result.Asset->GetLastBuildError().empty());

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureFailureTests/Ready", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTexture2DTests, MissingSourceInvalidatesDerivedDataAndCanRecover)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureInvalidateMount";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/TextureInvalidateTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "InvalidateSource.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), "/TextureInvalidateTests/Invalid");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTexture2D* Texture = Result.Asset;
	ASSERT_NE(Texture, nullptr);
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	ASSERT_NE(Texture->GetSourceData(), nullptr);
	ASSERT_NE(Texture->GetPlatformData(), nullptr);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureInvalidateTests/Invalid", AssetPath));
	const std::filesystem::path CopiedSource =
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureInvalidateMount" / "Invalid.png";
	ASSERT_TRUE(std::filesystem::remove(CopiedSource));

	std::string Error;
	EXPECT_FALSE(Texture->PostLoad(Error));
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::MissingSource);
	EXPECT_EQ(Texture->GetSourceData(), nullptr);
	EXPECT_EQ(Texture->GetPlatformData(), nullptr);
	EXPECT_FALSE(Texture->GetLastBuildError().empty());

	WriteTextureFixture(CopiedSource);
	ASSERT_TRUE(Texture->PostLoad(Error)) << Error;
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_NE(Texture->GetSourceData(), nullptr);
	EXPECT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_TRUE(Texture->GetLastBuildError().empty());

	ASSERT_TRUE(Durin::Asset::SavePackage(Texture->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTexture2DTests, StatusEnumsExposeSharedDisplayMetadata)
{
	InitializeDObjectSystem();
	Durin::DEnum* BuildStatusEnum = Durin::FindEnumByQualifiedName("Durin::ETextureBuildStatus");
	Durin::DEnum* ResourceStateEnum = Durin::FindEnumByQualifiedName("Durin::ERenderResourceState");
	ASSERT_NE(BuildStatusEnum, nullptr);
	ASSERT_NE(ResourceStateEnum, nullptr);
	EXPECT_EQ(BuildStatusEnum->GetDisplayName(), "Texture Build Status");
	EXPECT_EQ(ResourceStateEnum->GetDisplayName(), "Render Resource State");

	const Durin::FEnumValue* Unbuilt = BuildStatusEnum->FindValueRecordByValue(
		static_cast<Durin::uint64>(Durin::ETextureBuildStatus::Unbuilt));
	const Durin::FEnumValue* MissingSource = BuildStatusEnum->FindValueRecordByValue(
		static_cast<Durin::uint64>(Durin::ETextureBuildStatus::MissingSource));
	const Durin::FEnumValue* Building = ResourceStateEnum->FindValueRecordByValue(
		static_cast<Durin::uint64>(Durin::ERenderResourceState::Building));
	ASSERT_NE(Unbuilt, nullptr);
	ASSERT_NE(MissingSource, nullptr);
	ASSERT_NE(Building, nullptr);
	EXPECT_EQ(Unbuilt->DisplayName, "Not Built");
	EXPECT_EQ(MissingSource->DisplayName, "Missing Source");
	EXPECT_EQ(Building->DisplayName, "Building");
	EXPECT_EQ(BuildStatusEnum->FindValueRecordByValue(255), nullptr);
}
