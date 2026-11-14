#include "Asset/PackageSerialization.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "Asset/RegistryOperations.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetPackageCodec.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"

namespace Durin
{
	namespace
	{
		auto Error(Durin::UnifiedError::FErrorId Id, std::string Message) -> std::expected<void, Durin::UnifiedError::FError> { return std::unexpected(Durin::UnifiedError::FError{Id, std::move(Message)}); }
		auto Error(EAssetWriteError Code, std::string Message) -> FAssetWriteResult { return {Code, std::move(Message)}; }
		auto AssetPathResolutionError(
			const FPackagePathResolveResult& Resolution) -> std::expected<void, Durin::UnifiedError::FError>
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::Resolved:
				return {};
			case EAssetPathResolveState::ProjectionPending:
				return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::ProjectionPending, std::format("Registry projection for package {} is pending synchronization.",
						Resolution.FinalPath.ToString())});
			case EAssetPathResolveState::NotFound:
				return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::NotFound, std::format(
					"Asset {} is not present in the registry.",
					Resolution.RequestedPath.ToString())});
			case EAssetPathResolveState::MissingRedirectTarget:
				return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::NotFound, std::format(
					"Asset redirect {} has a missing target {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())});
			case EAssetPathResolveState::RedirectCycle:
				return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::CircularDependency, std::format(
					"Asset redirect {} contains a cycle at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())});
			case EAssetPathResolveState::RedirectDepthExceeded:
				return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::CircularDependency, std::format(
					"Asset redirect {} exceeds the maximum redirect depth at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())});
			case EAssetPathResolveState::UnknownTargetClass:
				return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::UnknownClass, std::format(
					"Asset {} resolves to a target with an unavailable reflected class.",
					Resolution.RequestedPath.ToString())});
			case EAssetPathResolveState::RedirectTypeMismatch:
				return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::TypeMismatch, std::format(
					"Asset {} resolves to a target with an incompatible class.",
					Resolution.RequestedPath.ToString())});
			case EAssetPathResolveState::CorruptRedirector:
				return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, std::format(
					"CorruptRedirector: asset {} traverses invalid redirect metadata at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())});
			}
			return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Asset resolution returned an unknown state."});
		}
	}

	auto CanonicalizeAssetPackageForCook(
		FByteView Bytes,
		FByteView BulkBytes,
		const FPackagePath& PackagePath,
		FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes) -> FAssetWriteResult
	{
		return CanonicalizeAssetPackageForCook(
			Bytes, BulkBytes, PackagePath, PackagePath, OutBytes, OutBulkBytes);
	}

	auto CanonicalizeAssetPackageForCook(
		FByteView Bytes,
		FByteView BulkBytes,
		const FPackagePath& SourcePackagePath,
		const FPackagePath& OutputPackagePath,
		FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes) -> FAssetWriteResult
	{
		OutBytes.clear();
		OutBulkBytes.clear();
		const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
		if (auto Result = AssetPrivate::ResolveAssetPackageReader(Bytes, Codec); !Result)
			return AssetWriteResultFromRead(Result);
		if (!Codec->bCanMutate)
			return Error(EAssetWriteError::UnsupportedVersion,
				"Cook canonicalization requires package mutation capability.");
		FByteBuffer RelocatedBytes;
		FByteBuffer RelocatedBulkBytes;
		if (SourcePackagePath != OutputPackagePath)
		{
			const AssetPrivate::FAssetPackageReadContext SourceContext{
				.PackageBytes = Bytes, .BulkBytes = BulkBytes,
				.PackagePath = SourcePackagePath,
				.PhysicalPackageBytes = Bytes.size()};
			AssetPrivate::FAssetPackageEncodedClosure Relocated;
			auto RelocateResult = AssetWriteResultFromEncoding(Codec->Relocate(
				SourceContext, OutputPackagePath, Relocated));
			if (!RelocateResult) return RelocateResult;
			RelocatedBytes = std::move(Relocated.PackageBytes);
			RelocatedBulkBytes = std::move(Relocated.BulkBytes);
			Bytes = RelocatedBytes;
			BulkBytes = RelocatedBulkBytes;
		}
		const AssetPrivate::FAssetPackageReadContext Context{
			.PackageBytes = Bytes, .BulkBytes = BulkBytes,
			.PackagePath = OutputPackagePath, .PhysicalPackageBytes = Bytes.size()};
		FAssetPackageHeader Header;
		auto Result = Codec->ReadHeader(Context, Header);
		if (!Result) return AssetWriteResultFromRead(Result);
		if ((!Header.TopLevelAssets.empty() && std::ranges::all_of(Header.TopLevelAssets, &FAssetData::IsRedirector)))
			return Error(EAssetWriteError::InvalidData,
				"CookCanonicalizationRedirectorPackage: redirector packages are uncooked-only.");
		std::vector<FAssetReferenceEdge> References;
		Result = Codec->ExtractReferences(Context, References);
		if (!Result) return AssetWriteResultFromRead(Result);

		std::vector<FAssetPackageReferenceMapping> Mappings;
		auto ResolveReference = [&](const FPackagePath& Path,
			std::string_view ExpectedClassName, std::string_view Route) -> std::expected<void, Durin::UnifiedError::FError> {
			DClass* ExpectedClass = nullptr;
			if (!ExpectedClassName.empty())
			{
				ExpectedClass = FindClassByQualifiedName(FName(ExpectedClassName));
				if (!ExpectedClass)
					return Error(Durin::AssetLoadErrors::UnknownClass, std::format(
						"CookCanonicalizationUnknownExpectedClass: {} expects unavailable class {}.",
						Route, ExpectedClassName));
			}
			const FPackagePathResolveResult Resolution = Durin::ResolvePackagePathForOperation(
				Path, {.ExpectedClass = ExpectedClass});
			if (!Resolution)
			{
				auto ResolutionError = AssetPathResolutionError(Resolution);
				ResolutionError.error().PushContext("operation", std::format("CookCanonicalizationUnresolvedReference: {} at {}", Path.ToString(), Route));
				return ResolutionError;
			}
			if (!Resolution.FinalPackageData
				|| (!Resolution.FinalPackageData->TopLevelAssets.empty() && std::ranges::all_of(Resolution.FinalPackageData->TopLevelAssets, &FAssetData::IsRedirector)))
				return Error(Durin::AssetLoadErrors::InvalidPackageType, "Cook canonicalization resolved a reference to a non-asset package.");
			if (Resolution.FinalPath == Path) return {};
			const auto Existing = std::ranges::find(
				Mappings, Path, &FAssetPackageReferenceMapping::SourcePath);
			if (Existing == Mappings.end())
				Mappings.push_back({.SourcePath = Path, .DestinationPath = Resolution.FinalPath});
			else if (Existing->DestinationPath != Resolution.FinalPath)
				return Error(Durin::AssetLoadErrors::StaleData, "Cook canonicalization observed inconsistent redirect resolution.");
			return {};
		};
		for (const FPackagePath& Dependency : Header.Dependencies)
		{
			Result = ResolveReference(Dependency, {}, "package dependency table");
			if (!Result) return AssetWriteResultFromRead(Result);
		}
		for (const FAssetReferenceEdge& Reference : References)
		{
			Result = ResolveReference(Reference.TargetPath.GetPackagePath(), Reference.ExpectedClass,
				Reference.DisplayRoute);
			if (!Result) return AssetWriteResultFromRead(Result);
		}
		if (Mappings.empty())
		{
			OutBytes.assign(Bytes.begin(), Bytes.end());
			OutBulkBytes.assign(BulkBytes.begin(), BulkBytes.end());
			return {};
		}
		auto Rewritten = AssetPrivate::RewritePackageReferencesForMutation(
			Bytes, BulkBytes, OutputPackagePath, Mappings,
			std::numeric_limits<uint64>::max(), OutBytes);
		if (Rewritten) OutBulkBytes.assign(BulkBytes.begin(), BulkBytes.end());
		return Rewritten;

	}

}
