#pragma once

#include "AssetToolsAPI.h"
#include "DObject/AssetPath.h"

namespace Durin
{
	class DObject;
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

	// Distinguishes rejection, committed state, and failures that require recovery.
	enum class EAssetOperationTerminalState : uint8
	{
		Rejected,
		Completed,
		RecoveryRequired,
	};

	// Reports the persistence state left by a completed editor asset command.
	enum class EAssetOperationPersistenceState : uint8
	{
		NotApplicable,
		Dirty,
		Persisted,
		PartiallyPersisted,
	};

	enum class EAssetOperationPhase : uint8
	{
		Execute,
		Undo,
		Redo,
	};

	struct FAssetOperationWarning
	{
		FPackagePath AssetPath;
		std::string Details;
	};

	// Carries structured command state; diagnostics are presentation data only.
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
		bool bPublished = false;

		auto Succeeded() const -> bool
		{
			return State == EAssetOperationTerminalState::Completed;
		}
		explicit operator bool() const { return Succeeded(); }
	};

	struct FAssetOperationNotification
	{
		EAssetOperationKind Kind = EAssetOperationKind::Create;
		EAssetOperationPhase Phase = EAssetOperationPhase::Execute;
		EAssetOperationPersistenceState Persistence =
			EAssetOperationPersistenceState::NotApplicable;
		std::vector<FPackagePath> AffectedAssets;
		std::vector<FAssetOperationWarning> Warnings;
	};

	using FPublishAssetOperation =
		std::function<void(const FAssetOperationNotification&)>;
}
