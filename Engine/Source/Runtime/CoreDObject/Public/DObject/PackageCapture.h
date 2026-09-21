#pragma once
#include <expected>
#include "Misc/FileError.h"
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
	enum class EPackageCaptureReason : uint8
	{
		None,
		RawOutsideField,
		BulkVersion,
		BulkMetadata,
		BulkAlignment,
		BulkLimit,
		BulkOutsideValue,
		ReferenceOutsideObject,
		InvalidHardReference,
		SoftPathLimit,
		ObjectOutsideGraph,
		OuterOutsideGraph,
		FieldOutsideObject,
		ValueOutsideField,
		WeakObject,
		FieldEventKind,
		TypeDepth,
		ValueManifest,
		ObjectTopology,
		DeltaGraph,
		DeltaObject,
		OuterTopology,
		AssetIdentity,
		MissingDeltaField,
		MissingSchemaField,
		PackageType,
		MissingAssets,
		CookTarget,
		PackagePath,
		ClassDefaults,
		OverrideOutsideGraph,
		OmittedAsset,
		DiscoveryMutation,
		CookGraph,
		BulkIdentity,
		EmissionMutation,
		FieldTypeChanged,
		MissingAsset,
		MissingDeltaObject,
		MissingChildType,
		ReplacementValue,
		ArchiveFailure,
		DefaultDelta,
	};
	struct FPackageCaptureError
	{
		EPackageCaptureReason Reason = EPackageCaptureReason::None;
		std::string ObjectPath;
		std::string SchemaName;
		std::string FieldName;
		std::string ArchivePath;
		std::vector<std::string> Route;
		uint64 Actual = 0;
		uint64 Expected = 0;
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::string Message;
		std::variant<std::monostate, FObjectPathError, FPropertyValueError,
			FPropertySnapshotError, FObjectValidationError, FReflectedMapKeyError, FDefaultDeltaDiagnostic> Cause;
	};
	COREDOBJECT_API auto ToString(const FPackageCaptureError& Error) -> std::string;
	COREDOBJECT_API auto GetPackageCaptureSaveError(const FPackageCaptureError& Error) -> EPackageSaveError;
	enum class EPackageCommitState : uint8 { NotCommitted, Committed, RecoveryRequired, PartiallyWritten };
	struct FPackageSaveResult
	{
		EPackageSaveError Error = EPackageSaveError::None;
		std::string Message;
		EPackageCommitState CommitState = EPackageCommitState::NotCommitted;
		std::vector<std::filesystem::path> RecoveryFiles;
		std::vector<std::filesystem::path> AffectedFiles;
		std::optional<FFileError> FileCause;
		std::optional<FPackageCaptureError> CaptureCause;
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
		uint32 FormatVersion = ObjectPackage::DastV10FormatVersion) -> std::expected<void, FPackageCaptureError>;
}
