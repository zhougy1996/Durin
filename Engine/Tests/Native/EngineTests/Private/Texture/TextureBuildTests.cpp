#include "NativeAssetTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "TextureTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "Texture/Texture2DBuild.h"
#include "Texture/TextureCubeBuildProvider.h"
#include "Texture/TextureDerivedData.h"
#include "Runtime/Engine/Private/Texture/TextureDerivedDataKey.h"
#include "Diagnostics/TexturePayloadInspection.h"
#include "Texture/VolumeTexture.h"
#include "Texture/VolumeTextureBuildProvider.h"
#include "Texture/TextureBuilder.h"
#include "Texture/VolumeTextureBuilder.h"
#include "DObject/DefaultDeltaPlan.h"
#include "Asset/EditorBulkDataStorage.h"

namespace
{
	class FTestTexture2DBuildProvider final : public Durin::ITexture2DBuildProvider
	{
	public:
		auto GetDescriptor() const -> Durin::FTexture2DBuildProviderDescriptor override
		{
			return {.ProducerIdentity = "Tests.Texture2D", .BuilderVersion = 7};
		}

		auto Build(
			const Durin::FTexture2DRecipeBuildRequest&,
			Durin::FTexture2DRecipeBuildProduct& OutProduct,
			const Durin::FTexture2DRecipeExecutionControl*) -> Durin::FTexture2DBuildResult override
		{
			OutProduct = {};
			return {Durin::ETexture2DBuildStatus::Succeeded, {}};
		}
	};
}

TEST(FTextureSourceTests, ResolvesVolumeMipsAndKeepsReadHandlesAlive)
{
	const Durin::FTextureSourceBlock Block{
		.Width = 4, .Height = 2, .Depth = 4, .NumSlices = 1};
	const Durin::FTextureSourceLayer Layer{
		.Format = Durin::ETextureSourceFormat::R8_UNORM, .NumMips = 3};
	Durin::FTextureSource Source;
	const Durin::FByteBuffer Bytes(37, std::byte{3});
	ASSERT_TRUE(Source.InitLayered(Durin::ETextureSourceKind::Volume,
		std::span(&Block, 1), std::span(&Layer, 1),
		Durin::ETextureSourceGammaSpace::Linear, Bytes, 1));
	ASSERT_TRUE(Source.IsValid());
	const Durin::FTextureSource::FMipData Mips = Source.GetMipData();
	ASSERT_TRUE(Mips.IsValid());

	Durin::FTextureSourceMipInfo Mip = Source.GetMipInfo(0, 0, 1);
	ASSERT_TRUE(Mip.IsValid());
	EXPECT_EQ(Mip.ImageInfo.Width, 2u);
	EXPECT_EQ(Mip.ImageInfo.Height, 1u);
	EXPECT_EQ(Mip.ImageInfo.Depth, 2u);
	EXPECT_EQ(Mip.PayloadOffset, 32u);
	EXPECT_EQ(Mip.PayloadSize, 4u);
	EXPECT_FALSE(Source.GetMipInfo(1, 0, 0).IsValid());

	const Durin::Image::FImageView View = Mips.GetMipImage(0, 0, 2);
	ASSERT_TRUE(View.IsValid());
	ASSERT_EQ(View.GetPixels().size(), 1u);
	Source.ReleaseSourceMemory();
	EXPECT_EQ(View.GetPixels()[0], std::byte{3});
}

TEST(FTextureSourceTests, InitApisKeepCanonicalIdentityAcrossLosslessStorage)
{
	Durin::Image::FImage Image;
	const Durin::FByteBuffer Pixels(4 * 4 * 4, std::byte{0x2a});
	ASSERT_TRUE(Durin::Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Durin::Image::ERawImageFormat::RGBA8,
		.GammaSpace = Durin::Image::EImageGammaSpace::SRGB},
		Pixels, Image));
	Durin::FTextureSource Raw;
	Durin::FTextureSource Compressed;
	ASSERT_TRUE(Raw.Init2D(Image.GetView(), 4, 0,
		Durin::ETextureSourceCompression::Raw));
	ASSERT_TRUE(Compressed.Init2D(Image.GetView(), 4, 0,
		Durin::ETextureSourceCompression::RunLength));
	EXPECT_EQ(Compressed.GetCompression(),
		Durin::ETextureSourceCompression::RunLength);
	EXPECT_LT(Compressed.GetBulkData().GetPayloadSize(), Pixels.size());
	EXPECT_EQ(Raw.GetIdentity(), Compressed.GetIdentity());
	const Durin::FTextureSource::FMipData FirstMips = Compressed.GetMipData();
	const Durin::FTextureSource::FMipData SharedMips = Compressed.GetMipData();
	ASSERT_TRUE(FirstMips.IsValid());
	EXPECT_TRUE(FirstMips.GetData().SharesStorageWith(SharedMips.GetData()));
	const Durin::FSharedByteBuffer BaseMip = FirstMips.GetMipData(0, 0, 0);
	EXPECT_EQ(BaseMip.GetSize(), Pixels.size());
	EXPECT_TRUE(BaseMip.SharesStorageWith(FirstMips.GetData()));
	Compressed.ReleaseSourceMemory();
	const Durin::FTextureSource::FMipData ReloadedMips = Compressed.GetMipData();
	ASSERT_TRUE(ReloadedMips.IsValid());
	EXPECT_FALSE(FirstMips.GetData().SharesStorageWith(ReloadedMips.GetData()));
	EXPECT_TRUE(std::ranges::equal(FirstMips.GetData().GetBytes(), Pixels));

	Durin::Image::FImage UpdatedImage;
	const Durin::FByteBuffer UpdatedPixels(4 * 4 * 4, std::byte{0x33});
	ASSERT_TRUE(Durin::Image::FImage::TryCreate(Image.GetInfo(),
		UpdatedPixels, UpdatedImage));
	ASSERT_TRUE(Compressed.Init2D(UpdatedImage.GetView(), 4));
	const Durin::FTextureSource::FMipData UpdatedMips = Compressed.GetMipData();
	ASSERT_TRUE(UpdatedMips.IsValid());
	EXPECT_FALSE(FirstMips.GetData().SharesStorageWith(UpdatedMips.GetData()));
	EXPECT_TRUE(std::ranges::equal(UpdatedMips.GetData().GetBytes(), UpdatedPixels));
	EXPECT_TRUE(std::ranges::equal(FirstMips.GetData().GetBytes(), Pixels));
}

TEST(FTextureSourceTests, ZstdRecompressionPreservesIdentityBuffersAndRollback)
{
	using namespace Durin;
	for (const auto Format : {ETextureSourceFormat::RGBA8, ETextureSourceFormat::RGBA32_FLOAT})
	{
		const FTextureSourceBlock Block{.Width = 16, .Height = 16, .Depth = 2, .NumSlices = 2};
		const FTextureSourceLayer Layers[] = {{.Format = Format, .NumMips = 2},
			{.Format = ETextureSourceFormat::R8_UNORM, .NumMips = 1}};
		const size_t Stride = Format == ETextureSourceFormat::RGBA8 ? 4 : 16;
		FByteBuffer Pixels((16 * 16 * 4 + 8 * 8 * 2) * Stride + 16 * 16 * 4);
		for (size_t Index = 0; Index < Pixels.size(); ++Index)
			Pixels[Index] = static_cast<std::byte>(Index % 251);
		FTextureSource Source;
		ASSERT_TRUE(Source.InitLayered(ETextureSourceKind::Volume, std::span(&Block, 1),
			Layers, ETextureSourceGammaSpace::Linear, Pixels, 4, 0, ETextureSourceCompression::Raw));
		const auto Identity = Source.GetIdentity();
		const auto Instance = Source.GetBulkData().GetInstanceId();
		const auto Prior = Source.GetMipData().GetData();
		const auto Key = BuildTexture2DDerivedDataKey({.SourceIdentity = Identity,
			.TargetPlatform = ECookTargetPlatform::Win64, .TargetProfile = ECookTargetProfile::Game});
		ASSERT_TRUE(Source.Recompress());
		EXPECT_EQ(Source.GetCompression(), ETextureSourceCompression::Zstd);
		EXPECT_EQ(Source.GetIdentity(), Identity);
		EXPECT_EQ(Source.GetBulkData().GetInstanceId(), Instance);
		EXPECT_EQ(BuildTexture2DDerivedDataKey({.SourceIdentity = Source.GetIdentity(),
			.TargetPlatform = ECookTargetPlatform::Win64, .TargetProfile = ECookTargetProfile::Game}), Key);
		EXPECT_TRUE(std::ranges::equal(Prior.GetBytes(), Pixels));
		Source.ReleaseSourceMemory();
		const auto Reloaded = Source.GetMipData().GetData();
		EXPECT_TRUE(std::ranges::equal(Reloaded.GetBytes(), Pixels));
		const auto Stored = Source.GetBulkData().GetPayload().Wait().Buffer;
		ASSERT_TRUE(Source.Recompress());
		EXPECT_TRUE(Source.GetBulkData().GetPayload().Wait().Buffer.SharesStorageWith(Stored));
		EXPECT_FALSE(Source.Recompress(static_cast<ETextureSourceCompression>(255)));
		EXPECT_TRUE(Source.GetMipData().GetData().SharesStorageWith(Reloaded));
		EXPECT_EQ(Source.GetIdentity(), Identity);
		ASSERT_TRUE(Source.Recompress(ETextureSourceCompression::RunLength));
		ASSERT_TRUE(Source.Recompress(ETextureSourceCompression::Raw));
		EXPECT_TRUE(std::ranges::equal(Source.GetMipData().GetData().GetBytes(), Pixels));
	}
}

TEST(FTextureSourceTests, ZstdFallsBackAndRejectsDamagedFrames)
{
	using namespace Durin;
	Image::FImage Image;
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 1, .Height = 1,
		.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(4, std::byte{9}), Image));
	FTextureSource Source;
	ASSERT_TRUE(Source.Init2D(Image.GetView(), 4));
	EXPECT_EQ(Source.GetCompression(), ETextureSourceCompression::Raw);
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 64, .Height = 64,
		.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(64 * 64 * 4, std::byte{9}), Image));
	ASSERT_TRUE(Source.Init2D(Image.GetView(), 4));
	ASSERT_EQ(Source.GetCompression(), ETextureSourceCompression::Zstd);
	const auto Read = Source.GetBulkData().GetPayload().Wait();
	ASSERT_TRUE(Read);
	const FByteBuffer Valid(Read.Buffer.GetBytes().begin(), Read.Buffer.GetBytes().end());
	std::vector<FByteBuffer> Damaged;
	for (size_t Size = 1; Size < Valid.size(); ++Size)
		Damaged.emplace_back(Valid.begin(), Valid.begin() + Size);
	Damaged.push_back(Valid); Damaged.back().push_back(std::byte{0});
	Damaged.push_back(Valid); Damaged.back().insert(Damaged.back().end(), Valid.begin(), Valid.end());
	Damaged.push_back(Valid); Damaged.back()[0] ^= std::byte{1};
	Damaged.push_back(Valid); Damaged.back().back() ^= std::byte{1};
	// The single-segment frame uses a two-byte content size. Add a dictionary ID,
	// alter its decoded length, and advertise an excessive non-single window.
	ASSERT_EQ(std::to_integer<uint8>(Valid[4]), 0x60u);
	Damaged.push_back(Valid); Damaged.back()[4] |= std::byte{1};
	Damaged.back().insert(Damaged.back().begin() + 5, std::byte{1});
	Damaged.push_back(Valid); Damaged.back()[5] ^= std::byte{1};
	Damaged.push_back(Valid); Damaged.back()[4] = std::byte{0};
	Damaged.back()[5] = std::byte{0xf8};
	for (const auto& Bytes : Damaged)
	{
		Source.ReleaseSourceMemory();
		ASSERT_TRUE(const_cast<FEditorBulkData&>(Source.GetBulkData()).UpdatePayload(Bytes));
		EXPECT_FALSE(Source.GetMipData().IsValid());
		const auto Before = Source.GetBulkData().GetPayloadId();
		EXPECT_FALSE(Source.Recompress());
		EXPECT_EQ(Source.GetBulkData().GetPayloadId(), Before);
	}
}

TEST(FTextureSourceTests, RejectsUnknownCodecAndCanonicalHashMismatch)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Image::FImage Image;
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 16, .Height = 16,
		.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(1024, std::byte{31}), Image));
	FTextureSource Source;
	ASSERT_TRUE(Source.Init2D(Image.GetView(), 4));
	auto* Codec = FTextureSource::StaticStruct()->FindPropertyByName("Compression", false);
	auto* Hash = FTextureSource::StaticStruct()->FindPropertyByName("CanonicalPayloadHashLow", false);
	ASSERT_NE(Codec, nullptr); ASSERT_NE(Hash, nullptr);
	*Codec->ContainerPtrToValuePtr<ETextureSourceCompression>(&Source) = static_cast<ETextureSourceCompression>(255);
	EXPECT_FALSE(Source.IsValid()); EXPECT_FALSE(Source.GetMipData().IsValid());
	EXPECT_FALSE(Source.Recompress());
	*Codec->ContainerPtrToValuePtr<ETextureSourceCompression>(&Source) = ETextureSourceCompression::Zstd;
	*Hash->ContainerPtrToValuePtr<uint64>(&Source) ^= 1;
	EXPECT_FALSE(Source.GetMipData().IsValid()); EXPECT_FALSE(Source.Recompress());
}

TEST(FTextureSourceTests, StorageOnlyCommitKeepsCookedPixelsAndOwner)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Texture = NewObject<DTexture2D>(nullptr, "StorageOnlyTexture");
	Image::FImage Image;
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 16, .Height = 16,
		.Format = Image::ERawImageFormat::RGBA8, .GammaSpace = Image::EImageGammaSpace::SRGB},
		FByteBuffer(16 * 16 * 4, std::byte{71}), Image));
	FTextureSource Source;
	ASSERT_TRUE(Source.Init2D(Image.GetView(), 4, 0, ETextureSourceCompression::Raw));
	Texture->SetSource(Source);
	const auto Build = [&]() {
		const auto Input = Texture->CreateBuildRequest({});
		FTexturePlatformData Platform;
		EXPECT_TRUE(TextureBuilder::BuildMipChain(Input.SourceMips, ETextureUsage::Color, true, Platform));
		FByteBuffer Bytes;
		FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::DerivedDataPayload,
			{.Target = {"Win64", "Game"}});
		Platform.Serialize(Writer);
		EXPECT_FALSE(Writer.HasError());
		return Bytes;
	};
	const auto Before = Build();
	ASSERT_TRUE(Source.Recompress());
	ASSERT_TRUE(Texture->ReplaceSourceStorage(Source));
	EXPECT_EQ(Texture->GetSource().GetOwner(), Texture);
	EXPECT_EQ(Build(), Before);
	FTextureSource Different;
	ASSERT_TRUE(Different.Init2D(Image.GetView(), 4));
	EXPECT_FALSE(Texture->ReplaceSourceStorage(Different));
}

TEST(FTextureSourceTests, Texture2DPreservesSuppliedMipChainForRecipeBuild)
{
	InitializeDObjectSystem();
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "SuppliedMipTexture");
	ASSERT_NE(Texture, nullptr);
	std::vector<Durin::Image::FImage> Images(3);
	std::vector<Durin::Image::FImageView> Views;
	for (uint32 Index = 0; Index < 3; ++Index)
	{
		const uint32 Size = 4u >> Index;
		ASSERT_TRUE(Durin::Image::FImage::TryCreate({.Width = Size, .Height = Size,
			.Format = Durin::Image::ERawImageFormat::RGBA8,
			.GammaSpace = Durin::Image::EImageGammaSpace::SRGB},
			Durin::FByteBuffer(Size * Size * 4, static_cast<std::byte>(Index * 70)),
			Images[Index]));
		Views.push_back(Images[Index].GetView());
	}
	auto PreparedSource = Durin::PrepareTexture2DSourceMipChain(Views, 4, 0);
	ASSERT_TRUE(PreparedSource);
	Texture->SetSource(std::move(*PreparedSource));
	const auto Input = Texture->CreateBuildRequest({});
	ASSERT_EQ(Input.SourceMips.size(), 3u);
	Durin::FTexturePlatformData Platform;
	const Durin::FTexture2DBuildResult BuildResult =
		Durin::TextureBuilder::BuildMipChain(Input.SourceMips,
		Durin::ETextureUsage::Color, true, Platform, 0,
		Durin::ETextureCompressionQuality::Normal,
		Durin::ETextureAlphaMipMode::Average, 0.5f, nullptr);
	ASSERT_TRUE(BuildResult) << BuildResult.Diagnostic;
	EXPECT_EQ(Platform.Mips.size(), 3u);
	EXPECT_EQ(Platform.Mips.back().Width, 1u);
}

TEST(FTextureSourceTests, ImageRecipeRetainsAlphaPresentOnlyInSuppliedLowerMip)
{
	std::vector<Durin::Image::FImage> Mips(2);
	ASSERT_TRUE(Durin::Image::FImage::TryCreate({.Width = 2, .Height = 2,
		.Format = Durin::Image::ERawImageFormat::RGBA8},
		Durin::FByteBuffer(16, std::byte{255}), Mips[0]));
	ASSERT_TRUE(Durin::Image::FImage::TryCreate({.Width = 1, .Height = 1,
		.Format = Durin::Image::ERawImageFormat::RGBA8},
		Durin::FByteBuffer(4, std::byte{0}), Mips[1]));
	Durin::FTexturePlatformData Platform;
	const auto Result = Durin::TextureBuilder::BuildMipChain(
		Mips, Durin::ETextureUsage::Color, true, Platform);
	ASSERT_TRUE(Result) << Result.Diagnostic;
	EXPECT_EQ(Platform.PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	ASSERT_EQ(Platform.Mips.size(), 2u);
}

TEST(FTextureSourceTests, SourcePreparationFailurePreservesInstalledState)
{
	InitializeDObjectSystem();
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "AtomicTextureSourceEdit");
	ASSERT_NE(Texture, nullptr);
	Durin::Image::FImage ImportedImage;
	EXPECT_TRUE(Durin::Image::FImage::TryCreate({.Width = 1, .Height = 1,
		.Format = Durin::Image::ERawImageFormat::RGBA8}, Durin::FByteBuffer(4, std::byte{1}), ImportedImage));
	Durin::FTextureSource Imported;
	EXPECT_TRUE(Imported.Init2D(ImportedImage.GetView(), 4));
	Texture->SetSource(Imported);
	const Durin::FXxHash128 Identity = Texture->GetSource().GetIdentity();
	EXPECT_FALSE(Durin::Image::FImage::TryCreate({.Width = 1, .Height = 1,
		.Format = Durin::Image::ERawImageFormat::RGBA8},
		Durin::FByteBuffer(3, std::byte{2}), ImportedImage));
	EXPECT_FALSE(Durin::PrepareTexture2DSourceMipChain({}, 4, 0));
	EXPECT_EQ(Texture->GetSource().GetIdentity(), Identity);
}

TEST(FTextureSourceTests, ResolvesBlockLayerMipOrderingAndRejectsOversizedLayouts)
{
	const std::vector<Durin::FTextureSourceBlock> Blocks = {
		{.Width = 2, .Height = 1},
		{.Width = 1, .Height = 1, .NumSlices = 2}};
	const std::vector<Durin::FTextureSourceLayer> Layers = {
		{.Format = Durin::ETextureSourceFormat::R8_UNORM, .NumMips = 2},
		{.Format = Durin::ETextureSourceFormat::RG8_UNORM, .NumMips = 1}};
	Durin::FTextureSource Source;
	const Durin::FByteBuffer Bytes(15, std::byte{4});
	ASSERT_TRUE(Source.InitLayered(Durin::ETextureSourceKind::TextureArray,
		Blocks, Layers, Durin::ETextureSourceGammaSpace::Linear, Bytes, 2));
	Durin::FTextureSourceMipInfo Mip = Source.GetMipInfo(1, 0, 1);
	ASSERT_TRUE(Mip.IsValid());
	EXPECT_EQ(Mip.PayloadOffset, 9u);
	EXPECT_EQ(Mip.PayloadSize, 2u);
	EXPECT_EQ(Mip.ImageInfo.SliceCount, 2u);
	Mip = Source.GetMipInfo(1, 1, 0);
	ASSERT_TRUE(Mip.IsValid());
	EXPECT_EQ(Mip.PayloadOffset, 11u);
	EXPECT_EQ(Mip.PayloadSize, 4u);

	const Durin::FTextureSourceBlock Oversized{
		.Width = std::numeric_limits<uint32>::max(),
		.Height = std::numeric_limits<uint32>::max()};
	const Durin::FTextureSourceLayer OversizedLayer{
		.Format = Durin::ETextureSourceFormat::RGBA32_FLOAT};
	Durin::FTextureSource OversizedSource;
	EXPECT_FALSE(OversizedSource.InitLayered(Durin::ETextureSourceKind::Texture2D,
		std::span(&Oversized, 1), std::span(&OversizedLayer, 1),
		Durin::ETextureSourceGammaSpace::Linear, {}));
}

TEST(FTextureSourceTests, TextureOwnsSourceAndBuildInputCapturesIdentity)
{
	InitializeDObjectSystem();
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "OwnedTextureSource");
	ASSERT_NE(Texture, nullptr);
	EXPECT_EQ(Texture->GetSource().GetOwner(), Texture);
	Durin::Image::FImage ImportedImage;
	EXPECT_TRUE(Durin::Image::FImage::TryCreate({.Width = 1, .Height = 1,
		.Format = Durin::Image::ERawImageFormat::RGBA8}, Durin::FByteBuffer(4, std::byte{8}), ImportedImage));
	Durin::FTextureSource Imported;
	EXPECT_TRUE(Imported.Init2D(ImportedImage.GetView(), 4));
	Texture->SetSource(Imported);
	EXPECT_EQ(Texture->GetSource().GetOwner(), Texture);
	const auto BuildInput = Texture->CreateBuildRequest({});
	ASSERT_FALSE(BuildInput.SourceMips.empty());
	EXPECT_EQ(BuildInput.SourceIdentity,
		Texture->GetSource().GetIdentity());
	Texture->SetBuildSettings(Durin::ETextureUsage::Color, true, 0,
		Durin::ETextureCompressionQuality::High,
		Durin::ETextureAlphaMipMode::Average, 0.5f);
}

TEST(FTextureSourceTests, ImageBuildInputSurvivesSourceReplacementWithoutPixelCopies)
{
	InitializeDObjectSystem();
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "ImagePayloadOwner");
	Durin::Image::FImage PixelsImage;
	ASSERT_TRUE(Durin::Image::FImage::TryCreate({.Width = 2, .Height = 2,
		.Format = Durin::Image::ERawImageFormat::RGBA8}, Durin::FByteBuffer(16, std::byte{17}), PixelsImage));
	Durin::FTextureSource Pixels;
	ASSERT_TRUE(Pixels.Init2D(PixelsImage.GetView(), 4));
	Texture->SetSource(Pixels);
	const auto Input = Texture->CreateBuildRequest({.MaxResolution = 1});
	ASSERT_EQ(Input.SourceMips.size(), 1u);
	const auto SourceBuffer = Texture->GetSource().GetMipData().GetData();
	EXPECT_TRUE(Input.SourceMips.front().GetView().GetBuffer().SharesStorageWith(SourceBuffer));
	ASSERT_TRUE(Durin::Image::FImage::TryCreate({.Width = 2, .Height = 2,
		.Format = Durin::Image::ERawImageFormat::RGBA8},
		Durin::FByteBuffer(16, std::byte{29}), PixelsImage));
	ASSERT_TRUE(Pixels.Init2D(PixelsImage.GetView(), 4));
	Texture->SetSource(Pixels);
	EXPECT_NE(Input.SourceIdentity, Texture->GetSource().GetIdentity());
	EXPECT_EQ(Input.SourceMips.front().GetPixels().front(), std::byte{17});
	EXPECT_EQ(Input.Settings.MaxResolution, 1u);
}

TEST(FTextureSourceTests, RebuildPreservesAuthoredStorageAndRejectsMismatchedReplacement)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "ImagePayloadRebuildDdc");
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/ImagePayloadRebuild", Path));
	Durin::DTexture2D* Texture = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Texture));
	Durin::Image::FImage Image;
	ASSERT_TRUE(Durin::Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Durin::Image::ERawImageFormat::RGBA8,
		.GammaSpace = Durin::Image::EImageGammaSpace::SRGB},
		Durin::FByteBuffer(64, std::byte{255}), Image));
	Durin::FTextureSource Source;
	ASSERT_TRUE(Source.Init2D(Image.GetView(), 4, 0, Durin::ETextureSourceCompression::RunLength));
	std::string Error;
	Texture->SetSource(Source);
	const auto Identity = Texture->GetSource().GetIdentity();
	const auto Buffer = Texture->GetSource().GetMipData().GetData();
	ASSERT_TRUE(Durin::BuildTexture2DSynchronously(*Texture, Texture->CreateBuildRequest({}),
		{}, Error)) << Error;
	EXPECT_EQ(Texture->GetSource().GetIdentity(), Identity);
	EXPECT_EQ(Texture->GetSource().GetCompression(), Durin::ETextureSourceCompression::RunLength);
	EXPECT_EQ(Texture->GetSource().GetGammaSpace(), Durin::ETextureSourceGammaSpace::SRGB);
	EXPECT_TRUE(Texture->GetSource().GetMipData().GetData().SharesStorageWith(Buffer));
	const auto* Platform = Texture->GetPlatformData();
	Durin::Image::FImage DifferentImage;
	ASSERT_TRUE(Durin::Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Durin::Image::ERawImageFormat::RGBA8}, Durin::FByteBuffer(64, std::byte{41}), DifferentImage));
	Durin::FTextureSource Different;
	ASSERT_TRUE(Different.Init2D(DifferentImage.GetView(), 4));
	EXPECT_FALSE(Durin::BuildTexture2DSynchronously(*Texture, Texture->CreateBuildRequest({}),
		{.SourceReplacement = Different}, Error));
	EXPECT_EQ(Texture->GetSource().GetIdentity(), Identity);
	EXPECT_EQ(Texture->GetPlatformData(), Platform);
	ASSERT_TRUE(Durin::BuildTexture2DSynchronously(*Texture,
		Durin::MakeTexture2DBuildRequest(Different),
		{.SourceReplacement = Different}, Error)) << Error;
	EXPECT_EQ(Texture->GetSource().GetIdentity(), Different.GetIdentity());
}

TEST(FTexturePlatformDataTests, EnsureDoesNotBuildMissingAuthoredData)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	auto ExpectMissingAuthoredData = []<typename TTexture>() {
		auto* Texture = Durin::NewObject<TTexture>(nullptr, "MissingAuthoredPlatformData");
		Durin::DTexture& Base = *Texture;
		const auto RevisionIdentity = Texture->GetPlatformDataShared();
		EXPECT_EQ(Texture->GetPlatformData(), nullptr);
		EXPECT_FALSE(Base.HasPlatformData());
		EXPECT_EQ(Base.GetAssetImportData(), nullptr);
		EXPECT_EQ(std::as_const(Base).GetAssetImportData(), nullptr);
		EXPECT_FALSE(Base.EnsurePlatformDataLoadedBlocking());
		EXPECT_EQ(Texture->GetPlatformData(), nullptr);
	};
	ExpectMissingAuthoredData.template operator()<Durin::DTexture2D>();
	ExpectMissingAuthoredData.template operator()<Durin::DTextureCube>();
	ExpectMissingAuthoredData.template operator()<Durin::DVolumeTexture>();
}

TEST(FTexture2DBuildProviderTests, RejectsAmbiguityAndKeepsProductsValueOwned)
{
	Durin::FModuleTestOwner Owner("Texture2DBuildProviderContract");
	FTestTexture2DBuildProvider Provider;
	auto Registration = Owner.RegisterFeature(Provider);
	ASSERT_TRUE(Registration.IsValid());

	Durin::FTexture2DBuildRequest Request;
	Durin::Image::FImage SourceDataImage;
	EXPECT_TRUE(Durin::Image::FImage::TryCreate({.Width = 1, .Height = 1,
		.Format = Durin::Image::ERawImageFormat::RGBA8}, Durin::FByteBuffer(4), SourceDataImage));
	Durin::FTextureSource SourceData;
	EXPECT_TRUE(SourceData.Init2D(SourceDataImage.GetView(), 4));
	Request = Durin::MakeTexture2DBuildRequest(SourceData);
	const Durin::FXxHash128 SourceIdentity =
		Request.SourceIdentity;
	const auto Ambiguous = Durin::FModularFeatureRegistry::Get().InvokeSingle<
		Durin::ITexture2DBuildProvider>([&](Durin::ITexture2DBuildProvider& Feature) {
			return Feature.GetDescriptor();
		});
	EXPECT_EQ(Ambiguous.Status, Durin::EFeatureInvokeStatus::Ambiguous);
	ASSERT_TRUE(Owner.BeginRetirement().Succeeded());

	Durin::FTexture2DBuildProduct Product;
	Durin::FTexture2DBuildInputIdentity Identity;
	const Durin::FTexture2DBuildResult BuildResult =
		Durin::InvokeTexture2DBuildProvider(Request, Product, Identity);
	ASSERT_TRUE(BuildResult) << BuildResult.Diagnostic;
	EXPECT_EQ(Request.SourceMips.front().GetPixels().size(), 4u);
	EXPECT_TRUE(Identity.Provider.IsValid());
	EXPECT_EQ(Identity.SourceIdentity, SourceIdentity);
	EXPECT_EQ(Product.Provider, Identity.Provider);
}

TEST(FTextureBuildProviderTests, ModuleRetirementBoundsProviderUnavailability)
{
	auto& Modules = Durin::FModuleManager::Get();
	Modules.LoadModuleChecked("TextureBuild");
	ASSERT_TRUE(Modules.UnloadModule("TextureBuild").Succeeded());

	Durin::FTexture2DBuildRequest Request;
	Durin::Image::FImage SourceDataImage;
	EXPECT_TRUE(Durin::Image::FImage::TryCreate({.Width = 1, .Height = 1,
		.Format = Durin::Image::ERawImageFormat::RGBA8}, Durin::FByteBuffer(4), SourceDataImage));
	Durin::FTextureSource SourceData;
	EXPECT_TRUE(SourceData.Init2D(SourceDataImage.GetView(), 4));
	Request = Durin::MakeTexture2DBuildRequest(SourceData);
	Durin::FTexture2DBuildProduct Product;
	Durin::FTexture2DBuildInputIdentity Identity;
	const Durin::FTexture2DBuildResult BuildResult =
		Durin::InvokeTexture2DBuildProvider(Request, Product, Identity);
	EXPECT_FALSE(BuildResult);
	EXPECT_EQ(BuildResult.Status, Durin::ETexture2DBuildStatus::Failed);
	EXPECT_EQ(BuildResult.Diagnostic, "The Texture2D build provider is unavailable.");
	std::string Error;

	Durin::FVolumeTextureSourceData VolumeSource;
	VolumeSource.Width = 1;
	VolumeSource.Height = 1;
	VolumeSource.Depth = 1;
	ASSERT_TRUE(VolumeSource.SetVoxelBytes(Durin::FByteBuffer(1)));
	Durin::FVolumeTextureBuildProduct VolumeProduct;
	auto BuildResult1 = Durin::InvokeVolumeTextureBuildProvider({.SourceData = VolumeSource});
	Error = BuildResult1.Outcome.Diagnostic;
	VolumeProduct = BuildResult1 ? std::move(BuildResult1.Value->Product) : Durin::FVolumeTextureBuildProduct{};
	EXPECT_FALSE(BuildResult1) << BuildResult1.Outcome.Diagnostic;
	EXPECT_EQ(BuildResult1.Outcome.Code, Durin::ETextureBuildFailure::Unavailable);
	EXPECT_FALSE(BuildResult1.Value);
	EXPECT_EQ(Error, "The VolumeTexture build provider is unavailable.");
	Durin::FTextureCubeCanonicalBuildInput CubeCanonicalInput;
	Durin::FTextureCubeBuildProduct CubeProduct;
	auto BuildResult2 = Durin::InvokeTextureCubeBuildProvider({});
	Error = BuildResult2.Outcome.Diagnostic;
	CubeCanonicalInput = BuildResult2 ? std::move(BuildResult2.Value->CanonicalInput) : Durin::FTextureCubeCanonicalBuildInput{};
	CubeProduct = BuildResult2 ? std::move(BuildResult2.Value->Product) : Durin::FTextureCubeBuildProduct{};
	EXPECT_FALSE(BuildResult2) << BuildResult2.Outcome.Diagnostic;
	EXPECT_EQ(BuildResult2.Outcome.Code, Durin::ETextureBuildFailure::Unavailable);
	EXPECT_FALSE(BuildResult2.Value);
	EXPECT_EQ(Error, "The TextureCube build provider is unavailable.");

	Modules.LoadModuleChecked("TextureBuild");
	const auto Reloaded = Durin::FModularFeatureRegistry::Get().InvokeSingle<
		Durin::ITexture2DBuildProvider>([](Durin::ITexture2DBuildProvider& Provider) {
			return Provider.GetDescriptor();
		});
	EXPECT_EQ(Reloaded.Status, Durin::EFeatureInvokeStatus::Invoked);
	ASSERT_TRUE(Reloaded.Value.has_value());
	EXPECT_TRUE(Reloaded.Value->IsValid());
	const auto ReloadedVolume = Durin::FModularFeatureRegistry::Get().InvokeSingle<
		Durin::IVolumeTextureBuildProvider>(
			[](Durin::IVolumeTextureBuildProvider& Provider) {
				return Provider.GetDescriptor();
			});
	EXPECT_EQ(ReloadedVolume.Status, Durin::EFeatureInvokeStatus::Invoked);
	ASSERT_TRUE(ReloadedVolume.Value.has_value());
	EXPECT_TRUE(ReloadedVolume.Value->IsValid());
	const auto ReloadedCube = Durin::FModularFeatureRegistry::Get().InvokeSingle<
		Durin::ITextureCubeBuildProvider>(
			[](Durin::ITextureCubeBuildProvider& Provider) {
				return Provider.GetDescriptor();
			});
	EXPECT_EQ(ReloadedCube.Status, Durin::EFeatureInvokeStatus::Invoked);
	ASSERT_TRUE(ReloadedCube.Value.has_value());
	EXPECT_TRUE(ReloadedCube.Value->IsValid());
}

TEST(FTexture2DTests, TerminalRequestsRetireObjectRecordsAndBoundDiagnostics)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(EnsureTextureCompilingManager());
	constexpr uint32 PlatformData = 300;
	uint32 CompletionCount = 0;
	for (uint32 Index = 0; Index < PlatformData; ++Index)
	{
		auto* Texture = Durin::NewObject<Durin::DTexture2D>(
			nullptr, Durin::FName(std::format("TextureCompileLifetime{}", Index)));
		ASSERT_NE(Texture, nullptr);
		Durin::Image::FImage SourceImage;
		EXPECT_TRUE(Durin::Image::FImage::TryCreate({.Width = 1, .Height = 1,
			.Format = Durin::Image::ERawImageFormat::RGBA8}, Durin::FByteBuffer(4), SourceImage));
		Durin::FTextureSource Source;
		EXPECT_TRUE(Source.Init2D(SourceImage.GetView(), 4));
		std::string Error;
		ASSERT_TRUE(Durin::SubmitTexture2DCompilation(*Texture, {
			.Build = Durin::MakeTexture2DBuildRequest(Source, {.Usage = static_cast<Durin::ETextureUsage>(255)}),
			.Priority = Durin::ETexture2DCompilationPriority::Background}, Error,
			[&](Durin::FTexture2DCompilationResult Result) {
				++CompletionCount;
				EXPECT_EQ(Result.Status, Durin::ETexture2DCompilationStatus::Failed);
			})) << Error;
	}
	Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	const Durin::FTexture2DCompilationManagerDiagnostics Diagnostics =
		Durin::GetTexture2DCompilationManagerDiagnostics();
	EXPECT_EQ(CompletionCount, PlatformData);
	EXPECT_EQ(Diagnostics.ActiveRecordCount, 0u);
	EXPECT_EQ(Diagnostics.QueuedWorkCount, 0u);
	EXPECT_EQ(Diagnostics.RunningWorkCount, 0u);
	EXPECT_EQ(Diagnostics.PendingCompletionCount, 0u);
	EXPECT_LE(Diagnostics.RetainedWorkCount, 256u);
	EXPECT_EQ(Diagnostics.InFlightEstimatedBytes, 0u);
}

TEST(FTexture2DTests, PendingLimitIncludesFinishedComputesUntilDeliveryReturns)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(EnsureTextureCompilingManager());
	Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	constexpr uint32 PendingLimit = 1024;
	uint32 CompletionCount = 0;
	auto MakeRequest = [] {
		Durin::Image::FImage SourceImage;
		EXPECT_TRUE(Durin::Image::FImage::TryCreate({.Width = 1, .Height = 1,
			.Format = Durin::Image::ERawImageFormat::RGBA8}, Durin::FByteBuffer(4), SourceImage));
		Durin::FTextureSource Source;
		EXPECT_TRUE(Source.Init2D(SourceImage.GetView(), 4));
		return Durin::FTexture2DCompilationRequest{
			.Build = Durin::MakeTexture2DBuildRequest(Source, {.Usage = static_cast<Durin::ETextureUsage>(255)})};
	};
	auto* Overflow = Durin::NewObject<Durin::DTexture2D>(nullptr, Durin::FName("PendingLimitOverflow"));
	ASSERT_NE(nullptr, Overflow);
	struct FDrainRequests
	{
		~FDrainRequests() { Durin::FAssetCompilingManager::Get().FinishAllCompilation(); }
	} DrainRequests;
	for (uint32 Index = 0; Index < PendingLimit; ++Index)
	{
		auto* Texture = Durin::NewObject<Durin::DTexture2D>(
			nullptr, Durin::FName(std::format("PendingLimitTexture{}", Index)));
		ASSERT_NE(nullptr, Texture);
		std::string Error;
		ASSERT_TRUE(Durin::SubmitTexture2DCompilation(*Texture, MakeRequest(), Error,
			[&](Durin::FTexture2DCompilationResult Result) {
				EXPECT_EQ(Durin::ETexture2DCompilationStatus::Failed, Result.Status);
				if (++CompletionCount == 1)
				{
					std::string NestedError;
					EXPECT_FALSE(Durin::SubmitTexture2DCompilation(*Overflow, MakeRequest(), NestedError, {}));
					// A throwing consumer must release its request slot and allow the rest to drain.
					throw std::runtime_error("Completion failure for lifetime regression");
				}
			})) << Error;
	}
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (Durin::GetTexture2DCompilationManagerDiagnostics().PendingCompletionCount == 0
		&& std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
	EXPECT_GT(Durin::GetTexture2DCompilationManagerDiagnostics().PendingCompletionCount, 0u);
	EXPECT_EQ(0u, CompletionCount);
	std::string Error;
	EXPECT_FALSE(Durin::SubmitTexture2DCompilation(*Overflow, MakeRequest(), Error, {}));
	Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(PendingLimit, CompletionCount);
	EXPECT_EQ(0u, Durin::GetTexture2DCompilationManagerDiagnostics().ActiveRecordCount);
	EXPECT_EQ(0u, Durin::GetTexture2DCompilationManagerDiagnostics().InFlightEstimatedBytes);
	ASSERT_TRUE(Durin::SubmitTexture2DCompilation(*Overflow, MakeRequest(), Error,
		[&](Durin::FTexture2DCompilationResult) { ++CompletionCount; })) << Error;
	Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(PendingLimit + 1, CompletionCount);
}

TEST(FTexture2DTests, SamePathReplacementCannotReceiveDestroyedOwnerCompletion)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(EnsureTextureCompilingManager());
	std::mutex Mutex;
	std::condition_variable Condition;
	bool bEntered = false;
	bool bRelease = false;
	Durin::AssetPrivate::SetTexture2DCompilationPhaseHookForTests(
		[&](uint64, Durin::ETexture2DCompilationPhase Phase) {
			if (Phase != Durin::ETexture2DCompilationPhase::Preparing) return;
			std::unique_lock Lock(Mutex);
			if (bEntered) return;
			bEntered = true;
			Condition.notify_all();
			Condition.wait(Lock, [&] { return bRelease; });
		});

	auto MakeRequest = [](uint64 Hash) {
		Durin::FByteBuffer SourcePixels = Durin::FByteBuffer(4);
		SourcePixels[0] = static_cast<std::byte>(Hash);
		Durin::Image::FImage SourceImage;
		EXPECT_TRUE(Durin::Image::FImage::TryCreate({.Width = 1, .Height = 1,
			.Format = Durin::Image::ERawImageFormat::RGBA8}, std::move(SourcePixels), SourceImage));
		Durin::FTextureSource Source;
		EXPECT_TRUE(Source.Init2D(SourceImage.GetView(), 4));
		return Durin::FTexture2DCompilationRequest{
			.Build = Durin::MakeTexture2DBuildRequest(Source, {.Usage = static_cast<Durin::ETextureUsage>(255)}),
			.Priority = Durin::ETexture2DCompilationPriority::Interactive};
	};

	auto* First = Durin::NewObject<Durin::DTexture2D>(
		nullptr, Durin::FName("ReusedTextureCompileTarget"));
	ASSERT_NE(First, nullptr);
	const Durin::FObjectHandle FirstHandle = Durin::MakeObjectHandle(First);
	std::optional<Durin::FTexture2DCompilationResult> FirstResult;
	std::string Error;
	ASSERT_TRUE(Durin::SubmitTexture2DCompilation(
		*First, MakeRequest(41), Error,
		[&](Durin::FTexture2DCompilationResult Result) {
			FirstResult = std::move(Result);
		})) << Error;
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Condition.wait_for(
			Lock, std::chrono::seconds(5), [&] { return bEntered; }));
	}
	Durin::MarkAsGarbage(First);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectHandle(FirstHandle), nullptr);

	Durin::AssetPrivate::SetTexture2DCompilationPhaseHookForTests({});
	auto* Replacement = Durin::NewObject<Durin::DTexture2D>(
		nullptr, Durin::FName("ReusedTextureCompileTarget"));
	ASSERT_NE(Replacement, nullptr);
	EXPECT_NE(Durin::MakeObjectHandle(Replacement), FirstHandle);
	std::optional<Durin::FTexture2DCompilationResult> ReplacementResult;
	ASSERT_TRUE(Durin::SubmitTexture2DCompilation(
		*Replacement, MakeRequest(73), Error,
		[&](Durin::FTexture2DCompilationResult Result) {
			ReplacementResult = std::move(Result);
		})) << Error;
	{
		std::lock_guard Lock(Mutex);
		bRelease = true;
		Condition.notify_all();
	}
	Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_TRUE(FirstResult.has_value());
	EXPECT_EQ(FirstResult->Status, Durin::ETexture2DCompilationStatus::Failed);
	ASSERT_TRUE(ReplacementResult.has_value());
	EXPECT_EQ(ReplacementResult->Status, Durin::ETexture2DCompilationStatus::Failed);
	EXPECT_EQ(Durin::GetTexture2DCompilationManagerDiagnostics().ActiveRecordCount, 0u);
}

TEST(FVolumeTextureTests, RejectsInvalidMipFilterBeforeSourceReplacement)
{
	InitializeDObjectSystem();
	Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
	Durin::FVolumeTextureSourceData Source{
		.Width = 1, .Height = 1, .Depth = 1,
		.Format = Durin::EVolumeTextureFormat::R8_UNORM};
	ASSERT_TRUE(Source.SetVoxelBytes(Durin::FByteBuffer(1, std::byte{17})));
	auto PreparedSource = Durin::PrepareVolumeTextureSource(Source);
	ASSERT_TRUE(PreparedSource);
	auto* Texture = Durin::NewObject<Durin::DVolumeTexture>(nullptr, "VolumeSettingsBoundary");
	ASSERT_NE(Texture, nullptr);
	Texture->SetSource(std::move(*PreparedSource));
	const auto Identity = Texture->GetSource().GetIdentity();
	const auto Result = Durin::BuildVolumeTextureSynchronously(*Texture, {
		.SourceData = Source,
		.Settings = {.MipFilter = static_cast<Durin::EVolumeTextureMipFilter>(255)}}, {});
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Stage, Durin::ETextureBuildStage::Normalize);
	EXPECT_EQ(Texture->GetSource().GetIdentity(), Identity);
	EXPECT_EQ(Texture->GetBuildSettings().MipFilter, Durin::EVolumeTextureMipFilter::Box);
	EXPECT_EQ(Texture->GetPlatformData(), nullptr);
}

TEST(FVolumeTextureTests, BuildsDeterministicOddThreeAxisMipChain)
{
	Durin::FVolumeTextureSourceData Source;
	Source.Width = 3;
	Source.Height = 3;
	Source.Depth = 3;
	Source.Format = Durin::EVolumeTextureFormat::R8_UNORM;
	Durin::FByteBuffer Voxels(27);
	for (size_t Index = 0; Index < Voxels.size(); ++Index)
		Voxels[Index] = static_cast<std::byte>(Index);
	ASSERT_TRUE(Source.SetVoxelBytes(Voxels));
	Durin::FVolumeTexturePlatformData First;
	Durin::FVolumeTexturePlatformData Second;
	std::string Error;
	const Durin::FVolumeTextureBuildSettings Settings{};
	ASSERT_TRUE(Durin::VolumeTextureBuilder::BuildMipChain(
		Source, Settings, First, Error)) << Error;
	ASSERT_TRUE(Durin::VolumeTextureBuilder::BuildMipChain(
		Source, Settings, Second, Error)) << Error;
	ASSERT_EQ(First.Mips.size(), 2u);
	EXPECT_EQ(First.Mips[1].Width, 1u);
	EXPECT_EQ(First.Mips[1].Height, 1u);
	EXPECT_EQ(First.Mips[1].Depth, 1u);
	EXPECT_EQ(First.Mips[1].Voxels, (Durin::FByteBuffer{std::byte{7}}));
	EXPECT_EQ(First.Mips[0].Voxels, Second.Mips[0].Voxels);
	EXPECT_EQ(First.Mips[1].Voxels, Second.Mips[1].Voxels);
}

TEST(FVolumeTextureTests, AuthoredVoxelsHaveDistinctAtomicReflectionIdentity)
{
	Durin::FProperty* Property = Durin::FVolumeTextureSourceData::StaticStruct()
		->FindPropertyByName("Voxels", false);
	ASSERT_NE(Property, nullptr);
	EXPECT_EQ(Property->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::BulkData);
	Durin::FProperty* SchemaProperty = Durin::FVolumeTextureSourceData::StaticStruct()
		->FindPropertyByName("PayloadSchemaVersion", false);
	ASSERT_NE(SchemaProperty, nullptr);
	EXPECT_EQ(SchemaProperty->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::UInt32);
}

TEST(FVolumeTextureTests, PayloadRoundTripsAndRejectsCorruption)
{
	Durin::FVolumeTextureSourceData Source{
		.Width = 2, .Height = 2, .Depth = 2,
		.Format = Durin::EVolumeTextureFormat::R8_UNORM};
	const std::array Voxels{std::byte{0}, std::byte{32}, std::byte{64}, std::byte{96},
		std::byte{128}, std::byte{160}, std::byte{192}, std::byte{255}};
	ASSERT_TRUE(Source.SetVoxelBytes(Voxels));
	Durin::FVolumeTexturePlatformData Platform;
	std::string Error;
	ASSERT_TRUE(Durin::VolumeTextureBuilder::BuildMipChain(
		Source, {}, Platform, Error)) << Error;
	Durin::FByteBuffer Bytes;
	Durin::FCanonicalMemoryWriter Writer(Bytes, Durin::EArchivePurpose::DerivedDataPayload,
		{.Target = {"Win64", "Game"}});
	Platform.Serialize(Writer);
	ASSERT_FALSE(Writer.HasError()) << Writer.GetError();
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Bytes).ToString(),
		"3653410e7207268f7089e69dfa0f3d38");
	EXPECT_EQ(Bytes.size(), 177u);
	Durin::FVolumeTexturePlatformData Decoded;
	Durin::FCanonicalMemoryReader Reader(Bytes, Durin::EArchivePurpose::DerivedDataPayload,
		{.Target = {"Win64", "Game"}});
	Decoded.Serialize(Reader);
	ASSERT_TRUE(Durin::RequireArchiveEnd(Reader)) << Reader.GetError();
	EXPECT_EQ(Decoded.Mips.back().Voxels, Platform.Mips.back().Voxels);
	auto DifferentProducer = Bytes;
	for (uint32 Byte = 0; Byte < 4; ++Byte)
		DifferentProducer[8 + Byte] = static_cast<std::byte>(
			(Durin::VolumeTextureBuilderVersion + 17) >> (Byte * 8));
	Durin::FCanonicalMemoryReader CompatibleReader(DifferentProducer,
		Durin::EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
	Decoded.Serialize(CompatibleReader);
	ASSERT_TRUE(Durin::RequireArchiveEnd(CompatibleReader)) << CompatibleReader.GetError();
	EXPECT_EQ(Decoded.Mips.back().Voxels, Platform.Mips.back().Voxels);
	Bytes.back() ^= std::byte{1};
	Durin::FVolumeTexturePlatformData Discarded;
	Durin::FCanonicalMemoryReader CorruptReader(Bytes,
		Durin::EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
	Discarded.Serialize(CorruptReader);
	EXPECT_TRUE(CorruptReader.HasError());
	EXPECT_NE(CorruptReader.GetError().find("checksum"), std::string::npos);
}

TEST(FVolumeTextureTests, DdcBuildIsStableAndKeySensitive)
{
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "VolumeTextureBuildDdc");
	Durin::FVolumeTextureSourceData Source{
		.Width = 2, .Height = 2, .Depth = 2,
		.Format = Durin::EVolumeTextureFormat::R8_UNORM};
	std::array Voxels{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
		std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
	ASSERT_TRUE(Source.SetVoxelBytes(Voxels));
	EXPECT_TRUE(Source.IsValid());
	Source.PayloadSchemaVersion = Durin::VolumeTextureSourcePayloadSchemaVersion + 1;
	EXPECT_FALSE(Source.IsValid());
	Durin::FVolumeTextureBuildProduct Rejected;
	std::string SchemaError;
	auto BuildResult3 = Durin::InvokeVolumeTextureBuildProvider({.SourceData = Source});
	SchemaError = BuildResult3.Outcome.Diagnostic;
	Rejected = BuildResult3 ? std::move(BuildResult3.Value->Product) : Durin::FVolumeTextureBuildProduct{};
	EXPECT_FALSE(BuildResult3) << BuildResult3.Outcome.Diagnostic;
	EXPECT_FALSE(SchemaError.empty());
	Source.PayloadSchemaVersion = Durin::VolumeTextureSourcePayloadSchemaVersion;
	const Durin::FVolumeTextureBuildKeyInput GoldenKeyInput{
		.CanonicalSourceIdentity = Source.GetIdentity(),
		.Width = Source.Width,
		.Height = Source.Height,
		.Depth = Source.Depth,
		.Settings = {},
		.TargetPlatform = Durin::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::ECookTargetProfile::Game};
	std::string GoldenKeyError;
	EXPECT_EQ(Durin::BuildVolumeTextureDerivedDataKey(
		GoldenKeyInput, GoldenKeyError).ToString(),
		"f912c280977e4486722e8f7bb22a5277") << GoldenKeyError;
	Durin::FVolumeTextureBuildProduct First;
	Durin::FVolumeTextureBuildProduct Second;
	std::string Error;
	auto BuildResult4 = Durin::InvokeVolumeTextureBuildProvider({.SourceData = Source});
	Error = BuildResult4.Outcome.Diagnostic;
	First = BuildResult4 ? std::move(BuildResult4.Value->Product) : Durin::FVolumeTextureBuildProduct{};
	ASSERT_TRUE(BuildResult4) << BuildResult4.Outcome.Diagnostic;
	auto BuildResult5 = Durin::InvokeVolumeTextureBuildProvider({.SourceData = Source});
	Error = BuildResult5.Outcome.Diagnostic;
	Second = BuildResult5 ? std::move(BuildResult5.Value->Product) : Durin::FVolumeTextureBuildProduct{};
	ASSERT_TRUE(BuildResult5) << BuildResult5.Outcome.Diagnostic;
	EXPECT_EQ(First.DerivedDataKey, Second.DerivedDataKey);
	EXPECT_EQ(Second.Origin, Durin::EVolumeTextureBuildProductOrigin::CacheHit);
	EXPECT_TRUE(Second.PersistenceDiagnostic.empty());
	const auto CachePath = std::filesystem::path(Durin::FPaths::DerivedDataCacheDir())
		/ "VolumeTexture/Objects" / First.DerivedDataKey.ToString().substr(0, 2)
		/ (First.DerivedDataKey.ToString() + ".bin");
	Durin::FByteBuffer CachedBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(CachedBytes, CachePath));
	CachedBytes.push_back(std::byte{1});
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(CachedBytes, CachePath));
	Durin::FVolumeTextureBuildProduct Recovered;
	auto BuildResult6 = Durin::InvokeVolumeTextureBuildProvider({.SourceData = Source});
	Error = BuildResult6.Outcome.Diagnostic;
	Recovered = BuildResult6 ? std::move(BuildResult6.Value->Product) : Durin::FVolumeTextureBuildProduct{};
	ASSERT_TRUE(BuildResult6) << BuildResult6.Outcome.Diagnostic;
	EXPECT_EQ(Recovered.Origin, Durin::EVolumeTextureBuildProductOrigin::Rebuilt);
	EXPECT_EQ(Recovered.DerivedDataKey, First.DerivedDataKey);
	EXPECT_FALSE(Recovered.PersistenceDiagnostic.empty());
	EXPECT_LE(Recovered.PersistenceDiagnostic.size(), 2048u);
	EXPECT_TRUE(Error.empty());
	auto BuildResult7 = Durin::InvokeVolumeTextureBuildProvider({.SourceData = Source});
	Error = BuildResult7.Outcome.Diagnostic;
	Second = BuildResult7 ? std::move(BuildResult7.Value->Product) : Durin::FVolumeTextureBuildProduct{};
	ASSERT_TRUE(BuildResult7) << BuildResult7.Outcome.Diagnostic;
	EXPECT_EQ(Second.Origin, Durin::EVolumeTextureBuildProductOrigin::CacheHit);
	EXPECT_TRUE(Second.PersistenceDiagnostic.empty());
	Voxels[0] = std::byte{9};
	ASSERT_TRUE(Source.SetVoxelBytes(Voxels));
	Durin::FVolumeTextureBuildProduct Changed;
	auto BuildResult8 = Durin::InvokeVolumeTextureBuildProvider({.SourceData = Source});
	Error = BuildResult8.Outcome.Diagnostic;
	Changed = BuildResult8 ? std::move(BuildResult8.Value->Product) : Durin::FVolumeTextureBuildProduct{};
	ASSERT_TRUE(BuildResult8) << BuildResult8.Outcome.Diagnostic;
	EXPECT_NE(First.DerivedDataKey, Changed.DerivedDataKey);
}

TEST(FVolumeTextureTests, PackageReloadCookAndFailedReplacementAreTransactional)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "VolumeTextureAssetDdc");
	Durin::FVolumeTextureSourceData Source{
		.Width = 2, .Height = 2, .Depth = 2,
		.Format = Durin::EVolumeTextureFormat::R8_UNORM};
	const std::array Voxels{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
		std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
	ASSERT_TRUE(Source.SetVoxelBytes(Voxels));
	Durin::FVolumeTextureBuildProduct Product;
	std::string Error;
	auto BuildResult9 = Durin::InvokeVolumeTextureBuildProvider({.SourceData = Source});
	Error = BuildResult9.Outcome.Diagnostic;
	Product = BuildResult9 ? std::move(BuildResult9.Value->Product) : Durin::FVolumeTextureBuildProduct{};
	ASSERT_TRUE(BuildResult9) << BuildResult9.Outcome.Diagnostic;
	ASSERT_NE(Product.PlatformData, nullptr);
	const Durin::FVolumeTexturePlatformData Expected = *Product.PlatformData;
	const Durin::FCacheKeyProxy ExpectedKey = Product.DerivedDataKey;

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureImportTests/VolumePackage", AssetPath));
	Durin::DVolumeTexture* Texture = nullptr;
	const auto Created = Durin::CreatePackageLeafAssetForTesting(AssetPath, Texture);
	ASSERT_TRUE(Created) << Created.Message;
	ASSERT_NE(Texture, nullptr);
	auto PreparedTextureSource = Durin::PrepareVolumeTextureSource(Source);
	ASSERT_TRUE(PreparedTextureSource);
	Texture->SetSource(std::move(*PreparedTextureSource));
	Texture->SetBuildSettings({});
	Texture->SetPlatformData(
		std::make_unique<Durin::FVolumeTexturePlatformData>(*Product.PlatformData));
	Texture->UpdateResource();
	const auto ValidPlatformDataIdentity = Texture->GetPlatformDataShared();
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_FALSE(Durin::PrepareVolumeTextureSource({}));
	EXPECT_EQ(Texture->GetPlatformDataShared(), ValidPlatformDataIdentity);
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_EQ(Texture->GetPlatformData()->Mips.front().Voxels,
		Expected.Mips.front().Voxels);
	const Durin::FAssetResult Saved = Durin::SavePackage(Texture->GetPackage());
	ASSERT_TRUE(Saved) << Saved.Message;
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	Texture = nullptr;
	const Durin::FAssetResult Loaded = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Texture);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	ASSERT_NE(Texture, nullptr);
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_TRUE(ExpectedKey.IsValid());
	EXPECT_EQ(Texture->GetPlatformData()->Mips.front().Voxels,
		Expected.Mips.front().Voxels);

	const std::filesystem::path CookRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "VolumeTextureCook");
	Durin::Testing::RemoveTestWorkDirectory(CookRoot);
	Durin::FCookContext Cook(
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Texture, "/Game/CookedVolume", Cook, Error)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Cook, CookRoot, &Error)) << Error;
	EXPECT_FALSE(std::filesystem::exists(CookRoot / "Game/CookedVolume.dbulk"));
	Durin::FAssetPackageInspection CookedInspection;
	Durin::FPackagePath CookedInspectionPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
		"/Game/CookedVolume", CookedInspectionPath));
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(CookRoot / "Game/CookedVolume.dasset").generic_string(),
		CookedInspectionPath, CookedInspection));
	EXPECT_NE(CookedInspection.FindField("PlatformData"), nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	Texture = nullptr;
	Durin::Testing::FScopedAssetRuntimeForTests AssetRuntime;
	ASSERT_TRUE(AssetRuntime.RestartCooked(CookRoot));
	Durin::Testing::RegisterMountPointForTests(
		"/Game/", (CookRoot / "Game").generic_string() + "/");
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::FPackagePath CookedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/CookedVolume", CookedPath));
	Durin::DVolumeTexture* CookedTexture = nullptr;
	const Durin::FAssetResult CookedLoad =
		Durin::LoadObject(Durin::Testing::MakeTopLevelAssetObjectPathForTests(
			CookedPath, AssetPath.GetPackageName()), CookedTexture);
	ASSERT_TRUE(CookedLoad) << CookedLoad.Message;
	ASSERT_NE(CookedTexture, nullptr);
	const auto BulkStateBeforeGet = CookedTexture->GetCookedPlatformData().GetState();
	const auto PlatformBeforeGetIdentity = CookedTexture->GetPlatformDataShared();
	const Durin::DVolumeTexture& ConstTexture = *CookedTexture;
	EXPECT_EQ(ConstTexture.GetPlatformData(), nullptr);
	EXPECT_EQ(ConstTexture.GetPlatformData(), nullptr);
	EXPECT_FALSE(CookedTexture->HasPlatformData());
	EXPECT_EQ(CookedTexture->GetCookedPlatformData().GetState(), BulkStateBeforeGet);
	EXPECT_EQ(CookedTexture->GetPlatformDataShared(), PlatformBeforeGetIdentity);
	ASSERT_TRUE(static_cast<Durin::DTexture&>(*CookedTexture).EnsurePlatformDataLoadedBlocking());
	ASSERT_NE(CookedTexture->GetPlatformData(), nullptr);
	const auto* InstalledPlatform = CookedTexture->GetPlatformData();
	const auto InstalledPlatformDataIdentity = CookedTexture->GetPlatformDataShared();
	ASSERT_TRUE(static_cast<Durin::DTexture&>(*CookedTexture).EnsurePlatformDataLoadedBlocking());
	EXPECT_EQ(CookedTexture->GetPlatformData(), InstalledPlatform);
	EXPECT_EQ(CookedTexture->GetPlatformDataShared(), InstalledPlatformDataIdentity);
	auto* MissingPlatform = Durin::NewObject<Durin::DVolumeTexture>(
		nullptr, "MissingCookedPlatformData");
	EXPECT_FALSE(MissingPlatform->EnsurePlatformDataLoadedBlocking());
	EXPECT_EQ(MissingPlatform->GetPlatformData(), nullptr);
	EXPECT_FALSE(CookedTexture->CreateBuildInput().IsValid());
	EXPECT_EQ(CookedTexture->GetPlatformData()->Mips.front().Voxels,
		Expected.Mips.front().Voxels);
	ASSERT_TRUE(Durin::UnloadPackage(CookedPath));
	ASSERT_TRUE(AssetRuntime.Restore());
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST(FVolumeTextureTests, Large128CubedSourcePlansSavesAndReloadsAsAtomicBulkData)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "VolumeTextureLargeBlobDdc");
	Durin::FVolumeTextureSourceData Source;
	Source.Width = 128;
	Source.Height = 128;
	Source.Depth = 128;
	Source.Format = Durin::EVolumeTextureFormat::R8_UNORM;
	Durin::FByteBuffer Voxels(128ull * 128 * 128);
	for (size_t Index = 0; Index < Voxels.size(); ++Index)
		Voxels[Index] = static_cast<std::byte>((Index * 37) & 0xff);
	ASSERT_TRUE(Source.SetVoxelBytes(Voxels));

	Durin::FVolumeTexturePlatformData Platform;
	std::string Error;
	ASSERT_TRUE(Durin::VolumeTextureBuilder::BuildMipChain(
		Source, {}, Platform, Error)) << Error;
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureImportTests/LargeVolumeBlob", AssetPath));
	Durin::DVolumeTexture* Texture = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(AssetPath, Texture));
	auto PreparedTextureSource = Durin::PrepareVolumeTextureSource(Source);
	ASSERT_TRUE(PreparedTextureSource);
	ASSERT_TRUE(PreparedTextureSource->Recompress(Durin::ETextureSourceCompression::Raw));
	Texture->SetSource(std::move(*PreparedTextureSource));
	Texture->SetBuildSettings({});
	Texture->SetPlatformData(
		std::make_unique<Durin::FVolumeTexturePlatformData>(Platform));
	Texture->UpdateResource();

	Durin::FDefaultDeltaPlan Plan;
	Durin::FDefaultDeltaDiagnostic Diagnostic;
	ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
		Texture, Durin::EDefaultDeltaMode::Enabled, Plan, &Diagnostic))
		<< "reason=" << static_cast<int>(Diagnostic.Reason)
		<< " path=" << Diagnostic.LogicalPath;
	EXPECT_LT(Plan.FieldCount, 100u);
	Durin::FDefaultDeltaPlan NoDeltaPlan;
	ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
		Texture, Durin::EDefaultDeltaMode::NoDelta, NoDeltaPlan, &Diagnostic))
		<< "reason=" << static_cast<int>(Diagnostic.Reason)
		<< " path=" << Diagnostic.LogicalPath;
	EXPECT_LT(NoDeltaPlan.FieldCount, 100u);
	const Durin::FAssetResult Saved = Durin::SavePackage(Texture->GetPackage());
	ASSERT_TRUE(Saved) << Saved.Message;
	const Durin::FAssetCatalogEntry SavedData = Durin::FindAssetExact(AssetPath);
	ASSERT_TRUE(SavedData);
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(SavedData->PhysicalPath, Inspection));
	Durin::FTexturePayloadInspection PayloadInspection;
	ASSERT_TRUE(Durin::InspectTexturePayloadPackage(
		Inspection, PayloadInspection, &Error)) << Error;
	ASSERT_TRUE(PayloadInspection.bConstructFree);
	ASSERT_EQ(PayloadInspection.Entries.size(), 3u);
	const auto SourceEntry = std::ranges::find(
		PayloadInspection.Entries, Durin::ETexturePayloadStage::Source,
		&Durin::FTexturePayloadInspectionEntry::Stage);
	ASSERT_NE(SourceEntry, PayloadInspection.Entries.end());
	EXPECT_EQ(SourceEntry->Domain, "VolumeTexture");
	EXPECT_EQ(SourceEntry->State, Durin::ETexturePayloadState::Available);
	EXPECT_EQ(SourceEntry->Repair, Durin::ETexturePayloadRepairAction::None);
	EXPECT_EQ(SourceEntry->DomainSchemaVersion,
		Durin::TextureSourceSchemaVersion);
	EXPECT_EQ(SourceEntry->LogicalElementCount, Voxels.size());
	EXPECT_EQ(SourceEntry->LogicalByteCount, Voxels.size());
	EXPECT_EQ(SourceEntry->Placement, "EditorPackageCompanion");
	std::vector<std::filesystem::path> EditorBulkDataFiles;
	ASSERT_TRUE(Durin::InspectEditorBulkDataCompanionPaths(
		SavedData->PhysicalPath, Inspection, EditorBulkDataFiles, &Error)) << Error;
	ASSERT_EQ(EditorBulkDataFiles.size(), 1u);
	EXPECT_TRUE(std::filesystem::is_regular_file(EditorBulkDataFiles.front()));
	EXPECT_LT(std::filesystem::file_size(SavedData->PhysicalPath), 256ull * 1024);
	EXPECT_EQ(std::filesystem::file_size(EditorBulkDataFiles.front()),
		Voxels.size());
	const std::filesystem::path OrphanCompanion =
		std::filesystem::path(SavedData->PhysicalPath).parent_path()
		/ (std::filesystem::path(SavedData->PhysicalPath).stem().string()
			+ ".orphan.dbulk");
	std::filesystem::copy_file(EditorBulkDataFiles.front(), OrphanCompanion,
		std::filesystem::copy_options::overwrite_existing);
	ASSERT_TRUE(Durin::InspectTexturePayloadPackage(
		Inspection, PayloadInspection, &Error)) << Error;
	const auto OrphanEntry = std::ranges::find(
		PayloadInspection.Entries, Durin::ETexturePayloadRepairAction::RemoveOrphan,
		&Durin::FTexturePayloadInspectionEntry::Repair);
	// Stable companion publication deliberately ignores non-stable names;
	// they are not safe package-owned cleanup candidates.
	EXPECT_EQ(OrphanEntry, PayloadInspection.Entries.end());
	EXPECT_TRUE(std::filesystem::is_regular_file(OrphanCompanion));
	std::filesystem::remove(OrphanCompanion);
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	Texture = nullptr;
	const std::filesystem::path HeldCompanion =
		EditorBulkDataFiles.front().generic_string() + ".held";
	std::filesystem::rename(EditorBulkDataFiles.front(), HeldCompanion);
	ASSERT_TRUE(Durin::InspectTexturePayloadPackage(
		Inspection, PayloadInspection, &Error)) << Error;
	EXPECT_EQ(PayloadInspection.Entries.front().State,
		Durin::ETexturePayloadState::Missing);
	EXPECT_EQ(PayloadInspection.Entries.front().Repair,
		Durin::ETexturePayloadRepairAction::RestoreEditorCompanion);
	const Durin::FAssetResult MissingLoad =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Texture);
	EXPECT_FALSE(MissingLoad);
	EXPECT_EQ(Texture, nullptr);
	std::filesystem::rename(HeldCompanion, EditorBulkDataFiles.front());
	Durin::FByteBuffer CompanionBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		CompanionBytes, EditorBulkDataFiles.front()));
	Durin::FByteBuffer CorruptCompanion = CompanionBytes;
	CorruptCompanion.back() ^= std::byte{1};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(CorruptCompanion)),
		EditorBulkDataFiles.front().generic_string()));
	ASSERT_TRUE(Durin::InspectTexturePayloadPackage(
		Inspection, PayloadInspection, &Error)) << Error;
	EXPECT_EQ(PayloadInspection.Entries.front().State,
		Durin::ETexturePayloadState::Corrupt);
	EXPECT_EQ(PayloadInspection.Entries.front().Repair,
		Durin::ETexturePayloadRepairAction::RestoreEditorCompanion);
	const Durin::FAssetResult CorruptLoad =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Texture);
	EXPECT_FALSE(CorruptLoad);
	EXPECT_EQ(Texture, nullptr);
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(CompanionBytes)),
		EditorBulkDataFiles.front().generic_string()));
	const Durin::FAssetResult Loaded = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Texture);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	ASSERT_NE(Texture, nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(
		AssetPath, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	Texture = nullptr;
	const Durin::FAssetResult WarmLoaded =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Texture);
	ASSERT_TRUE(WarmLoaded) << WarmLoaded.Message;
	ASSERT_NE(Texture, nullptr);
	const Durin::FPackageResourceHandle WarmResource =
		Durin::GetPackageResourceManager().FindPackage(AssetPath.ToString());
	ASSERT_TRUE(WarmResource);
	EXPECT_EQ(WarmResource->GetReadStats().RequestCount, 1u);
	EXPECT_TRUE(std::ranges::equal(
		Texture->CreateBuildInput().GetVoxelBytes(), Source.GetVoxelBytes()));
	EXPECT_EQ(Texture->GetSource().GetCompression(), Durin::ETextureSourceCompression::Raw);
	EXPECT_EQ(WarmResource->GetReadStats().RequestCount, 1u);
	Texture = nullptr;
	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::InitializeAssetManager());
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
	EXPECT_FALSE(std::filesystem::exists(EditorBulkDataFiles.front()));
}

TEST(FVolumeTextureTests, BuildsAllPortableFormatsAcrossDegenerateAxes)
{
	const std::array Formats{
		Durin::EVolumeTextureFormat::R8_UNORM,
		Durin::EVolumeTextureFormat::RG8_UNORM,
		Durin::EVolumeTextureFormat::RGBA8_UNORM,
		Durin::EVolumeTextureFormat::R16_FLOAT,
		Durin::EVolumeTextureFormat::RGBA16_FLOAT};
	const std::array<uint32, 5> BytesPerVoxel{1, 2, 4, 2, 8};
	for (size_t Index = 0; Index < Formats.size(); ++Index)
	{
		Durin::FVolumeTextureSourceData Source;
		Source.Width = 1;
		Source.Height = 3;
		Source.Depth = 5;
		Source.Format = Formats[Index];
		const Durin::FByteBuffer Voxels(15 * BytesPerVoxel[Index], std::byte{0});
		ASSERT_TRUE(Source.SetVoxelBytes(Voxels));
		Durin::FVolumeTextureBuildSettings Settings;
		Settings.OutputFormat = Formats[Index];
		Durin::FVolumeTexturePlatformData Platform;
		std::string Error;
		ASSERT_TRUE(Durin::VolumeTextureBuilder::BuildMipChain(
			Source, Settings, Platform, Error)) << Error;
		ASSERT_EQ(Platform.Mips.size(), 3u);
		const std::array<uint32, 3> MiddleExtent{
			Platform.Mips[1].Width, Platform.Mips[1].Height, Platform.Mips[1].Depth};
		const std::array<uint32, 3> TailExtent{
			Platform.Mips[2].Width, Platform.Mips[2].Height, Platform.Mips[2].Depth};
		EXPECT_EQ(MiddleExtent, (std::array<uint32, 3>{1, 1, 2}));
		EXPECT_EQ(TailExtent, (std::array<uint32, 3>{1, 1, 1}));
		EXPECT_TRUE(Platform.IsValid());
	}
}

TEST(FTexture2DTests, StandardTranslationFeedsDetachedNormalizedBuildProduct)
{
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "NormalizedTexture2DBuildDdc");
	Durin::FTextureSource SourceData;
	std::string Error;
	const Durin::FByteView TransparentPngData =
		std::as_bytes(std::span{TransparentPngBytes});
	ASSERT_TRUE(Durin::AssetForge::Builtins::TranslateTexture2DSource(
		TransparentPngData, SourceData, Error)) << Error;
	ASSERT_TRUE(SourceData.IsValid());
	EXPECT_EQ(SourceData.GetWidth(), 2u);
	EXPECT_EQ(SourceData.GetHeight(), 1u);
	EXPECT_EQ(SourceData.GetSourceChannelCount(), 4u);
	EXPECT_TRUE(SourceData.HasTransparency());
	EXPECT_EQ(SourceData.GetFormat(), Durin::ETextureSourceFormat::RGBA8);

	Durin::FTexture2DBuildProduct Product;
	const auto Request = Durin::MakeTexture2DBuildRequest(SourceData);
	Durin::FTexture2DBuildInputIdentity Identity;
	const Durin::FTexture2DBuildResult BuildResult =
		Durin::InvokeTexture2DBuildProvider(Request, Product, Identity);
	ASSERT_TRUE(BuildResult) << BuildResult.Diagnostic;
	EXPECT_TRUE(!Request.SourceMips.empty());
	EXPECT_TRUE(Product.PlatformData.IsValid());
	EXPECT_TRUE(Product.DerivedDataKey.IsValid());

	const Durin::FTexture2DBuildResult InvalidResult =
		Durin::InvokeTexture2DBuildProvider({}, Product, Identity);
	EXPECT_FALSE(InvalidResult);
	EXPECT_EQ(InvalidResult.Status, Durin::ETexture2DBuildStatus::Failed);
	EXPECT_FALSE(Product.DerivedDataKey.IsValid());
}

TEST(FTexture2DTests, DdcStoreFailureKeepsCompleteProductAndReportsDiagnostic)
{
	InitializeDObjectSystem();
	const std::filesystem::path BlockedRoot =
		Durin::Testing::GetTestWorkDirectory() / "Texture2DBlockedDdcRoot";
	FScopedDerivedDataCacheRoot CacheRoot(BlockedRoot);
	const std::array<std::byte, 1> BlockingFile{std::byte{0xff}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(BlockingFile, BlockedRoot));
	Durin::FTextureSource SourceData;
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::TranslateTexture2DSource(
		std::as_bytes(std::span{TransparentPngBytes}), SourceData, Error)) << Error;
	Durin::FTexture2DBuildProduct Product;
	const auto Request = Durin::MakeTexture2DBuildRequest(SourceData);
	Durin::FTexture2DBuildInputIdentity Identity;
	const Durin::FTexture2DBuildResult BuildResult =
		Durin::InvokeTexture2DBuildProvider(Request, Product, Identity);
	ASSERT_TRUE(BuildResult) << BuildResult.Diagnostic;
	EXPECT_TRUE(!Request.SourceMips.empty());
	EXPECT_TRUE(Product.PlatformData.IsValid());
	EXPECT_TRUE(Product.DerivedDataKey.IsValid());
	EXPECT_FALSE(Product.PersistenceDiagnostic.empty());
}

TEST(FTexture2DTests, CanonicalImportedPixelsRoundTripThroughExternalAuthoredBulk)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "Texture2DExternalAuthoredBulkDdc");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "Texture2DExternalAuthoredBulk.tga";
	WriteLargeTextureFixture(Source);
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureImportTests/ExternalAuthoredBulk", AssetPath));
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Imported =
		Durin::AssetForge::Builtins::ImportTexture2DForTest(
			Source.generic_string(), AssetPath.GetView());
	ASSERT_TRUE(Imported) << Imported.Message;
	ASSERT_NE(Imported.Asset, nullptr);
	ASSERT_TRUE(Imported.Asset->GetSource().IsValid());
	// This case exercises companion corruption and reload, so retain external Raw storage.
	auto ExternalSource = Imported.Asset->GetSource();
	ASSERT_TRUE(ExternalSource.Recompress(Durin::ETextureSourceCompression::Raw));
	ASSERT_TRUE(Imported.Asset->ReplaceSourceStorage(std::move(ExternalSource)));
	ASSERT_TRUE(Durin::SavePackage(Imported.Asset->GetPackage()));
	const Durin::FXxHash128 ImportedIdentity =
		Imported.Asset->GetSource().GetIdentity();
	EXPECT_FALSE(ImportedIdentity.IsZero());
	EXPECT_TRUE(Imported.Asset->GetSource().GetBulkData().GetInstanceId().IsValid());

	const Durin::FAssetCatalogEntry Entry =
		Durin::FindAssetExact(AssetPath);
	ASSERT_TRUE(Entry);
	Durin::FAssetPackageInspection Inspection;
	std::string Error;
	const Durin::FAssetResult Inspected =
		Durin::InspectAssetPackage(Entry->PhysicalPath, Inspection);
	ASSERT_TRUE(Inspected) << Inspected.Message;
	std::vector<Durin::FEditorBulkDataStorageDescriptor> Descriptors;
	ASSERT_TRUE(Durin::InspectEditorBulkDataStorageDescriptors(
		Inspection, Descriptors, &Error)) << Error;
	ASSERT_EQ(Descriptors.size(), 1u);
	EXPECT_EQ(Descriptors.front().StorageKind,
		Durin::EEditorBulkDataStorageKind::External);
	EXPECT_TRUE(Descriptors.front().PayloadId.IsValid());
	EXPECT_EQ(Descriptors.front().ContentHash,
		Imported.Asset->GetSource().GetBulkData().GetPayloadId());
	std::vector<std::filesystem::path> Companions;
	ASSERT_TRUE(Durin::InspectEditorBulkDataCompanionPaths(
		Entry->PhysicalPath, Inspection, Companions, &Error)) << Error;
	ASSERT_EQ(Companions.size(), 1u);
	ASSERT_TRUE(std::filesystem::is_regular_file(Companions.front()));
	Durin::FByteBuffer CompanionBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		CompanionBytes, Companions.front()));

	const std::filesystem::path CachePath = GetTextureCachePath(*Imported.Asset);
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(std::filesystem::remove(Source));
	std::error_code IgnoredError;
	std::filesystem::remove(CachePath, IgnoredError);
	Durin::DTexture2D* LoadedTexture = nullptr;
	const Durin::FAssetResult Loaded =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), LoadedTexture);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	ASSERT_NE(LoadedTexture, nullptr);
	EXPECT_EQ(LoadedTexture->GetSource().GetIdentity(), ImportedIdentity);
	EXPECT_TRUE(LoadedTexture->HasPlatformData());

	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	LoadedTexture = nullptr;
	const Durin::FAssetResult WarmLoaded =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), LoadedTexture);
	ASSERT_TRUE(WarmLoaded) << WarmLoaded.Message;
	ASSERT_NE(LoadedTexture, nullptr);
	const Durin::FPackageResourceHandle WarmResource =
		Durin::GetPackageResourceManager().FindPackage(AssetPath.ToString());
	ASSERT_TRUE(WarmResource);
	EXPECT_EQ(WarmResource->GetReadStats().RequestCount, 1u);
	const auto WarmInput = LoadedTexture->CreateBuildRequest({});
	EXPECT_EQ(WarmInput.SourceIdentity, ImportedIdentity);
	EXPECT_EQ(WarmResource->GetReadStats().RequestCount, 1u);
	const auto& WarmDecoded = WarmInput.SourceMips.front();
	EXPECT_TRUE(WarmDecoded.IsValid());
	EXPECT_EQ(WarmResource->GetReadStats().RequestCount, 1u);
	// Installing detached source must not reload or decode its package-backed payload.
	auto DetachedSource = LoadedTexture->GetSource();
	DetachedSource.ReleaseSourceMemory();
	LoadedTexture->SetSource(std::move(DetachedSource));
	EXPECT_EQ(WarmResource->GetReadStats().RequestCount, 1u);
	EXPECT_EQ(LoadedTexture->GetSource().GetOwner(), LoadedTexture);
	EXPECT_EQ(LoadedTexture->GetSource().GetIdentity(), ImportedIdentity);

	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	std::filesystem::path Backup = Companions.front();
	Backup += Durin::EditorBulkDataCompanionBackupSuffix;
	std::filesystem::copy_file(Companions.front(), Backup,
		std::filesystem::copy_options::overwrite_existing);
	auto CorruptBytes = CompanionBytes;
	CorruptBytes.back() ^= std::byte{1};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		CorruptBytes, Companions.front()));
	LoadedTexture = nullptr;
	const Durin::FAssetResult Recovered =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), LoadedTexture);
	ASSERT_TRUE(Recovered) << Recovered.Message;
	EXPECT_EQ(LoadedTexture->GetSource().GetIdentity(), ImportedIdentity);
	EXPECT_FALSE(std::filesystem::exists(Backup));

	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(std::filesystem::remove(Companions.front()));
	LoadedTexture = nullptr;
	const Durin::FAssetResult Missing =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), LoadedTexture);
	EXPECT_FALSE(Missing);
	EXPECT_EQ(LoadedTexture, nullptr);
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		CompanionBytes, Companions.front()));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST(FTexture2DTests, CompilationAppliesLatestNormalizedProduct)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "Texture2DCompilationDdc");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "Texture2DCompilation.png";
	WriteTextureFixture(Source);
	Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Imported =
		Durin::AssetForge::Builtins::ImportTexture2DForTest(
			Source.generic_string(), "/TextureImportTests/SourceDomain");
	ASSERT_TRUE(Imported) << Imported.Message;

	Durin::FTextureSource SourceData;
	std::string Error;
	const Durin::FByteView TransparentPngData =
		std::as_bytes(std::span{TransparentPngBytes});
	ASSERT_TRUE(Durin::AssetForge::Builtins::TranslateTexture2DSource(
		TransparentPngData, SourceData, Error)) << Error;
	std::optional<Durin::FTexture2DCompilationResult> CompletionResult;
	int32 CompletionCount = 0;
	ASSERT_TRUE(Durin::SubmitTexture2DCompilation(*Imported.Asset, {
		.Build = Durin::MakeTexture2DBuildRequest(SourceData, {.MaxResolution = 1}),
		.ResultApplication = {.SourceReplacement = SourceData},
		.Priority = Durin::ETexture2DCompilationPriority::Interactive}, Error,
		[&](Durin::FTexture2DCompilationResult Result) {
			++CompletionCount;
			CompletionResult = std::move(Result);
		})) << Error;
	EXPECT_TRUE(Durin::HasPendingTexture2DCompilation(*Imported.Asset));
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Imported.Asset, 10.0));
	EXPECT_FALSE(Durin::HasPendingTexture2DCompilation(*Imported.Asset));
	ASSERT_TRUE(CompletionResult.has_value());
	EXPECT_EQ(CompletionCount, 1);
	EXPECT_EQ(CompletionResult->Status,
		Durin::ETexture2DCompilationStatus::Succeeded);
	EXPECT_EQ(Imported.Asset->GetMaxResolution(), 1u);
	ASSERT_NE(Imported.Asset->GetPlatformData(), nullptr);
	EXPECT_EQ(Imported.Asset->GetPlatformData()->Mips.front().Width, 1u);
}

TEST(FTexture2DTests, AsyncCompilationReportsFailureAndSupersessionOnce)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "Texture2DCompletionDdc");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "Texture2DCompletion.png";
	WriteTextureFixture(Source);
	Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Imported =
		Durin::AssetForge::Builtins::ImportTexture2DForTest(
			Source.generic_string(), "/TextureImportTests/CompletionContract");
	ASSERT_TRUE(Imported) << Imported.Message;

	const Durin::FByteView Encoded =
		std::as_bytes(std::span{TransparentPngBytes});
	auto MakeRequest = [&](Durin::FTextureSource SourceData) {
		return Durin::FTexture2DCompilationRequest{
			.Build = Durin::MakeTexture2DBuildRequest(SourceData, {.MaxResolution = 1}),
			.ResultApplication = {.SourceReplacement = SourceData},
			.Priority = Durin::ETexture2DCompilationPriority::Interactive};
	};

	Durin::FTextureSource FailedSource;
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::TranslateTexture2DSource(
		Encoded, FailedSource, Error)) << Error;
	auto FailedRequest = MakeRequest(std::move(FailedSource));
	FailedRequest.Build.Settings.Usage = static_cast<Durin::ETextureUsage>(255);
	std::optional<Durin::FTexture2DCompilationResult> FailedResult;
	ASSERT_TRUE(Durin::SubmitTexture2DCompilation(
		*Imported.Asset, std::move(FailedRequest), Error,
		[&](Durin::FTexture2DCompilationResult Result) {
			FailedResult = std::move(Result);
		})) << Error;
	EXPECT_FALSE(Durin::WaitForTexture2DCompilation(*Imported.Asset, 10.0));
	ASSERT_TRUE(FailedResult.has_value());
	EXPECT_EQ(FailedResult->Status, Durin::ETexture2DCompilationStatus::Failed);

	Durin::FTextureSource FirstSource;
	Durin::FTextureSource SecondSource;
	ASSERT_TRUE(Durin::AssetForge::Builtins::TranslateTexture2DSource(
		Encoded, FirstSource, Error)) << Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::TranslateTexture2DSource(
		Encoded, SecondSource, Error)) << Error;
	std::optional<Durin::FTexture2DCompilationResult> FirstResult;
	std::optional<Durin::FTexture2DCompilationResult> SecondResult;
	int32 FirstCompletionCount = 0;
	ASSERT_TRUE(Durin::SubmitTexture2DCompilation(
		*Imported.Asset, MakeRequest(std::move(FirstSource)), Error,
		[&](Durin::FTexture2DCompilationResult Result) {
			++FirstCompletionCount;
			FirstResult = std::move(Result);
		})) << Error;
	ASSERT_TRUE(Durin::SubmitTexture2DCompilation(
		*Imported.Asset, MakeRequest(std::move(SecondSource)), Error,
		[&](Durin::FTexture2DCompilationResult Result) {
			SecondResult = std::move(Result);
		})) << Error;
	ASSERT_TRUE(FirstResult.has_value());
	EXPECT_EQ(FirstCompletionCount, 1);
	EXPECT_EQ(FirstResult->Status,
		Durin::ETexture2DCompilationStatus::Superseded);
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Imported.Asset, 10.0));
	ASSERT_TRUE(SecondResult.has_value());
	EXPECT_EQ(SecondResult->Status,
		Durin::ETexture2DCompilationStatus::Succeeded);
	EXPECT_EQ(FirstCompletionCount, 1);
}

TEST(FTexture2DTests, UsagePresetsChooseColorSpaceAndMipFilter)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "UsagePresetSource.png";
	WriteTextureFixture(Source);

	struct FExpectedPreset
	{
		Durin::ETextureUsage Usage;
		std::string_view AssetName;
		Durin::EPixelFormat PixelFormat;
		std::array<uint8, 4> ExpectedPixel;
	};
	const std::array Presets = {
		FExpectedPreset{Durin::ETextureUsage::Color, "PresetColor", Durin::EPixelFormat::BC3_UNORM_SRGB, {188, 0, 0, 128}},
		FExpectedPreset{Durin::ETextureUsage::Normal, "PresetNormal", Durin::EPixelFormat::BC5_UNORM, {128, 37, 0, 0}},
		FExpectedPreset{Durin::ETextureUsage::DataMask, "PresetDataMask", Durin::EPixelFormat::BC7_UNORM, {128, 0, 0, 128}}
	};

	for (const FExpectedPreset& Preset : Presets)
	{
		Durin::FTexture2DImportSettings Settings;
		Settings.Usage = Preset.Usage;
		const std::string AssetPathString = std::format("/TextureImportTests/{}", Preset.AssetName);
		Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(Source.generic_string(), AssetPathString, Settings);
		ASSERT_TRUE(Result) << Result.Message;
		ASSERT_NE(Result.Asset, nullptr);
		EXPECT_EQ(Result.Asset->GetUsage(), Preset.Usage);
		EXPECT_EQ(Result.Asset->IsSRGB(), Preset.Usage == Durin::ETextureUsage::Color);
		ASSERT_NE(Result.Asset->GetPlatformData(), nullptr);
		EXPECT_EQ(Result.Asset->GetPlatformData()->PixelFormat, Preset.PixelFormat);
		ASSERT_EQ(Result.Asset->GetPlatformData()->Mips.size(), 2u);
		ExpectPixelNear(DecodeFirstCompressedPixel(Preset.PixelFormat,
			Result.Asset->GetPlatformData()->Mips.back().Pixels), Preset.ExpectedPixel);

		Durin::FPackagePath AssetPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(AssetPathString, AssetPath));
		ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
		Durin::DTexture2D* Loaded = nullptr;
		const Durin::FAssetResult LoadResult =
			Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded);
		ASSERT_TRUE(LoadResult) << LoadResult.Message;
		ASSERT_NE(Loaded, nullptr);
		EXPECT_EQ(Loaded->GetUsage(), Preset.Usage);
		EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Preset.PixelFormat);
		ExpectPixelNear(DecodeFirstCompressedPixel(Preset.PixelFormat,
			Loaded->GetPlatformData()->Mips.back().Pixels), Preset.ExpectedPixel);
		ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
		ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
	}
}

TEST(FTexture2DTests, BuildsCompleteNpotMipChainWithoutDroppingEdges)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "NpotTextureSource.tga";
	WriteNpotTextureFixture(Source);
	Durin::FTexture2DImportSettings Settings;
	Settings.Usage = Durin::ETextureUsage::DataMask;
	Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(Source.generic_string(), "/TextureImportTests/Npot", Settings);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	const Durin::FTexturePlatformData* PlatformData = Result.Asset->GetPlatformData();
	ASSERT_NE(PlatformData, nullptr);
	EXPECT_EQ(PlatformData->PixelFormat, Durin::EPixelFormat::BC7_UNORM);
	ASSERT_EQ(PlatformData->Mips.size(), 3u);
	EXPECT_EQ(std::pair(PlatformData->Mips[0].Width, PlatformData->Mips[0].Height), std::pair(5u, 3u));
	EXPECT_EQ(std::pair(PlatformData->Mips[1].Width, PlatformData->Mips[1].Height), std::pair(2u, 1u));
	EXPECT_EQ(std::pair(PlatformData->Mips[2].Width, PlatformData->Mips[2].Height), std::pair(1u, 1u));
	ExpectPixelNear(DecodeFirstCompressedPixel(PlatformData->PixelFormat, PlatformData->Mips[2].Pixels),
		{43, 43, 43, 255});

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/Npot", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));

	Durin::Testing::TFactoryImportResult<Durin::DTexture2D> ColorResult = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Source.generic_string(), "/TextureImportTests/NpotColor");
	ASSERT_TRUE(ColorResult) << ColorResult.Message;
	ASSERT_NE(ColorResult.Asset, nullptr);
	ASSERT_NE(ColorResult.Asset->GetPlatformData(), nullptr);
	EXPECT_EQ(ColorResult.Asset->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC1_UNORM_SRGB);
	EXPECT_TRUE(ColorResult.Asset->GetPlatformData()->IsValid());
	Durin::FPackagePath ColorAssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/NpotColor", ColorAssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(ColorAssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(ColorAssetPath));
}

TEST(FTexture2DTests, MaximumResolutionSelectsMipAlignedBaseLevel)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "LimitedTextureSource.tga";
	WriteNpotTextureFixture(Source);
	Durin::FTexture2DImportSettings Settings;
	Settings.MaxResolution = 4;
	Settings.CompressionQuality = Durin::ETextureCompressionQuality::Low;
	Settings.AlphaMipMode = Durin::ETextureAlphaMipMode::PreserveCoverage;
	Settings.AlphaCoverageThreshold = 0.4f;
	Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Source.generic_string(), "/TextureImportTests/Limited", Settings);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	EXPECT_EQ(Result.Asset->GetMaxResolution(), 4u);
	EXPECT_EQ(Result.Asset->GetCompressionQuality(), Durin::ETextureCompressionQuality::Low);
	EXPECT_EQ(Result.Asset->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::PreserveCoverage);
	EXPECT_FLOAT_EQ(Result.Asset->GetAlphaCoverageThreshold(), 0.4f);
	const Durin::FTexturePlatformData* PlatformData = Result.Asset->GetPlatformData();
	ASSERT_NE(PlatformData, nullptr);
	ASSERT_EQ(PlatformData->Mips.size(), 2u);
	EXPECT_EQ(std::pair(PlatformData->Mips[0].Width, PlatformData->Mips[0].Height), std::pair(2u, 1u));
	EXPECT_EQ(std::pair(PlatformData->Mips[1].Width, PlatformData->Mips[1].Height), std::pair(1u, 1u));

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/Limited", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetMaxResolution(), 4u);
	EXPECT_EQ(Loaded->GetCompressionQuality(), Durin::ETextureCompressionQuality::Low);
	EXPECT_EQ(Loaded->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::PreserveCoverage);
	EXPECT_FLOAT_EQ(Loaded->GetAlphaCoverageThreshold(), 0.4f);
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	EXPECT_EQ(Loaded->GetPlatformData()->Mips.front().Width, 2u);
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST(FTexture2DTests, PreservesMaskedAlphaCoverageWithoutChangingColor)
{
	Durin::FByteBuffer Pixels(8 * 8 * 4);
	constexpr std::array<uint8, 16> OpaqueCounts = {
		3, 3, 3, 3,
		3, 2, 2, 1,
		0, 0, 0, 0,
		0, 0, 0, 0
	};
	for (uint32 BlockY = 0; BlockY < 4; ++BlockY)
	{
		for (uint32 BlockX = 0; BlockX < 4; ++BlockX)
		{
			const uint8 OpaqueCount = OpaqueCounts[BlockY * 4 + BlockX];
			for (uint32 Pixel = 0; Pixel < 4; ++Pixel)
			{
				const uint32 X = BlockX * 2 + Pixel % 2;
				const uint32 Y = BlockY * 2 + Pixel / 2;
				const size_t Offset = (static_cast<size_t>(Y) * 8 + X) * 4;
				Pixels[Offset] = static_cast<std::byte>(X * 24);
				Pixels[Offset + 1] = static_cast<std::byte>(Y * 24);
				Pixels[Offset + 2] = std::byte{64};
				Pixels[Offset + 3] = Pixel < OpaqueCount
					? std::byte{255} : std::byte{0};
			}
		}
	}

	Durin::Image::FImage Source;
	ASSERT_TRUE(Durin::Image::FImage::TryCreate({.Width = 8, .Height = 8,
		.Format = Durin::Image::ERawImageFormat::RGBA8}, std::move(Pixels), Source));
	Durin::FTexturePlatformData Average;
	Durin::FTexturePlatformData Preserved;
	const Durin::FTexture2DBuildResult AverageResult = Durin::TextureBuilder::BuildMipChain(
		std::span(&Source, 1), Durin::ETextureUsage::Color, false,
		Average, 0, Durin::ETextureCompressionQuality::High,
		Durin::ETextureAlphaMipMode::Average, 0.5f);
	ASSERT_TRUE(AverageResult) << AverageResult.Diagnostic;
	EXPECT_TRUE(AverageResult.Diagnostic.empty());
	const Durin::FTexture2DBuildResult PreservedResult = Durin::TextureBuilder::BuildMipChain(
		std::span(&Source, 1), Durin::ETextureUsage::Color, false,
		Preserved, 0, Durin::ETextureCompressionQuality::High,
		Durin::ETextureAlphaMipMode::PreserveCoverage, 0.5f);
	ASSERT_TRUE(PreservedResult) << PreservedResult.Diagnostic;
	ASSERT_GE(Average.Mips.size(), 2u);
	ASSERT_EQ(Preserved.Mips.size(), Average.Mips.size());

	const Durin::FByteBuffer AveragePixels = DecodeBC3Mip(Average.Mips[1]);
	const Durin::FByteBuffer PreservedPixels = DecodeBC3Mip(Preserved.Mips[1]);
	const double SourceCoverage = 20.0 / 64.0;
	const double AverageError = std::abs(CalculateDecodedCoverage(AveragePixels, 128) - SourceCoverage);
	const double PreservedError = std::abs(CalculateDecodedCoverage(PreservedPixels, 128) - SourceCoverage);
	EXPECT_LT(PreservedError, AverageError);
	for (size_t Offset = 0; Offset < AveragePixels.size(); Offset += 4)
	{
		EXPECT_EQ(PreservedPixels[Offset], AveragePixels[Offset]);
		EXPECT_EQ(PreservedPixels[Offset + 1], AveragePixels[Offset + 1]);
		EXPECT_EQ(PreservedPixels[Offset + 2], AveragePixels[Offset + 2]);
	}
}

TEST(FTexture2DTests, CompressedLayoutsCoverNpotAndTailMips)
{
	const Durin::FPixelFormatLayout BC1Npot = Durin::GetPixelFormatLayout(Durin::EPixelFormat::BC1_UNORM, 5, 3);
	EXPECT_EQ(BC1Npot.BlocksWide, 2u);
	EXPECT_EQ(BC1Npot.BlocksHigh, 1u);
	EXPECT_EQ(BC1Npot.RowPitch, 16u);
	EXPECT_EQ(BC1Npot.DataSize, 16u);

	const Durin::FPixelFormatLayout BC3Npot = Durin::GetPixelFormatLayout(Durin::EPixelFormat::BC3_UNORM, 5, 5);
	EXPECT_EQ(BC3Npot.BlocksWide, 2u);
	EXPECT_EQ(BC3Npot.BlocksHigh, 2u);
	EXPECT_EQ(BC3Npot.RowPitch, 32u);
	EXPECT_EQ(BC3Npot.DataSize, 64u);

	const Durin::FPixelFormatLayout BC7Tail = Durin::GetPixelFormatLayout(Durin::EPixelFormat::BC7_UNORM, 1, 1);
	EXPECT_EQ(BC7Tail.BlocksWide, 1u);
	EXPECT_EQ(BC7Tail.BlocksHigh, 1u);
	EXPECT_EQ(BC7Tail.RowPitch, 16u);
	EXPECT_EQ(BC7Tail.DataSize, 16u);

	Durin::FTexture2DMipData Mip;
	Mip.Width = 5;
	Mip.Height = 3;
	Mip.RowPitch = static_cast<uint32>(BC1Npot.RowPitch);
	Mip.Pixels.resize(static_cast<size_t>(BC1Npot.DataSize));
	EXPECT_TRUE(Mip.IsValid(Durin::EPixelFormat::BC1_UNORM));
	Mip.RowPitch = 8;
	EXPECT_FALSE(Mip.IsValid(Durin::EPixelFormat::BC1_UNORM));
}

TEST(FTexture2DTests, CooperativeBuildCancellationUsesFrozenCheckpointIntervals)
{
	static_assert(Durin::TextureBuilder::CancellationBlockInterval == 64);
	static_assert(Durin::TextureBuilder::CancellationScanlineInterval == 8);
	Durin::Image::FImage Source;
	ASSERT_TRUE(Durin::Image::FImage::TryCreate({.Width = 512, .Height = 512,
		.Format = Durin::Image::ERawImageFormat::RGBA8},
		Durin::FByteBuffer(512 * 512 * 4, std::byte{127}), Source));
	uint32 CheckpointCount = 0;
	const Durin::TextureBuilder::FBuildExecutionControl Control{
		.ShouldCancel = [&] { return ++CheckpointCount == 20; }};
	Durin::FTexturePlatformData Platform;
	const Durin::FTexture2DBuildResult BuildResult = Durin::TextureBuilder::BuildMipChain(
		std::span(&Source, 1),
		Durin::ETextureUsage::DataMask,
		false,
		Platform,
		0,
		Durin::ETextureCompressionQuality::High,
		Durin::ETextureAlphaMipMode::Average,
		0.5f,
		&Control, false);
	EXPECT_FALSE(BuildResult);
	EXPECT_EQ(CheckpointCount, 20u);
	EXPECT_EQ(BuildResult.Status, Durin::ETexture2DBuildStatus::Cancelled);
	EXPECT_EQ(BuildResult.Diagnostic, "Texture build was cancelled.");
	EXPECT_FALSE(Platform.IsValid());
}

TEST(FTexture2DTests, ValidationFailureIsNotReclassifiedAsCancellation)
{
	Durin::Image::FImage InvalidSource;
	const Durin::TextureBuilder::FBuildExecutionControl Control{
		.ShouldCancel = [] { return true; }};
	Durin::FTexturePlatformData Platform;
	const Durin::FTexture2DBuildResult BuildResult = Durin::TextureBuilder::BuildMipChain(
		std::span(&InvalidSource, 1), Durin::ETextureUsage::Color, true, Platform, 0,
		Durin::ETextureCompressionQuality::Normal,
		Durin::ETextureAlphaMipMode::Average, 0.5f, &Control);

	EXPECT_EQ(BuildResult.Status, Durin::ETexture2DBuildStatus::Failed);
	EXPECT_EQ(BuildResult.Diagnostic,
		"Texture source data is unavailable or invalid.");
	EXPECT_FALSE(Platform.IsValid());
}

TEST(FTexture2DTests, PreservesLinearBuildSettingAndRebuildsColorSpace)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "LinearTextureSource.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportSettings Settings;
	Settings.bSRGB = false;
	Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(Source.generic_string(), "/TextureImportTests/Linear", Settings);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	EXPECT_FALSE(Result.Asset->IsSRGB());
	EXPECT_EQ(Result.Asset->GetUsage(), Durin::ETextureUsage::Color);
	ASSERT_NE(Result.Asset->GetPlatformData(), nullptr);
	EXPECT_EQ(Result.Asset->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM);

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/Linear", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_FALSE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM);
	ExpectPixelNear(DecodeFirstCompressedPixel(Loaded->GetPlatformData()->PixelFormat,
		Loaded->GetPlatformData()->Mips.back().Pixels), {128, 0, 0, 128});

	const Durin::FByteBuffer LinearTail = Loaded->GetPlatformData()->Mips.back().Pixels;
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::SetTexture2DSRGB(*Loaded, true, Error)) << Error;
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Loaded, 10.0))
		<< Durin::GetTexture2DCompilationDiagnostic(*Loaded).Message;
	EXPECT_TRUE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	EXPECT_NE(Loaded->GetPlatformData()->Mips.back().Pixels, LinearTail);
	ExpectPixelNear(DecodeFirstCompressedPixel(Loaded->GetPlatformData()->PixelFormat,
		Loaded->GetPlatformData()->Mips.back().Pixels), {188, 0, 0, 128});
	ASSERT_TRUE(Durin::AssetForge::Builtins::SetTexture2DUsage(
		*Loaded, Durin::ETextureUsage::Normal, Error)) << Error;
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Loaded, 10.0))
		<< Durin::GetTexture2DCompilationDiagnostic(*Loaded).Message;
	EXPECT_EQ(Loaded->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);
	ExpectPixelNear(DecodeFirstCompressedPixel(Loaded->GetPlatformData()->PixelFormat,
		Loaded->GetPlatformData()->Mips.back().Pixels), {128, 37, 0, 0});
	ASSERT_TRUE(Durin::UnloadPackage(
		AssetPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST(FTexture2DTests, ReflectedBuildSettingsRebuildTransactionallyAndSupportUndoRedo)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "TransactionalTextureSource.png";
	WriteTextureFixture(Source);
	Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Source.generic_string(), "/TextureImportTests/Transactional");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTexture2D* Texture = Result.Asset;
	ASSERT_NE(Texture, nullptr);
	ASSERT_NE(Texture->GetPackage(), nullptr);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());

	Durin::FProperty* UsageProperty = Texture->GetClass()->FindPropertyByName("Usage");
	Durin::FProperty* SRGBProperty = Texture->GetClass()->FindPropertyByName("bSRGB");
	Durin::FProperty* MaxResolutionProperty = Texture->GetClass()->FindPropertyByName("MaxResolution");
	Durin::FProperty* CompressionQualityProperty = Texture->GetClass()->FindPropertyByName("CompressionQuality");
	Durin::FProperty* AlphaMipModeProperty = Texture->GetClass()->FindPropertyByName("AlphaMipMode");
	Durin::FProperty* AlphaCoverageThresholdProperty = Texture->GetClass()->FindPropertyByName("AlphaCoverageThreshold");
	ASSERT_NE(UsageProperty, nullptr);
	ASSERT_NE(SRGBProperty, nullptr);
	ASSERT_NE(MaxResolutionProperty, nullptr);
	ASSERT_NE(CompressionQualityProperty, nullptr);
	ASSERT_NE(AlphaMipModeProperty, nullptr);
	ASSERT_NE(AlphaCoverageThresholdProperty, nullptr);
	Durin::Editor::FPropertyView PropertyView;
	Durin::Tests::FTestTransactorOwner Transactions;
	std::string Error;
	const Durin::Editor::FPropertyViewContext Context{
		.Transactor = Transactions.Get(),
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};

	const auto SubmitUsage = [&](Durin::ETextureUsage Usage) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, UsageProperty),
			[Usage](Durin::FProperty* Property, void* Container, uint32 ArrayIndex) {
				static_cast<Durin::FEnumProperty*>(Property)->SetValueFromUInt64(
					Container, static_cast<uint64>(Usage), ArrayIndex);
			}, false);
	};
	const auto SubmitSRGB = [&](bool bSRGB) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, SRGBProperty),
			[bSRGB](Durin::FProperty* Property, void* Container, uint32 ArrayIndex) {
				*Property->ContainerPtrToValuePtr<bool>(Container, ArrayIndex) = bSRGB;
			}, false);
	};
	const auto SubmitMaxResolution = [&](uint32 MaxResolution) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, MaxResolutionProperty),
			[MaxResolution](Durin::FProperty* Property, void* Container, uint32 ArrayIndex) {
				*Property->ContainerPtrToValuePtr<uint32>(Container, ArrayIndex) = MaxResolution;
			}, false);
	};
	const auto SubmitCompressionQuality = [&](Durin::ETextureCompressionQuality Quality) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, CompressionQualityProperty),
			[Quality](Durin::FProperty* Property, void* Container, uint32 ArrayIndex) {
				static_cast<Durin::FEnumProperty*>(Property)->SetValueFromUInt64(
					Container, static_cast<uint64>(Quality), ArrayIndex);
			}, false);
	};
	const auto SubmitAlphaMipMode = [&](Durin::ETextureAlphaMipMode Mode) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, AlphaMipModeProperty),
			[Mode](Durin::FProperty* Property, void* Container, uint32 ArrayIndex) {
				static_cast<Durin::FEnumProperty*>(Property)->SetValueFromUInt64(
					Container, static_cast<uint64>(Mode), ArrayIndex);
			}, false);
	};
	const auto SubmitAlphaCoverageThreshold = [&](float Threshold) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, AlphaCoverageThresholdProperty),
			[Threshold](Durin::FProperty* Property, void* Container, uint32 ArrayIndex) {
				*Property->ContainerPtrToValuePtr<float>(Container, ArrayIndex) = Threshold;
			}, false);
	};

	const auto InitialPlatformDataIdentity = Texture->GetPlatformDataShared();
	ASSERT_TRUE(SubmitUsage(Durin::ETextureUsage::Normal)) << Error;
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Texture->IsSRGB());
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);
	EXPECT_NE(Texture->GetPlatformDataShared(), InitialPlatformDataIdentity);
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());

	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_TRUE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Texture->IsSRGB());

	ASSERT_TRUE(SubmitSRGB(true)) << Error;
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_TRUE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_FALSE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);

	ASSERT_TRUE(SubmitMaxResolution(1)) << Error;
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetMaxResolution(), 1u);
	ASSERT_EQ(Texture->GetPlatformData()->Mips.size(), 1u);
	EXPECT_EQ(Texture->GetPlatformData()->Mips.front().Width, 1u);
	ASSERT_TRUE(SubmitCompressionQuality(Durin::ETextureCompressionQuality::High)) << Error;
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetCompressionQuality(), Durin::ETextureCompressionQuality::High);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetCompressionQuality(), Durin::ETextureCompressionQuality::Normal);
	EXPECT_EQ(Texture->GetMaxResolution(), 1u);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetMaxResolution(), 0u);
	EXPECT_EQ(Texture->GetPlatformData()->Mips.front().Width, 2u);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetMaxResolution(), 1u);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetCompressionQuality(), Durin::ETextureCompressionQuality::High);
	ASSERT_TRUE(SubmitAlphaMipMode(Durin::ETextureAlphaMipMode::PreserveCoverage)) << Error;
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::PreserveCoverage);
	ASSERT_TRUE(SubmitAlphaCoverageThreshold(0.4f)) << Error;
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_FLOAT_EQ(Texture->GetAlphaCoverageThreshold(), 0.4f);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_FLOAT_EQ(Texture->GetAlphaCoverageThreshold(), 0.5f);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::Average);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::PreserveCoverage);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_FLOAT_EQ(Texture->GetAlphaCoverageThreshold(), 0.4f);

	Error.clear();
	EXPECT_FALSE(SubmitUsage(static_cast<Durin::ETextureUsage>(255)));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);
	Error.clear();
	EXPECT_FALSE(SubmitCompressionQuality(static_cast<Durin::ETextureCompressionQuality>(255)));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Texture->GetCompressionQuality(), Durin::ETextureCompressionQuality::High);
	Error.clear();
	EXPECT_FALSE(SubmitAlphaMipMode(static_cast<Durin::ETextureAlphaMipMode>(255)));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Texture->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::PreserveCoverage);
	Error.clear();
	EXPECT_FALSE(SubmitAlphaCoverageThreshold(1.0f));
	EXPECT_FALSE(Error.empty());
	EXPECT_FLOAT_EQ(Texture->GetAlphaCoverageThreshold(), 0.4f);

	Transactions->Reset();
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/Transactional", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(
		AssetPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST(FTexture2DTests, AsyncBuildSettingCancellationAndSupersessionPreserveTransactions)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "AsyncTransactionalTextureSource.png";
	WriteTextureFixture(Source);
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Imported = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Source.generic_string(), "/TextureImportTests/AsyncTransactional");
	ASSERT_TRUE(Imported) << Imported.Message;
	Durin::DTexture2D* Texture = Imported.Asset;
	ASSERT_NE(Texture, nullptr);
	Durin::FProperty* UsageProperty = Texture->GetClass()->FindPropertyByName("Usage");
	ASSERT_NE(UsageProperty, nullptr);
	Durin::Editor::FPropertyView PropertyView;
	Durin::Tests::FTestTransactorOwner Transactions;
	std::string Error;
	const Durin::Editor::FPropertyViewContext Context{
		.Transactor = Transactions.Get(),
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); }};
	const auto SubmitUsage = [&](Durin::ETextureUsage Usage) {
		return PropertyView.SubmitPropertyValueEdit(
			Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, UsageProperty),
			[Usage](Durin::FProperty* Property, void* Container, uint32 ArrayIndex) {
				static_cast<Durin::FEnumProperty*>(Property)->SetValueFromUInt64(
					Container, static_cast<uint64>(Usage), ArrayIndex);
			},
			false);
	};
	ASSERT_TRUE(EnsureTextureCompilingManager());
	std::mutex Mutex;
	std::condition_variable Condition;
	bool bEntered = false;
	bool bRelease = false;
	Durin::AssetPrivate::SetTexture2DCompilationPhaseHookForTests(
		[&](uint64, Durin::ETexture2DCompilationPhase Phase) {
			if (Phase != Durin::ETexture2DCompilationPhase::Preparing) return;
			std::unique_lock Lock(Mutex);
			if (bEntered) return;
			bEntered = true;
			Condition.notify_all();
			Condition.wait(Lock, [&] { return bRelease; });
		});

	ASSERT_TRUE(SubmitUsage(Durin::ETextureUsage::Normal)) << Error;
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Condition.wait_for(Lock, std::chrono::seconds(10), [&] {
			return bEntered;
		}));
	}
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());
	EXPECT_FALSE(Transactions->CanUndo());
	Texture->SetBuildSettings(Texture->GetUsage(), Texture->IsSRGB(),
		Texture->GetMaxResolution(), Texture->GetCompressionQuality(),
		Texture->GetAlphaMipMode(), Texture->GetAlphaCoverageThreshold());
	{
		std::lock_guard Lock(Mutex);
		bRelease = true;
		Condition.notify_all();
	}
	Durin::AssetPrivate::SetTexture2DCompilationPhaseHookForTests({});
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());
	EXPECT_FALSE(Transactions->CanUndo());

	bEntered = false;
	bRelease = false;
	Durin::AssetPrivate::SetTexture2DCompilationPhaseHookForTests(
		[&](uint64, Durin::ETexture2DCompilationPhase Phase) {
			if (Phase != Durin::ETexture2DCompilationPhase::Preparing) return;
			std::unique_lock Lock(Mutex);
			if (bEntered) return;
			bEntered = true;
			Condition.notify_all();
			Condition.wait(Lock, [&] { return bRelease; });
		});
	ASSERT_TRUE(SubmitUsage(Durin::ETextureUsage::Normal)) << Error;
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Condition.wait_for(Lock, std::chrono::seconds(10), [&] {
			return bEntered;
		}));
	}
	ASSERT_TRUE(SubmitUsage(Durin::ETextureUsage::DataMask)) << Error;
	{
		std::lock_guard Lock(Mutex);
		bRelease = true;
		Condition.notify_all();
	}
	Durin::AssetPrivate::SetTexture2DCompilationPhaseHookForTests({});
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::DataMask);
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	ASSERT_TRUE(Transactions->CanUndo());
	const Durin::Editor::FTransactionId UndoId = Transactions->GetUndoId();
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Transactions->HasPendingOperation());
	EXPECT_NE(Transactions->GetUndoId(), UndoId);
	EXPECT_FALSE(Transactions->CanUndo());
	EXPECT_TRUE(Transactions->CanRedo());
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::DataMask);

	Transactions->Reset();
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureImportTests/AsyncTransactional", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(
		AssetPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST(FVolumeTextureTests, ArchiveBoundsReplacementAndSaveImmutability)
{
	using namespace Durin;
	FVolumeTexturePlatformData Source;
	Source.PixelFormat = EPixelFormat::R8_UNORM;
	auto& Mip = Source.Mips.emplace_back();
	Mip.Width = Mip.Height = Mip.Depth = Mip.RowPitch = Mip.DepthPitch = 1;
	Mip.Voxels = {std::byte{42}};
	ASSERT_TRUE(Source.IsValid());
	const FArchiveState Context{.Target = {"Win64", "Game"}};
	FByteBuffer Bytes;
	FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::DerivedDataPayload, Context);
	Source.Serialize(Writer);
	ASSERT_FALSE(Writer.HasError()) << Writer.GetError();
	FCountingArchive Counter(EArchivePurpose::DerivedDataPayload, Context);
	Source.Serialize(Counter);
	EXPECT_FALSE(Counter.HasError());
	EXPECT_EQ(Counter.Tell(), Bytes.size());
	FHashingArchive Hasher(EArchivePurpose::DerivedDataPayload, Context);
	Source.Serialize(Hasher);
	EXPECT_FALSE(Hasher.HasError());
	EXPECT_EQ(Hasher.Finalize(), FXxHash128::HashBuffer(Bytes));
	EXPECT_EQ(Source.Mips.size(), 1u);
	EXPECT_EQ(Source.Mips.front().Voxels, FByteBuffer{std::byte{42}});

	FByteBuffer Adjacent = Bytes;
	Adjacent.insert(Adjacent.end(), Bytes.begin(), Bytes.end());
	FCanonicalMemoryReader Parent(Adjacent);
	FByteView Region;
	ASSERT_TRUE(Parent.ReadRegion(Bytes.size(), Region));
	EXPECT_EQ(Region.data(), Adjacent.data());
	FCanonicalMemoryReader First(Region, EArchivePurpose::DerivedDataPayload, Context);
	FVolumeTexturePlatformData Loaded = Source;
	Loaded.Mips.push_back(Source.Mips.front());
	Loaded.Serialize(First);
	ASSERT_FALSE(First.HasError()) << First.GetError();
	EXPECT_TRUE(RequireArchiveEnd(First));
	EXPECT_EQ(Loaded.Mips.size(), 1u);
	EXPECT_EQ(Parent.GetRemainingPayloadBytes(), Bytes.size());

	for (size_t Size = 0; Size < Bytes.size(); ++Size)
	{
		FCanonicalMemoryReader Truncated(FByteView(Bytes).first(Size),
			EArchivePurpose::DerivedDataPayload, Context);
		FVolumeTexturePlatformData Discarded;
		Discarded.Serialize(Truncated);
		EXPECT_TRUE(Truncated.HasError()) << Size;
	}
	FCanonicalMemoryReader Trailing(Adjacent, EArchivePurpose::DerivedDataPayload, Context);
	Loaded.Serialize(Trailing);
	ASSERT_FALSE(Trailing.HasError());
	EXPECT_FALSE(RequireArchiveEnd(Trailing));
	EXPECT_EQ(Trailing.GetFailure()->Code, EArchiveFailureCode::TrailingData);

	auto Oversized = Bytes;
	const uint64 Size = MaximumTexturePayloadBytes + 1;
	for (uint32 Index = 0; Index < 8; ++Index)
		Oversized[56 + Index] = static_cast<std::byte>(Size >> (Index * 8));
	FCanonicalMemoryReader OversizedReader(Oversized, EArchivePurpose::DerivedDataPayload, Context);
	FVolumeTexturePlatformData Discarded;
	Discarded.Serialize(OversizedReader);
	ASSERT_TRUE(OversizedReader.HasError());
	EXPECT_EQ(OversizedReader.GetFailure()->Code, EArchiveFailureCode::LimitExceeded);
	EXPECT_TRUE(Discarded.Mips.empty());
	EXPECT_EQ(OversizedReader.Tell(), TexturePayloadHeaderSize);

	FArchiveState Unsupported{.Target = {"Other", "Game"}};
	FCanonicalMemoryReader UnsupportedReader(Bytes, EArchivePurpose::DerivedDataPayload, Unsupported);
	Discarded.Serialize(UnsupportedReader);
	ASSERT_TRUE(UnsupportedReader.HasError());
	EXPECT_EQ(UnsupportedReader.GetFailure()->Code, EArchiveFailureCode::UnsupportedTarget);
	EXPECT_EQ(UnsupportedReader.Tell(), 0u);
}

TEST(FVolumeTextureTests, RawArchiveWithoutBorrowingFailsExplicitly)
{
	using namespace Durin;
	// Supplies exact raw transfer but cannot lend the backing byte owner.
	class FRawReader final : public FArchive
	{
	public:
		explicit FRawReader(FByteView Bytes)
			: FArchive({.Direction = EArchiveDirection::Load,
				.Capabilities = EArchiveCapability::RawBytes,
				.Target = {"Win64", "Game"}}), Reader(Bytes) {}
		auto SerializeRawBytes(FMutableByteView Bytes) -> void override
		{
			Reader.ReadBytes(Bytes);
			if (Reader.HasError()) Fail(Reader.GetFailure()->Code, Reader.GetError());
		}
		FCanonicalMemoryReader Reader;
	};
	FVolumeTexturePlatformData Source;
	Source.PixelFormat = EPixelFormat::R8_UNORM;
	auto& Mip = Source.Mips.emplace_back();
	Mip.Width = Mip.Height = Mip.Depth = Mip.RowPitch = Mip.DepthPitch = 1;
	Mip.Voxels = {std::byte{42}};
	FByteBuffer Bytes;
	const FArchiveState Context{.Target = {"Win64", "Game"}};
	FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::DerivedDataPayload, Context);
	Source.Serialize(Writer);
	ASSERT_FALSE(Writer.HasError());
	FRawReader Reader(Bytes);
	FVolumeTexturePlatformData Discarded;
	Discarded.Serialize(Reader);
	ASSERT_TRUE(Reader.HasError());
	EXPECT_EQ(Reader.GetFailure()->Code, EArchiveFailureCode::UnsupportedCapability);
	EXPECT_EQ(Reader.Reader.Tell(), TexturePayloadHeaderSize);
	EXPECT_TRUE(Discarded.Mips.empty());
}
