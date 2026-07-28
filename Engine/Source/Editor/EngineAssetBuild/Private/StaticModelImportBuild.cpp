#include "StaticModelImportBuild.h"
#include "StaticModelImportBuildInternal.h"

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

		auto ImportFailurePointName(EImportTransactionFailurePoint Point) -> std::string_view
		{
			switch (Point)
			{
			case EImportTransactionFailurePoint::DirectoryCreation:
				return "source-directory-creation";
			case EImportTransactionFailurePoint::SourceWrite:
				return "source-staging";
			case EImportTransactionFailurePoint::SourcePublication:
				return "source-publication";
			case EImportTransactionFailurePoint::Decode:
				return "texture-decode";
			case EImportTransactionFailurePoint::TextureBuild:
				return "texture-candidate-build";
			case EImportTransactionFailurePoint::DerivedDataPublication:
				return "texture-derived-data-publication";
			case EImportTransactionFailurePoint::PackageStaging:
				return "package-staging";
			case EImportTransactionFailurePoint::DependencyPackagePublication:
				return "dependency-package-publication";
			case EImportTransactionFailurePoint::RegistryPublication:
				return "registry-publication";
			case EImportTransactionFailurePoint::RootPackagePublication:
				return "root-package-publication";
			default:
				return "none";
			}
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
		struct FResolvedPackage
		{
			DPackage* Package = nullptr;
			FAssetPath AssetPath;
			std::filesystem::path PhysicalPath;
		};

		struct FResolvedSource
		{
			FSourcePath SourcePath;
			std::filesystem::path PhysicalPath;
			std::filesystem::path StagedPath;
			std::filesystem::path ExternalInputPath;
			std::vector<uint8> Bytes;
			FXxHash128 ContentHash;
			std::filesystem::file_time_type ObservedLastWriteTime{};
			uintmax_t ObservedFileSize = 0;
			bool bRequiresWrite = false;
		};

		struct FResolvedTexture
		{
			FAssetPath AssetPath;
			std::filesystem::path PackagePath;
			FTexture2DImportSettings Settings;
			size_t SourceIndex = 0;
		};

		struct FResolvedMutation
		{
			std::function<bool(std::string&)> Apply;
			std::function<void()> Rollback;
		};

		struct FResolvedRoot
		{
			DPackage* ExistingPackage = nullptr;
			std::optional<size_t> TextureIndex;
		};

		// Owns every path, byte payload, and policy decision needed after preflight.
		struct FResolvedImportPlan
		{
			std::vector<FResolvedPackage> Packages;
			std::vector<FResolvedSource> Sources;
			std::vector<FResolvedTexture> Textures;
			std::vector<FResolvedMutation> Mutations;
			FResolvedRoot Root;
		};

		struct FPreparedTexture
		{
			DTexture2D* Texture = nullptr;
			std::filesystem::path DerivedDataPath;
			bool bDerivedDataExisted = false;
		};

		std::vector<FPortableTextureBuildRequest> Requests;
		std::vector<DPackage*> RequestedPackages;
		std::vector<FResolvedMutation> RequestedMutations;
		DPackage* RequestedRootPackage = nullptr;
		std::vector<FPreparedTexture> PreparedTextures;
		FResolvedImportPlan Plan;
		std::vector<DPackage*> PreparedPackages;
		std::vector<bool> PublishedSources;
		std::vector<bool> AppliedMutations;
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

		auto FailInjected(
			EImportTransactionFailurePoint Point,
			std::string_view Identity,
			std::string& OutError) -> bool
		{
			if (!ShouldFail(Point)) return false;
			OutError = std::format(
				"Injected multi-asset import failure at phase '{}' for '{}'.",
				ImportFailurePointName(Point),
				Identity);
			return true;
		}

		auto ResolvePlan(std::string& OutError) -> bool;
		auto BuildCandidates(std::string& OutError) -> bool;
		auto ValidatePreparedPlan(std::string& OutError) -> bool;
		auto VerifyExternalInputs(std::string& OutError) const -> bool;
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
		if (Package) Impl->RequestedPackages.push_back(Package);
		if (bRootPackage) Impl->RequestedRootPackage = Package;
	}

	auto FMultiAssetImportTransaction::AddLoadedObjectMutation(
		std::function<bool(std::string&)> Apply,
		std::function<void()> Rollback) -> void
	{
		check(!Impl->bAttempted);
		Impl->RequestedMutations.push_back({std::move(Apply), std::move(Rollback)});
	}

	auto FMultiAssetImportTransactionTestAccess::SetFailurePoint(
		FMultiAssetImportTransaction& Transaction,
		EImportTransactionFailurePoint Point,
		size_t Occurrence) -> void
	{
		check(!Transaction.Impl->bAttempted);
		Transaction.Impl->FailurePoint = Point;
		Transaction.Impl->FailureOccurrence = Occurrence;
	}

	auto FMultiAssetImportTransaction::FImpl::ResolvePlan(std::string& OutError) -> bool
	{
		Plan = {};
		std::unordered_set<std::string> PackageIdentities;
		for (DPackage* Package : RequestedPackages)
		{
			FAssetPath Path;
			if (!Package || !Package->IsAssetPackage()
				|| !FAssetPath::TryCreate(Package->GetPackagePath(), Path)
				|| !PackageIdentities.insert(Lower(Path.ToString())).second)
			{
				OutError = "The transaction contains an invalid or duplicate existing package.";
				return false;
			}
			const PathUtilities::FContentPathResult Destination =
				PathUtilities::ResolveContentPath(
					Path.GetView(), PathUtilities::EPathExistence::AllowMissing);
			if (!Destination)
			{
				OutError = Destination.Message;
				return false;
			}
			std::filesystem::path PhysicalPath = Destination.PhysicalPath;
			PhysicalPath += ".dasset";
			Plan.Packages.push_back({
				.Package = Package,
				.AssetPath = std::move(Path),
				.PhysicalPath = std::move(PhysicalPath)});
		}

		std::unordered_map<std::string, size_t> SourceIdentities;
		bool bHasRoot = RequestedRootPackage != nullptr;
		Plan.Root.ExistingPackage = RequestedRootPackage;
		Plan.Mutations = RequestedMutations;
		for (const FPortableTextureBuildRequest& Request : Requests)
		{
			if (!Request.AssetPath.IsValid()
				|| Request.ExternalSource.empty() == Request.EncodedBytes.empty())
			{
				OutError = "Each texture request requires a valid asset path and exactly one source payload.";
				return false;
			}
			if (!PackageIdentities.insert(Lower(Request.AssetPath.ToString())).second
				|| Asset::FindLoadedPackage(Request.AssetPath)
				|| Asset::GetAssetRegistry().FindAsset(Request.AssetPath))
			{
				OutError = std::format(
					"Texture package {} collides with an existing or planned asset.",
					Request.AssetPath.ToString());
				return false;
			}
			const PathUtilities::FContentPathResult PackageDestination =
				PathUtilities::ResolveContentPath(
					Request.AssetPath.GetView(), PathUtilities::EPathExistence::AllowMissing);
			if (!PackageDestination)
			{
				OutError = PackageDestination.Message;
				return false;
			}
			std::filesystem::path PackageFile = PackageDestination.PhysicalPath;
			PackageFile += ".dasset";
			std::error_code Ec;
			if (std::filesystem::exists(PackageFile, Ec))
			{
				OutError = std::format(
					"Texture package destination {} already exists.", PackageFile.generic_string());
				return false;
			}
			if (Ec && !IsMissingPathError(Ec))
			{
				OutError = std::format(
					"Failed to inspect package destination {}: {}",
					PackageFile.generic_string(), Ec.message());
				return false;
			}
			if (Request.bRootPackage)
			{
				if (bHasRoot)
				{
					OutError = "The multi-asset import transaction has more than one root package.";
					return false;
				}
				bHasRoot = true;
			}

			FResolvedSource Source;
			Source.Bytes = Request.EncodedBytes;
			if (!Request.ExternalSource.empty())
			{
				Ec.clear();
				const std::filesystem::path Input =
					std::filesystem::absolute(Request.ExternalSource, Ec).lexically_normal();
				if (Ec || !std::filesystem::is_regular_file(Input, Ec))
				{
					OutError = "External texture source does not exist.";
					return false;
				}
				Ec.clear();
				const uintmax_t FileSizeBefore = std::filesystem::file_size(Input, Ec);
				const auto LastWriteTimeBefore = !Ec
					? std::filesystem::last_write_time(Input, Ec)
					: std::filesystem::file_time_type{};
				if (Ec)
				{
					OutError = std::format(
						"Failed to observe external texture source {}: {}",
						Input.generic_string(), Ec.message());
					return false;
				}
				if (!LoadBytes(Input, Source.Bytes, OutError)) return false;
				Ec.clear();
				Source.ObservedFileSize = std::filesystem::file_size(Input, Ec);
				if (!Ec) Source.ObservedLastWriteTime = std::filesystem::last_write_time(Input, Ec);
				if (Ec || Source.ObservedFileSize != FileSizeBefore
					|| Source.ObservedLastWriteTime != LastWriteTimeBefore)
				{
					OutError = std::format(
						"External texture source {} changed during transaction resolution.",
						Input.generic_string());
					return false;
				}
				Source.ExternalInputPath = Input;
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
						return false;
					}
					Source.SourcePath = Mounted.SourcePath;
					Source.PhysicalPath = Mounted.PhysicalPath;
				}
			}

			if (Source.PhysicalPath.empty())
			{
				if (Request.SourceDestination.IsEmpty())
				{
					OutError = "Embedded and external-unmounted images require an explicit mounted source destination.";
					return false;
				}
				const PathUtilities::FSourcePathResult Destination =
					PathUtilities::ResolveSourcePath(
						Request.SourceDestination.Path,
						PathUtilities::EPathExistence::AllowMissing);
				if (!Destination)
				{
					OutError = Destination.Message;
					return false;
				}
				const PathUtilities::FMountPolicyResult Mutation =
					PathUtilities::CheckSourceMutation(
						Request.AssetPath.ToString(), Destination.NormalizedVirtualPath);
				if (!Mutation)
				{
					OutError = Mutation.Message;
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
						return false;
					}
				}
				else if (bSourceExists)
				{
					OutError = std::format(
						"Source destination {} is occupied.", Source.PhysicalPath.generic_string());
					return false;
				}
				else
				{
					Source.bRequiresWrite = true;
					Source.StagedPath = Source.PhysicalPath;
					Source.StagedPath += ".import-stage";
					Ec.clear();
					if (std::filesystem::exists(Source.StagedPath, Ec))
					{
						OutError = std::format(
							"Source staging path {} is occupied.", Source.StagedPath.generic_string());
						return false;
					}
					if (Ec && !IsMissingPathError(Ec))
					{
						OutError = std::format(
							"Failed to inspect source staging path {}: {}",
							Source.StagedPath.generic_string(), Ec.message());
						return false;
					}
				}
			}

			const std::string SourceIdentity = Lower(Source.SourcePath.Path);
			size_t SourceIndex = 0;
			if (const auto It = SourceIdentities.find(SourceIdentity);
				It != SourceIdentities.end())
			{
				SourceIndex = It->second;
				const FResolvedSource& Existing = Plan.Sources[SourceIndex];
				if (Existing.Bytes != Source.Bytes || Existing.PhysicalPath != Source.PhysicalPath)
				{
					OutError = std::format(
						"Planned source {} has conflicting encoded bytes or destinations.",
						Source.SourcePath.Path);
					return false;
				}
			}
			else
			{
				SourceIndex = Plan.Sources.size();
				SourceIdentities.emplace(SourceIdentity, SourceIndex);
				Plan.Sources.push_back(std::move(Source));
			}
			const size_t TextureIndex = Plan.Textures.size();
			Plan.Textures.push_back({
				.AssetPath = Request.AssetPath,
				.PackagePath = std::move(PackageFile),
				.Settings = Request.Settings,
				.SourceIndex = SourceIndex});
			if (Request.bRootPackage) Plan.Root.TextureIndex = TextureIndex;
		}
		for (FResolvedSource& Source : Plan.Sources)
			Source.ContentHash = FXxHash128::HashBuffer(Source.Bytes);
		return true;
	}

	auto FMultiAssetImportTransaction::FImpl::VerifyExternalInputs(
		std::string& OutError) const -> bool
	{
		for (const FResolvedSource& Source : Plan.Sources)
		{
			if (Source.ExternalInputPath.empty()) continue;
			std::error_code Ec;
			if (!std::filesystem::is_regular_file(Source.ExternalInputPath, Ec))
			{
				OutError = std::format(
					"External texture source {} changed after transaction resolution.",
					Source.ExternalInputPath.generic_string());
				return false;
			}
			const uintmax_t FileSize = std::filesystem::file_size(Source.ExternalInputPath, Ec);
			const auto LastWriteTime = !Ec
				? std::filesystem::last_write_time(Source.ExternalInputPath, Ec)
				: std::filesystem::file_time_type{};
			if (Ec || FileSize != Source.ObservedFileSize
				|| LastWriteTime != Source.ObservedLastWriteTime)
			{
				OutError = std::format(
					"External texture source {} changed after transaction resolution.",
					Source.ExternalInputPath.generic_string());
				return false;
			}
		}
		return true;
	}

	auto FMultiAssetImportTransaction::FImpl::BuildCandidates(std::string& OutError) -> bool
	{
		if (!VerifyExternalInputs(OutError)) return false;
		PreparedPackages.clear();
		PreparedPackages.reserve(Plan.Packages.size() + Plan.Textures.size());
		for (const FResolvedPackage& Package : Plan.Packages)
			PreparedPackages.push_back(Package.Package);
		RootPackage = Plan.Root.ExistingPackage;
		for (size_t TextureIndex = 0; TextureIndex < Plan.Textures.size(); ++TextureIndex)
		{
			const FResolvedTexture& Resolved = Plan.Textures[TextureIndex];
			DTexture2D* Texture = nullptr;
			const Asset::FAssetResult CreateResult = Asset::CreateAsset(Resolved.AssetPath, Texture);
			if (!CreateResult)
			{
				OutError = CreateResult.Message;
				return false;
			}
			if (Plan.Root.TextureIndex == TextureIndex) RootPackage = Texture->GetPackage();

			const FResolvedSource& Source = Plan.Sources[Resolved.SourceIndex];
			const bool bSRGB = Resolved.Settings.bSRGB.value_or(
				TextureBuild::GetDefaultSRGB(Resolved.Settings.Usage));
			const std::string DerivedDataKey = BuildTexture2DDerivedDataKey({
				.SourceContentHash = Source.ContentHash,
				.Usage = Resolved.Settings.Usage,
				.bSRGB = bSRGB,
				.CompressionQuality = Resolved.Settings.CompressionQuality,
				.AlphaMipMode = Resolved.Settings.AlphaMipMode,
				.MaximumResolution = Resolved.Settings.MaxResolution,
				.AlphaCoverageThreshold = Resolved.Settings.AlphaCoverageThreshold,
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game});
			Asset::FDerivedDataObjectStore Store("Textures/Objects", MaximumTexturePayloadBytes);
			std::filesystem::path DerivedDataPath;
			if (!Store.GetObjectPath(DerivedDataKey, DerivedDataPath, &OutError)) return false;
			std::error_code Ec;
			const bool bDerivedDataExisted =
				std::filesystem::exists(DerivedDataPath, Ec)
				&& std::filesystem::is_regular_file(DerivedDataPath, Ec);
			PreparedTextures.push_back({
				.Texture = Texture,
				.DerivedDataPath = std::move(DerivedDataPath),
				.bDerivedDataExisted = bDerivedDataExisted});
			Textures.push_back(Texture);
			PreparedPackages.push_back(Texture->GetPackage());
			if (!FTextureBuildOperations::Build(
				*Texture,
				Source.Bytes,
				Source.SourcePath,
				Resolved.Settings,
				[this, &Resolved, &Source](
					EImportTransactionFailurePoint Point,
					std::string& Error) {
					return FailInjected(
						Point,
						std::format(
							"{} ({})", Resolved.AssetPath.ToString(), Source.SourcePath.Path),
						Error);
				},
				OutError))
			{
				return false;
			}
		}

		return true;
	}

	auto FMultiAssetImportTransaction::FImpl::ValidatePreparedPlan(
		std::string& OutError) -> bool
	{
		std::unordered_set<DPackage*> UniquePackages;
		std::erase_if(PreparedPackages, [&](DPackage* Package) {
			return !Package || !UniquePackages.insert(Package).second;
		});
		if (RootPackage && !UniquePackages.contains(RootPackage))
		{
			OutError = "The selected root package is not part of the transaction.";
			return false;
		}
		return true;
	}

	auto FMultiAssetImportTransaction::Prepare(std::string& OutError) -> bool
	{
		if (Impl->bAttempted)
		{
			OutError = "The multi-asset import transaction has already been attempted.";
			return false;
		}
		Impl->bAttempted = true;
		if (Impl->Requests.empty() && Impl->RequestedPackages.empty())
		{
			OutError = "The multi-asset import transaction has no outputs.";
			return false;
		}
		if (!Impl->ResolvePlan(OutError))
		{
			Rollback();
			return false;
		}
		Impl->PublishedSources.assign(Impl->Plan.Sources.size(), false);
		Impl->AppliedMutations.assign(Impl->Plan.Mutations.size(), false);
		if (!Impl->BuildCandidates(OutError) || !Impl->ValidatePreparedPlan(OutError))
		{
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
		if (!Impl->VerifyExternalInputs(OutError))
		{
			Rollback();
			return false;
		}
		for (const FImpl::FResolvedSource& Source : Impl->Plan.Sources)
		{
			if (!Source.bRequiresWrite) continue;
			if (Impl->FailInjected(
				EImportTransactionFailurePoint::DirectoryCreation,
				Source.SourcePath.Path,
				OutError))
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
			if (Impl->FailInjected(
				EImportTransactionFailurePoint::SourceWrite,
				Source.SourcePath.Path,
				OutError))
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
		if (!Impl->VerifyExternalInputs(OutError))
		{
			Rollback();
			return false;
		}
		for (size_t MutationIndex = 0; MutationIndex < Impl->Plan.Mutations.size(); ++MutationIndex)
		{
			const FImpl::FResolvedMutation& Mutation = Impl->Plan.Mutations[MutationIndex];
			if (!Mutation.Apply || !Mutation.Apply(OutError))
			{
				if (OutError.empty()) OutError = "A loaded-object mutation failed.";
				Rollback();
				return false;
			}
			Impl->AppliedMutations[MutationIndex] = true;
		}
		for (size_t SourceIndex = 0; SourceIndex < Impl->Plan.Sources.size(); ++SourceIndex)
		{
			const FImpl::FResolvedSource& Source = Impl->Plan.Sources[SourceIndex];
			if (!Source.bRequiresWrite) continue;
			if (Impl->FailInjected(
				EImportTransactionFailurePoint::SourcePublication,
				Source.SourcePath.Path,
				OutError))
			{
				Rollback();
				return false;
			}
			std::error_code Ec;
			std::filesystem::rename(Source.StagedPath, Source.PhysicalPath, Ec);
			if (Ec)
			{
				OutError = std::format(
					"Failed to publish source {}: {}", Source.SourcePath.Path, Ec.message());
				Rollback();
				return false;
			}
			Impl->PublishedSources[SourceIndex] = true;
		}

		const Asset::FAssetResult SaveResult = Asset::SavePackagesAtomically(
			Impl->PreparedPackages,
			{
				.RootPackage = Impl->RootPackage,
				.ShouldFail = [this](Asset::EAssetBundleSavePhase Phase, size_t) {
					switch (Phase)
					{
					case Asset::EAssetBundleSavePhase::CreateDirectories:
					case Asset::EAssetBundleSavePhase::StagePackage:
						return Impl->ShouldFail(EImportTransactionFailurePoint::PackageStaging);
					case Asset::EAssetBundleSavePhase::PublishPackage:
						return Impl->ShouldFail(
							EImportTransactionFailurePoint::DependencyPackagePublication);
					case Asset::EAssetBundleSavePhase::PublishRootPackage:
						return Impl->ShouldFail(
							EImportTransactionFailurePoint::RootPackagePublication);
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
		for (size_t Index = Impl->Plan.Mutations.size(); Index > 0; --Index)
		{
			const size_t MutationIndex = Index - 1;
			const FImpl::FResolvedMutation& Mutation = Impl->Plan.Mutations[MutationIndex];
			if (MutationIndex < Impl->AppliedMutations.size()
				&& Impl->AppliedMutations[MutationIndex])
			{
				if (Mutation.Rollback) Mutation.Rollback();
				Impl->AppliedMutations[MutationIndex] = false;
			}
		}
		for (size_t SourceIndex = 0; SourceIndex < Impl->Plan.Sources.size(); ++SourceIndex)
		{
			const FImpl::FResolvedSource& Source = Impl->Plan.Sources[SourceIndex];
			std::error_code Ec;
			if (SourceIndex < Impl->PublishedSources.size()
				&& Impl->PublishedSources[SourceIndex])
			{
				std::filesystem::remove(Source.PhysicalPath, Ec);
				Impl->PublishedSources[SourceIndex] = false;
			}
			std::filesystem::remove(Source.StagedPath, Ec);
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
		Impl->PreparedPackages.clear();
		Impl->PublishedSources.clear();
		Impl->AppliedMutations.clear();
		Impl->Textures.clear();
		Impl->Plan = {};
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
