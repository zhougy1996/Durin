#pragma once

#include "DObject/AssetPath.h"

namespace Durin::AssetForge
{
	// Classifies the presentation severity of a Scene import diagnostic.
	enum class EImportDiagnosticSeverity : uint8
	{
		Warning,
		Error
	};

	// Identifies the concrete Scene import boundary that rejected or degraded an attempt.
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

	// Reports one attributable diagnostic from a Scene import attempt.
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

	// Describes one creation-only asset produced by Scene import.
	struct FImportOutputSummary
	{
		std::string StableIdentity;
		std::string Role;
		FAssetPath AssetPath;
		std::string AssetClassName;
		auto operator==(const FImportOutputSummary&) const -> bool = default;
	};
}
