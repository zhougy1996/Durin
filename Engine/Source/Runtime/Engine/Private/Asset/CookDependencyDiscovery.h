#pragma once

#include "Asset/Cook.h"
#include "Asset/MutationExtensions.h"
#include "Asset/PackageInspection.h"
#include "Asset/PackageSchema.h"
#include "AssetRegistry/References.h"
#include "AssetPackageCodec.h"
#include "Asset/Load.h"
#include "Misc/MountPaths.h"

namespace Durin::AssetPrivate
{

	// Evaluates bounded dependency identities from the workflow-stable source tree.
	// Owns metadata and small values; ordinary loading owns objects and lazy resources.
	class FCookDependencyDiscovery
	{
	public:
		using FResolveContributor = std::function<FAssetResult(const FAssetData&, FCookContributorRegistration&)>;
		FCookDependencyDiscovery(const FCookRequest& Request, FAssetRegistrySnapshot Registry,
			FResolveContributor ResolveContributor);
		FCookDependencyDiscovery(const FCookDependencyDiscovery&) = delete;
		auto operator=(const FCookDependencyDiscovery&) -> FCookDependencyDiscovery& = delete;
		auto Acquire(std::span<const FPackagePath> Roots, const FAssetReferenceStoreCapture& ExternalRoots) -> FAssetResult;
		auto CheckCancellation() -> FAssetResult;
		auto ReadInput(const FPackagePath& Path, ECookBuildDependencyKind Kind, std::string_view Name, FByteBuffer& Out) -> FAssetResult;
		auto GetPackages() const -> const std::vector<FPackagePath>& { return RuntimePackages; }
		auto GetRegistry() const -> const FAssetRegistrySnapshot& { return Registry; }
		auto GetContributor(const FPackagePath& Path) const -> const FCookContributorRegistration& { return Inputs.at(Path).Contributor; }
		auto GetDependencies(const FPackagePath& Path) const -> const std::vector<FCookBuildDependency>& { return Inputs.at(Path).Dependencies; }
		auto GetRetainedBytes() const -> uint64 { return RetainedBytes; }
		auto GetStatus() const -> ECookInputStatus { return Status; }
		auto IsReusable(const FPackagePath& Path) const -> bool;
	private:
		struct FInput
		{
			FByteBuffer PackageIdentity;
			FByteBuffer BulkIdentity;
			std::map<std::string, std::filesystem::path> DeclaredFiles;
			FAssetPackageInspection Inspection;
			std::vector<FAssetReferenceEdge> References;
			FCookContributorRegistration Contributor;
			std::vector<FCookBuildDependency> Dependencies;
			std::map<std::pair<ECookBuildDependencyKind, std::string>, FByteBuffer> DeclaredValues;
			bool bDeclared = false;
		};
		auto AcquirePackage(const FPackagePath& Path) -> FAssetResult;
		auto ReadFile(const std::filesystem::path& File, FByteBuffer& Out) -> FAssetResult;
		auto Resolve(const FPackagePath& Requested, FPackagePath& Final) -> FAssetResult;
		auto CaptureSchema(FInput& Input, FCookPackageBuildInputs& Node) -> FAssetResult;
		auto Fail(EAssetError Error, std::string Message, ECookInputStatus Status = ECookInputStatus::InvalidDependency) -> FAssetResult;
		auto Fail(const FAssetResult& Result) -> FAssetResult;
		const FCookRequest& Request;
		FAssetRegistrySnapshot Registry;
		FResolveContributor ResolveContributor;
		FReflectionSchemaCatalog Schema;
		std::string ShaderBuildIdentity;
		std::unordered_map<std::string, FByteBuffer> SchemaValues;
		FAssetResult Failure;
		ECookInputStatus Status = ECookInputStatus::None;
		std::unordered_map<FPackagePath, FInput> Inputs;
		std::unordered_set<std::string> UnversionedPackages;
		std::vector<FPackagePath> RuntimePackages;
		uint64 RetainedBytes = 0;
	};
}
