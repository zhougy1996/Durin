#include "Texture/TextureCube.h"

#include "AssetCore.h"
#include "AssetSystem.h"
#include "DerivedDataObjectStore.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DynamicRHI.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/SourcePath.h"
#include "Serialization/Archive.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/TextureCubePostLoad.h"
#include "Texture/TextureCubeAuthoring.h"
#include "Texture/TextureDerivedData.h"

namespace Durin
{
	namespace
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceSuffixes = {
			"px", "nx", "py", "ny", "pz", "nz"};
		constexpr uint64 TextureCubeDerivedDataBudgetBytes = 4ull * 1024ull * 1024ull * 1024ull;
		constexpr uint32 TextureCubeDerivedDataCleanupDeleteLimit = 16;
		constexpr std::string_view TextureSourceRoot = "Textures";
		constexpr std::string_view TextureDecoderId = "DurinImage";
		constexpr uint32 TextureDecoderVersion = 1;

		auto GetTextureCubeObjectStore() -> Asset::FDerivedDataObjectStore
		{
			return Asset::FDerivedDataObjectStore("TextureCube/Objects", MaximumTexturePayloadBytes);
		}

		auto FaceToIndex(ETextureCubeFace Face) -> size_t
		{
			const size_t Index = static_cast<size_t>(Face);
			check(Index < TextureCubeFaceCount);
			return Index;
		}

		auto FindOwningMount(std::string_view VirtualPath) -> const PathUtilities::FMountPoint*
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(VirtualPath);
			return Lookup ? Lookup.Mount : nullptr;
		}

		auto MakeCanonicalSourceLocation(
			const FAssetPath& AssetPath,
			std::string_view Suffix,
			std::string_view Extension,
			std::string_view RequestedSourcePath,
			std::filesystem::path& OutPhysicalPath,
			std::string& OutStoredPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FMountPoint* Mount = FindOwningMount(AssetPath.ToString());
			if (!Mount)
			{
				OutError = std::format("TextureCube asset {} is not beneath a registered package mount.",
					AssetPath.ToString());
				return false;
			}
			if (RequestedSourcePath.empty())
			{
				std::filesystem::path RelativeAssetPath(
					std::string(AssetPath.ToString().substr(Mount->VirtualRoot.size())));
				const std::string FileName = std::format("{}{}{}",
					RelativeAssetPath.stem().generic_string(), Suffix, Extension);
				RelativeAssetPath.replace_filename(FileName);
				const std::filesystem::path StoredPath =
					std::filesystem::path(TextureSourceRoot) / RelativeAssetPath;
				OutStoredPath = Mount->VirtualRoot + StoredPath.lexically_normal().generic_string();
			}
			else
			{
				OutStoredPath = RequestedSourcePath;
			}
			const PathUtilities::FSourcePathResult Resolved =
				PathUtilities::ResolveSourcePath(
					OutStoredPath, PathUtilities::EPathExistence::AllowMissing);
			if (!Resolved)
			{
				OutError = Resolved.Message;
				return false;
			}
			OutPhysicalPath = Resolved.PhysicalPath;
			return true;
		}

		auto HashTextureSource(const std::filesystem::path& Path,
			FXxHash128& OutHash, std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Path.generic_string()))
			{
				OutError = std::format("Failed to read TextureCube source file: {}", Path.generic_string());
				return false;
			}
			OutHash = FXxHash128::HashBuffer(Bytes);
			OutError.clear();
			return true;
		}

		auto MakeSourceFile(const std::string& StoredPath, const FXxHash128& Hash) -> FTextureSourceFile
		{
			return {
				.SourcePath = {.Path = StoredPath},
				.SourceContentHashLow = Hash.HashLow,
				.SourceContentHashHigh = Hash.HashHigh};
		}

		auto ResolveCubeSource(const DTextureCube& Texture, ETextureCubeFace Face) -> std::filesystem::path
		{
			const FTextureCubeSourceImportData& Provenance = Texture.GetSourceImportData();
			if (Provenance.SourceLayout == ETextureCubeSourceLayout::SixFaces
				&& Provenance.GetFace(Face).HasSource())
			{
				const PathUtilities::FSourcePathResult Resolved =
					PathUtilities::ResolveSourcePath(
						Provenance.GetFace(Face).SourcePath.Path,
						PathUtilities::EPathExistence::RequireFile);
				if (Resolved) return Resolved.PhysicalPath;
			}
			return {};
		}

		auto ResolvePanoramaSource(const DTextureCube& Texture) -> std::filesystem::path
		{
			const FTextureCubeSourceImportData& Provenance = Texture.GetSourceImportData();
			if (Provenance.SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama
				&& Provenance.Panorama.HasSource())
			{
				const PathUtilities::FSourcePathResult Resolved =
					PathUtilities::ResolveSourcePath(
						Provenance.Panorama.SourcePath.Path,
						PathUtilities::EPathExistence::RequireFile);
				if (Resolved) return Resolved.PhysicalPath;
			}
			return {};
		}

		auto ValidateCubeProvenance(const DTextureCube& Texture, std::string& OutError) -> bool
		{
			const FTextureCubeSourceImportData& Provenance = Texture.GetSourceImportData();
			if (!Provenance.HasSource()) return true;
			if (!Texture.GetPackage())
			{
				OutError = "TextureCube source cannot be validated without an owning package.";
				return false;
			}
			auto ValidateSourcePath = [&](std::string_view SourcePath) -> bool {
				const PathUtilities::FMountPolicyResult Dependency =
					PathUtilities::CheckMountDependency(
						Texture.GetPackage()->GetPackagePath(), SourcePath);
				if (!Dependency)
				{
					OutError = Dependency.Message;
					return false;
				}
				const PathUtilities::FSourcePathResult Resolved =
					PathUtilities::ResolveSourcePath(
						SourcePath, PathUtilities::EPathExistence::AllowMissing);
				if (!Resolved
					&& Resolved.Error != PathUtilities::EMountPathError::UnavailableRoot)
				{
					OutError = Resolved.Message;
					return false;
				}
				return true;
			};
			if (Provenance.DecoderId != TextureDecoderId
				|| Provenance.DecoderVersion != TextureDecoderVersion
				|| Provenance.ProjectionVersion != TextureCubeProjectionVersion)
			{
				OutError = "TextureCube source decoder or projection version is unsupported.";
				return false;
			}
			if (Provenance.SourceLayout != Texture.GetSourceLayout())
			{
				OutError = "TextureCube source provenance layout does not match its authored layout.";
				return false;
			}
			if (Provenance.SourceLayout == ETextureCubeSourceLayout::SixFaces)
			{
				if (Provenance.Panorama.HasSource())
				{
					OutError = "Six-face TextureCube provenance contains an inactive panorama source.";
					return false;
				}
				for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
				{
					const FTextureSourceFile& Source =
						Provenance.GetFace(static_cast<ETextureCubeFace>(FaceIndex));
					if (!Source.HasSource() || !Source.HasContentHash())
					{
						OutError = "TextureCube face source provenance is incomplete.";
						return false;
					}
					if (!ValidateSourcePath(Source.SourcePath.Path)) return false;
				}
				return true;
			}
			if (Provenance.SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
			{
				if (!Provenance.Panorama.HasSource() || !Provenance.Panorama.HasContentHash())
				{
					OutError = "TextureCube panorama source provenance is incomplete.";
					return false;
				}
				if (!ValidateSourcePath(Provenance.Panorama.SourcePath.Path)) return false;
				for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
					if (Provenance.GetFace(static_cast<ETextureCubeFace>(FaceIndex)).HasSource())
					{
						OutError = "Panorama TextureCube provenance contains inactive face sources.";
						return false;
					}
				return true;
			}
			OutError = "TextureCube source provenance layout is invalid.";
			return false;
		}


		auto ValidateCubeSourceData(const FTextureCubeSourceData& SourceData, std::string& OutError) -> bool
		{
			const FTextureSourceData& Reference = SourceData.Faces[0];
			for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			{
				const FTextureSourceData& Face = SourceData.Faces[FaceIndex];
				if (!Face.IsValid())
				{
					OutError = std::format("{} face source data is invalid.", FaceNames[FaceIndex]);
					return false;
				}
				if (Face.Width != Face.Height)
				{
					OutError = std::format("{} face must be square, but is {}x{}.",
						FaceNames[FaceIndex], Face.Width, Face.Height);
					return false;
				}
				if (Face.Width != Reference.Width || Face.Height != Reference.Height)
				{
					OutError = std::format("{} face dimensions {}x{} do not match PositiveX {}x{}; all faces must be identical.",
						FaceNames[FaceIndex], Face.Width, Face.Height, Reference.Width, Reference.Height);
					return false;
				}
				if (Face.SourceChannelCount != Reference.SourceChannelCount)
				{
					OutError = std::format("{} face source channel count {} does not match PositiveX {}; all faces must use an identical source format.",
						FaceNames[FaceIndex], Face.SourceChannelCount, Reference.SourceChannelCount);
					return false;
				}
			}
			return true;
		}

	}

	auto FTextureCubeSourceImportData::GetFace(ETextureCubeFace Face) const -> const FTextureSourceFile&
	{
		switch (Face)
		{
		case ETextureCubeFace::PositiveX: return PositiveX;
		case ETextureCubeFace::NegativeX: return NegativeX;
		case ETextureCubeFace::PositiveY: return PositiveY;
		case ETextureCubeFace::NegativeY: return NegativeY;
		case ETextureCubeFace::PositiveZ: return PositiveZ;
		case ETextureCubeFace::NegativeZ: return NegativeZ;
		}
		checkf(false, "Invalid cube face");
		return PositiveX;
	}

	auto FTextureCubeSourceImportData::GetMutableFace(ETextureCubeFace Face) -> FTextureSourceFile&
	{
		return const_cast<FTextureSourceFile&>(std::as_const(*this).GetFace(Face));
	}

	auto FTextureCubeSourceImportData::HasSource() const -> bool
	{
		if (SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
			return Panorama.HasSource();
		for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			if (GetFace(static_cast<ETextureCubeFace>(FaceIndex)).HasSource()) return true;
		return false;
	}

	auto FTextureCubeSourceData::IsValid() const -> bool
	{
		std::string Error;
		return ValidateCubeSourceData(*this, Error);
	}

	auto FTextureCubePlatformData::IsValid() const -> bool
	{
		if (PixelFormat == EPixelFormat::Unknown) return false;
		const FTexturePlatformData& Reference = Faces[0];
		if (!Reference.IsValid() || Reference.PixelFormat != PixelFormat || Reference.Mips.front().Width != Reference.Mips.front().Height)
			return false;
		for (const FTexturePlatformData& Face : Faces)
		{
			if (!Face.IsValid() || Face.PixelFormat != PixelFormat || Face.Mips.size() != Reference.Mips.size()) return false;
			for (size_t MipIndex = 0; MipIndex < Face.Mips.size(); ++MipIndex)
			{
				if (Face.Mips[MipIndex].Width != Reference.Mips[MipIndex].Width
					|| Face.Mips[MipIndex].Height != Reference.Mips[MipIndex].Height
					|| Face.Mips[MipIndex].RowPitch != Reference.Mips[MipIndex].RowPitch) return false;
			}
		}
		return true;
	}

	DTextureCube::DTextureCube(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{}

	DTextureCube::~DTextureCube() = default;

	auto DTextureCube::GetSourceFile(ETextureCubeFace Face) const -> const std::string&
	{
		if (SourceImportData.SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			const FTextureSourceFile& Source = SourceImportData.GetFace(Face);
			if (Source.HasSource()) return Source.SourcePath.Path;
		}
		static const std::string EmptySource;
		return EmptySource;
	}

	auto DTextureCube::ResolvePanoramaSource() const -> std::filesystem::path
	{
		return Durin::ResolvePanoramaSource(*this);
	}

	auto DTextureCube::GetBuiltFaceDimension() const -> uint32
	{
		if (PlatformData && !PlatformData->Faces[0].Mips.empty())
			return PlatformData->Faces[0].Mips[0].Width;
		return SourceData && SourceData->IsValid() ? SourceData->Faces[0].Width : 0;
	}

	auto DTextureCube::GetBuiltMipCount() const -> uint32
	{
		return PlatformData && PlatformData->IsValid()
			? static_cast<uint32>(PlatformData->Faces[0].Mips.size()) : 0;
	}

	auto DTextureCube::GetBuiltPixelFormat() const -> EPixelFormat
	{
		return PlatformData && PlatformData->IsValid() ? PlatformData->PixelFormat : EPixelFormat::Unknown;
	}

	auto DTextureCube::InvalidatePlatformData() -> void
	{
		PlatformData.reset();
		InvalidateRenderResource();
	}

	auto DTextureCube::CreateRenderResourceCandidate(
		FTextureReference* TextureReference,
		uint64 Revision,
		const std::shared_ptr<FTextureResourceCompletion>& Completion)
		-> std::unique_ptr<FTextureAssetResource>
	{
		check(PlatformData && PlatformData->IsValid());
		return std::make_unique<FTextureCubeResource>(
			TextureReference,
			std::make_shared<const FTextureCubePlatformData>(*PlatformData),
			Revision,
			Completion);
	}

	auto DTextureCube::RebuildPlatformData(std::string& OutError) -> bool
	{
		const FTextureCubeAuthoringHandlers Handlers = GetTextureCubeAuthoringHandlers();
		if (Handlers.Rebuild) return Handlers.Rebuild(*this, OutError);
		OutError = "TextureCube rebuild policy is unavailable.";
		return false;
	}

	auto DTextureCube::PostLoad(std::string& OutError) -> bool
	{
		if (Asset::GetPackageLoadContext().Mode == Asset::EPackageLoadMode::CookedRuntime)
			return LoadCookedPlatformData(OutError);
		if (Asset::IsAssetMigrationLoad())
		{
			OutError.clear();
			return true;
		}

		return InvokeTextureCubeUncookedPostLoadHandler(*this, OutError);
	}

	auto DTextureCube::LoadCookedPlatformData(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::CookedFailure;
			DerivedDataDiagnostic.Message = std::format(
				"Cooked TextureCube '{}': {}", GetObjectPath(), Message);
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = DerivedDataDiagnostic.Message;
			OutError = LastBuildError;
			return false;
		};
		if (CookedPayload.PayloadId != TextureCubePrimaryCookedPayloadId
			|| CookedPayload.LocationKind
				!= static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
			|| CookedPayload.PayloadSchemaVersion != TexturePayloadSchemaVersion
			|| CookedPayload.TargetPlatform != static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
			|| CookedPayload.TargetProfile != static_cast<uint32>(Asset::ECookTargetProfile::Game)
			|| CookedPayload.CompressionMethod
				!= static_cast<uint32>(Asset::ECookedPayloadCompression::None))
			return FailCooked("required TXPL descriptor is missing or incompatible.");

		const Asset::FPackageLoadContext& LoadContext = Asset::GetPackageLoadContext();
		std::filesystem::path PackagePath;
		std::filesystem::path CompanionPath;
		if (!GetPackage()
			|| !Asset::ResolveCookedPackagePath(
				LoadContext.CookRoot, GetPackage()->GetPackagePath(), PackagePath, &OutError)
			|| !Asset::ResolveCookedCompanionPath(
				LoadContext.CookRoot, PackagePath, CompanionPath, &OutError))
			return FailCooked(OutError.empty()
				? "package companion path could not be resolved." : OutError);

		Asset::FCookedBulkContainer Container;
		if (!Asset::LoadCookedBulkFile(
			CompanionPath, Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game, Container, &OutError))
			return FailCooked(OutError);
		std::span<const uint8> Bytes;
		if (!Asset::ResolveCookedPayload(Container, CookedPayload, Bytes, &OutError))
			return FailCooked(OutError);
		auto CandidatePlatformData = std::make_unique<FTextureCubePlatformData>();
		FCanonicalMemoryReader PayloadAr(Bytes, EArchivePurpose::CookedPayload);
		CandidatePlatformData->Serialize(PayloadAr, {
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (PayloadAr.HasError()) return FailCooked(PayloadAr.GetFailure()->Message);

		SourceData.reset();
		PlatformData = std::move(CandidatePlatformData);
		DerivedDataKey.clear();
		bLoadedFromDerivedDataCache = false;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::CookedLoaded,
			.Message = std::format("Loaded cooked TextureCube payload for '{}'.", GetObjectPath())};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		OutError.clear();
		return true;
	}

	auto DTextureCube::AddToCook(
		Asset::FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError,
		bool bRetainDiagnosticSourceMetadata) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
		{
			OutError = std::format(
				"TextureCube '{}' supports only the Win64 game cook target.", GetObjectPath());
			return false;
		}
		std::vector<uint8> PayloadBytes;
		if (!PlatformData && !PostLoad(OutError))
		{
			OutError = std::format("Failed to cook TextureCube '{}': {}", GetObjectPath(), OutError);
			return false;
		}
		if (!PlatformData)
		{
			OutError = std::format("Failed to cook TextureCube '{}': platform data is unavailable.",
				GetObjectPath());
			return false;
		}
		FCanonicalMemoryWriter CookAr(PayloadBytes, EArchivePurpose::CookedPayload);
		PlatformData->Serialize(CookAr, {
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (CookAr.HasError())
		{
			OutError = std::format("Failed to cook TextureCube '{}': {}",
				GetObjectPath(), CookAr.GetFailure()->Message);
			return false;
		}

		Asset::FCookedBulkPayload BulkPayload{
			.PayloadId = TextureCubePrimaryCookedPayloadId,
			.Flags = 1,
			.PayloadSchemaVersion = TexturePayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = TexturePayloadAlignment,
			.Bytes = std::move(PayloadBytes)};
		return Context.AddPackage(
			std::string(VirtualPackagePath), {std::move(BulkPayload)},
			[this, bRetainDiagnosticSourceMetadata](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<uint8>& OutPackageBytes, std::string* Error) {
				if (Descriptors.size() != 1
					|| Descriptors.front().PayloadId != TextureCubePrimaryCookedPayloadId)
				{
					if (Error) *Error = "TextureCube cook did not produce its required descriptor.";
					return false;
				}
				const FTextureCubeSourceImportData SavedSourceImportData = SourceImportData;
				const Asset::FCookedPayloadDescriptor SavedCookedPayload = CookedPayload;
				CookedPayload = Descriptors.front();
				if (!bRetainDiagnosticSourceMetadata)
				{
					SourceImportData = {};
				}
				Asset::FAssetPackageSerializationOptions Options;
				if (!bRetainDiagnosticSourceMetadata)
				{
					Options.PropertyFilter = [this](const DObject* Object, const FProperty* Property) {
						if (Object != this) return true;
						const FName Name = Property->NamePrivate;
						return Name != FName("SourceImportData");
					};
				}
				const Asset::FAssetResult Result =
					Asset::SerializeAssetPackageBytes(GetPackage(), OutPackageBytes, Options);
				SourceImportData = SavedSourceImportData;
				CookedPayload = SavedCookedPayload;
				if (!Result)
				{
					if (Error) *Error = Result.Message;
					return false;
				}
				return true;
			}, &OutError);
	}

	auto DTextureCube::RefreshBuildStatus() -> void
	{
		const std::shared_ptr<FTextureResourceCompletion>& Completion =
			GetRenderCompletion();
		if (Completion->GetFailedRevision() == GetBuildRevision())
		{
			if (Completion->GetFailureReason()
				== ETextureRenderFailure::UnsupportedFormat)
			{
				BuildStatus = ETextureBuildStatus::UnsupportedFormat;
				LastBuildError = "The current RHI does not support this cube texture format and usage.";
			}
			else
			{
				BuildStatus = ETextureBuildStatus::UploadFailure;
				LastBuildError = "GPU cube texture creation or upload failed.";
			}
		}
	}

	auto DTextureCube::ValidateImportSources(const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const FTextureCubeImportSettings& Settings) -> FTextureCubeImportValidation
	{
		const FTextureCubeAuthoringHandlers Handlers = GetTextureCubeAuthoringHandlers();
		return Handlers.ValidateFaces ? Handlers.ValidateFaces(FaceFiles, Settings)
			: FTextureCubeImportValidation{false, "TextureCube validation policy is unavailable."};
	}

	auto DTextureCube::ValidatePanoramaImportSource(std::string_view PanoramaFile,
		const FTextureCubePanoramaImportSettings& Settings) -> FTextureCubeImportValidation
	{
		const FTextureCubeAuthoringHandlers Handlers = GetTextureCubeAuthoringHandlers();
		return Handlers.ValidatePanorama ? Handlers.ValidatePanorama(PanoramaFile, Settings)
			: FTextureCubeImportValidation{false, "TextureCube validation policy is unavailable."};
	}

	auto DTextureCube::ImportPanoramaAsset(std::string_view PanoramaFile, std::string_view AssetPath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string_view SourceDestination,
		bool bEngineAuthoringContext) -> FTextureCubeImportResult
	{
		if (PanoramaFile.empty()) return {false, "Panorama source is missing.", nullptr};
		const std::filesystem::path Input = std::filesystem::absolute(PanoramaFile).lexically_normal();
		std::string Error;
		FAssetPath ParsedAssetPath;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::GetAssetRegistry().FindAssetExact(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};

		std::filesystem::path Destination;
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(
			ParsedAssetPath, "_panorama", Input.extension().generic_string(),
			SourceDestination,
			Destination, StoredSourcePath, Error))
			return {false, std::move(Error), nullptr};
		FMountedSourceFile MountedSource;
		if (!PrepareMountedSourceFile(
			Input, ParsedAssetPath.ToString(), StoredSourcePath, MountedSource, Error,
			bEngineAuthoringContext))
			return {false, std::move(Error), nullptr};
		DTextureCube* Texture = nullptr;
		Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!CreateResult)
		{
			RollbackMountedSourceFile(MountedSource);
			return {false, CreateResult.Message, nullptr};
		}

		std::vector<uint8> Bytes;
		const FTextureCubeAuthoringHandlers Handlers = GetTextureCubeAuthoringHandlers();
		if (!Handlers.BuildPanorama
			|| !FFileHelper::LoadFileToArray(Bytes, MountedSource.PhysicalPath.generic_string())
			|| !Handlers.BuildPanorama(*Texture, Bytes, Input.extension().generic_string(),
				MountedSource.SourcePath, Settings, Error))
		{
			if (Error.empty()) Error = "TextureCube panorama build policy is unavailable.";
			RollbackMountedSourceFile(MountedSource);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::move(Error), nullptr};
		}

		Asset::FAssetResult SaveResult = Asset::SavePackage(Texture->GetPackage());
		if (!SaveResult)
		{
			RollbackMountedSourceFile(MountedSource);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, SaveResult.Message, nullptr};
		}
		CommitMountedSourceFile(MountedSource);
		return {true, {}, Texture};
	}

	auto DTextureCube::ReimportPanorama(std::string_view PanoramaFile,
		const FTextureCubePanoramaImportSettings& Settings, std::string& OutError) -> bool
	{
		OutError.clear();
		if (SourceLayout != ETextureCubeSourceLayout::EquirectangularPanorama)
		{
			OutError = "Only panorama-backed texture cubes can be reimported through this API.";
			return false;
		}
		if (PanoramaFile.empty())
		{
			OutError = "Panorama source is missing.";
			return false;
		}

		if (!GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		FMountedSourceFile MountedSource;
		if (!ResolveMountedSourceReference(
			GetPackage()->GetPackagePath(),
			SourceImportData.Panorama.SourcePath.Path,
			MountedSource, OutError)) return false;
		const std::filesystem::path Input = MountedSource.PhysicalPath;
		std::error_code EquivalentError;
		const std::filesystem::path Requested =
			std::filesystem::absolute(PanoramaFile).lexically_normal();
		if (!std::filesystem::equivalent(Input, Requested, EquivalentError)
			|| EquivalentError)
		{
			OutError =
				"Reimport is read-only and must use the persisted mounted panorama source.";
			return false;
		}
		std::vector<uint8> EncodedBytes;
		const FTextureCubeAuthoringHandlers Handlers = GetTextureCubeAuthoringHandlers();
		if (!Handlers.BuildPanorama
			|| !FFileHelper::LoadFileToArray(EncodedBytes, Input.generic_string())
			|| !Handlers.BuildPanorama(*this, EncodedBytes,
				Input.extension().generic_string(), MountedSource.SourcePath, Settings, OutError))
		{
			if (OutError.empty()) OutError = "TextureCube panorama build policy is unavailable.";
			return false;
		}
		const Asset::FAssetResult SaveResult = Asset::SavePackage(GetPackage());
		if (!SaveResult)
		{
			OutError = SaveResult.Message;
			return false;
		}
		return true;

	}

	auto DTextureCube::ReimportSources(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		OutError.clear();
		if (SourceLayout != ETextureCubeSourceLayout::SixFaces)
		{
			OutError = "Only six-face texture cubes can be reimported through this API.";
			return false;
		}
		if (!GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		std::array<std::string, TextureCubeFaceCount> MountedFaceFiles;
		std::array<FMountedSourceFile, TextureCubeFaceCount> MountedSources;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			const ETextureCubeFace Face = static_cast<ETextureCubeFace>(FaceIndex);
			if (!ResolveMountedSourceReference(
				GetPackage()->GetPackagePath(),
				SourceImportData.GetFace(Face).SourcePath.Path,
				MountedSources[FaceIndex], OutError)) return false;
			std::error_code EquivalentError;
			if (!std::filesystem::equivalent(
				MountedSources[FaceIndex].PhysicalPath,
				std::filesystem::absolute(FaceFiles[FaceIndex]).lexically_normal(),
				EquivalentError) || EquivalentError)
			{
				OutError =
					"Reimport is read-only and every face must use its persisted mounted source.";
				return false;
			}
			MountedFaceFiles[FaceIndex] =
				MountedSources[FaceIndex].PhysicalPath.generic_string();
		}
		std::array<std::vector<uint8>, TextureCubeFaceCount> OwnedBytes;
		std::array<std::span<const uint8>, TextureCubeFaceCount> Bytes;
		std::array<FSourcePath, TextureCubeFaceCount> Paths;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			if (!FFileHelper::LoadFileToArray(
				OwnedBytes[FaceIndex], MountedSources[FaceIndex].PhysicalPath.generic_string()))
			{
				OutError = "Failed to read mounted TextureCube face source.";
				return false;
			}
			Bytes[FaceIndex] = OwnedBytes[FaceIndex];
			Paths[FaceIndex] = MountedSources[FaceIndex].SourcePath;
		}
		const FTextureCubeAuthoringHandlers Handlers = GetTextureCubeAuthoringHandlers();
		if (!Handlers.BuildFaces
			|| !Handlers.BuildFaces(*this, Bytes, Paths, Settings, OutError))
		{
			if (OutError.empty()) OutError = "TextureCube face build policy is unavailable.";
			return false;
		}
		const Asset::FAssetResult SaveResult = Asset::SavePackage(GetPackage());
		if (!SaveResult)
		{
			OutError = SaveResult.Message;
			return false;
		}
		return true;

	}

	auto DTextureCube::ExchangeImportedState(DTextureCube& Other) noexcept -> void
	{
		if (&Other == this) return;
		std::swap(SourceLayout, Other.SourceLayout);
		std::swap(SourceImportData, Other.SourceImportData);
		std::swap(PanoramaFaceDimension, Other.PanoramaFaceDimension);
		std::swap(PanoramaExposureEV, Other.PanoramaExposureEV);
		std::swap(OriginalSourceWidth, Other.OriginalSourceWidth);
		std::swap(OriginalSourceHeight, Other.OriginalSourceHeight);
		std::swap(bSRGB, Other.bSRGB);
		std::swap(CookedPayload, Other.CookedPayload);
		std::swap(SourceData, Other.SourceData);
		std::swap(PlatformData, Other.PlatformData);
		std::swap(DerivedDataKey, Other.DerivedDataKey);
		std::swap(DerivedDataDiagnostic, Other.DerivedDataDiagnostic);
		std::swap(bLoadedFromDerivedDataCache, Other.bLoadedFromDerivedDataCache);
		std::swap(BuildStatus, Other.BuildStatus);
		std::swap(LastBuildError, Other.LastBuildError);
		QueueRenderResourceBuild();
		Other.QueueRenderResourceBuild();
		MarkPackageDirty();
		Other.MarkPackageDirty();
	}

	auto DTextureCube::PublishAuthoringCandidate(
		ETextureCubeSourceLayout InSourceLayout,
		FTextureCubeSourceImportData InSourceImportData,
		uint32 InPanoramaFaceDimension,
		float InPanoramaExposureEV,
		uint32 InOriginalSourceWidth,
		uint32 InOriginalSourceHeight,
		bool bInSRGB,
		std::unique_ptr<FTextureCubeSourceData> InSourceData,
		std::unique_ptr<FTextureCubePlatformData> InPlatformData,
		std::string InDerivedDataKey,
		FTextureDerivedDataDiagnostic InDiagnostic) -> void
	{
		check(InSourceData && InPlatformData && InPlatformData->IsValid());
		SourceLayout = InSourceLayout;
		SourceImportData = std::move(InSourceImportData);
		PanoramaFaceDimension = InPanoramaFaceDimension;
		PanoramaExposureEV = InPanoramaExposureEV;
		OriginalSourceWidth = InOriginalSourceWidth;
		OriginalSourceHeight = InOriginalSourceHeight;
		bSRGB = bInSRGB;
		SourceData = std::move(InSourceData);
		PlatformData = std::move(InPlatformData);
		DerivedDataKey = std::move(InDerivedDataKey);
		DerivedDataDiagnostic = std::move(InDiagnostic);
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		bLoadedFromDerivedDataCache = false;
		QueueRenderResourceBuild();
		MarkPackageDirty();
	}

	auto DTextureCube::PublishDerivedDataLoad(
		std::unique_ptr<FTextureCubePlatformData> InPlatformData,
		std::string InDerivedDataKey,
		std::string& OutError) -> bool
	{
		if (!InPlatformData || !InPlatformData->IsValid() || InDerivedDataKey.empty())
		{
			OutError = "TextureCube DDC publication requires valid platform data and key.";
			return false;
		}
		SourceData.reset();
		PlatformData = std::move(InPlatformData);
		DerivedDataKey = std::move(InDerivedDataKey);
		bLoadedFromDerivedDataCache = true;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Hit,
			.Key = DerivedDataKey,
			.Message = "Loaded TextureCube platform data from DDC."};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		OutError.clear();
		return true;
	}

	auto DTextureCube::ChangePanoramaSourceReference(
		std::string_view SourceVirtualPath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		FMountedSourceFile Source;
		if (!ResolveMountedSourceReference(
			GetPackage()->GetPackagePath(), SourceVirtualPath, Source, OutError))
			return false;
		const ETextureCubeSourceLayout PreviousLayout = SourceLayout;
		const FTextureCubeSourceImportData Previous = SourceImportData;
		SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama;
		SourceImportData = {};
		SourceImportData.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama;
		SourceImportData.Panorama.SourcePath = Source.SourcePath;
		if (!ReimportPanorama(Source.PhysicalPath.generic_string(), Settings, OutError))
		{
			SourceLayout = PreviousLayout;
			SourceImportData = Previous;
			return false;
		}
		return true;
	}

	auto DTextureCube::ChangeSourceReferences(
		const std::array<std::string, TextureCubeFaceCount>& SourceVirtualPaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		std::array<FMountedSourceFile, TextureCubeFaceCount> Sources;
		std::array<std::string, TextureCubeFaceCount> PhysicalFiles;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			if (!ResolveMountedSourceReference(
				GetPackage()->GetPackagePath(), SourceVirtualPaths[FaceIndex],
				Sources[FaceIndex], OutError)) return false;
			PhysicalFiles[FaceIndex] = Sources[FaceIndex].PhysicalPath.generic_string();
		}
		const ETextureCubeSourceLayout PreviousLayout = SourceLayout;
		const FTextureCubeSourceImportData Previous = SourceImportData;
		SourceLayout = ETextureCubeSourceLayout::SixFaces;
		SourceImportData = {};
		SourceImportData.SourceLayout = ETextureCubeSourceLayout::SixFaces;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			SourceImportData.GetMutableFace(static_cast<ETextureCubeFace>(FaceIndex)).SourcePath =
				Sources[FaceIndex].SourcePath;
		if (!ReimportSources(PhysicalFiles, Settings, OutError))
		{
			SourceLayout = PreviousLayout;
			SourceImportData = Previous;
			return false;
		}
		return true;
	}

	auto DTextureCube::IngestAndChangePanoramaSource(
		std::string_view FilePath,
		std::string_view TargetSourceVirtualPath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		FMountedSourceFile Source;
		if (!PrepareMountedSourceFile(
			FilePath, GetPackage()->GetPackagePath(),
			TargetSourceVirtualPath, Source, OutError)) return false;
		const bool bChanged =
			ChangePanoramaSourceReference(Source.SourcePath.Path, Settings, OutError);
		if (bChanged)
			CommitMountedSourceFile(Source);
		else
			RollbackMountedSourceFile(Source);
		return bChanged;
	}

	auto DTextureCube::IngestAndChangeSources(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const std::array<std::string, TextureCubeFaceCount>& TargetSourceVirtualPaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		std::array<FMountedSourceFile, TextureCubeFaceCount> Sources;
		std::array<std::string, TextureCubeFaceCount> VirtualPaths;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			if (!PrepareMountedSourceFile(
				FaceFiles[FaceIndex], GetPackage()->GetPackagePath(),
				TargetSourceVirtualPaths[FaceIndex], Sources[FaceIndex], OutError))
			{
				for (FMountedSourceFile& Source : Sources)
					RollbackMountedSourceFile(Source);
				return false;
			}
			VirtualPaths[FaceIndex] = Sources[FaceIndex].SourcePath.Path;
		}
		const bool bChanged = ChangeSourceReferences(VirtualPaths, Settings, OutError);
		for (FMountedSourceFile& Source : Sources)
		{
			if (bChanged)
				CommitMountedSourceFile(Source);
			else
				RollbackMountedSourceFile(Source);
		}
		return bChanged;
	}

	auto DTextureCube::ImportAsset(const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		std::string_view AssetPath, const FTextureCubeImportSettings& Settings,
		const std::array<std::string, TextureCubeFaceCount>& SourceDestinations,
		bool bEngineAuthoringContext)
		-> FTextureCubeImportResult
	{
		std::array<std::filesystem::path, TextureCubeFaceCount> Inputs;
		std::string ValidationError;
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			Inputs[Index] = std::filesystem::absolute(FaceFiles[Index]).lexically_normal();
			if (!std::filesystem::is_regular_file(Inputs[Index]))
				return {false, std::format("{} face source is unavailable.", FaceNames[Index]), nullptr};
		}

		FAssetPath ParsedAssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &PathError)) return {false, std::move(PathError), nullptr};
		if (Asset::GetAssetRegistry().FindAssetExact(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};

		std::array<std::string, TextureCubeFaceCount> StoredSourcePaths;
		std::array<std::filesystem::path, TextureCubeFaceCount> Destinations;
		std::array<FMountedSourceFile, TextureCubeFaceCount> MountedSources;
		auto RollbackMountedSources = [&] {
			for (FMountedSourceFile& Source : MountedSources)
				RollbackMountedSourceFile(Source);
		};
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			if (!MakeCanonicalSourceLocation(
					ParsedAssetPath, std::format("_{}", FaceSuffixes[FaceIndex]),
					Inputs[FaceIndex].extension().generic_string(),
					SourceDestinations[FaceIndex],
					Destinations[FaceIndex], StoredSourcePaths[FaceIndex], PathError)
				|| !PrepareMountedSourceFile(
					Inputs[FaceIndex], ParsedAssetPath.ToString(),
					StoredSourcePaths[FaceIndex], MountedSources[FaceIndex], PathError,
					bEngineAuthoringContext))
			{
				RollbackMountedSources();
				return {false, std::move(PathError), nullptr};
			}
			Destinations[FaceIndex] = MountedSources[FaceIndex].PhysicalPath;
			StoredSourcePaths[FaceIndex] = MountedSources[FaceIndex].SourcePath.Path;
		}

		DTextureCube* Texture = nullptr;
		Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!CreateResult)
		{
			RollbackMountedSources();
			return {false, CreateResult.Message, nullptr};
		}

		std::array<std::vector<uint8>, TextureCubeFaceCount> OwnedBytes;
		std::array<std::span<const uint8>, TextureCubeFaceCount> Bytes;
		std::array<FSourcePath, TextureCubeFaceCount> Paths;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			if (!FFileHelper::LoadFileToArray(
				OwnedBytes[FaceIndex], MountedSources[FaceIndex].PhysicalPath.generic_string()))
			{
				RollbackMountedSources();
				Asset::UnloadPackage(ParsedAssetPath);
				return {false, "Failed to read mounted TextureCube face source.", nullptr};
			}
			Bytes[FaceIndex] = OwnedBytes[FaceIndex];
			Paths[FaceIndex] = MountedSources[FaceIndex].SourcePath;
		}
		const FTextureCubeAuthoringHandlers Handlers = GetTextureCubeAuthoringHandlers();
		if (!Handlers.BuildFaces
			|| !Handlers.BuildFaces(*Texture, Bytes, Paths, Settings, PathError))
		{
			if (PathError.empty()) PathError = "TextureCube face build policy is unavailable.";
			RollbackMountedSources();
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::move(PathError), nullptr};
		}

		Asset::FAssetResult SaveResult = Asset::SavePackage(Texture->GetPackage());
		if (!SaveResult)
		{
			RollbackMountedSources();
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, SaveResult.Message, nullptr};
		}
		for (FMountedSourceFile& Source : MountedSources)
			CommitMountedSourceFile(Source);
		return {true, {}, Texture};
	}
}
