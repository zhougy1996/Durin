#include "AssetRegistry/RegistryResult.h"

namespace Durin
{
	namespace
	{
		auto GetErrorCode(EAssetRegistryError Error) -> std::string_view
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
	}

	auto FAssetRegistryResult::GetDiagnostic() const -> FDiagnostic
	{
		return {
			.Domain = "AssetRegistry",
			.Code = std::string(GetErrorCode(Error)),
			.Severity = Error == EAssetRegistryError::None
				? EDiagnosticSeverity::Info : EDiagnosticSeverity::Error,
			.Message = Message};
	}
}
