#pragma once

#include "AssetImportCoreAPI.h"
#include "AssetMutation.h"
#include "ImportRecord.h"

namespace Durin::Asset::Import
{
	enum class EImportRecordIndexDiagnostic : uint8
	{
		InvalidRecord,
		DuplicateRecordId,
		DuplicateManager,
		MissingManagedOutput,
		OutputFingerprintMismatch
	};

	struct FImportRecordIndexDiagnostic
	{
		EImportRecordIndexDiagnostic Category = EImportRecordIndexDiagnostic::InvalidRecord;
		FAssetPath RecordPath;
		FAssetPath OutputPath;
		std::string Message;
	};

	struct FImportRecordManagement
	{
		FAssetPath RecordPath;
		FGuid RecordId;
		std::string OutputIdentity;
		std::string OutputClassName;
		std::string AuthoredFingerprint;
		EImportRecordOutputPolicy Policy = EImportRecordOutputPolicy::Managed;
		bool bOutputMissing = false;
		bool bFingerprintMismatch = false;

		auto operator==(const FImportRecordManagement&) const -> bool = default;
	};

	struct FImportRecordInspection
	{
		bool bSucceeded = false;
		std::string Message;
		FAssetPath SelectedOutputPath;
		FAssetPath RecordPath;
		DImportRecord* Record = nullptr;
		std::vector<FImportRecordManagement> Outputs;
		std::vector<FImportRecordIndexDiagnostic> Diagnostics;
		bool bConflicted = false;
		bool bHasMissingManagedOutput = false;
		bool bHasFingerprintMismatch = false;

		explicit operator bool() const { return bSucceeded; }
	};

	struct FImportRecordEditResult
	{
		bool bSucceeded = false;
		std::string Message;
		DImportRecord* Record = nullptr;
		FAssetPath RevealPath;
		std::vector<FImportRecordIndexDiagnostic> Diagnostics;

		explicit operator bool() const { return bSucceeded; }
	};

	class ASSETIMPORTCORE_API FImportRecordIndex final
		: public Asset::IAssetReferenceStore
	{
	public:
		FImportRecordIndex();
		~FImportRecordIndex();
		FImportRecordIndex(const FImportRecordIndex&) = delete;
		auto operator=(const FImportRecordIndex&) -> FImportRecordIndex& = delete;

		auto Rebuild(std::string& OutError) -> bool;
		auto EnsureCurrent(std::string& OutError) -> bool;
		auto FindManagers(const FAssetPath& OutputPath) const
			-> std::vector<FImportRecordManagement>;
		auto FindRecordOutputs(const FAssetPath& RecordPath) const
			-> std::vector<FImportRecordManagement>;
		auto IsRecordConflicted(const FAssetPath& RecordPath) const -> bool;
		auto GetDiagnostics() const -> std::vector<FImportRecordIndexDiagnostic>;
		auto GetRevision() const -> uint64;
		auto GetObservedAssetRegistryRevision() const -> uint64;
		auto ClearForProjectSwitch() -> void;
		auto NotifyAssetDeleted(const FAssetPath& Path) -> void;
		auto NotifyAssetDuplicated() -> void;
		auto NotifyPackageUnloaded(const FAssetPath& Path) -> void;
		auto CaptureSnapshot(
			Asset::FAssetReferenceStoreSnapshot& OutSnapshot)
			-> Asset::FAssetResult override;
		auto PrepareRewrite(
			std::span<const Asset::FAssetReferenceRewrite> Rewrites,
			std::string_view ExpectedFingerprint,
			Asset::FAssetReferenceStoreRewriteContribution& OutContribution)
			-> Asset::FAssetResult override;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	ASSETIMPORTCORE_API auto GetImportRecordIndex() -> FImportRecordIndex&;
	ASSETIMPORTCORE_API auto ComputeImportPackageFingerprint(
		DPackage* Package,
		std::string& OutFingerprint,
		std::string& OutError) -> bool;
	ASSETIMPORTCORE_API auto ComputePersistedImportPackageFingerprint(
		const FAssetPath& Path,
		std::string& OutFingerprint,
		std::string& OutError) -> bool;
	ASSETIMPORTCORE_API auto InspectImportRecordForOutput(
		const FAssetPath& OutputPath,
		FImportRecordIndex& Index) -> FImportRecordInspection;
	ASSETIMPORTCORE_API auto InspectImportRecord(
		const FAssetPath& RecordPath,
		FImportRecordIndex& Index) -> FImportRecordInspection;
	ASSETIMPORTCORE_API auto DetachImportRecordOutput(
		DImportRecord& Record,
		std::string_view StableOutputIdentity,
		FImportRecordIndex& Index,
		const Asset::FAssetBundleSaveOptions& SaveOptions = {}) -> FImportRecordEditResult;
	ASSETIMPORTCORE_API auto RepairDuplicatedImportRecord(
		DImportRecord& Record,
		FImportRecordIndex& Index,
		const Asset::FAssetBundleSaveOptions& SaveOptions = {}) -> FImportRecordEditResult;
}
