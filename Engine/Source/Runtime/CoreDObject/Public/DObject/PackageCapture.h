#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "CoreDObjectAPI.h"
#include "DObject/PackageFormat.h"
#include "DObject/ObjectKey.h"
#include "DObject/DefaultDeltaPlan.h"
#include "DObject/PackageSaveOverrides.h"
#include "DObject/PackageBulkStorage.h"

namespace Durin
{
	class DPackage;
	class ITargetPlatform;
	// Transient or explicitly omitted outers exclude their entire subtree.
	COREDOBJECT_API auto IsPackageCaptureObjectOmitted(const DObject* Object,
		const FObjectSaveOverrides* Overrides = nullptr) -> bool;
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
	COREDOBJECT_API auto ToError(const FPackageCaptureError& Error) -> UnifiedError::FError;
	struct FPackageCaptureOptions
	{
		bool bCooking = false;
		bool bRetainEditorOnlyData = false;
		FArchiveTarget Target;
		const ITargetPlatform* TargetPlatform = nullptr;
		std::shared_ptr<const FObjectSaveOverrides> SaveOverrides;
		std::function<bool(const DObject*, const FProperty*)> PropertyFilter;
		std::function<bool(const DObject*)> ObjectFilter;
		std::unordered_map<const DObject*, FSoftObjectPath> RedirectDestinations;
	};
	// GameThread capture. Does not validate catalog dependencies or publish assets.
	COREDOBJECT_API auto CapturePackageLinker(DPackage* Package, EDefaultDeltaMode DeltaMode,
		const FPackageCaptureOptions& Options, ObjectPackage::FLinkerTables& OutLinker,
		uint32 FormatVersion = ObjectPackage::DastV12FormatVersion, std::vector<FObjectKey>* OutExports = nullptr) -> std::expected<void, FPackageCaptureError>;
}
