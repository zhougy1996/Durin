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
			std::filesystem::path& OutPath) -> FTexture2DSubmissionResult
		{
			const FAssetPathResult Resolved =
				FMountPaths::ResolveAssetPath(
					PackagePath, EMountPathExistence::AllowMissing);
			if (!Resolved)
			{
				return std::unexpected(FTexture2DSubmissionError{.Code = ETexture2DSubmissionError::Mount, .PackagePath = std::string(PackagePath), .MountCause = Resolved.Error});
			}
			OutPath = Resolved.PhysicalPath;
			OutPath += ".dasset";
			return {};
		}

		auto PublishTexture2DImportData(
			DTexture2D& Texture,
			std::string Filename,
			ESourceHintBase HintBase,
			std::string DisplayLabel,
			FXxHash128 ContentHash,
			uint64 ByteCount) -> FTexture2DCompilationOperationResult
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
			if (const auto Validation = State.Validate(); !Validation)
			{ return {.Error = {.Code = ETexture2DCompilationError::ImportValidation,
				.ObjectPath = Texture.GetObjectPath(), .ImportCause = std::make_shared<FAssetImportDataError>(Validation.error())}}; }
			if (!ImportData) return {.Error = {.Code = ETexture2DCompilationError::ImportAllocation, .ObjectPath = Texture.GetObjectPath()}};
			ImportData->SetState(std::move(State));
			Texture.SetAssetImportData(*ImportData);
			Texture.MarkPackageDirty();
			return {};
		}

		auto SubmitTexture2DFromFilename(
			DTexture2D& Texture,
			std::string Filename,
			ESourceHintBase HintBase,
			const FTexture2DBuildSettings& Settings,
			ETexture2DCompilationPriority Priority,
			FTexture2DCompilationCompletion Completion,
			bool bPublishImportData,
			bool bSave,
			std::optional<std::filesystem::path> SelectedPhysicalPath = {}) -> FTexture2DSubmissionResult
		{
			if (!Texture.GetPackage())
			{
				return std::unexpected(FTexture2DSubmissionError{.Code = ETexture2DSubmissionError::Package, .ObjectPath = Texture.GetObjectPath()});
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
				if (!Resolved) return std::unexpected(FTexture2DSubmissionError{.Code = ETexture2DSubmissionError::SourceHint, .ObjectPath = Texture.GetObjectPath(),
					.Filename = Filename, .SourceHintCause = Resolved.error()});
				PhysicalPath = PhysicalPathText;
			}
			if (!std::filesystem::is_regular_file(PhysicalPath))
			{
				return std::unexpected(FTexture2DSubmissionError{.Code = ETexture2DSubmissionError::SourceFile, .ObjectPath = Texture.GetObjectPath(), .Filename = PhysicalPath.generic_string()});
			}
			auto Captured = CaptureEncodedSource(
				PhysicalPath.generic_string(), PhysicalPath,
				64ull * 1'024ull * 1'024ull);
			if (!Captured)
				return std::unexpected(FTexture2DSubmissionError{.Code = ETexture2DSubmissionError::Capture, .ObjectPath = Texture.GetObjectPath(),
				.Filename = Filename, .CaptureCause = std::make_shared<FEncodedSourceError>(Captured.error())});
			auto Snapshot = std::move(*Captured);
			if (SelectedPhysicalPath)
			{
				if (auto Hint = MakeSourceHint(PhysicalPath.generic_string(), OwningPackagePath.generic_string()); !Hint)
					return std::unexpected(FTexture2DSubmissionError{.Code = ETexture2DSubmissionError::SourceHint, .ObjectPath = Texture.GetObjectPath(),
					.Filename = Filename, .SourceHintCause = Hint.error()});
				else { HintBase = Hint->Base; Filename = std::move(Hint->Hint); }
			}
			auto Translated = TranslateTexture2DSource(Snapshot.GetBytes());
			if (!Translated)
				return std::unexpected(FTexture2DSubmissionError{.Code = ETexture2DSubmissionError::Translation, .ObjectPath = Texture.GetObjectPath(),
				.Filename = Filename, .TranslationCause = Translated.error()});
			const FXxHash128 ContentHash = Snapshot.ContentHash;
			const uint64 ByteCount = Snapshot.FileSize;
			const std::string DisplayLabel = PhysicalPath.filename().generic_string();
			FTextureSource Candidate = std::move(*Translated);
			const auto Submitted = SubmitTexture2DCompilation(Texture, {
				.Build = MakeTexture2DBuildRequest(Candidate, Settings),
				.ResultApplication = {
					.SourceReplacement = std::move(Candidate),
					.bMarkPackageDirty = bPublishImportData,
					.bReportLoadMutation = !bPublishImportData},
				.Priority = Priority},
				[&Texture, Filename, HintBase, DisplayLabel,
					ContentHash, ByteCount,
					bPublishImportData, bSave,
					Completion = std::move(Completion)](
						FTexture2DCompilationResult Result) mutable {
					if (Result.Succeeded() && bPublishImportData)
					{
						if (const auto Published = PublishTexture2DImportData(Texture, std::move(Filename), HintBase,
							DisplayLabel, ContentHash, ByteCount); !Published)
						{
							Result.Status = ETexture2DCompilationStatus::Failed;
							Result.Error = Published.Error;
						}
						else if (bSave)
						{
							const FAssetWriteResult Saved =
								SavePackage(Texture.GetPackage());
							if (!Saved)
							{
								Result.Status = ETexture2DCompilationStatus::Failed;
								Result.Error = {.Code = ETexture2DCompilationError::Save, .ObjectPath = Texture.GetObjectPath(),
									.SaveCause = std::make_shared<FAssetWriteResult>(Saved)};
							}
						}
					}
					if (Completion) Completion(std::move(Result));
				});
			if (!Submitted) return std::unexpected(FTexture2DSubmissionError{.Code = ETexture2DSubmissionError::Compilation,
				.ObjectPath = Texture.GetObjectPath(), .Filename = Filename, .CompilationCause = Submitted.Error});
			return {};
		}
	}

	DTexture2DFactory::DTexture2DFactory(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SupportedClass = DTexture2D::StaticClass();
		Formats = {"png", "jpg", "jpeg", "bmp", "tga"};
	}

	auto FormatTexture2DSubmissionError(const FTexture2DSubmissionError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case ETexture2DSubmissionError::None: return {};
		case ETexture2DSubmissionError::Package: return "Texture2D source capture requires an owning package.";
		case ETexture2DSubmissionError::Mount: return "Texture2D package path could not be resolved: " + Error.PackagePath;
		case ETexture2DSubmissionError::SourceHint: return Error.SourceHintCause ? FormatSourceHintError(*Error.SourceHintCause) : "Invalid Texture2D source hint.";
		case ETexture2DSubmissionError::SourceFile: return "Texture2D source file is missing: " + Error.Filename;
		case ETexture2DSubmissionError::Capture: return Error.CaptureCause ? FormatEncodedSourceError(*Error.CaptureCause) : "Texture2D source capture failed.";
		case ETexture2DSubmissionError::Translation: return Error.TranslationCause ? FormatTexture2DTranslationError(*Error.TranslationCause) : "Texture2D source translation failed.";
		case ETexture2DSubmissionError::Compilation: return Error.CompilationCause ? FormatTexture2DCompilationError(*Error.CompilationCause) : "Texture2D compilation submission failed.";
		case ETexture2DSubmissionError::MissingSource: return "Texture2D has no source hint to reimport.";
		}
		return {};
	}

	auto FormatTexture2DPreparationError(const FTexture2DPreparationError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case ETexture2DPreparationError::None: return {};
		case ETexture2DPreparationError::Path: return Error.SystemError.message();
		case ETexture2DPreparationError::Format: return "Unsupported texture source format.";
		case ETexture2DPreparationError::Capture:
			return Error.CaptureCause ? FormatEncodedSourceError(*Error.CaptureCause) : "Texture source capture failed.";
		case ETexture2DPreparationError::Translation:
			return Error.TranslationCause ? FormatTexture2DTranslationError(*Error.TranslationCause) : "Texture source translation failed.";
		}
		return {};
	}

	auto FTexture2DFactoryError::Format() const -> std::string
	{
		if (const auto* Preparation = std::get_if<FTexture2DPreparationError>(&Cause)) return FormatTexture2DPreparationError(*Preparation);
		if (const auto* Submission = std::get_if<FTexture2DSubmissionError>(&Cause)) return FormatTexture2DSubmissionError(*Submission);
		return FormatTexture2DCompilationError(std::get<FTexture2DCompilationError>(Cause));
	}

	auto PrepareTexture2DImport(std::string_view Filename) -> FTexture2DPreparationResult
	{
		FPreparedTexture2DImport OutPrepared;
		std::error_code Error;
		const auto Input = std::filesystem::absolute(Filename, Error).lexically_normal();
		if (Error) return std::unexpected(FTexture2DPreparationError{.Code = ETexture2DPreparationError::Path, .Filename = std::string(Filename), .SystemError = Error});
		if (!IsTexture2DSourceExtension(Input.extension().generic_string()))
			return std::unexpected(FTexture2DPreparationError{.Code = ETexture2DPreparationError::Format, .Filename = std::string(Filename)});
		auto Captured = CaptureEncodedSource(Input.generic_string(), Input,
			64ull * 1'024ull * 1'024ull);
		if (!Captured)
			return std::unexpected(FTexture2DPreparationError{.Code = ETexture2DPreparationError::Capture, .Filename = std::string(Filename),
				.CaptureCause = std::make_shared<FEncodedSourceError>(Captured.error())});
		auto Snapshot = std::move(*Captured);
		auto Translated = TranslateTexture2DSource(Snapshot.GetBytes());
		if (!Translated)
			return std::unexpected(FTexture2DPreparationError{.Code = ETexture2DPreparationError::Translation, .Filename = std::string(Filename),
				.TranslationCause = Translated.error()});
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
		auto CompilationFailed = [&](const FTexture2DCompilationError& Cause) -> DObject* {
			if (Diagnostics) Diagnostics->ReportFailure({
				.Code = EFactoryError::TextureCompilation, .Filename = std::string(Filename),
				.TextureCompilationCause = std::make_shared<FTexture2DCompilationError>(Cause)});
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
		auto Build = MakeTexture2DBuildRequest(InputData.Source, {
			.Usage = EffectiveSettings.Usage,
			.CompressionQuality = EffectiveSettings.CompressionQuality,
			.AlphaMipMode = EffectiveSettings.AlphaMipMode,
			.AlphaCoverageThreshold = EffectiveSettings.AlphaCoverageThreshold,
			.MaxResolution = EffectiveSettings.MaxResolution,
			.bSRGB = EffectiveSettings.bSRGB});
		if (Prepared)
		{
			if (const auto Submitted = SubmitTexture2DCompilation(*Texture, {
				.Build = std::move(Build),
				.ResultApplication = {.SourceReplacement = InputData.Source}},
				[Texture, SourceHint, HintBase, Label = Input.filename().generic_string(),
					Hash = InputData.ContentHash, Count = InputData.ByteCount,
					Done = Completion](FTexture2DCompilationResult Result) mutable {
					if (Result.Succeeded())
					{
						if (const auto Published = PublishTexture2DImportData(*Texture,
							std::move(SourceHint), HintBase, std::move(Label), Hash, Count); !Published)
						{
							Result.Status = ETexture2DCompilationStatus::Failed;
							Result.Error = Published.Error;
						}
					}
					if (Done) Done(std::move(Result));
				}); !Submitted) return CompilationFailed(Submitted.Error);
		}
		else
		{
			if (const auto Built = BuildTexture2DSynchronously(*Texture, std::move(Build),
				{.SourceReplacement = InputData.Source}); !Built)
				return CompilationFailed(Built.Error);
			if (const auto Published = PublishTexture2DImportData(*Texture, std::move(SourceHint), HintBase,
				Input.filename().generic_string(), InputData.ContentHash,
				InputData.ByteCount); !Published) return CompilationFailed(Published.Error);
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
		const auto Submitted = SubmitTexture2DFromFilename(
			*Texture, Source->Hint, Source->HintBase,
			MakeTexture2DBuildSettings(*Texture),
			ETexture2DCompilationPriority::Interactive,
			[Completion](FTexture2DCompilationResult Result) mutable {
				if (Completion) Completion(Result.Succeeded()
					? FReimportResult{EReimportStatus::Succeeded, {}}
					: FReimportResult{EReimportStatus::SourceOrBuildFailure,
						FormatTexture2DCompilationError(Result.Error), std::make_shared<FTexture2DFactoryError>(Result.Error)});
			}, true, false);
		if (!Submitted && Completion)
			Completion({EReimportStatus::SourceOrBuildFailure, FormatTexture2DSubmissionError(Submitted.error()),
				std::make_shared<FTexture2DFactoryError>(Submitted.error())});
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
		const auto Submitted = SubmitTexture2DFromFilename(
			*Texture, {}, ESourceHintBase::AssetRelative,
			MakeTexture2DBuildSettings(*Texture),
			ETexture2DCompilationPriority::Interactive,
			[Completion](FTexture2DCompilationResult Result) mutable {
				if (Completion) Completion(Result.Succeeded()
					? FReimportResult{EReimportStatus::Succeeded, {}}
					: FReimportResult{EReimportStatus::SourceOrBuildFailure,
						FormatTexture2DCompilationError(Result.Error), std::make_shared<FTexture2DFactoryError>(Result.Error)});
			}, true, false, Requested);
		if (!Submitted && Completion)
			Completion({EReimportStatus::SourceOrBuildFailure, FormatTexture2DSubmissionError(Submitted.error()),
				std::make_shared<FTexture2DFactoryError>(Submitted.error())});
	}

	auto IsTexture2DSourceExtension(std::string_view Extension) -> bool
	{
		const std::string Lowercase = StringUtils::FoldAscii(Extension);
		return Lowercase == ".png" || Lowercase == ".jpg" || Lowercase == ".jpeg"
			|| Lowercase == ".bmp" || Lowercase == ".tga";
	}

	auto FormatTexture2DTranslationError(const FTexture2DTranslationError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case ETexture2DTranslationError::None: return {};
		case ETexture2DTranslationError::Decode:
			return Error.DecodeCause ? Image::FormatImageDecodeError(*Error.DecodeCause) : "Texture image decode failed.";
		case ETexture2DTranslationError::Dimensions:
			return std::format("Texture dimensions {}x{} exceed the 16384 pixel limit.", Error.Width, Error.Height);
		case ETexture2DTranslationError::Image:
		case ETexture2DTranslationError::Source: return "Decoded texture source data is invalid.";
		}
		return {};
	}

	auto TranslateTexture2DSource(FByteView EncodedBytes) -> FTexture2DTranslationResult
	{
		FTextureSource SourceData;
		auto Decoded = Image::DecodeImageFromMemory(EncodedBytes, {.MaximumDecodedPixels = 16384ull * 16384ull});
		if (!Decoded) return std::unexpected(FTexture2DTranslationError{.Code = ETexture2DTranslationError::Decode, .DecodeCause = Decoded.error()});
		auto DecodedImage = std::move(*Decoded);
		auto Fail = [&](ETexture2DTranslationError Code) -> FTexture2DTranslationResult {
			return std::unexpected(FTexture2DTranslationError{.Code = Code, .Width = DecodedImage.Width, .Height = DecodedImage.Height,
				.SourceChannelCount = DecodedImage.SourceChannelCount});
		};
		if (DecodedImage.Width > 16384 || DecodedImage.Height > 16384)
			return Fail(ETexture2DTranslationError::Dimensions);
		Image::FImage Image;
		if (!Image::FImage::TryCreate({.Width = DecodedImage.Width,
			.Height = DecodedImage.Height, .Format = Image::ERawImageFormat::RGBA8},
			std::move(DecodedImage.Pixels), Image)) return Fail(ETexture2DTranslationError::Image);
		if (SourceData.Init2D(Image.GetView(), DecodedImage.SourceChannelCount,
			DecodedImage.bHasTransparency ? 1 : 0)) return SourceData;
		return Fail(ETexture2DTranslationError::Source);
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
		FTexture2DCompilationCompletion Completion) -> FTexture2DCompilationOperationResult
	{
		if (!Texture.GetPackage())
			return {.Error = {.Code = ETexture2DCompilationError::MissingPackage, .ObjectPath = Texture.GetObjectPath()}};
		if (!Texture.GetSource().IsValid())
			return {.Error = {.Code = ETexture2DCompilationError::InvalidSource, .ObjectPath = Texture.GetObjectPath()}};
		return SubmitTexture2DCompilation(Texture, {
			.Build = Texture.CreateBuildRequest(Settings),
			.ResultApplication = {
				.bMarkPackageDirty = true,
				.bReportLoadMutation = false,
				.bSourceDecoderInvoked = false},
			.Priority = Priority}, std::move(Completion));
	}

	auto ReimportTexture2D(
		DTexture2D& Texture,
		FTexture2DCompilationCompletion Completion) -> FTexture2DSubmissionResult
	{
		const DAssetImportData* ImportData = Texture.GetAssetImportData();
		const FSourceFile* Source = ImportData
			? ImportData->GetSourceData().FindByRole("source") : nullptr;
		if (!Source || Source->Hint.empty())
		{
			return std::unexpected(FTexture2DSubmissionError{.Code = ETexture2DSubmissionError::MissingSource, .ObjectPath = Texture.GetObjectPath()});
		}
		return SubmitTexture2DFromFilename(
			Texture, Source->Hint, Source->HintBase,
			MakeTexture2DBuildSettings(Texture),
			ETexture2DCompilationPriority::Interactive,
			std::move(Completion), true, true);
	}

	auto ReimportTexture2DFromFile(
		DTexture2D& Texture,
		std::string_view FilePath,
		FTexture2DCompilationCompletion Completion) -> FTexture2DSubmissionResult
	{
		if (FilePath.empty())
		{
			return std::unexpected(FTexture2DSubmissionError{.Code = ETexture2DSubmissionError::SourceFile, .ObjectPath = Texture.GetObjectPath(), .Filename = std::string(FilePath)});
		}
		const std::filesystem::path Requested =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Requested))
		{
			return std::unexpected(FTexture2DSubmissionError{.Code = ETexture2DSubmissionError::SourceFile, .ObjectPath = Texture.GetObjectPath(), .Filename = std::string(FilePath)});
		}
		return SubmitTexture2DFromFilename(
			Texture, {}, ESourceHintBase::AssetRelative,
			MakeTexture2DBuildSettings(Texture),
			ETexture2DCompilationPriority::Interactive,
			std::move(Completion), true, true, Requested);
	}

	namespace
	{
		auto RejectTextureSettings(const DTexture2D& Texture, ETexture2DInputError Code,
			const FTexture2DBuildSettings& Settings) -> FTexture2DCompilationOperationResult
		{
			return {.Error = {.Code = ETexture2DCompilationError::InvalidSettings,
				.InputCause = FTexture2DInputError{.Code = Code, .Settings = Settings},
				.ObjectPath = Texture.GetObjectPath()}};
		}
	}

	auto SetTexture2DUsage(DTexture2D& Texture, ETextureUsage Usage) -> FTexture2DCompilationOperationResult
	{
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.Usage = Usage;
		if (!IsValidTextureUsage(Usage)) return RejectTextureSettings(Texture, ETexture2DInputError::InvalidUsage, Settings);
		if (Texture.GetUsage() == Usage) return {};
		Settings.bSRGB = GetDefaultTextureSRGB(Usage);
		return RebuildTexture2DFromSource(Texture, Settings);
	}

	auto SetTexture2DSRGB(DTexture2D& Texture, bool bSRGB) -> FTexture2DCompilationOperationResult
	{
		if (Texture.IsSRGB() == bSRGB) return {};
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.bSRGB = bSRGB;
		return RebuildTexture2DFromSource(Texture, Settings);
	}

	auto SetTexture2DMaxResolution(DTexture2D& Texture, uint32 MaxResolution) -> FTexture2DCompilationOperationResult
	{
		if (Texture.GetMaxResolution() == MaxResolution) return {};
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.MaxResolution = MaxResolution;
		return RebuildTexture2DFromSource(Texture, Settings);
	}

	auto SetTexture2DCompressionQuality(DTexture2D& Texture,
		ETextureCompressionQuality Quality) -> FTexture2DCompilationOperationResult
	{
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.CompressionQuality = Quality;
		if (!IsValidTextureCompressionQuality(Quality)) return RejectTextureSettings(Texture, ETexture2DInputError::InvalidCompressionQuality, Settings);
		if (Texture.GetCompressionQuality() == Quality) return {};
		return RebuildTexture2DFromSource(Texture, Settings);
	}

	auto SetTexture2DAlphaMipMode(DTexture2D& Texture, ETextureAlphaMipMode Mode) -> FTexture2DCompilationOperationResult
	{
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.AlphaMipMode = Mode;
		if (!IsValidTextureAlphaMipMode(Mode)) return RejectTextureSettings(Texture, ETexture2DInputError::InvalidAlphaMipMode, Settings);
		if (Texture.GetAlphaMipMode() == Mode) return {};
		return RebuildTexture2DFromSource(Texture, Settings);
	}

	auto SetTexture2DAlphaCoverageThreshold(DTexture2D& Texture, float Threshold) -> FTexture2DCompilationOperationResult
	{
		FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.AlphaCoverageThreshold = Threshold;
		if (!IsValidTextureAlphaCoverageThreshold(Threshold)) return RejectTextureSettings(Texture, ETexture2DInputError::InvalidAlphaCoverageThreshold, Settings);
		if (Texture.GetAlphaCoverageThreshold() == Threshold) return {};
		return RebuildTexture2DFromSource(Texture, Settings);
	}
}
