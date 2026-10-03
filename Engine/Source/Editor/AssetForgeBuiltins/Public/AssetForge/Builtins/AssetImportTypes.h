#pragma once

#include "CoreMinimal.h"

#include "DObject/AssetPath.h"

namespace Durin::AssetForge
{
	// Classifies the presentation severity of a asset import diagnostic.
	enum class EImportDiagnosticSeverity : uint8
	{
		Warning,
		Error
	};

	// Identifies the concrete asset import boundary that rejected or degraded an attempt.
	enum class EImportDiagnosticCategory : uint8
	{
		InvalidRequest,
		TranslationFailure,
		CandidateFailure,
		ValidationFailure,
		PersistenceFailure,
		InvalidSource,
		MissingDependency,
		UnsafeDependency,
		DuplicateSource,
		DependencyCycle,
		ResourceLimitExceeded,
		InvalidPlan,
		Collision,
		Canceled
	};

	// Reports one attributable diagnostic from a asset import attempt.
	struct FImportDiagnostic
	{
		EImportDiagnosticSeverity Severity = EImportDiagnosticSeverity::Error;
		EImportDiagnosticCategory Category = EImportDiagnosticCategory::InvalidRequest;
		std::string Phase;
		std::string SourceIdentity;
		std::string OutputIdentity;
		std::string Message;

		auto operator==(const FImportDiagnostic&) const -> bool = default;
	};

	enum class EImportOutputDisposition : uint8 { Create, Update, Preserve, Conflict };
	enum class EImportOutputState : uint8 { Unexecuted, Saved, Preserved, Failed };

	// Describes one planned asset produced by asset import.
	struct FImportOutputSummary
	{
		std::string StableIdentity;
		std::string Role;
		FPackagePath AssetPath;
		std::string AssetClassName;
		EImportOutputDisposition Disposition = EImportOutputDisposition::Create;
		EImportOutputState State = EImportOutputState::Unexecuted;
		auto operator==(const FImportOutputSummary&) const -> bool = default;
	};
}
