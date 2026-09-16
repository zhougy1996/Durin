#include "AssetForge/Builtins/Texture2DImport.h"
#include "AssetForge/Builtins/Texture2DFactory.h"
#include "Asset/AssetImportData.h"
#include "DObject/Package.h"
#include "EncodedSourceSnapshot.h"
#include "Asset/PackageSerialization.h"
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
			std::filesystem::path& OutPath,
			std::string& OutError) -> bool
		{
			const FAssetPathResult Resolved =
				FMountPaths::ResolveAssetPath(
					PackagePath, EMountPathExistence::AllowMissing);
			if (!Resolved)
			{
				OutError = Resolved.Message;
				return false;
			}
			OutPath = Resolved.PhysicalPath;
			OutPath += ".dasset";
			OutError.clear();
			return true;
		}

		auto PublishTexture2DImportData(
			DTexture2D& Texture,
			std::string Filename,
			ESourceHintBase HintBase,
			std::string DisplayLabel,
			FXxHash128 ContentHash,
			uint64 ByteCount,
			std::string& OutError) -> bool
		{
			FAssetImportDataState State;
			State.SourceData.Sources.push_back({
				.Role = "source",
				.DisplayLabel = std::move(DisplayLabel),
				.Hint = std::move(Filename),
				.HintBase = HintBase,
				.ContentHashLow = ContentHash.HashLow,
				.ContentHashHigh = ContentHash.HashHigh,
				.ByteCount = ByteCount});
			auto* ImportData = Texture.GetAssetImportData();
			if (!ImportData)
				ImportData = NewObject<DAssetImportData>(
					&Texture, "AssetImportData");
			State.SourceData.Normalize();
			if (!State.Validate(OutError)) return false;
			if (!ImportData) { OutError = "Could not allocate asset import data."; return false; }
			ImportData->SetState(std::move(State));
			Texture.SetAssetImportData(*ImportData);
			Texture.MarkPackageDirty();
			return true;
		}

		auto SubmitTexture2DFromFilename(
			DTexture2D& Texture,
			std::string Filename,
			ESourceHintBase HintBase,
			const FTexture2DBuildSettings& Settings,
			std::string& OutError,
			ETexture2DCompilationPriority Priority,
			FTexture2DCompilationCompletion Completion,
			bool bPublishImportData,
			bool bSave,
			std::optional<std::filesystem::path> SelectedPhysicalPath = {}) -> bool
		{
			if (!Texture.GetPackage())
			{
				OutError = "Texture2D source capture requires an owning package.";
				return false;
			}
			std::filesystem::path OwningPackagePath;
			if (!ResolveOwningPackagePhysicalPath(
					Texture.GetPackage()->GetPackagePath(), OwningPackagePath, OutError))
				return false;
			std::filesystem::path PhysicalPath;
			if (SelectedPhysicalPath)
				PhysicalPath = std::move(*SelectedPhysicalPath);
			else
			{
				std::string PhysicalPathText;
				const bool bResolved = ResolveSourceHint(
					HintBase, Filename, OwningPackagePath.generic_string(),
					PhysicalPathText, OutError);
				if (!bResolved) return false;
				PhysicalPath = PhysicalPathText;
			}
			if (!std::filesystem::is_regular_file(PhysicalPath))
			{
				OutError = std::format("Texture2D source file is missing: {}.", Filename);
				return false;
			}
			FEncodedSourceSnapshot Snapshot;
			if (!CaptureEncodedSource(
				PhysicalPath.generic_string(), PhysicalPath, Snapshot, OutError,
				64ull * 1'024ull * 1'024ull)) return false;
			if (SelectedPhysicalPath
				&& !MakeSourceHint(
					PhysicalPath.generic_string(), OwningPackagePath.generic_string(),
					HintBase, Filename, OutError)) return false;
			FTextureSource SourceData;
			if (!TranslateTexture2DSource(
				Snapshot.GetBytes(), SourceData, OutError)) return false;
			const FXxHash128 ContentHash = Snapshot.ContentHash;
			const uint64 ByteCount = Snapshot.FileSize;
			const std::string DisplayLabel = PhysicalPath.filename().generic_string();
			FTextureSource Candidate = std::move(SourceData);
			return SubmitTexture2DCompilation(Texture, {
				.Build = MakeTexture2DBuildRequest(Candidate, Settings),
				.ResultApplication = {
					.SourceReplacement = std::move(Candidate),
					.bMarkPackageDirty = bPublishImportData,
					.bReportLoadMutation = !bPublishImportData},
				.Priority = Priority}, OutError,
				[&Texture, Filename, HintBase, DisplayLabel,
					ContentHash, ByteCount,
					bPublishImportData, bSave,
					Completion = std::move(Completion)](
						FTexture2DCompilationResult Result) mutable {
					if (Result.Succeeded() && bPublishImportData)
					{
						std::string Error;
						if (!PublishTexture2DImportData(Texture, std::move(Filename), HintBase,
							DisplayLabel, ContentHash, ByteCount, Error))
						{
							Result.Status = ETexture2DCompilationStatus::Failed;
							Result.Diagnostic = std::move(Error);
						}
						else if (bSave)
						{
							const FAssetResult Saved =
								SavePackage(Texture.GetPackage());
							if (!Saved)
							{
								Result.Status = ETexture2DCompilationStatus::Failed;
								Result.Diagnostic = Saved.Message;
							}
						}
					}
					if (Completion) Completion(std::move(Result));
				});
		}
	}

	DTexture2DFactory::DTexture2DFactory(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SupportedClass = DTexture2D::StaticClass();
		Formats = {"png", "jpg", "jpeg", "bmp", "tga"};
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
		auto Failed = [&](std::string Message) -> DObject* {
			if (Diagnostics) Diagnostics->Report(Message);
			return nullptr;
		};
		if (InClass != DTexture2D::StaticClass())
			return Failed("Texture2D factory requires the exact Texture2D class.");
		auto* Package = Cast<DPackage>(InParent);
		if (!Package || !Package->IsAssetPackage())
			return Failed("Texture2D factory requires an asset package parent.");
		const std::filesystem::path Input =
			std::filesystem::absolute(Filename).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
			return Failed("Source file does not exist.");
		if (!IsTexture2DSourceExtension(Input.extension().generic_string()))
			return Failed("Unsupported texture source format.");

		std::string Error;
		FEncodedSourceSnapshot Snapshot;
		if (!CaptureEncodedSource(
			Input.generic_string(), Input, Snapshot, Error,
			64ull * 1'024ull * 1'024ull)) return Failed(std::move(Error));
		std::filesystem::path OwningPackagePath;
		if (!ResolveOwningPackagePhysicalPath(
			Package->GetPackagePath(), OwningPackagePath, Error))
			return Failed(std::move(Error));
		std::string SourceHint;
		ESourceHintBase HintBase;
		if (!MakeSourceHint(
			Input.generic_string(), OwningPackagePath.generic_string(),
			HintBase, SourceHint, Error)) return Failed(std::move(Error));
		FTextureSource SourceData;
		if (!TranslateTexture2DSource(
			Snapshot.GetBytes(), SourceData, Error)) return Failed(std::move(Error));
		const FTexture2DImportSettings EffectiveSettings = bAutoDetectSettings
			? InferTexture2DImportSettings(Filename, &SourceData) : Settings;

		auto* Texture = NewObject<DTexture2D>(
			InClass, Package, InName, Flags);
		if (!Texture) return Failed("Texture2D object could not be created.");
		FTextureSource Candidate = std::move(SourceData);
		if (!BuildTexture2DSynchronously(*Texture, MakeTexture2DBuildRequest(Candidate, {
				.Usage = EffectiveSettings.Usage,
				.CompressionQuality = EffectiveSettings.CompressionQuality,
				.AlphaMipMode = EffectiveSettings.AlphaMipMode,
				.AlphaCoverageThreshold = EffectiveSettings.AlphaCoverageThreshold,
				.MaxResolution = EffectiveSettings.MaxResolution,
				.bSRGB = EffectiveSettings.bSRGB}), {.SourceReplacement = Candidate}, Error))
			return Failed(std::move(Error));
		if (!PublishTexture2DImportData(
			*Texture, std::move(SourceHint), HintBase,
			Input.filename().generic_string(), Snapshot.ContentHash,
			Snapshot.FileSize, Error)) return Failed(std::move(Error));
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
		if (!Cast<DTexture2D>(&Object)) return {};
		return {{"Reimport Texture2D From File", "Supported Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"}};
	}

	auto DTexture2DFactory::GetReimportCapabilities(const DObject& Object) const
		-> FReimportCapabilities
	{
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

	auto DTexture2DFactory::Reimport(
		DObject& Object, FReimportCompletion Completion) const -> void
	{
		auto* Texture = Cast<DTexture2D>(&Object);
		const DAssetImportData* ImportData = Texture ? Texture->GetAssetImportData() : nullptr;
		const FSourceFile* Source = ImportData
			? ImportData->GetSourceData().FindByRole("source") : nullptr;
		if (!Texture || !Source || Source->Hint.empty())
		{
			if (Completion) Completion({EReimportStatus::MissingSource,
				"Texture2D has no source hint to reimport."});
			return;
		}
		std::string Error;
		const bool bSubmitted = SubmitTexture2DFromFilename(
			*Texture, Source->Hint, Source->HintBase,
			MakeTexture2DBuildSettings(*Texture), Error,
			ETexture2DCompilationPriority::Interactive,
			[Completion](FTexture2DCompilationResult Result) mutable {
				if (Completion) Completion(Result.Succeeded()
					? FReimportResult{EReimportStatus::Succeeded, {}}
					: FReimportResult{EReimportStatus::SourceOrBuildFailure,
						Result.Diagnostic.empty() ? "Texture2D reimport failed."
							: std::move(Result.Diagnostic)});
			}, true, false);
		if (!bSubmitted && Completion)
			Completion({EReimportStatus::SourceOrBuildFailure, std::move(Error)});
	}

	auto DTexture2DFactory::ReimportFromFiles(DObject& Object,
		std::span<const std::string> Filenames, FReimportCompletion Completion) const
		-> void
	{
		auto* Texture = Cast<DTexture2D>(&Object);
		if (!Texture || Filenames.size() != 1 || Filenames.front().empty())
		{
			if (Completion) Completion({EReimportStatus::SourceOrBuildFailure,
				"Texture2D reimport requires exactly one source file."});
			return;
		}
		const std::filesystem::path Requested =
			std::filesystem::absolute(Filenames.front()).lexically_normal();
		std::string Error;
		const bool bSubmitted = SubmitTexture2DFromFilename(
			*Texture, {}, ESourceHintBase::AssetRelative,
			MakeTexture2DBuildSettings(*Texture), Error,
			ETexture2DCompilationPriority::Interactive,
			[Completion](FTexture2DCompilationResult Result) mutable {
				if (Completion) Completion(Result.Succeeded()
					? FReimportResult{EReimportStatus::Succeeded, {}}
					: FReimportResult{EReimportStatus::SourceOrBuildFailure,
						Result.Diagnostic.empty() ? "Texture2D reimport from file failed."
							: std::move(Result.Diagnostic)});
			}, true, false, Requested);
		if (!bSubmitted && Completion)
			Completion({EReimportStatus::SourceOrBuildFailure, std::move(Error)});
	}

	auto IsTexture2DSourceExtension(std::string_view Extension) -> bool
	{
		const std::string Lowercase = StringUtils::FoldAscii(Extension);
		return Lowercase == ".png" || Lowercase == ".jpg" || Lowercase == ".jpeg"
			|| Lowercase == ".bmp" || Lowercase == ".tga";
	}

	auto TranslateTexture2DSource(
		FByteView EncodedBytes,
		FTextureSource& OutSourceData,
		std::string& OutError) -> bool
	{
		OutSourceData = {};
		Image::FDecodedImage DecodedImage;
		if (!Image::DecodeImageFromMemory(
			EncodedBytes,
			DecodedImage,
			OutError,
			{.MaximumDecodedPixels = 16384ull * 16384ull})) return false;
		if (DecodedImage.Width > 16384 || DecodedImage.Height > 16384)
		{
			OutError = std::format("Texture dimensions {}x{} exceed the 16384 pixel limit.",
				DecodedImage.Width, DecodedImage.Height);
			return false;
		}

		Image::FImage Decoded;
		if (Image::FImage::TryCreate({.Width = DecodedImage.Width,
			.Height = DecodedImage.Height, .Format = Image::ERawImageFormat::RGBA8},
			std::move(DecodedImage.Pixels), Decoded, &OutError)
			&& OutSourceData.Init2D(Decoded.GetView(), DecodedImage.SourceChannelCount,
				DecodedImage.bHasTransparency ? 1 : 0)) return true;
		OutSourceData = {};
		OutError = "Decoded texture source data is invalid.";
		return false;
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
		std::string& OutError,
		ETexture2DCompilationPriority Priority,
		FTexture2DCompilationCompletion Completion) -> bool
	{
		if (!Texture.GetPackage() || !Texture.GetSource().IsValid())
		{
			OutError = "Only packaged Texture2D assets with canonical imported pixels can rebuild.";
			return false;
		}
		return SubmitTexture2DCompilation(Texture, {
			.Build = Texture.CreateBuildRequest(Settings),
			.ResultApplication = {
				.bMarkPackageDirty = true,
				.bReportLoadMutation = false,
				.bSourceDecoderInvoked = false},
			.Priority = Priority}, OutError, std::move(Completion));
	}

	auto ReimportTexture2D(
		DTexture2D& Texture,
		std::string& OutError,
		FTexture2DCompilationCompletion Completion) -> bool
	{
		const DAssetImportData* ImportData = Texture.GetAssetImportData();
		const FSourceFile* Source = ImportData
			? ImportData->GetSourceData().FindByRole("source") : nullptr;
		if (!Source || Source->Hint.empty())
		{
			OutError = "Texture2D has no source hint to reimport.";
			return false;
		}
		return SubmitTexture2DFromFilename(
			Texture, Source->Hint, Source->HintBase,
			MakeTexture2DBuildSettings(Texture),
			OutError, ETexture2DCompilationPriority::Interactive,
			std::move(Completion), true, true);
	}

	auto ReimportTexture2DFromFile(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string& OutError,
		FTexture2DCompilationCompletion Completion) -> bool
	{
		if (FilePath.empty())
		{
			OutError = "A Texture2D source file must be selected.";
			return false;
		}
		const std::filesystem::path Requested =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Requested))
		{
			OutError = "The selected Texture2D source file does not exist.";
			return false;
		}
		return SubmitTexture2DFromFilename(
			Texture, {}, ESourceHintBase::AssetRelative,
			MakeTexture2DBuildSettings(Texture), OutError,
			ETexture2DCompilationPriority::Interactive,
			std::move(Completion), true, true, Requested);
	}

	auto SetTexture2DUsage(
		DTexture2D& Texture, ETextureUsage Usage, std::string& OutError) -> bool
	{
		if (!IsValidTextureUsage(Usage))
		{
			OutError = "Texture usage preset is invalid.";
			return false;
		}
		if (Texture.GetUsage() == Usage) return true;
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.Usage = Usage;
		Settings.bSRGB = GetDefaultTextureSRGB(Usage);
		return RebuildTexture2DFromSource(Texture, Settings, OutError);
	}

	auto SetTexture2DSRGB(
		DTexture2D& Texture, bool bSRGB, std::string& OutError) -> bool
	{
		if (Texture.IsSRGB() == bSRGB) return true;
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.bSRGB = bSRGB;
		return RebuildTexture2DFromSource(Texture, Settings, OutError);
	}

	auto SetTexture2DMaxResolution(
		DTexture2D& Texture, uint32 MaxResolution, std::string& OutError) -> bool
	{
		if (Texture.GetMaxResolution() == MaxResolution) return true;
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.MaxResolution = MaxResolution;
		return RebuildTexture2DFromSource(Texture, Settings, OutError);
	}

	auto SetTexture2DCompressionQuality(
		DTexture2D& Texture,
		ETextureCompressionQuality Quality,
		std::string& OutError) -> bool
	{
		if (!IsValidTextureCompressionQuality(Quality))
		{
			OutError = "Texture compression quality is invalid.";
			return false;
		}
		if (Texture.GetCompressionQuality() == Quality) return true;
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.CompressionQuality = Quality;
		return RebuildTexture2DFromSource(Texture, Settings, OutError);
	}

	auto SetTexture2DAlphaMipMode(
		DTexture2D& Texture, ETextureAlphaMipMode Mode, std::string& OutError) -> bool
	{
		if (!IsValidTextureAlphaMipMode(Mode))
		{
			OutError = "Texture alpha mip mode is invalid.";
			return false;
		}
		if (Texture.GetAlphaMipMode() == Mode) return true;
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.AlphaMipMode = Mode;
		return RebuildTexture2DFromSource(Texture, Settings, OutError);
	}

	auto SetTexture2DAlphaCoverageThreshold(
		DTexture2D& Texture, float Threshold, std::string& OutError) -> bool
	{
		if (!IsValidTextureAlphaCoverageThreshold(Threshold))
		{
			OutError = "Texture alpha coverage threshold must be greater than zero and less than one.";
			return false;
		}
		if (Texture.GetAlphaCoverageThreshold() == Threshold) return true;
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.AlphaCoverageThreshold = Threshold;
		return RebuildTexture2DFromSource(Texture, Settings, OutError);
	}
}
