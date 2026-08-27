#pragma once

#include "AssetToolsAPI.h"
#include "Factories/Factory.h"

namespace Durin
{
	struct FReimportOptions
	{
		bool bSave = true;
	};

	// Routes loaded-object reimport through the unique reflected factory for its class.
	class FReimportManager
	{
	public:
		ASSETTOOLS_API static auto GetCapabilities(const DObject& Object)
			-> FReimportCapabilities;
		ASSETTOOLS_API static auto Reimport(
			DObject& Object,
			const FReimportOptions& Options,
			FReimportCompletion Completion) -> void;
		ASSETTOOLS_API static auto ReimportFromFiles(
			DObject& Object,
			std::span<const std::string> Filenames,
			const FReimportOptions& Options,
			FReimportCompletion Completion) -> void;
	};
}
