#include "AssetForge/Builtins/Texture2DImport.h"
#include "AssetForge/Builtins/SceneImportData.h"
#include "AssetForge/Builtins/Texture2DFactory.h"
#include "Asset/AssetImportData.h"
#include "DObject/Package.h"
#include "EncodedSourceSnapshot.h"
#include "Asset/SourceHint.h"
#include "Asset/Asset.h"
#include "DObject/DObjectGlobals.h"
#include "Hash/XxHash.h"
#include "Image/ImageDecoder.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/StringHelper.h"
#include "Texture/Texture2DCompilation.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin;
	auto InferTexture2DImportSettings(std::string_view Filename, const FTextureSource* Source)
		-> FTexture2DImportSettings
	{
		std::string Stem = std::filesystem::path(Filename).stem().generic_string();
		for (char& Character : Stem)
			if (Character >= 'A' && Character <= 'Z') Character += 'a' - 'A';
		const size_t Separator = Stem.find_last_of("_- .");
		const std::string_view Token = std::string_view(Stem).substr(
			Separator == std::string::npos ? 0 : Separator + 1);
		FTexture2DImportSettings Settings;
		if (Token == "normal" || Token == "normalgl" || Token == "normaldx" || Token == "n")
			Settings.Usage = ETextureUsage::Normal;
		else if (Token == "roughness" || Token == "metallic" || Token == "metalness"
			|| Token == "ao" || Token == "occlusion" || Token == "height"
			|| Token == "displacement" || Token == "mask" || Token == "opacity"
			|| Token == "orm" || Token == "rma" || Token == "mra")
			Settings.Usage = ETextureUsage::DataMask;
		else if (Token != "color" && Token != "albedo" && Token != "basecolor"
			&& Token != "diffuse" && Token != "emissive" && Source
			&& Source->GetSourceChannelCount() >= 3 && Source->GetFormat() == ETextureSourceFormat::RGBA8)
		{
			// Inspect encoded RGB, without gamma conversion or another source read.
			// Tangent-space normals should be varied, nearly unit length, centered
			// in X/Y and predominantly face +Z. Flat colors remain ambiguous.
			const auto Mips = Source->GetMipData();
			const auto Image = Mips.GetMipImage(0, 0, 0);
			if (!Image.IsValid()) return Settings;
			const auto Pixels = Image.GetPixels();
			const size_t Count = Pixels.size() / 4;
			if (Count < 16) return Settings;
			const size_t Samples = std::min<size_t>(Count, 4096);
			size_t NormalSamples = 0;
			double SumX = 0, SumY = 0, SumZ = 0, SumXY2 = 0;
			for (size_t Sample = 0; Sample < Samples; ++Sample)
			{
				const size_t Offset = (Sample * Count / Samples) * 4;
				const double X = std::to_integer<uint8>(Pixels[Offset]) / 127.5 - 1.0;
				const double Y = std::to_integer<uint8>(Pixels[Offset + 1]) / 127.5 - 1.0;
				const double Z = std::to_integer<uint8>(Pixels[Offset + 2]) / 127.5 - 1.0;
				const double Length2 = X * X + Y * Y + Z * Z;
				if (Z > 0.2 && Length2 > 0.88 && Length2 < 1.12) ++NormalSamples;
				SumX += X; SumY += Y; SumZ += Z; SumXY2 += X * X + Y * Y;
			}
			const double MeanX = SumX / Samples, MeanY = SumY / Samples;
			const double Variance = SumXY2 / Samples - MeanX * MeanX - MeanY * MeanY;
			if (NormalSamples * 100 >= Samples * 95 && SumZ / Samples > 0.5
				&& MeanX > -0.12 && MeanX < 0.12 && MeanY > -0.12 && MeanY < 0.12
				&& Variance > 0.002)
				Settings.Usage = ETextureUsage::Normal;
		}
		return Settings;
	}

	auto MakeTexture2DBuildSettings(const DTexture2D& Texture)
		-> FTexture2DBuildSettings;
	namespace
	{
		auto ResolveOwningPackagePhysicalPath(
			std::string_view PackagePath,
			std::filesystem::path& OutPath) -> std::expected<void, std::string>
		{
			const auto Resolved =
				FMountPaths::ResolveAssetPath(
					PackagePath, EMountPathExistence::AllowMissing);
			if (!Resolved)
			{
				return std::unexpected(std::string("Texture2D package path could not be resolved: ") + std::string(PackagePath));
			}
			OutPath = Resolved->PhysicalPath;
			OutPath += ".dasset";
			return {};
		}

		auto MakeReimportResult(std::expected<void, std::string> Result) -> FReimportResult
		{
			if (Result) return {EReimportResult::Succeeded, {}};
			auto Cause = std::make_shared<FTexture2DFactoryError>(Result.error());
			return {EReimportResult::Failed, std::move(Result.error()), std::move(Cause)};
		}

		auto CommitTexture2DImport(DTexture2D& Texture, FTextureSource Source,
			const FTexture2DBuildSettings& Settings, std::string Filename,
			ESourceHintBase HintBase, std::string DisplayLabel,
			FXxHash128 ContentHash, uint64 ByteCount) -> std::expected<void, std::string>
		{
			FAssetImportDataState State;
			State.SourceData.Sources.push_back({
				.Role = "source", .DisplayLabel = std::move(DisplayLabel),
				.Hint = std::move(Filename), .HintBase = HintBase,
				.ContentHashLow = ContentHash.HashLow, .ContentHashHigh = ContentHash.HashHigh,
				.ByteCount = ByteCount});
			if (auto Committed = Texture.SetImportedSource(std::move(Source), Settings, std::move(State)); !Committed)
				return Committed;
			// Derived data is rebuildable. Admission/build failure cannot undo the import.
			Texture.PostEditChange();
			return {};
		}

		auto ImportTexture2DFromFilename(
			DTexture2D& Texture,
			std::string Filename,
			ESourceHintBase HintBase,
			const FTexture2DBuildSettings& Settings,
			std::optional<std::filesystem::path> SelectedPhysicalPath = {}) -> std::expected<void, std::string>
		{
			if (!Texture.GetPackage())
			{
				return std::unexpected(std::string("Texture2D source capture requires an owning package."));
			}
			std::filesystem::path OwningPackagePath;
			if (const auto Resolved = ResolveOwningPackagePhysicalPath(
				Texture.GetPackage()->GetPackagePath(), OwningPackagePath); !Resolved) return Resolved;
			std::filesystem::path PhysicalPath;
			if (SelectedPhysicalPath)
				PhysicalPath = std::move(*SelectedPhysicalPath);
			else
			{
				std::string PhysicalPathText;
				auto Resolved = ResolveSourceHint(HintBase, Filename, OwningPackagePath.generic_string());
				if (Resolved) { PhysicalPathText = Resolved->generic_string(); }
				if (!Resolved) return std::unexpected(FormatSourceHintError(Resolved.error()));
				PhysicalPath = PhysicalPathText;
			}
			if (!std::filesystem::is_regular_file(PhysicalPath))
			{
				return std::unexpected(std::string("Texture2D source file is missing: ") + PhysicalPath.generic_string());
			}
			auto Captured = CaptureEncodedSource(
				PhysicalPath.generic_string(), PhysicalPath,
				64ull * 1'024ull * 1'024ull);
			if (!Captured)
				return std::unexpected(FormatEncodedSourceError(Captured.error()));
			auto Snapshot = std::move(*Captured);
			if (SelectedPhysicalPath)
			{
				if (auto Hint = MakeSourceHint(PhysicalPath.generic_string(), OwningPackagePath.generic_string()); !Hint)
					return std::unexpected(FormatSourceHintError(Hint.error()));
				else { HintBase = Hint->Base; Filename = std::move(Hint->Hint); }
			}
			auto Translated = TranslateTexture2DSource(Snapshot.GetBytes());
			if (!Translated)
				return std::unexpected(Translated.error());
			const FXxHash128 ContentHash = Snapshot.ContentHash;
			const uint64 ByteCount = Snapshot.FileSize;
			const std::string DisplayLabel = PhysicalPath.filename().generic_string();
			FTextureSource Candidate = std::move(*Translated);
			return CommitTexture2DImport(Texture, std::move(Candidate), Settings,
				std::move(Filename), HintBase, DisplayLabel, ContentHash, ByteCount);
		}
	}

	DTexture2DFactory::DTexture2DFactory(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SupportedClass = DTexture2D::StaticClass();
		Formats = {"png", "jpg", "jpeg", "bmp", "tga"};
	}

	auto PrepareTexture2DImport(std::string_view Filename) -> FTexture2DPreparationResult
	{
		FPreparedTexture2DImport OutPrepared;
		std::error_code Error;
		const auto Input = std::filesystem::absolute(Filename, Error).lexically_normal();
		if (Error) return std::unexpected(Error.message());
		if (!IsTexture2DSourceExtension(Input.extension().generic_string()))
			return std::unexpected(std::string("Unsupported texture source format."));
		auto Captured = CaptureEncodedSource(Input.generic_string(), Input,
			64ull * 1'024ull * 1'024ull);
		if (!Captured)
			return std::unexpected(FormatEncodedSourceError(Captured.error()));
		auto Snapshot = std::move(*Captured);
		auto Translated = TranslateTexture2DSource(Snapshot.GetBytes());
		if (!Translated)
			return std::unexpected(Translated.error());
		OutPrepared.Source = std::move(*Translated);
		OutPrepared.Filename = Input.generic_string();
		OutPrepared.ContentHash = Snapshot.ContentHash;
		OutPrepared.ByteCount = Snapshot.FileSize;
		OutPrepared.InferredSettings = InferTexture2DImportSettings(Filename, &OutPrepared.Source);
		return OutPrepared;
	}

	auto DTexture2DFactory::FactoryCreateFromFile(
		DClass* InClass,
		DObject* InParent,
		FName InName,
		EObjectFlags Flags,
		std::string_view Filename,
		DObject*,
		FFactoryDiagnostics* Diagnostics) const -> DObject*
	{
		auto Reject = [&](EFactoryError Code, std::string_view PreparedFilename = {}) -> DObject* {
			if (Diagnostics) Diagnostics->ReportFailure({
				.Code = Code,
				.ExpectedClass = DTexture2D::StaticClass()->GetName(),
				.RequestedClass = InClass ? InClass->GetName() : std::string{},
				.Filename = std::string(Filename), .PreparedFilename = std::string(PreparedFilename)});
			return nullptr;
		};

		if (InClass != DTexture2D::StaticClass())
			return Reject(EFactoryError::ExactClass);
		auto* Package = Cast<DPackage>(InParent);
		if (!Package || !Package->IsAssetPackage())
			return Reject(EFactoryError::AssetPackageParent);
		FPreparedTexture2DImport Captured;
		if (!Prepared)
		{
			auto Preparation = PrepareTexture2DImport(Filename);
			if (!Preparation)
			{
				if (Diagnostics) Diagnostics->ReportDomainFailure(std::make_shared<FTexture2DFactoryError>(Preparation.error()));
				return nullptr;
			}
			Captured = std::move(*Preparation);
		}
		const auto& InputData = Prepared ? *Prepared : Captured;
		const std::filesystem::path Input(InputData.Filename);
		if (!InputData.Source.IsValid()
			|| Input != std::filesystem::absolute(Filename).lexically_normal())
			return Reject(EFactoryError::PreparedSourceMismatch, InputData.Filename);
		std::filesystem::path OwningPackagePath;
		if (const auto Resolved = ResolveOwningPackagePhysicalPath(
			Package->GetPackagePath(), OwningPackagePath); !Resolved)
		{
			if (Diagnostics) Diagnostics->ReportDomainFailure(std::make_shared<FTexture2DFactoryError>(Resolved.error()));
			return nullptr;
		}
		std::string SourceHint;
		ESourceHintBase HintBase;
		if (auto Hint = MakeSourceHint(Input.generic_string(), OwningPackagePath.generic_string()); !Hint)
		{
			if (Diagnostics) Diagnostics->ReportFailure({
				.Code = EFactoryError::SourceHint, .Filename = std::string(Filename),
				.SourceHintCause = std::make_shared<FSourceHintError>(Hint.error())});
			return nullptr;
		}
		else { HintBase = Hint->Base; SourceHint = std::move(Hint->Hint); }
		const FTexture2DImportSettings EffectiveSettings = bAutoDetectSettings
			? InputData.InferredSettings : Settings;
		auto* Texture = NewObject<DTexture2D>(InClass, Package, InName, Flags);
		if (!Texture) return Reject(EFactoryError::ObjectCreation);
		const FTexture2DBuildSettings BuildSettings{
			.Usage = EffectiveSettings.Usage,
			.CompressionQuality = EffectiveSettings.CompressionQuality,
			.AlphaMipMode = EffectiveSettings.AlphaMipMode,
			.AlphaCoverageThreshold = EffectiveSettings.AlphaCoverageThreshold,
			.MaxResolution = EffectiveSettings.MaxResolution,
			.bSRGB = EffectiveSettings.bSRGB};
		if (const auto Imported = CommitTexture2DImport(*Texture, InputData.Source, BuildSettings,
			std::move(SourceHint), HintBase, Input.filename().generic_string(),
			InputData.ContentHash, InputData.ByteCount); !Imported)
		{
			if (Diagnostics) Diagnostics->ReportDomainFailure(std::make_shared<FTexture2DFactoryError>(Imported.error()));
			return nullptr;
		}
		return Texture;
	}

	auto DTexture2DFactory::QueryReimportActions(std::string_view AssetClassName) const
		-> FReimportActions
	{
		if (AssetClassName != DTexture2D::StaticClass()->GetQualifiedName().ToString())
			return {};
		return {.bSupportsReimport = true, .bSupportsReimportFromFile = true};
	}

	auto DTexture2DFactory::GetSourceFileDialogs(const DObject& Object) const
		-> std::vector<FReimportSourceFileDialog>
	{
		if (!Cast<DTexture2D>(&Object) || IsOneTimeModelImportedAsset(Object)) return {};
		return {{"Reimport Texture2D From File", "Supported Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"}};
	}

	auto DTexture2DFactory::GetReimportCapabilities(const DObject& Object) const
		-> FReimportCapabilities
	{
		if (IsOneTimeModelImportedAsset(Object)) return {.Diagnostic = std::string(ModelOneTimeImportDiagnostic)};
		const auto* Texture = Cast<DTexture2D>(&Object);
		if (!Texture || !Texture->GetPackage())
			return {.Diagnostic = "Only packaged Texture2D assets can be reimported."};
		const DAssetImportData* ImportData = Texture->GetAssetImportData();
		const FSourceFile* Source = ImportData
			? ImportData->GetSourceData().FindByRole("source") : nullptr;
		const bool bHasSource = Source && !Source->Hint.empty();
		return {
			.bCanReimport = bHasSource,
			.bCanReimportFromFile = true,
			.Diagnostic = bHasSource ? std::string{}
				: "Texture2D has no source hint to reimport."};
	}

	auto DTexture2DFactory::Reimport(DObject& Object, FReimportCompletion Completion) const -> void
	{
		auto* Texture = Cast<DTexture2D>(&Object);
		auto Result = Texture ? ReimportTexture2D(*Texture)
			: FReimportResult{EReimportResult::Failed, "Only Texture2D assets can be reimported."};
		if (Completion) Completion(std::move(Result));
	}

	auto DTexture2DFactory::ReimportFromFiles(DObject& Object,
		std::span<const std::string> Filenames, FReimportCompletion Completion) const -> void
	{
		auto* Texture = Cast<DTexture2D>(&Object);
		if (!Texture || Filenames.size() != 1)
		{
			if (Completion) Completion({EReimportResult::Failed, "Texture2D reimport requires exactly one source file."});
			return;
		}
		auto Result = ReimportTexture2DFromFile(*Texture, Filenames.front());
		if (Completion) Completion(std::move(Result));
	}

	auto IsTexture2DSourceExtension(std::string_view Extension) -> bool
	{
		const std::string Lowercase = StringUtils::FoldAscii(Extension);
		return Lowercase == ".png" || Lowercase == ".jpg" || Lowercase == ".jpeg"
			|| Lowercase == ".bmp" || Lowercase == ".tga";
	}

	auto TranslateTexture2DSource(FByteView EncodedBytes) -> FTexture2DTranslationResult
	{
		FTextureSource SourceData;
		auto Decoded = Image::DecodeImageFromMemory(EncodedBytes, {.MaximumDecodedPixels = 16384ull * 16384ull});
		if (!Decoded) return std::unexpected(Image::ToString(Decoded.error()));
		auto DecodedImage = std::move(*Decoded);
		if (DecodedImage.Width > 16384 || DecodedImage.Height > 16384)
			return std::unexpected(std::format("Texture dimensions {}x{} exceed the 16384 pixel limit.",
				DecodedImage.Width, DecodedImage.Height));
		Image::FImage Image;
		auto ImageResult1 = Image::FImage::TryCreate({.Width = DecodedImage.Width,
			.Height = DecodedImage.Height, .Format = Image::ERawImageFormat::RGBA8}, std::move(DecodedImage.Pixels));
		if (!ImageResult1) return std::unexpected(std::string("Decoded texture source data is invalid."));
		Image = std::move(*ImageResult1);
		if (SourceData.Init2D(Image.GetView(), DecodedImage.SourceChannelCount,
			DecodedImage.bHasTransparency ? 1 : 0)) return SourceData;
		return std::unexpected(std::string("Decoded texture source data is invalid."));
	}

	auto MakeTexture2DBuildSettings(const DTexture2D& Texture)
		-> FTexture2DBuildSettings
	{
		return {
			.Usage = Texture.GetUsage(),
			.CompressionQuality = Texture.GetCompressionQuality(),
			.AlphaMipMode = Texture.GetAlphaMipMode(),
			.AlphaCoverageThreshold = Texture.GetAlphaCoverageThreshold(),
			.MaxResolution = Texture.GetMaxResolution(),
			.bSRGB = Texture.IsSRGB()};
	}

	auto RebuildTexture2DFromSource(
		DTexture2D& Texture,
		const FTexture2DBuildSettings& Settings,
		ETexture2DCompilationPriority Priority,
		FTextureCompilationCompletion Completion) -> std::expected<void, FTextureBuildOperationError>
	{
		if (!Texture.GetPackage())
			return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::InvalidInput, "Texture updates require a package."});
		if (!Texture.GetSource().IsValid())
			return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::InvalidInput, "Texture source is invalid."});
		auto Build = Texture.CreateBuildRequest(Settings);
		if (!Build) return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::InvalidInput, FormatTexture2DInputError(Build.error())});
		return Texture.BeginCachePlatformData({
			.Build = std::move(*Build),
			.ResultApplication = {
				.bMarkPackageDirty = true},
			.Priority = Priority}, std::move(Completion));
	}

	auto ReimportTexture2D(
		DTexture2D& Texture) -> FReimportResult
	{
		if (IsOneTimeModelImportedAsset(Texture)) return MakeReimportResult(std::unexpected(std::string(ModelOneTimeImportDiagnostic)));
		const DAssetImportData* ImportData = Texture.GetAssetImportData();
		const FSourceFile* Source = ImportData
			? ImportData->GetSourceData().FindByRole("source") : nullptr;
		if (!Source || Source->Hint.empty())
		{
			return MakeReimportResult(std::unexpected(std::string("Texture2D has no source hint to reimport.")));
		}
		return MakeReimportResult(ImportTexture2DFromFilename(
			Texture, Source->Hint, Source->HintBase,
			MakeTexture2DBuildSettings(Texture)));
	}

	auto ReimportTexture2DFromFile(
		DTexture2D& Texture,
		std::string_view FilePath) -> FReimportResult
	{
		if (IsOneTimeModelImportedAsset(Texture)) return MakeReimportResult(std::unexpected(std::string(ModelOneTimeImportDiagnostic)));
		if (FilePath.empty())
		{
			return MakeReimportResult(std::unexpected(std::string("Texture2D source file is missing: ") + std::string(FilePath)));
		}
		const std::filesystem::path Requested =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Requested))
		{
			return MakeReimportResult(std::unexpected(std::string("Texture2D source file is missing: ") + std::string(FilePath)));
		}
		return MakeReimportResult(ImportTexture2DFromFilename(
			Texture, {}, ESourceHintBase::AssetRelative,
			MakeTexture2DBuildSettings(Texture), Requested));
	}

	namespace
	{
		auto RejectTextureSettings(const DTexture2D& Texture, ETexture2DInputError Code,
			const FTexture2DBuildSettings& Settings) -> std::expected<void, FTextureBuildOperationError>
		{
			return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::InvalidInput,
				FormatTexture2DInputError(FTexture2DInputError{.Code = Code, .Settings = Settings})});
		}
	}

	auto SetTexture2DUsage(DTexture2D& Texture, ETextureUsage Usage) -> std::expected<void, FTextureBuildOperationError>
	{
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.Usage = Usage;
		if (!IsValidTextureUsage(Usage)) return RejectTextureSettings(Texture, ETexture2DInputError::InvalidUsage, Settings);
		if (Texture.GetUsage() == Usage) return {};
		Settings.bSRGB = GetDefaultTextureSRGB(Usage);
		return RebuildTexture2DFromSource(Texture, Settings);
	}

	auto SetTexture2DSRGB(DTexture2D& Texture, bool bSRGB) -> std::expected<void, FTextureBuildOperationError>
	{
		if (Texture.IsSRGB() == bSRGB) return {};
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.bSRGB = bSRGB;
		return RebuildTexture2DFromSource(Texture, Settings);
	}

	auto SetTexture2DMaxResolution(DTexture2D& Texture, uint32 MaxResolution) -> std::expected<void, FTextureBuildOperationError>
	{
		if (Texture.GetMaxResolution() == MaxResolution) return {};
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.MaxResolution = MaxResolution;
		return RebuildTexture2DFromSource(Texture, Settings);
	}

	auto SetTexture2DCompressionQuality(DTexture2D& Texture,
		ETextureCompressionQuality Quality) -> std::expected<void, FTextureBuildOperationError>
	{
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.CompressionQuality = Quality;
		if (!IsValidTextureCompressionQuality(Quality)) return RejectTextureSettings(Texture, ETexture2DInputError::InvalidCompressionQuality, Settings);
		if (Texture.GetCompressionQuality() == Quality) return {};
		return RebuildTexture2DFromSource(Texture, Settings);
	}

	auto SetTexture2DAlphaMipMode(DTexture2D& Texture, ETextureAlphaMipMode Mode) -> std::expected<void, FTextureBuildOperationError>
	{
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.AlphaMipMode = Mode;
		if (!IsValidTextureAlphaMipMode(Mode)) return RejectTextureSettings(Texture, ETexture2DInputError::InvalidAlphaMipMode, Settings);
		if (Texture.GetAlphaMipMode() == Mode) return {};
		return RebuildTexture2DFromSource(Texture, Settings);
	}

	auto SetTexture2DAlphaCoverageThreshold(DTexture2D& Texture, float Threshold) -> std::expected<void, FTextureBuildOperationError>
	{
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.AlphaCoverageThreshold = Threshold;
		if (!IsValidTextureAlphaCoverageThreshold(Threshold)) return RejectTextureSettings(Texture, ETexture2DInputError::InvalidAlphaCoverageThreshold, Settings);
		if (Texture.GetAlphaCoverageThreshold() == Threshold) return {};
		return RebuildTexture2DFromSource(Texture, Settings);
	}
}
