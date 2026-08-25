#include "AssetForge/Builtins/Texture2DImport.h"
#include "DObject/Package.h"
#include "EncodedSourceSnapshot.h"
#include "Texture2DBuildAdapter.h"
#include "Texture2DPostLoad.h"
#include "AssetForge/ImportService.h"

#include "AssetAuthoring.h"
#include "Hash/XxHash.h"
#include "Image/ImageDecoder.h"
#include "Misc/Paths.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureBuilder.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
	namespace
	{
		constexpr std::string_view DefaultTextureSourceRoot = "Textures";

		auto FindOwningMount(std::string_view VirtualPath)
			-> const PathUtilities::FMountPoint*
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(VirtualPath);
			return Lookup ? Lookup.Mount : nullptr;
		}

		auto MakeCanonicalSourceLocation(
			const FAssetPath& AssetPath,
			std::string_view Extension,
			std::string_view RequestedSourcePath,
			std::string& OutStoredPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FMountPoint* Mount = FindOwningMount(AssetPath.ToString());
			if (!Mount)
			{
				OutError = std::format(
					"Texture asset {} is not beneath a registered package mount.",
					AssetPath.ToString());
				return false;
			}
			std::filesystem::path StoredPath;
			if (RequestedSourcePath.empty())
				StoredPath = std::filesystem::path(DefaultTextureSourceRoot)
					/ (std::string(AssetPath.GetAssetName()) + std::string(Extension));
			else if (RequestedSourcePath.starts_with('/'))
			{
				const PathUtilities::FSourcePathResult Requested =
					PathUtilities::ResolveSourcePath(
						RequestedSourcePath, PathUtilities::EPathExistence::AllowMissing);
				if (!Requested)
				{
					OutError = Requested.Message;
					return false;
				}
				if (Requested.Mount != Mount)
				{
					OutError = "Texture source relocation must remain within the asset's owning mount.";
					return false;
				}
				StoredPath = Requested.RelativePath;
			}
			else
				StoredPath = std::filesystem::path(RequestedSourcePath);
			StoredPath = StoredPath.lexically_normal();
			const std::string RelativeString = StoredPath.generic_string();
			if (StoredPath.empty() || StoredPath == "." || StoredPath.is_absolute()
				|| RelativeString == ".." || RelativeString.starts_with("../"))
			{
				OutError = std::format(
					"Texture source path '{}' must be a normalized mount-relative path.",
					RequestedSourcePath);
				return false;
			}
			OutStoredPath = Mount->VirtualRoot + RelativeString;
			std::string RequestedExtension = StoredPath.extension().generic_string();
			std::string InputExtension(Extension);
			std::ranges::transform(RequestedExtension, RequestedExtension.begin(), [](char Value) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(Value)));
			});
			std::ranges::transform(InputExtension, InputExtension.begin(), [](char Value) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(Value)));
			});
			if (RequestedExtension != InputExtension)
			{
				OutError = std::format(
					"Texture source destination extension '{}' must match the input extension '{}'.",
					StoredPath.extension().generic_string(), Extension);
				return false;
			}
			return true;
		}

		template<typename TMountedSource>
		auto SubmitTexture2DFromMountedSource(
			DTexture2D& Texture,
			const TMountedSource& Source,
			const Asset::Build::FTexture2DBuildSettings& Settings,
			std::string& OutError,
			Asset::Build::ETexture2DBuildPriority Priority,
			Asset::Build::FAsyncBuildCompletion Completion = {}) -> bool
		{
			FEncodedSourceSnapshot Snapshot;
			if (!CaptureEncodedSource(
				Source.SourcePath, Source.PhysicalPath, Snapshot, OutError)) return false;
			FTextureSourceData SourceData;
			if (!TranslateTexture2DSource(Snapshot.GetBytes(), SourceData, OutError)) return false;
			return Asset::Build::SubmitTexture2DBuild(Texture, {
				.SourceData = std::move(SourceData),
				.SourceContentHashLow = Snapshot.ContentHash.HashLow,
				.SourceContentHashHigh = Snapshot.ContentHash.HashHigh,
				.SourcePath = Snapshot.SourcePath,
				.Settings = Settings,
				.DecoderId = "DurinImage",
				.DecoderVersion = 1,
				.SourceFileSize = Snapshot.FileSize,
				.SourceLastWriteTime = Snapshot.LastWriteTime,
				.Priority = Priority}, OutError, std::move(Completion));
		}

		auto ExecuteTexture2DImportReplacement(
			DTexture2D& Texture,
			const FSourcePath& Source,
			const Asset::Build::FTexture2DBuildSettings& BuildSettings,
			EImportMode Mode,
			std::string& OutError,
			Asset::Build::FAsyncBuildCompletion Completion = {}) -> bool
		{
			FAssetPath Destination;
			if (!Texture.GetPackage() || !FAssetPath::TryCreate(
				Texture.GetPackage()->GetPackagePath(), Destination, &OutError)) return false;
			FImportProvenance Existing;
			std::optional<FImportProvenance> ExistingValue;
			std::string ProvenanceError;
			if (InspectTexture2DImportProvenance(
				Texture, Existing, ProvenanceError)) ExistingValue = std::move(Existing);
			FTexture2DImportSettings Settings{
				.Usage = BuildSettings.Usage,
				.CompressionQuality = BuildSettings.CompressionQuality,
				.AlphaMipMode = BuildSettings.AlphaMipMode,
				.AlphaCoverageThreshold = BuildSettings.AlphaCoverageThreshold,
				.MaxResolution = BuildSettings.MaxResolution,
				.bSRGB = BuildSettings.bSRGB};
			FImportRequest Request;
			if (!MakeTexture2DImportRequest(
				Source, Destination, Settings, Mode,
				{.OwnerId = std::format("Texture2D.Reimport:{}", Destination.ToString()),
					.ConflictIdentities = {Destination.ToString()}},
				std::move(ExistingValue), Request, OutError)) return false;
			const FImportResult Result = GetImportService().RunImportInline(
				Request, std::format("Reimport Texture2D {}", Destination.GetAssetName()));
			const bool bSucceeded = Result.Outcome.State == EImportOperationState::Succeeded;
			OutError = bSucceeded ? std::string{} : Result.Outcome.Diagnostic;
			if (Completion)
				Completion({
					.Status = bSucceeded ? Asset::Build::EAsyncBuildStatus::Succeeded
						: Result.Outcome.State == EImportOperationState::Canceled
							? Asset::Build::EAsyncBuildStatus::Canceled
							: Asset::Build::EAsyncBuildStatus::Failed,
					.Diagnostic = OutError});
			return bSucceeded;
		}

		auto SubmitTexture2DImportReplacement(
			DTexture2D& Texture,
			const FSourcePath& Source,
			const Asset::Build::FTexture2DBuildSettings& BuildSettings,
			EImportMode Mode,
			std::string& OutError,
			Asset::Build::FAsyncBuildCompletion Completion = {}) -> bool
		{
			FAssetPath Destination;
			if (!Texture.GetPackage() || !FAssetPath::TryCreate(
				Texture.GetPackage()->GetPackagePath(), Destination, &OutError)) return false;
			FImportProvenance Existing;
			std::optional<FImportProvenance> ExistingValue;
			std::string ProvenanceError;
			if (InspectTexture2DImportProvenance(
				Texture, Existing, ProvenanceError)) ExistingValue = std::move(Existing);
			FTexture2DImportSettings Settings{
				.Usage = BuildSettings.Usage,
				.CompressionQuality = BuildSettings.CompressionQuality,
				.AlphaMipMode = BuildSettings.AlphaMipMode,
				.AlphaCoverageThreshold = BuildSettings.AlphaCoverageThreshold,
				.MaxResolution = BuildSettings.MaxResolution,
				.bSRGB = BuildSettings.bSRGB};
			FImportRequest Request;
			if (!MakeTexture2DImportRequest(
				Source, Destination, Settings, Mode,
				{.OwnerId = std::format("Texture2D.Reimport:{}", Destination.ToString()),
					.ConflictIdentities = {Destination.ToString()}},
				std::move(ExistingValue), Request, OutError)) return false;
			auto Handle = GetImportService().SubmitImport(
				std::move(Request), std::format("Reimport Texture2D {}", Destination.GetAssetName()),
				[Completion = std::move(Completion)](const FImportResult& Result) {
					if (!Completion) return;
					Completion({
						.Status = Result.Outcome.State == EImportOperationState::Succeeded
							? Asset::Build::EAsyncBuildStatus::Succeeded
							: Result.Outcome.State == EImportOperationState::Canceled
								? Asset::Build::EAsyncBuildStatus::Canceled
								: Asset::Build::EAsyncBuildStatus::Failed,
						.Diagnostic = Result.Outcome.Diagnostic});
				});
			if (!Handle)
			{
				OutError = "Texture2D AssetForge reimport could not be submitted.";
				return false;
			}
			OutError.clear();
			return true;
		}
	}

	auto IsTexture2DSourceExtension(std::string_view Extension) -> bool
	{
		std::string Lowercase(Extension);
		std::ranges::transform(Lowercase, Lowercase.begin(), [](unsigned char Character) {
			return static_cast<char>(std::tolower(Character));
		});
		return Lowercase == ".png" || Lowercase == ".jpg" || Lowercase == ".jpeg"
			|| Lowercase == ".bmp" || Lowercase == ".tga";
	}

	auto TranslateTexture2DSource(
		std::span<const std::byte> EncodedBytes,
		FTextureSourceData& OutSourceData,
		std::string& OutError) -> bool
	{
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

		OutSourceData = {
			.Pixels = std::move(DecodedImage.Pixels),
			.Width = DecodedImage.Width,
			.Height = DecodedImage.Height,
			.SourceChannelCount = DecodedImage.SourceChannelCount,
			.Format = ETextureSourceFormat::RGBA8,
			.bHasTransparency = DecodedImage.bHasTransparency};
		if (OutSourceData.IsValid()) return true;
		OutSourceData = {};
		OutError = "Decoded texture source data is invalid.";
		return false;
	}

	auto BuildTexture2DCandidateFromSource(
		DTexture2D& Texture,
		std::span<const std::byte> EncodedBytes,
		const FSourcePath& SourcePath,
		const FTexture2DImportSettings& Settings,
		std::string& OutError,
		int64 SourceLastWriteTime) -> bool
	{
		auto Bytes = std::make_shared<const std::vector<std::byte>>(
			EncodedBytes.begin(), EncodedBytes.end());
		FEncodedSourceSnapshot Source{
			.SourcePath = SourcePath,
			.Bytes = std::move(Bytes),
			.FileSize = EncodedBytes.size(),
			.LastWriteTime = SourceLastWriteTime};
		Source.ContentHash = FXxHash128::HashBuffer(Source.GetBytes());
		return BuildTexture2DCandidateFromSnapshot(Texture, Source, Settings, OutError);
	}

	auto BuildTexture2DCandidateFromSnapshot(
		DTexture2D& Texture,
		const FEncodedSourceSnapshot& Source,
		const FTexture2DImportSettings& Settings,
		std::string& OutError) -> bool
	{
		FTextureSourceData SourceData;
		if (!TranslateTexture2DSource(Source.GetBytes(), SourceData, OutError)) return false;
		Asset::Build::FTexture2DBuildProduct Product;
		if (!Asset::Build::BuildTexture2D({
			.SourceData = std::move(SourceData),
			.SourceContentHashLow = Source.ContentHash.HashLow,
			.SourceContentHashHigh = Source.ContentHash.HashHigh,
			.Settings = {
				.Usage = Settings.Usage,
				.CompressionQuality = Settings.CompressionQuality,
				.AlphaMipMode = Settings.AlphaMipMode,
				.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
				.MaxResolution = Settings.MaxResolution,
				.bSRGB = Settings.bSRGB}}, Product, OutError)) return false;
		return Asset::Build::PublishTexture2DProduct(Texture, std::move(Product), {
			.SourcePath = Source.SourcePath,
			.DecoderId = "DurinImage",
			.DecoderVersion = 1,
			.SourceFileSize = Source.FileSize,
			.SourceLastWriteTime = Source.LastWriteTime}, OutError);
	}

	auto ImportTexture2D(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTexture2DImportSettings& Settings,
		bool bEngineAuthoringContext) -> FImportResult
	{
		auto Failed = [](std::string Message) {
			FImportResult Result;
			Result.Outcome.State = EImportOperationState::Failed;
			Result.Outcome.Diagnostic = std::move(Message);
			return Result;
		};
		const std::filesystem::path Input =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
			return Failed("Source file does not exist.");
		if (!IsTexture2DSourceExtension(Input.extension().generic_string()))
			return Failed("Unsupported texture source format.");
		if (Settings.Usage != ETextureUsage::Color
			&& Settings.Usage != ETextureUsage::Normal
			&& Settings.Usage != ETextureUsage::DataMask)
			return Failed("Texture usage preset is invalid.");
		if (Settings.CompressionQuality != ETextureCompressionQuality::Low
			&& Settings.CompressionQuality != ETextureCompressionQuality::Normal
			&& Settings.CompressionQuality != ETextureCompressionQuality::High)
			return Failed("Texture compression quality is invalid.");
		if (Settings.AlphaMipMode != ETextureAlphaMipMode::Average
			&& Settings.AlphaMipMode != ETextureAlphaMipMode::PreserveCoverage)
			return Failed("Texture alpha mip mode is invalid.");
		if (!std::isfinite(Settings.AlphaCoverageThreshold)
			|| Settings.AlphaCoverageThreshold <= 0.0f
			|| Settings.AlphaCoverageThreshold >= 1.0f)
			return Failed(
				"Texture alpha coverage threshold must be greater than zero and less than one.");

		FAssetPath ParsedAssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &Error))
			return Failed(std::move(Error));
		if (Asset::FindAssetExact(ParsedAssetPath)
			|| Asset::FindResidentPackage(ParsedAssetPath))
			return Failed(std::format(
				"Asset {} already exists.", ParsedAssetPath.ToString()));

		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(
			ParsedAssetPath,
			Input.extension().generic_string(),
			Settings.SourceDestination,
			StoredSourcePath,
			Error)) return Failed(std::move(Error));
		FScopedMountedSourceFile MountedSource;
		if (!PrepareMountedSourceFile(
			Input,
			ParsedAssetPath.ToString(),
			StoredSourcePath,
			MountedSource,
			Error,
			bEngineAuthoringContext
				? EMountedSourceMutationContext::EngineAuthoring
				: EMountedSourceMutationContext::DependencySafe))
			return Failed(std::move(Error));

		FImportRequest Request;
		if (!MakeTexture2DImportRequest(
			MountedSource.SourcePath, ParsedAssetPath, Settings,
			EImportMode::Import,
			{.OwnerId = std::format("Texture2D.Import:{}", ParsedAssetPath.ToString()),
				.ConflictIdentities = {ParsedAssetPath.ToString()}},
			{}, Request, Error))
			return Failed(std::move(Error));
		FImportResult Imported = GetImportService().RunImportInline(
			Request, std::format("Import Texture2D {}", ParsedAssetPath.GetAssetName()));
		if (Imported.Outcome.State != EImportOperationState::Succeeded)
			return Imported;
		DObject* ImportedObject = nullptr;
		(void)Asset::LoadAsset(ParsedAssetPath, ImportedObject);
		auto* Texture = Cast<DTexture2D>(ImportedObject);
		if (!Texture)
			return Failed("Texture2D AssetForge import published no destination asset.");
		MountedSource.Commit();
		return Imported;
	}

	auto SubmitTexture2DImport(std::string_view FilePath,
		const FAssetPath& Destination, const FTexture2DImportSettings& Settings,
		bool bEngineAuthoringContext, FImportCompletion Completion,
		std::string& OutError) -> FImportHandle
	{
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input)
			|| !IsTexture2DSourceExtension(Input.extension().generic_string()))
		{
			OutError = "Texture2D source is unavailable or unsupported.";
			return {};
		}
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(Destination, Input.extension().generic_string(),
			Settings.SourceDestination, StoredSourcePath, OutError)) return {};
		auto Mounted = std::make_shared<FScopedMountedSourceFile>();
		if (!PrepareMountedSourceFile(Input, Destination.ToString(), StoredSourcePath,
			*Mounted, OutError, bEngineAuthoringContext
				? EMountedSourceMutationContext::EngineAuthoring
				: EMountedSourceMutationContext::DependencySafe)) return {};
		FImportRequest Request;
		if (!MakeTexture2DImportRequest(Mounted->SourcePath, Destination, Settings,
			EImportMode::Import,
			{.OwnerId = std::format("Texture2D.Import:{}", Destination.ToString()),
				.ConflictIdentities = {Destination.ToString()}}, {}, Request, OutError)) return {};
		OutError.clear();
		return GetImportService().SubmitImport(std::move(Request),
			std::format("Import Texture2D {}", Destination.GetAssetName()),
			[Mounted, Completion = std::move(Completion)](const FImportResult& Result) {
				if (Result.Outcome.State == EImportOperationState::Succeeded) Mounted->Commit();
				if (Completion) Completion(Result);
			});
	}

	auto ImportTexture2DAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTexture2DImportSettings& Settings,
		bool bEngineAuthoringContext) -> FTexture2DImportResult
	{
		const FImportResult Imported = ImportTexture2D(
			FilePath, AssetPath, Settings, bEngineAuthoringContext);
		if (Imported.Outcome.State != EImportOperationState::Succeeded)
			return {false, Imported.Outcome.Diagnostic.empty()
				? "Texture2D AssetForge import failed."
				: Imported.Outcome.Diagnostic, nullptr};
		FAssetPath ParsedAssetPath;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath))
			return {false, "Texture2D destination is invalid after import.", nullptr};
		DObject* ImportedObject = nullptr;
		(void)Asset::LoadAsset(ParsedAssetPath, ImportedObject);
		auto* Texture = Cast<DTexture2D>(ImportedObject);
		return Texture
			? FTexture2DImportResult{true, {}, Texture}
			: FTexture2DImportResult{false,
				"Texture2D AssetForge import published no destination asset.", nullptr};
	}

	auto MakeTexture2DBuildSettings(const DTexture2D& Texture)
		-> Asset::Build::FTexture2DBuildSettings
	{
		return {
			.Usage = Texture.GetUsage(),
			.CompressionQuality = Texture.GetCompressionQuality(),
			.AlphaMipMode = Texture.GetAlphaMipMode(),
			.AlphaCoverageThreshold = Texture.GetAlphaCoverageThreshold(),
			.MaxResolution = Texture.GetMaxResolution(),
			.bSRGB = Texture.IsSRGB()};
	}

	auto RebuildTexture2DFromCurrentSource(
		DTexture2D& Texture,
		const Asset::Build::FTexture2DBuildSettings& Settings,
		std::string& OutError,
		Asset::Build::ETexture2DBuildPriority Priority,
		Asset::Build::FAsyncBuildCompletion Completion) -> bool
	{
		if (!Texture.GetPackage() || !Texture.GetSourceImportData().HasSource())
		{
			OutError = "Only packaged Texture2D assets with mounted provenance can rebuild.";
			return false;
		}
		const FTextureSourceDiagnostic Source = Texture.InspectSource();
		if (Source.Status == ETextureSourceStatus::Missing
			|| Source.Status == ETextureSourceStatus::Invalid
			|| Source.PhysicalPath.empty())
		{
			OutError = Source.Message.empty()
				? "The mounted Texture2D source is unavailable." : Source.Message;
			return false;
		}
		FMountedSourceResolution MountedSource{
			.SourcePath = Texture.GetSourceImportData().Source.SourcePath,
			.PhysicalPath = Source.PhysicalPath,
			.bExists = true};
		return SubmitTexture2DFromMountedSource(
			Texture, MountedSource, Settings, OutError, Priority, std::move(Completion));
	}

	auto ReimportTexture2DSource(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string& OutError,
		Asset::Build::FAsyncBuildCompletion Completion) -> bool
	{
		const FTextureSourceDiagnostic Source = Texture.InspectSource();
		if (!FilePath.empty())
		{
			std::error_code EquivalentError;
			const std::filesystem::path Requested =
				std::filesystem::absolute(FilePath).lexically_normal();
			if (Source.PhysicalPath.empty()
				|| !std::filesystem::equivalent(Source.PhysicalPath, Requested, EquivalentError)
				|| EquivalentError)
			{
				OutError =
					"Reimport is read-only and must use the persisted mounted source. "
					"Use ChangeSourceReference or IngestAndChangeSource first.";
				return false;
			}
		}
		if (!Texture.GetSourceImportData().HasSource())
		{
			OutError = "Texture2D has no mounted source to reimport.";
			return false;
		}
		return SubmitTexture2DImportReplacement(
			Texture, Texture.GetSourceImportData().Source.SourcePath,
			MakeTexture2DBuildSettings(Texture), EImportMode::Reimport,
			OutError, std::move(Completion));
	}

	auto ChangeTexture2DSourceReference(
		DTexture2D& Texture,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged textures can retain source provenance.";
			return false;
		}
		FMountedSourceResolution Source;
		if (!ResolveMountedSourceReference(
			Texture.GetPackage()->GetPackagePath(), SourceVirtualPath,
			EMountedSourceExistencePolicy::RequireFile, Source, OutError))
			return false;
		return ExecuteTexture2DImportReplacement(
			Texture, Source.SourcePath, MakeTexture2DBuildSettings(Texture),
			EImportMode::ReplaceSource, OutError);
	}

	auto IngestAndChangeTexture2DSource(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string_view TargetSourceVirtualPath,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged textures can retain source provenance.";
			return false;
		}
		FScopedMountedSourceFile Source;
		if (!PrepareMountedSourceFile(
			FilePath,
			Texture.GetPackage()->GetPackagePath(),
			TargetSourceVirtualPath,
			Source,
			OutError)) return false;
		const bool bSubmitted = ExecuteTexture2DImportReplacement(
			Texture, Source.SourcePath, MakeTexture2DBuildSettings(Texture),
			EImportMode::ReplaceSource, OutError);
		if (bSubmitted) Source.Commit();
		return bSubmitted;
	}

	auto RepairTexture2DSourcePath(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string& OutError) -> bool
	{
		const PathUtilities::FSourcePathResult Classified =
			PathUtilities::ClassifySourcePath(
				std::filesystem::absolute(FilePath).lexically_normal());
		if (!Classified)
		{
			OutError =
				"Source repair requires a mounted source reference. "
				"Use IngestAndChangeSource with an explicit destination for external files.";
			return false;
		}
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged textures can retain source provenance.";
			return false;
		}
		FMountedSourceResolution Source;
		if (!ResolveMountedSourceReference(
			Texture.GetPackage()->GetPackagePath(), Classified.NormalizedVirtualPath,
			EMountedSourceExistencePolicy::RequireFile, Source, OutError)) return false;
		return ExecuteTexture2DImportReplacement(
			Texture, Source.SourcePath, MakeTexture2DBuildSettings(Texture),
			EImportMode::Repair, OutError);
	}

	auto ChangeTexture2DSourceLocation(
		DTexture2D& Texture,
		std::string_view SourceDestination,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged textures can retain source provenance.";
			return false;
		}
		if (SourceDestination.empty())
		{
			OutError = "Choose a source destination inside the asset's mount.";
			return false;
		}
		const FTextureSourceDiagnostic Current = Texture.InspectSource();
		if (Current.PhysicalPath.empty()
			|| !std::filesystem::is_regular_file(Current.PhysicalPath))
		{
			OutError = Current.Message.empty()
				? "Texture source is missing. Reimport it before changing its location."
				: Current.Message;
			return false;
		}
		FAssetPath AssetPath;
		if (!FAssetPath::TryCreate(
			Texture.GetPackage()->GetPackagePath(), AssetPath, &OutError)) return false;
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(
			AssetPath,
			std::filesystem::path(Current.PhysicalPath).extension().generic_string(),
			SourceDestination,
			StoredSourcePath,
			OutError)) return false;
		FMountedSourceRelocation Relocation;
		if (!PrepareMountedSourceRelocation(
			Texture.GetPackage()->GetPackagePath(),
			Texture.GetSourceImportData().Source.SourcePath.Path,
			StoredSourcePath,
			Relocation,
			OutError)) return false;
		FMountedSourceResolution Source{
			.SourcePath = Relocation.DestinationSourcePath,
			.PhysicalPath = Relocation.DestinationPhysicalPath,
			.bExists = true};
		const bool bSubmitted = ExecuteTexture2DImportReplacement(
			Texture, Source.SourcePath, MakeTexture2DBuildSettings(Texture),
			EImportMode::ReplaceSource, OutError);
		if (!bSubmitted) RollbackMountedSourceRelocation(Relocation);
		return bSubmitted;
	}

	auto SetTexture2DUsage(
		DTexture2D& Texture, ETextureUsage Usage, std::string& OutError) -> bool
	{
		if (!Asset::Build::TextureBuilder::IsValidUsage(Usage))
		{
			OutError = "Texture usage preset is invalid.";
			return false;
		}
		if (Texture.GetUsage() == Usage) return true;
		Asset::Build::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.Usage = Usage;
		Settings.bSRGB = Asset::Build::TextureBuilder::GetDefaultSRGB(Usage);
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DSRGB(
		DTexture2D& Texture, bool bSRGB, std::string& OutError) -> bool
	{
		if (Texture.IsSRGB() == bSRGB) return true;
		Asset::Build::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.bSRGB = bSRGB;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DMaxResolution(
		DTexture2D& Texture, uint32 MaxResolution, std::string& OutError) -> bool
	{
		if (Texture.GetMaxResolution() == MaxResolution) return true;
		Asset::Build::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.MaxResolution = MaxResolution;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DCompressionQuality(
		DTexture2D& Texture,
		ETextureCompressionQuality Quality,
		std::string& OutError) -> bool
	{
		if (!Asset::Build::TextureBuilder::IsValidCompressionQuality(Quality))
		{
			OutError = "Texture compression quality is invalid.";
			return false;
		}
		if (Texture.GetCompressionQuality() == Quality) return true;
		Asset::Build::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.CompressionQuality = Quality;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DAlphaMipMode(
		DTexture2D& Texture, ETextureAlphaMipMode Mode, std::string& OutError) -> bool
	{
		if (!Asset::Build::TextureBuilder::IsValidAlphaMipMode(Mode))
		{
			OutError = "Texture alpha mip mode is invalid.";
			return false;
		}
		if (Texture.GetAlphaMipMode() == Mode) return true;
		Asset::Build::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.AlphaMipMode = Mode;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DAlphaCoverageThreshold(
		DTexture2D& Texture, float Threshold, std::string& OutError) -> bool
	{
		if (!Asset::Build::TextureBuilder::IsValidAlphaCoverageThreshold(Threshold))
		{
			OutError = "Texture alpha coverage threshold must be greater than zero and less than one.";
			return false;
		}
		if (Texture.GetAlphaCoverageThreshold() == Threshold) return true;
		Asset::Build::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.AlphaCoverageThreshold = Threshold;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}
}
