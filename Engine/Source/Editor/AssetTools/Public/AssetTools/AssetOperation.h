#pragma once

#include "AssetToolsAPI.h"
#include "DObject/AssetPath.h"

namespace Durin
{
	class DObject;
	class FFactoryDiagnostics;
	struct FAssetImportValidation;
	class DPackage;

	// Identifies the reusable editor command represented by an AssetTools result.
	enum class EAssetOperationKind : uint8
	{
		Create,
		Import,
		Duplicate,
		Save,
		Relocate,
		FixUpRedirectors,
		Delete,
	};

	// Distinguishes rejection, committed state, and uncertain content.
	enum class EAssetOperationTerminalState : uint8
	{
		Rejected,
		Completed,
		ContentCommittedProjectionPending,
		ContentUncertain,
		PartiallyWritten,
	};

	// Reports the persistence state left by a completed editor asset command.
	enum class EAssetOperationPersistenceState : uint8
	{
		NotApplicable,
		Dirty,
		Persisted,
		PartiallyPersisted,
	};

	struct FAssetOperationWarning
	{
		FPackagePath AssetPath;
		std::string Details;
	};

	// Carries structured command state; diagnostics are presentation data only.
	enum class EAssetCreationError : uint8 { None, Path, Class, Occupied, Filename, PackageCreation, ProductType, ProductOuter, ProductName, ProductRegistration, FactoryRejected };
	struct FAssetCreationError
	{
		EAssetCreationError Code = EAssetCreationError::None;
		std::string RequestedPath, RequestedClass, Filename;
		std::string ActualPath, ActualClass;
	};
	ASSETTOOLS_API auto FormatAssetCreationError(const FAssetCreationError& Error) -> std::string;

	struct FAssetOperationResult
	{
		EAssetOperationKind Kind = EAssetOperationKind::Create;
		EAssetOperationTerminalState State = EAssetOperationTerminalState::Completed;
		EAssetOperationPersistenceState Persistence =
			EAssetOperationPersistenceState::NotApplicable;
		std::vector<FPackagePath> AffectedAssets;
		std::vector<FAssetOperationWarning> Warnings;
		std::string Message;
		DObject* Asset = nullptr;
		DPackage* Package = nullptr;
		std::string PhysicalPath;
		std::string FailedParticipant;
		std::vector<std::filesystem::path> BackupLocations;
		std::vector<std::filesystem::path> AffectedFiles;
		std::shared_ptr<const FAssetImportValidation> ImportCause;
		std::optional<FAssetCreationError> CreationCause;
		std::shared_ptr<const FFactoryDiagnostics> FactoryCause;

		auto Succeeded() const -> bool
		{
			return State == EAssetOperationTerminalState::Completed
				|| State == EAssetOperationTerminalState::ContentCommittedProjectionPending;
		}
		explicit operator bool() const { return Succeeded(); }
	};

	struct FAssetOperationNotification
	{
		EAssetOperationKind Kind = EAssetOperationKind::Create;
		EAssetOperationPersistenceState Persistence =
			EAssetOperationPersistenceState::NotApplicable;
		std::vector<FPackagePath> AffectedAssets;
		std::vector<FAssetOperationWarning> Warnings;
	};

	using FPublishAssetOperation =
		std::function<void(const FAssetOperationNotification&)>;
}
