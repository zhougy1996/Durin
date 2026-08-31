#pragma once

#include "Diagnostics/Diagnostic.h"

namespace Durin::Asset
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

	constexpr auto GetAssetRegistryErrorCode(EAssetRegistryError Error)
		-> std::string_view
	{
		switch (Error)
		{
		case EAssetRegistryError::None: return "None";
		case EAssetRegistryError::InvalidPath: return "InvalidPath";
		case EAssetRegistryError::AlreadyExists: return "AlreadyExists";
		case EAssetRegistryError::NotFound: return "NotFound";
		case EAssetRegistryError::IoError: return "IoError";
		case EAssetRegistryError::CorruptFile: return "CorruptFile";
		case EAssetRegistryError::UnsupportedVersion: return "UnsupportedVersion";
		case EAssetRegistryError::MissingDependency: return "MissingDependency";
		case EAssetRegistryError::StaleData: return "StaleData";
		}
		return "Unknown";
	}

	struct FAssetRegistryResult
	{
		EAssetRegistryError Error = EAssetRegistryError::None;
		std::string Message;

		auto Succeeded() const -> bool { return Error == EAssetRegistryError::None; }
		explicit operator bool() const { return Succeeded(); }
		auto GetDiagnostic() const -> FDiagnostic
		{
			return {
				.Domain = "AssetRegistry",
				.Code = std::string(GetAssetRegistryErrorCode(Error)),
				.Severity = Error == EAssetRegistryError::None
					? EDiagnosticSeverity::Info : EDiagnosticSeverity::Error,
				.Message = Message};
		}
	};
}
