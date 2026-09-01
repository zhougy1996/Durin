#include "Asset/AssetDefinitions.h"

namespace Durin
{
	namespace
	{
		auto GetErrorCode(EAssetError Error) -> std::string_view
		{
			switch (Error)
			{
			case EAssetError::None: return "None";
			case EAssetError::InvalidPath: return "InvalidPath";
			case EAssetError::AlreadyExists: return "AlreadyExists";
			case EAssetError::NotFound: return "NotFound";
			case EAssetError::IoError: return "IoError";
			case EAssetError::CorruptFile: return "CorruptFile";
			case EAssetError::UnsupportedVersion: return "UnsupportedVersion";
			case EAssetError::UnknownClass: return "UnknownClass";
			case EAssetError::TypeMismatch: return "TypeMismatch";
			case EAssetError::MissingDependency: return "MissingDependency";
			case EAssetError::CircularDependency: return "CircularDependency";
			case EAssetError::InvalidObjectGraph: return "InvalidObjectGraph";
			case EAssetError::UnsupportedProperty: return "UnsupportedProperty";
			case EAssetError::InvalidPackageType: return "InvalidPackageType";
			case EAssetError::InUse: return "InUse";
			case EAssetError::StaleData: return "StaleData";
			case EAssetError::ReadOnlyMode: return "ReadOnlyMode";
			case EAssetError::ShuttingDown: return "ShuttingDown";
			}
			return "Unknown";
		}
	}

	auto FAssetResult::GetDiagnostic() const -> FDiagnostic
	{
		return {
			.Domain = "Asset",
			.Code = std::string(GetErrorCode(Error)),
			.Severity = Error == EAssetError::None
				? EDiagnosticSeverity::Info : EDiagnosticSeverity::Error,
			.Message = Message};
	}
}
