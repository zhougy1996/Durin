#pragma once

#include "AssetForge/Operations/ImportOperation.h"
#include "AssetForge/Persistence/ImportProvenance.h"

namespace Durin::AssetForge
{
enum class EImportMode : uint8
	{
		Import,
		Preview,
		Reimport,
		ReplaceSource,
		Repair,
		// Rebuilds disposable runtime and DDC state without publishing authored packages.
		Recover
	};

struct FDeclaredSource
	{
		std::string StableIdentity;
		std::string Role;
		FSourcePath SourcePath;
	};

	struct FImportRequest
	{
		EImportMode Mode = EImportMode::Import;
		FSourcePath RootSource;
		// Additional caller-declared sources are captured into the same immutable
		// snapshot before translator dependency discovery. This is required for
		// formats such as a six-face cube whose complete source set cannot be
		// inferred from one root file.
		std::vector<FDeclaredSource> DeclaredSources;
		std::string TranslatorId;
		FSchemaPayload TranslatorSettings;
		std::vector<FPlanningPassStackEntry> PlanningPassStack;
		FAssetPath Destination;
		FImportOperationOwner Owner;
		EImportOperationLifetime Lifetime = EImportOperationLifetime::EditorOperation;
		FSourceCaptureLimits SourceLimits;
		FGraphLimits GraphLimits;
		Asset::FAssetBundleSaveOptions SaveOptions;
		std::optional<FImportProvenance> ExistingProvenance;
	};
}
