#pragma once
#include "CoreDObjectAPI.h"
#include "DObject/DObjectGlobals.h"
#include "Serialization/Archive.h"
#include <variant>
#include "DObject/ContainerOps.h"
#include "DObject/ObjectValidation.h"
#include "DObject/ObjectDiagnostic.h"

namespace Durin
{
	enum class EPropertyValueError : uint8
	{
		None, NullProperty, NullValue, UnavailableOperation, StorageAlreadyLive,
		StorageNotLive, CustomAccessorStorage, InvalidArrayIndex, InvalidLayout, CopyFailed,
	};
	enum class EPropertyValueOperation : uint8 { Storage, DefaultConstruct, CopyConstruct, CopyAssign };
	struct FPropertyValueError
	{
		EPropertyValueError Code = EPropertyValueError::None;
		EPropertyValueOperation Operation = EPropertyValueOperation::Storage;
		std::string PropertyName;
		std::string StructName;
		uint32 ArrayIndex = 0;
		uint32 ArrayDim = 0;
		size_t ValueSize = 0;
		size_t ValueAlignment = 0;
		auto HasError() const -> bool { return Code != EPropertyValueError::None; }
	};
	struct FPropertyValueResult
	{
		FPropertyValueError Error;
		auto Succeeded() const -> bool { return !Error.HasError(); }
		explicit operator bool() const { return Succeeded(); }
	};
	COREDOBJECT_API auto FormatPropertyValueError(const FPropertyValueError& Error) -> std::string;

	enum class EReflectedMapKeyError : uint8
	{
		None, NullProperty, NullContainer, InvalidArrayIndex, UnsupportedKind,
		UnknownEnumStorage, IncompleteStructEquality,
	};

	struct FReflectedMapKeyRoute
	{
		std::string PropertyName;
		uint32 ArrayIndex = 0;
	};

	struct FReflectedMapKeyError
	{
		EReflectedMapKeyError Code = EReflectedMapKeyError::None;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		std::string PropertyName;
		uint32 ArrayIndex = 0;
		uint32 ArrayDim = 0;
		// Owned outer-to-inner route, including the failing property.
		std::vector<FReflectedMapKeyRoute> Route;
		auto HasError() const -> bool { return Code != EReflectedMapKeyError::None; }
	};

	struct FReflectedMapKeyResult
	{
		FReflectedMapKeyError Error;
		auto Succeeded() const -> bool { return !Error.HasError(); }
		explicit operator bool() const { return Succeeded(); }
	};

	COREDOBJECT_API auto FormatReflectedMapKeyError(const FReflectedMapKeyError& Error) -> std::string;
	enum class EPropertySnapshotError : uint8
	{
		None, NullProperty, NullContainer, MissingStruct, MissingArrayOperations,
		MissingMapOperations, UnsupportedKind, InvalidMapKey, InvalidArrayIndex,
		IncompatibleType, ArchiveFailure, InvalidReferenceIndex, UnresolvedReference, TrailingBytes,
	};
	enum class EPropertySnapshotOperation : uint8 { Capture, Restore };
	struct FPropertySnapshotError
	{
		EPropertySnapshotError Code = EPropertySnapshotError::None;
		EPropertySnapshotOperation Operation = EPropertySnapshotOperation::Capture;
		std::string PropertyName;
		std::string ExpectedPropertyName;
		DurinCodeGen::EPropertyGenFlags ExpectedKind = DurinCodeGen::EPropertyGenFlags::None;
		DurinCodeGen::EPropertyGenFlags ActualKind = DurinCodeGen::EPropertyGenFlags::None;
		std::string ArchivePath;
		std::vector<std::string> PropertyRoute;
		uint32 ArrayIndex = 0;
		uint32 ArrayDim = 0;
		uint64 ActualCount = 0;
		uint64 ExpectedCount = 0;
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::string Message;
		auto HasError() const -> bool { return Code != EPropertySnapshotError::None; }
	};
	struct FPropertySnapshotResult
	{
		FPropertySnapshotError Error;
		auto Succeeded() const -> bool { return !Error.HasError(); }
		explicit operator bool() const { return Succeeded(); }
	};
	COREDOBJECT_API auto FormatPropertySnapshotError(const FPropertySnapshotError& Error) -> std::string;
	enum class EObjectPropertyCopyError : uint8
	{
		None, InvalidObjects, UnmappedDefaultReference, ContainerTraversal, ValueCopy,
		Snapshot, ArchiveWrite, ArchiveRead, InvalidReferenceIndex, TrailingBytes,
	};
	enum class EObjectPropertyCopyOperation : uint8 { Defaults, Editable };
	struct FObjectPropertyCopyError
	{
		EObjectPropertyCopyError Code = EObjectPropertyCopyError::None;
		EObjectPropertyCopyOperation Operation = EObjectPropertyCopyOperation::Editable;
		std::string SourcePath;
		std::string DestinationPath;
		std::string SourceType;
		std::string DestinationType;
		std::string ReferencePath;
		std::string PropertyName;
		uint32 ArrayIndex = 0;
		std::vector<std::string> PropertyRoute;
		std::string ArchivePath;
		std::optional<EArchiveFailureCode> ArchiveCode;
		uint64 ActualCount = 0;
		uint64 ExpectedCount = 0;
		std::string Message;
		std::optional<FPropertySnapshotError> RollbackCause;
	};
	struct FObjectPropertyCopyResult
	{
		FObjectPropertyCopyError Error;
		auto Succeeded() const -> bool { return Error.Code == EObjectPropertyCopyError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	COREDOBJECT_API auto FormatObjectPropertyCopyError(const FObjectPropertyCopyError& Error) -> std::string;

	enum class EPropertyEditValueError : uint8
	{
		None, NullProperty, NullContainer, InvalidArrayIndex, MissingStruct,
		NonFinite, BelowMinimum, AboveMaximum,
	};
	struct FPropertyEditValueError
	{
		EPropertyEditValueError Code = EPropertyEditValueError::None;
		std::string PropertyName;
		uint32 ArrayIndex = 0;
		uint32 ArrayDim = 0;
		std::vector<std::string> Route;
		FPropertyMetadataNumber Current;
		FPropertyMetadataNumber Minimum;
		FPropertyMetadataNumber Maximum;
	};
	struct FPropertyEditValueResult
	{
		FPropertyEditValueError Error;
		auto Succeeded() const -> bool { return Error.Code == EPropertyEditValueError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	COREDOBJECT_API auto FormatPropertyEditValueError(const FPropertyEditValueError& Error) -> std::string;

	enum class EPropertyContainerOperation : uint8 { Resize, Insert, RenameKey };
	enum class EPropertyContainerRequirement : uint8 { None, DefaultConstruct, Destroy, CopyConstruct, CopyAssign };
	struct FPropertyContainerError
	{
		EContainerOpResult Code = EContainerOpResult::Success;
		EPropertyContainerOperation Operation = EPropertyContainerOperation::Resize;
		EPropertyContainerRequirement Requirement = EPropertyContainerRequirement::None;
		std::string PropertyName;
		std::string ValuePropertyName;
		std::string StructName;
		uint32 ArrayIndex = 0;
		uint32 ArrayDim = 0;
		uint64 CurrentCount = 0;
		uint64 RequestedCount = 0;
	};
	struct FPropertyContainerResult
	{
		FPropertyContainerError Error;
		auto Succeeded() const -> bool { return Error.Code == EContainerOpResult::Success; }
		explicit operator bool() const { return Succeeded(); }
	};
	COREDOBJECT_API auto FormatPropertyContainerError(const FPropertyContainerError& Error) -> std::string;

}
