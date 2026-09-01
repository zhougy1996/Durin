#pragma once

#include "AssetRegistryAPI.h"
#include "Diagnostics/Diagnostic.h"

namespace Durin
{
	enum class EAssetRegistryError : uint8
	{
		None,
		InvalidPath,
		AlreadyExists,
		NotFound,
		IoError,
		CorruptFile,
		UnsupportedVersion,
		MissingDependency,
		StaleData
	};

	struct FAssetRegistryResult
	{
		EAssetRegistryError Error = EAssetRegistryError::None;
		std::string Message;

		auto Succeeded() const -> bool { return Error == EAssetRegistryError::None; }
		explicit operator bool() const { return Succeeded(); }
		ASSETREGISTRY_API auto GetDiagnostic() const -> FDiagnostic;
	};
}
