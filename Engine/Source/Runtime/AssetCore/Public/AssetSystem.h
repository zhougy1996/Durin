#pragma once

#include "AssetCoreAPI.h"
#include "DObject/CoreDObject.h"

namespace Durin::Asset
{
	enum class EAssetError : uint8
	{
		None,
		InvalidPath,
		AlreadyExists,
		NotFound,
		IoError,
		CorruptFile,
		UnsupportedVersion,
		UnknownClass,
		TypeMismatch,
		MissingDependency,
		CircularDependency,
		InvalidObjectGraph,
		UnsupportedProperty,
		InvalidPackageType,
		InUse
	};

	struct FAssetResult
	{
		EAssetError Error = EAssetError::None;
		std::string Message;

		auto Succeeded() const -> bool { return Error == EAssetError::None; }
		explicit operator bool() const { return Succeeded(); }
	};

	struct FAssetData
	{
		FAssetPath PackagePath;
		std::string PhysicalPath;
		std::string AssetClassName;
		uint32 FormatVersion = 0;
		std::vector<FAssetPath> Dependencies;
		std::filesystem::file_time_type LastWriteTime{};
	};

	struct FAssetMoveContribution
	{
		std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Files;
		std::function<void()> Apply;
		std::function<void()> Rollback;
	};

	struct FAssetDeleteContribution
	{
		std::vector<std::filesystem::path> Files;
	};

	struct FAssetDeleteAnalysis
	{
		FAssetPath AssetPath;
		std::vector<FAssetPath> DirectReferencers;
		std::vector<std::filesystem::path> CompanionFiles;
		bool bLoaded = false;
		bool bLoading = false;

		// A loaded package is cache state, not a usage claim. Deletion safely unloads it after
		// persistent referencers have been ruled out.
		auto CanDelete() const -> bool { return DirectReferencers.empty() && !bLoading; }
	};

	using FAssetMoveContributor = std::function<FAssetResult(DObject*, const FAssetPath&, const FAssetPath&, FAssetMoveContribution&)>;
	ASSETCORE_API auto RegisterAssetMoveContributor(DClass* Class, FAssetMoveContributor Contributor) -> void;
	using FAssetDeleteContributor = std::function<FAssetResult(DObject*, FAssetDeleteContribution&)>;
	ASSETCORE_API auto RegisterAssetDeleteContributor(DClass* Class, FAssetDeleteContributor Contributor) -> void;

	class FAssetRegistry
	{
	public:
		ASSETCORE_API auto ScanMountedContent() -> FAssetResult;
		ASSETCORE_API auto FindAsset(const FAssetPath& Path) const -> const FAssetData*;
		auto GetAssets() const -> const std::unordered_map<FAssetPath, FAssetData>& { return Assets; }
		auto GetScanErrors() const -> const std::vector<FAssetResult>& { return ScanErrors; }

	private:
		auto AddOrUpdate(FAssetData Data) -> void;
		std::unordered_map<FAssetPath, FAssetData> Assets;
		std::vector<FAssetResult> ScanErrors;

		friend class FAssetManager;
	};

	class FAssetManager
	{
	public:
		ASSETCORE_API static auto Get() -> FAssetManager&;

		ASSETCORE_API auto CreateAsset(const FAssetPath& Path, DClass* Class, size_t Size, DObject*& OutAsset) -> FAssetResult;
		ASSETCORE_API auto LoadAsset(const FAssetPath& Path, DObject*& OutAsset) -> FAssetResult;
		ASSETCORE_API auto SavePackage(DPackage* Package) -> FAssetResult;
		ASSETCORE_API auto MoveAsset(const FAssetPath& OldPath, const FAssetPath& NewPath) -> FAssetResult;
		ASSETCORE_API auto AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult;
		ASSETCORE_API auto DeleteAsset(const FAssetPath& Path) -> FAssetResult;
		ASSETCORE_API auto FindLoadedPackage(const FAssetPath& Path) const -> DPackage*;
		ASSETCORE_API auto UnloadPackage(const FAssetPath& Path) -> FAssetResult;
		ASSETCORE_API auto Shutdown() -> void;

		auto GetRegistry() -> FAssetRegistry& { return Registry; }
		auto GetRegistry() const -> const FAssetRegistry& { return Registry; }

	private:
		FAssetManager() = default;
		auto LoadPackageInternal(const FAssetPath& Path, DPackage*& OutPackage) -> FAssetResult;
		auto IsPackageReferenced(const DPackage* Package) const -> bool;

		FAssetRegistry Registry;
		std::unordered_map<FAssetPath, DPackage*> LoadedPackages;
		std::unordered_set<FAssetPath> LoadingPackages;
		uint32 LoadDepth = 0;
		std::vector<FAssetPath> TransactionPackages;
	};

	template<typename T>
	auto CreateAsset(const FAssetPath& Path, T*& OutAsset) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = FAssetManager::Get().CreateAsset(Path, T::StaticClass(), sizeof(T), Object);
		OutAsset = Result ? Cast<T>(Object) : nullptr;
		return Result;
	}

	template<typename T>
	auto LoadAsset(const FAssetPath& Path, T*& OutAsset) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = FAssetManager::Get().LoadAsset(Path, Object);
		if (Result && Object && !Object->IsA<T>())
		{
			OutAsset = nullptr;
			return {EAssetError::TypeMismatch, std::format("Asset {} is not a {}.", Path.ToString(), T::StaticClass()->GetQualifiedName().ToString())};
		}
		OutAsset = static_cast<T*>(Object);
		return Result;
	}

	ASSETCORE_API auto LoadAsset(const FAssetPath& Path, DObject*& OutAsset) -> FAssetResult;
	ASSETCORE_API auto SavePackage(DPackage* Package) -> FAssetResult;
	ASSETCORE_API auto MoveAsset(const FAssetPath& OldPath, const FAssetPath& NewPath) -> FAssetResult;
	ASSETCORE_API auto AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult;
	ASSETCORE_API auto DeleteAsset(const FAssetPath& Path) -> FAssetResult;
	ASSETCORE_API auto FindLoadedPackage(const FAssetPath& Path) -> DPackage*;
	ASSETCORE_API auto UnloadPackage(const FAssetPath& Path) -> FAssetResult;
	ASSETCORE_API auto ShutdownAssetManager() -> void;
	ASSETCORE_API auto GetAssetRegistry() -> FAssetRegistry&;
}
