#pragma once

#include "Asset/Cook.h"
#include "Asset/MutationExtensions.h"
#include "Asset/PackageInspection.h"
#include "Asset/PackageSchema.h"
#include "AssetRegistry/References.h"
#include "AssetPackageCodec.h"
#include "DObject/StrongObjectPtr.h"
#include "Misc/MountPaths.h"

namespace Durin::AssetPrivate
{
	enum class ECookCapturePhase : uint8 { Acquiring, Sealed, Capturing, Detached, Failed, Cancelled };

	class FCookInputCapture
	{
	public:
		using FResolveContributor = std::function<FAssetResult(const FAssetData&, FCookContributorRegistration&)>;
		FCookInputCapture(const FCookRequest& Request, FAssetRegistrySnapshot Registry,
			FResolveContributor ResolveContributor);
		~FCookInputCapture();
		FCookInputCapture(const FCookInputCapture&) = delete;
		auto operator=(const FCookInputCapture&) -> FCookInputCapture& = delete;
		auto Acquire(std::span<const FPackagePath> Roots, const FAssetReferenceStoreCapture& ExternalRoots) -> FAssetResult;
		auto Verify() -> FAssetResult;
		auto LoadObject(const FObjectPath& Path, DObject*& Out) -> FAssetResult;
		auto LoadPackage(const FPackagePath& Path, DPackage*& Out) -> FAssetResult;
		auto SetCurrentPackage(const FPackagePath& Path) -> void { CurrentPackage = Path; }
		auto ReadInput(ECookBuildDependencyKind Kind, std::string_view Name, FByteBuffer& Out) -> FAssetResult;
		auto Detach() -> void;
		auto GetPackages() const -> const std::vector<FPackagePath>& { return RuntimePackages; }
		auto GetRegistry() const -> const FAssetRegistrySnapshot& { return Registry; }
		auto GetContributor(const FPackagePath& Path) const -> const FCookContributorRegistration& { return Inputs.at(Path).Contributor; }
		auto GetDependencies(const FPackagePath& Path) const -> const std::vector<FCookBuildDependency>& { return Inputs.at(Path).Dependencies; }
		auto GetRetainedBytes() const -> uint64 { return RetainedBytes; }
		auto GetStatus() const -> ECookInputStatus { return Status; }
		auto IsReusable(const FPackagePath& Path) const -> bool;
		auto GetPhase() const -> ECookCapturePhase { return Phase; }
		static auto GetActive() -> FCookInputCapture* { return Active; }
	private:
		struct FInput
		{
			FByteBuffer PackageBytes;
			FByteBuffer BulkBytes;
			FByteBuffer BulkIdentity;
			uint64 BulkSize = 0;
			FPackageResourceHandle Resource;
			const FAssetPackageCodec* Codec = nullptr;
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
		auto ReleaseObjects() -> void;
		static thread_local FCookInputCapture* Active;
		const FCookRequest& Request;
		FAssetRegistrySnapshot Registry;
		FResolveContributor ResolveContributor;
		FReflectionSchemaCatalog Schema;
		std::vector<FMountPoint> Mounts;
		std::vector<FStrongObjectPtr> ObjectPins;
		std::unordered_map<std::string, FByteBuffer> SchemaValues;
		ECookCapturePhase Phase = ECookCapturePhase::Acquiring;
		FAssetResult Failure;
		ECookInputStatus Status = ECookInputStatus::None;
		std::unordered_map<FPackagePath, FInput> Inputs;
		std::unordered_map<FPackagePath, FStrongObjectPtr> Loaded;
		std::unordered_set<FPackagePath> Participants;
		std::unordered_set<std::string> UnversionedPackages;
		std::vector<FPackagePath> RuntimePackages;
		FPackagePath CurrentPackage;
		uint64 RetainedBytes = 0;
	};
}
