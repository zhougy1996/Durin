#pragma once
#include "CoreDObjectAPI.h"
#include "DObject/PackageFormat.h"
#include "DObject/DefaultDeltaPlan.h"
#include "DObject/PackageSaveOverrides.h"
#include "DObject/PackageBulkStorage.h"

namespace Durin
{
	class DPackage;
	enum class EPackageSaveError : uint8
	{
		None, InvalidPath, InvalidPackageType, InvalidObjectGraph, UnsupportedProperty,
		UnsupportedVersion, CorruptFile, IoError, StaleData, Cancelled, Busy, ShuttingDown
	};
	enum class EPackageCommitState : uint8 { NotCommitted, Committed, RecoveryRequired, PartiallyWritten };
	struct FPackageSaveResult
	{
		EPackageSaveError Error = EPackageSaveError::None;
		std::string Message;
		EPackageCommitState CommitState = EPackageCommitState::NotCommitted;
		std::vector<std::filesystem::path> RecoveryFiles;
		std::vector<std::filesystem::path> AffectedFiles;
		auto Succeeded() const -> bool { return Error == EPackageSaveError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	struct FPackageCaptureOptions
	{
		bool bCooking = false;
		bool bRetainEditorOnlyData = false;
		FArchiveTarget Target;
		std::shared_ptr<const FObjectSaveOverrides> SaveOverrides;
		std::function<bool(const DObject*, const FProperty*)> PropertyFilter;
		std::unordered_map<const DObject*, FObjectPath> RedirectDestinations;
	};
	// GameThread capture. Does not validate catalog dependencies or publish assets.
	COREDOBJECT_API auto CapturePackageLinker(DPackage* Package, EDefaultDeltaMode DeltaMode,
		const FPackageCaptureOptions& Options, ObjectPackage::FLinkerTables& OutLinker,
		std::string* OutError = nullptr,
		uint32 FormatVersion = ObjectPackage::DastV10FormatVersion) -> FPackageSaveResult;
}
