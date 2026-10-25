#include "Asset/PackageSerialization.h"
#include "Asset/RegistryOperations.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetPackageCodec.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"

namespace Durin
{
	namespace
	{
		auto Error(EAssetReadError Code, std::string Message) -> FAssetReadResult { return {Code, std::move(Message)}; }
		auto Error(EAssetWriteError Code, std::string Message) -> FAssetWriteResult { return {Code, std::move(Message)}; }
		auto AssetPathResolutionError(
			const FAssetPathResolveResult& Resolution) -> FAssetReadResult
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::Resolved:
				return {};
			case EAssetPathResolveState::ProjectionPending:
				return {EAssetReadError::ProjectionPending,
					std::format("Registry projection for package {} is pending synchronization.",
						Resolution.FinalPath.ToString())};
			case EAssetPathResolveState::NotFound:
				return {EAssetReadError::NotFound, std::format(
					"Asset {} is not present in the registry.",
					Resolution.RequestedPath.ToString())};
			case EAssetPathResolveState::MissingRedirectTarget:
				return {EAssetReadError::NotFound, std::format(
					"Asset redirect {} has a missing target {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())};
			case EAssetPathResolveState::RedirectCycle:
				return {EAssetReadError::CircularDependency, std::format(
					"Asset redirect {} contains a cycle at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())};
			case EAssetPathResolveState::RedirectDepthExceeded:
				return {EAssetReadError::CircularDependency, std::format(
					"Asset redirect {} exceeds the maximum redirect depth at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())};
			case EAssetPathResolveState::UnknownTargetClass:
				return {EAssetReadError::UnknownClass, std::format(
					"Asset {} resolves to a target with an unavailable reflected class.",
					Resolution.RequestedPath.ToString())};
			case EAssetPathResolveState::RedirectTypeMismatch:
				return {EAssetReadError::TypeMismatch, std::format(
					"Asset {} resolves to a target with an incompatible class.",
					Resolution.RequestedPath.ToString())};
			case EAssetPathResolveState::CorruptRedirector:
				return {EAssetReadError::CorruptFile, std::format(
					"CorruptRedirector: asset {} traverses invalid redirect metadata at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())};
			}
			return {EAssetReadError::CorruptFile,
				"Asset resolution returned an unknown state."};
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
		if (Header.EntryKind != EAssetRegistryEntryKind::Asset)
			return Error(EAssetWriteError::InvalidData,
				"CookCanonicalizationRedirectorPackage: redirector packages are uncooked-only.");
		std::vector<FAssetReferenceEdge> References;
		Result = Codec->ExtractReferences(Context, References);
		if (!Result) return AssetWriteResultFromRead(Result);

		std::vector<FAssetPackageReferenceMapping> Mappings;
		auto ResolveReference = [&](const FPackagePath& Path,
			std::string_view ExpectedClassName, std::string_view Route) -> FAssetReadResult {
			DClass* ExpectedClass = nullptr;
			if (!ExpectedClassName.empty())
			{
				ExpectedClass = FindClassByQualifiedName(FName(ExpectedClassName));
				if (!ExpectedClass)
					return Error(EAssetReadError::UnknownClass, std::format(
						"CookCanonicalizationUnknownExpectedClass: {} expects unavailable class {}.",
						Route, ExpectedClassName));
			}
			const FAssetPathResolveResult Resolution = Durin::ResolveAssetPathForOperation(
				Path, {.ExpectedClass = ExpectedClass});
			if (!Resolution)
			{
				auto ResolutionError = AssetPathResolutionError(Resolution);
				ResolutionError.Message = std::format(
					"CookCanonicalizationUnresolvedReference: {} at {}. {}",
					Path.ToString(), Route, ResolutionError.Message);
				return ResolutionError;
			}
			if (!Resolution.FinalAssetData
				|| Resolution.FinalAssetData->EntryKind != EAssetRegistryEntryKind::Asset)
				return Error(EAssetReadError::InvalidPackageType,
					"Cook canonicalization resolved a reference to a non-asset package.");
			if (Resolution.FinalPath == Path) return {};
			const auto Existing = std::ranges::find(
				Mappings, Path, &FAssetPackageReferenceMapping::SourcePath);
			if (Existing == Mappings.end())
				Mappings.push_back({.SourcePath = Path, .DestinationPath = Resolution.FinalPath});
			else if (Existing->DestinationPath != Resolution.FinalPath)
				return Error(EAssetReadError::StaleData,
					"Cook canonicalization observed inconsistent redirect resolution.");
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
