#include "StaticModelImportBuild.h"

#include "AssetSystem.h"
#include "DerivedDataObjectStore.h"
#include "Hash/XxHash.h"
#include "ImageDecoder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/SourcePath.h"
#include "Texture/TextureBuild.h"
#include "Texture/TextureDerivedData.h"

namespace Durin
{
	namespace
	{
		auto Lower(std::string Value) -> std::string
		{
			std::ranges::transform(Value, Value.begin(), [](char Character) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
			});
			return Value;
		}

		auto IsMissingPathError(const std::error_code& ErrorCode) -> bool
		{
			return ErrorCode == std::errc::no_such_file_or_directory
				|| ErrorCode.value() == 2
				|| ErrorCode.value() == 3;
		}

		auto LoadBytes(
			const std::filesystem::path& Path,
			std::vector<uint8>& OutBytes,
			std::string& OutError) -> bool
		{
			if (!FFileHelper::LoadFileToArray(OutBytes, Path.generic_string()))
			{
				OutError = std::format("Failed to read image source {}.", Path.generic_string());
				return false;
			}
			return true;
		}

		auto SanitizeSourceName(std::string_view Value, std::string_view Fallback) -> std::string
		{
			std::string Result;
			bool bLastWasSeparator = false;
			for (const char Character : Value)
			{
				const bool bValid =
					(Character >= 'a' && Character <= 'z')
					|| (Character >= 'A' && Character <= 'Z')
					|| (Character >= '0' && Character <= '9');
				if (bValid)
				{
					Result.push_back(Character);
					bLastWasSeparator = false;
				}
				else if (!Result.empty() && !bLastWasSeparator)
				{
					Result.push_back('_');
					bLastWasSeparator = true;
				}
			}
			while (!Result.empty() && Result.back() == '_') Result.pop_back();
			return Result.empty() ? std::string(Fallback) : Result;
		}
	}

	auto BuildEmbeddedImageSourcePath(
		const FSourcePath& RootModelSource,
		std::string_view ImageIdentity,
		std::string_view SuggestedName,
		std::string_view Extension,
		FSourcePath& OutPath,
		std::string& OutError) -> bool
	{
		OutPath = {};
		std::string NormalizedExtension(Extension);
		std::ranges::transform(
			NormalizedExtension, NormalizedExtension.begin(), [](char Character) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
			});
		if (RootModelSource.IsEmpty() || ImageIdentity.empty()
			|| NormalizedExtension.empty() || NormalizedExtension.front() != '.'
			|| !Asset::IsSupportedImageExtension(NormalizedExtension))
		{
			OutError = "Embedded image extraction requires a root source, identity, and supported extension.";
			return false;
		}
		const PathUtilities::FSourcePathResult Root =
			PathUtilities::ResolveSourcePath(
				RootModelSource.Path, PathUtilities::EPathExistence::AllowMissing);
		if (!Root)
		{
			OutError = Root.Message;
			return false;
		}
		const std::filesystem::path VirtualRoot(Root.NormalizedVirtualPath);
		const std::string ModelName =
			SanitizeSourceName(VirtualRoot.stem().generic_string(), "Model");
		const std::string ImageName = SanitizeSourceName(SuggestedName, "Image");
		const std::string IdentityHash = FXxHash64::HashBuffer(ImageIdentity).ToString().substr(0, 8);
		OutPath.Path = (
			VirtualRoot.parent_path()
			/ (ModelName + "_Embedded")
			/ (ImageName + "_" + IdentityHash + NormalizedExtension)).generic_string();
		OutError.clear();
		return true;
	}

	struct FMultiAssetImportTransaction::FImpl
	{
		struct FSourceOperation
		{
			FSourcePath SourcePath;
			std::filesystem::path PhysicalPath;
			std::filesystem::path StagedPath;
			std::vector<uint8> Bytes;
			bool bRequiresWrite = false;
			bool bPublished = false;
		};

		struct FPreparedTexture
		{
			DTexture2D* Texture = nullptr;
			std::filesystem::path DerivedDataPath;
			bool bDerivedDataExisted = false;
		};

		struct FMutation
		{
			std::function<bool(std::string&)> Apply;
			std::function<void()> Rollback;
			bool bApplied = false;
		};

		std::vector<FPortableTextureBuildRequest> Requests;
		std::vector<FPreparedTexture> PreparedTextures;
		std::vector<FSourceOperation> Sources;
		std::vector<DPackage*> Packages;
		std::vector<FMutation> Mutations;
		std::vector<DTexture2D*> Textures;
		DPackage* RootPackage = nullptr;
		EImportTransactionFailurePoint FailurePoint = EImportTransactionFailurePoint::None;
		size_t FailureOccurrence = 0;
		size_t FailureVisits = 0;
		bool bPrepared = false;
		bool bStaged = false;
		bool bPublished = false;
		bool bAttempted = false;

		auto ShouldFail(EImportTransactionFailurePoint Point) -> bool
		{
			if (Point != FailurePoint) return false;
			return FailureVisits++ == FailureOccurrence;
		}

		auto FailInjected(EImportTransactionFailurePoint Point, std::string& OutError) -> bool
		{
			if (!ShouldFail(Point)) return false;
			OutError = std::format(
				"Injected multi-asset import failure at phase {}.",
				static_cast<uint32>(Point));
			return true;
		}
	};

	FMultiAssetImportTransaction::FMultiAssetImportTransaction()
		: Impl(std::make_unique<FImpl>())
	{
	}

	FMultiAssetImportTransaction::~FMultiAssetImportTransaction()
	{
		if (Impl && !Impl->bPublished) Rollback();
	}

	auto FMultiAssetImportTransaction::AddTexture(FPortableTextureBuildRequest Request) -> void
	{
		check(!Impl->bAttempted);
		Impl->Requests.push_back(std::move(Request));
	}

	auto FMultiAssetImportTransaction::AddPackage(DPackage* Package, bool bRootPackage) -> void
	{
		check(!Impl->bAttempted);
		if (Package) Impl->Packages.push_back(Package);
		if (bRootPackage) Impl->RootPackage = Package;
	}

	auto FMultiAssetImportTransaction::AddLoadedObjectMutation(
		std::function<bool(std::string&)> Apply,
		std::function<void()> Rollback) -> void
	{
		check(!Impl->bAttempted);
		Impl->Mutations.push_back({std::move(Apply), std::move(Rollback)});
	}

	auto FMultiAssetImportTransaction::SetFailurePoint(
		EImportTransactionFailurePoint Point,
		size_t Occurrence) -> void
	{
		check(!Impl->bAttempted);
		Impl->FailurePoint = Point;
		Impl->FailureOccurrence = Occurrence;
	}

	auto FMultiAssetImportTransaction::Prepare(std::string& OutError) -> bool
	{
		if (Impl->bAttempted)
		{
			OutError = "The multi-asset import transaction has already been attempted.";
			return false;
		}
		Impl->bAttempted = true;
		if (Impl->Requests.empty() && Impl->Packages.empty())
		{
			OutError = "The multi-asset import transaction has no outputs.";
			return false;
		}

		std::unordered_set<std::string> PackageIdentities;
		std::unordered_map<std::string, std::vector<uint8>> PreflightSources;
		for (DPackage* Package : Impl->Packages)
		{
			FAssetPath Path;
			if (!Package || !Package->IsAssetPackage()
				|| !FAssetPath::TryCreate(Package->GetPackagePath(), Path)
				|| !PackageIdentities.insert(Lower(Path.ToString())).second)
			{
				OutError = "The transaction contains an invalid or duplicate existing package.";
				Rollback();
				return false;
			}
		}
		for (const FPortableTextureBuildRequest& Request : Impl->Requests)
		{
			if (!Request.AssetPath.IsValid()
				|| Request.ExternalSource.empty() == Request.EncodedBytes.empty())
			{
				OutError = "Each texture request requires a valid asset path and exactly one source payload.";
				Rollback();
				return false;
			}
			if (!PackageIdentities.insert(Lower(Request.AssetPath.ToString())).second
				|| Asset::FindLoadedPackage(Request.AssetPath)
				|| Asset::GetAssetRegistry().FindAsset(Request.AssetPath))
			{
				OutError = std::format(
					"Texture package {} collides with an existing or planned asset.",
					Request.AssetPath.ToString());
				Rollback();
				return false;
			}
			const PathUtilities::FContentPathResult PackageDestination =
				PathUtilities::ResolveContentPath(
					Request.AssetPath.GetView(), PathUtilities::EPathExistence::AllowMissing);
			if (!PackageDestination)
			{
				OutError = PackageDestination.Message;
				Rollback();
				return false;
			}
			std::filesystem::path PackageFile = PackageDestination.PhysicalPath;
			PackageFile += ".dasset";
			std::error_code Ec;
			if (std::filesystem::exists(PackageFile, Ec))
			{
				OutError = std::format(
					"Texture package destination {} already exists.", PackageFile.generic_string());
				Rollback();
				return false;
			}
			if (Ec && !IsMissingPathError(Ec))
			{
				OutError = std::format(
					"Failed to inspect package destination {}: {}",
					PackageFile.generic_string(), Ec.message());
				Rollback();
				return false;
			}

			std::vector<uint8> Bytes = Request.EncodedBytes;
			FSourcePath SourcePath = Request.SourceDestination;
			std::filesystem::path PhysicalPath;
			if (!Request.ExternalSource.empty())
			{
				const std::filesystem::path Input =
					std::filesystem::absolute(Request.ExternalSource, Ec).lexically_normal();
				if (Ec || !std::filesystem::is_regular_file(Input, Ec)
					|| !LoadBytes(Input, Bytes, OutError))
				{
					if (OutError.empty()) OutError = "External texture source does not exist.";
					Rollback();
					return false;
				}
				const PathUtilities::FSourcePathResult Classified =
					PathUtilities::ClassifySourcePath(Input);
				if (Classified)
				{
					FMountedSourceFile Mounted;
					if (!ResolveMountedSourceReference(
						Request.AssetPath.ToString(),
						Classified.NormalizedVirtualPath,
						Mounted,
						OutError))
					{
						Rollback();
						return false;
					}
					SourcePath = Mounted.SourcePath;
					PhysicalPath = Mounted.PhysicalPath;
				}
			}
			if (PhysicalPath.empty())
			{
				if (SourcePath.IsEmpty())
				{
					OutError = "Embedded and external-unmounted images require an explicit mounted source destination.";
					Rollback();
					return false;
				}
				const PathUtilities::FSourcePathResult Destination =
					PathUtilities::ResolveSourcePath(
						SourcePath.Path, PathUtilities::EPathExistence::AllowMissing);
				if (!Destination)
				{
					OutError = Destination.Message;
					Rollback();
					return false;
				}
				const PathUtilities::FMountPolicyResult Mutation =
					PathUtilities::CheckSourceMutation(
						Request.AssetPath.ToString(), Destination.NormalizedVirtualPath);
				if (!Mutation)
				{
					OutError = Mutation.Message;
					Rollback();
					return false;
				}
				SourcePath.Path = Destination.NormalizedVirtualPath;
				PhysicalPath = Destination.PhysicalPath;
				Ec.clear();
				const bool bExists = std::filesystem::exists(PhysicalPath, Ec);
				if (Ec && !IsMissingPathError(Ec))
				{
					OutError = std::format(
						"Failed to inspect source destination {}: {}",
						PhysicalPath.generic_string(), Ec.message());
					Rollback();
					return false;
				}
				if (bExists)
				{
					if (!std::filesystem::is_regular_file(PhysicalPath, Ec))
					{
						OutError = std::format(
							"Source destination {} is occupied.", PhysicalPath.generic_string());
						Rollback();
						return false;
					}
					std::vector<uint8> ExistingBytes;
					if (!LoadBytes(PhysicalPath, ExistingBytes, OutError)
						|| ExistingBytes != Bytes)
					{
						if (OutError.empty()) OutError = std::format(
							"A different source file already exists at {}.",
							PhysicalPath.generic_string());
						Rollback();
						return false;
					}
				}
				std::filesystem::path StagedPath = PhysicalPath;
				StagedPath += ".import-stage";
				Ec.clear();
				if (std::filesystem::exists(StagedPath, Ec))
				{
					OutError = std::format(
						"Source staging path {} is occupied.", StagedPath.generic_string());
					Rollback();
					return false;
				}
			}
			const std::string Identity = Lower(SourcePath.Path);
			if (const auto It = PreflightSources.find(Identity); It != PreflightSources.end())
			{
				if (It->second != Bytes)
				{
					OutError = std::format(
						"Planned source {} has conflicting encoded bytes.", SourcePath.Path);
					Rollback();
					return false;
				}
			}
			else PreflightSources.emplace(Identity, std::move(Bytes));
		}

		PackageIdentities.clear();
		std::unordered_map<std::string, size_t> SourceIdentities;
		for (FPortableTextureBuildRequest& Request : Impl->Requests)
		{
			if (!Request.AssetPath.IsValid()
				|| Request.ExternalSource.empty() == Request.EncodedBytes.empty())
			{
				OutError = "Each texture request requires a valid asset path and exactly one source payload.";
				Rollback();
				return false;
			}
			const std::string PackageIdentity = Lower(Request.AssetPath.ToString());
			if (!PackageIdentities.insert(PackageIdentity).second
				|| Asset::FindLoadedPackage(Request.AssetPath)
				|| Asset::GetAssetRegistry().FindAsset(Request.AssetPath))
			{
				OutError = std::format(
					"Texture package {} collides with an existing or planned asset.",
					Request.AssetPath.ToString());
				Rollback();
				return false;
			}
			const PathUtilities::FContentPathResult PackageDestination =
				PathUtilities::ResolveContentPath(
					Request.AssetPath.GetView(), PathUtilities::EPathExistence::AllowMissing);
			if (!PackageDestination)
			{
				OutError = PackageDestination.Message;
				Rollback();
				return false;
			}
			std::filesystem::path PackageFile = PackageDestination.PhysicalPath;
			PackageFile += ".dasset";
			std::error_code Ec;
			if (std::filesystem::exists(PackageFile, Ec))
			{
				OutError = std::format(
					"Texture package destination {} already exists.", PackageFile.generic_string());
				Rollback();
				return false;
			}

			std::vector<uint8> EncodedBytes = std::move(Request.EncodedBytes);
			FMountedSourceFile MountedSource;
			if (!Request.ExternalSource.empty())
			{
				const std::filesystem::path Input =
					std::filesystem::absolute(Request.ExternalSource, Ec).lexically_normal();
				if (Ec || !std::filesystem::is_regular_file(Input, Ec)
					|| !LoadBytes(Input, EncodedBytes, OutError))
				{
					if (OutError.empty()) OutError = "External texture source does not exist.";
					Rollback();
					return false;
				}
				const PathUtilities::FSourcePathResult Classified =
					PathUtilities::ClassifySourcePath(Input);
				if (Classified)
				{
					if (!ResolveMountedSourceReference(
						Request.AssetPath.ToString(),
						Classified.NormalizedVirtualPath,
						MountedSource,
						OutError))
					{
						Rollback();
						return false;
					}
				}
			}

			FImpl::FSourceOperation Source;
			Source.Bytes = std::move(EncodedBytes);
			if (!MountedSource.SourcePath.IsEmpty())
			{
				Source.SourcePath = MountedSource.SourcePath;
				Source.PhysicalPath = MountedSource.PhysicalPath;
			}
			else
			{
				if (Request.SourceDestination.IsEmpty())
				{
					OutError = "Embedded and external-unmounted images require an explicit mounted source destination.";
					Rollback();
					return false;
				}
				const PathUtilities::FSourcePathResult Destination =
					PathUtilities::ResolveSourcePath(
						Request.SourceDestination.Path,
						PathUtilities::EPathExistence::AllowMissing);
				if (!Destination)
				{
					OutError = Destination.Message;
					Rollback();
					return false;
				}
				const PathUtilities::FMountPolicyResult Mutation =
					PathUtilities::CheckSourceMutation(
						Request.AssetPath.ToString(), Destination.NormalizedVirtualPath);
				if (!Mutation)
				{
					OutError = Mutation.Message;
					Rollback();
					return false;
				}
				Source.SourcePath.Path = Destination.NormalizedVirtualPath;
				Source.PhysicalPath = Destination.PhysicalPath;
				Ec.clear();
				const bool bSourceExists = std::filesystem::exists(Source.PhysicalPath, Ec);
				if (Ec && !IsMissingPathError(Ec))
				{
					OutError = std::format(
						"Failed to inspect source destination {}: {}",
						Source.PhysicalPath.generic_string(), Ec.message());
					Rollback();
					return false;
				}
				Ec.clear();
				if (bSourceExists && std::filesystem::is_regular_file(Source.PhysicalPath, Ec))
				{
					std::vector<uint8> ExistingBytes;
					if (!LoadBytes(Source.PhysicalPath, ExistingBytes, OutError)
						|| ExistingBytes != Source.Bytes)
					{
						if (OutError.empty()) OutError = std::format(
							"A different source file already exists at {}.",
							Source.PhysicalPath.generic_string());
						Rollback();
						return false;
					}
				}
				else if (bSourceExists)
				{
					OutError = std::format(
						"Source destination {} is occupied.", Source.PhysicalPath.generic_string());
					Rollback();
					return false;
				}
				else
				{
					Source.bRequiresWrite = true;
					Source.StagedPath = Source.PhysicalPath;
					Source.StagedPath += ".import-stage";
					if (std::filesystem::exists(Source.StagedPath, Ec))
					{
						OutError = std::format(
							"Source staging path {} is occupied.", Source.StagedPath.generic_string());
						Rollback();
						return false;
					}
				}
			}

			const std::string SourceIdentity = Lower(Source.SourcePath.Path);
			size_t SourceIndex = 0;
			if (const auto It = SourceIdentities.find(SourceIdentity); It != SourceIdentities.end())
			{
				SourceIndex = It->second;
				if (Impl->Sources[SourceIndex].Bytes != Source.Bytes)
				{
					OutError = std::format(
						"Planned source {} has conflicting encoded bytes.", Source.SourcePath.Path);
					Rollback();
					return false;
				}
			}
			else
			{
				SourceIndex = Impl->Sources.size();
				SourceIdentities.emplace(SourceIdentity, SourceIndex);
				Impl->Sources.push_back(std::move(Source));
			}

			DTexture2D* Texture = nullptr;
			const Asset::FAssetResult CreateResult = Asset::CreateAsset(Request.AssetPath, Texture);
			if (!CreateResult)
			{
				OutError = CreateResult.Message;
				Rollback();
				return false;
			}
			if (Request.bRootPackage)
			{
				if (Impl->RootPackage && Impl->RootPackage != Texture->GetPackage())
				{
					OutError = "The multi-asset import transaction has more than one root package.";
					Rollback();
					return false;
				}
				Impl->RootPackage = Texture->GetPackage();
			}

			const FXxHash128 SourceHash = FXxHash128::HashBuffer(Impl->Sources[SourceIndex].Bytes);
			const bool bSRGB = Request.Settings.bSRGB.value_or(
				TextureBuild::GetDefaultSRGB(Request.Settings.Usage));
			const std::string DerivedDataKey = BuildTexture2DDerivedDataKey({
				.SourceContentHash = SourceHash,
				.Usage = Request.Settings.Usage,
				.bSRGB = bSRGB,
				.CompressionQuality = Request.Settings.CompressionQuality,
				.AlphaMipMode = Request.Settings.AlphaMipMode,
				.MaximumResolution = Request.Settings.MaxResolution,
				.AlphaCoverageThreshold = Request.Settings.AlphaCoverageThreshold,
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game});
			Asset::FDerivedDataObjectStore Store("Textures/Objects", MaximumTexturePayloadBytes);
			std::filesystem::path DerivedDataPath;
			if (!Store.GetObjectPath(DerivedDataKey, DerivedDataPath, &OutError))
			{
				Rollback();
				return false;
			}
			Ec.clear();
			const bool bDerivedDataExisted =
				std::filesystem::exists(DerivedDataPath, Ec)
				&& std::filesystem::is_regular_file(DerivedDataPath, Ec);
			Impl->PreparedTextures.push_back({
				.Texture = Texture,
				.DerivedDataPath = std::move(DerivedDataPath),
				.bDerivedDataExisted = bDerivedDataExisted});
			Impl->Textures.push_back(Texture);
			Impl->Packages.push_back(Texture->GetPackage());
			if (Impl->FailInjected(EImportTransactionFailurePoint::Decode, OutError)
				|| Impl->FailInjected(EImportTransactionFailurePoint::TextureBuild, OutError)
				|| Impl->FailInjected(EImportTransactionFailurePoint::DerivedDataPublication, OutError)
				|| !Texture->BuildFromEncodedBytes(
					Impl->Sources[SourceIndex].Bytes,
					Impl->Sources[SourceIndex].SourcePath,
					Request.Settings,
					OutError))
			{
				Rollback();
				return false;
			}
		}

		std::unordered_set<DPackage*> UniquePackages;
		std::erase_if(Impl->Packages, [&](DPackage* Package) {
			return !Package || !UniquePackages.insert(Package).second;
		});
		if (Impl->RootPackage
			&& !UniquePackages.contains(Impl->RootPackage))
		{
			OutError = "The selected root package is not part of the transaction.";
			Rollback();
			return false;
		}
		Impl->bPrepared = true;
		OutError.clear();
		return true;
	}

	auto FMultiAssetImportTransaction::Stage(std::string& OutError) -> bool
	{
		if (!Impl->bPrepared || Impl->bStaged)
		{
			OutError = "The multi-asset import transaction is not ready to stage.";
			return false;
		}
		for (FImpl::FSourceOperation& Source : Impl->Sources)
		{
			if (!Source.bRequiresWrite) continue;
			if (Impl->FailInjected(EImportTransactionFailurePoint::DirectoryCreation, OutError))
			{
				Rollback();
				return false;
			}
			std::error_code Ec;
			std::filesystem::create_directories(Source.PhysicalPath.parent_path(), Ec);
			if (Ec)
			{
				OutError = std::format(
					"Failed to create source directory {}: {}",
					Source.PhysicalPath.parent_path().generic_string(), Ec.message());
				Rollback();
				return false;
			}
			if (Impl->FailInjected(EImportTransactionFailurePoint::SourceWrite, OutError))
			{
				Rollback();
				return false;
			}
			FFileHelper::FAtomicFileError Error;
			if (!FFileHelper::SaveArrayToFileAtomically(
				std::span{reinterpret_cast<const std::byte*>(Source.Bytes.data()), Source.Bytes.size()},
				Source.StagedPath,
				&Error))
			{
				OutError = Error.ToString();
				Rollback();
				return false;
			}
		}
		Impl->bStaged = true;
		OutError.clear();
		return true;
	}

	auto FMultiAssetImportTransaction::Publish(std::string& OutError) -> bool
	{
		if (!Impl->bStaged || Impl->bPublished)
		{
			OutError = "The multi-asset import transaction is not ready to publish.";
			return false;
		}
		for (FImpl::FMutation& Mutation : Impl->Mutations)
		{
			if (!Mutation.Apply || !Mutation.Apply(OutError))
			{
				if (OutError.empty()) OutError = "A loaded-object mutation failed.";
				Rollback();
				return false;
			}
			Mutation.bApplied = true;
		}
		for (FImpl::FSourceOperation& Source : Impl->Sources)
		{
			if (!Source.bRequiresWrite) continue;
			std::error_code Ec;
			std::filesystem::rename(Source.StagedPath, Source.PhysicalPath, Ec);
			if (Ec)
			{
				OutError = std::format(
					"Failed to publish source {}: {}", Source.SourcePath.Path, Ec.message());
				Rollback();
				return false;
			}
			Source.bPublished = true;
		}

		const Asset::FAssetResult SaveResult = Asset::SavePackagesAtomically(
			Impl->Packages,
			{
				.RootPackage = Impl->RootPackage,
				.ShouldFail = [this](Asset::EAssetBundleSavePhase Phase, size_t) {
					switch (Phase)
					{
					case Asset::EAssetBundleSavePhase::CreateDirectories:
					case Asset::EAssetBundleSavePhase::StagePackage:
					case Asset::EAssetBundleSavePhase::PublishPackage:
						return Impl->ShouldFail(EImportTransactionFailurePoint::PackageSave);
					case Asset::EAssetBundleSavePhase::PublishRootPackage:
						return Impl->ShouldFail(EImportTransactionFailurePoint::RootPackageSave);
					case Asset::EAssetBundleSavePhase::PublishRegistry:
						return Impl->ShouldFail(EImportTransactionFailurePoint::RegistryPublication);
					default:
						return false;
					}
				}});
		if (!SaveResult)
		{
			OutError = SaveResult.Message;
			Rollback();
			return false;
		}
		Impl->bPublished = true;
		OutError.clear();
		return true;
	}

	auto FMultiAssetImportTransaction::Rollback() -> void
	{
		if (!Impl || Impl->bPublished) return;
		for (auto It = Impl->Mutations.rbegin(); It != Impl->Mutations.rend(); ++It)
		{
			if (It->bApplied && It->Rollback) It->Rollback();
			It->bApplied = false;
		}
		for (FImpl::FSourceOperation& Source : Impl->Sources)
		{
			std::error_code Ec;
			if (Source.bPublished) std::filesystem::remove(Source.PhysicalPath, Ec);
			std::filesystem::remove(Source.StagedPath, Ec);
			Source.bPublished = false;
		}
		for (FImpl::FPreparedTexture& Prepared : Impl->PreparedTextures)
		{
			if (!Prepared.bDerivedDataExisted)
			{
				std::error_code Ec;
				std::filesystem::remove(Prepared.DerivedDataPath, Ec);
			}
		}
		for (auto It = Impl->Textures.rbegin(); It != Impl->Textures.rend(); ++It)
		{
			DPackage* Package = (*It)->GetPackage();
			FAssetPath Path;
			if (Package && FAssetPath::TryCreate(Package->GetPackagePath(), Path))
			{
				const Asset::FAssetResult Result = Asset::DiscardUnpublishedPackage(Package);
				if (!Result) DURIN_ERROR(
					"Failed to roll back loaded package {}: {}", Path.ToString(), Result.Message);
			}
		}
		Impl->PreparedTextures.clear();
		Impl->Textures.clear();
		Impl->Sources.clear();
		Impl->bPrepared = false;
		Impl->bStaged = false;
	}

	auto FMultiAssetImportTransaction::Execute() -> FImportTransactionResult
	{
		std::string Error;
		if (!Prepare(Error) || !Stage(Error) || !Publish(Error))
			return {false, std::move(Error), {}};
		return {true, {}, Impl->Textures};
	}

	auto FMultiAssetImportTransaction::GetTextures() const -> std::span<DTexture2D* const>
	{
		return Impl->Textures;
	}
}
