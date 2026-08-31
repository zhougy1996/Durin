#pragma once

#include "Diagnostics/Diagnostic.h"

namespace Durin::Asset
{
	// Classifies Engine-owned asset loading, storage, Cook, and mutation failures.
	enum class EAssetError : uint8
	{
		None,
		InvalidPath,
		AlreadyExists,
		NotFound,
		IoError,
		CorruptFile,
		UnsupportedVersion,
		UnknownClass,
		TypeMismatch,
		MissingDependency,
		CircularDependency,
		InvalidObjectGraph,
		UnsupportedProperty,
		InvalidPackageType,
		InUse,
		StaleData,
		ReadOnlyMode,
		ShuttingDown
	};

	constexpr auto GetAssetErrorCode(EAssetError Error) -> std::string_view
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

	struct FAssetResult
	{
		EAssetError Error = EAssetError::None;
		std::string Message;

		auto Succeeded() const -> bool { return Error == EAssetError::None; }
		explicit operator bool() const { return Succeeded(); }
		auto GetDiagnostic() const -> FDiagnostic
		{
			return {
				.Domain = "Asset",
				.Code = std::string(GetAssetErrorCode(Error)),
				.Severity = Error == EAssetError::None
					? EDiagnosticSeverity::Info : EDiagnosticSeverity::Error,
				.Message = Message};
		}
	};
}
