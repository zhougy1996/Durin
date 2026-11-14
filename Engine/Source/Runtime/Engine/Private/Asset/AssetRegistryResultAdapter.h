#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

#include "AssetRegistry/RegistryResult.h"

namespace Durin::AssetPrivate
{
	inline auto ToAssetResult(FAssetRegistryResult Result) -> std::expected<void, Durin::UnifiedError::FError>
	{
		if (Result) return {};
		std::string_view Code;
		switch (Result.Error)
		{
		case EAssetRegistryError::None: Code = "None"; break;
		case EAssetRegistryError::InvalidPath: Code = "InvalidPath"; break;
		case EAssetRegistryError::AlreadyExists: Code = "AlreadyExists"; break;
		case EAssetRegistryError::NotFound: Code = "NotFound"; break;
		case EAssetRegistryError::IoError: Code = "IoError"; break;
		case EAssetRegistryError::CorruptFile: Code = "CorruptFile"; break;
		case EAssetRegistryError::UnsupportedVersion: Code = "UnsupportedVersion"; break;
		case EAssetRegistryError::MissingDependency: Code = "MissingDependency"; break;
		case EAssetRegistryError::StaleData: Code = "StaleData"; break;
		}
		auto Error = UnifiedError::FError{{"AssetRegistry", Code}, FormatAssetRegistryError(Result)};
		if (!Result.Context.Path.empty()) Error.PushContext("path", Result.Context.Path);
		Error.PushContext("registry reason", std::to_string(static_cast<uint32>(Result.Context.Reason)));
		return std::unexpected(std::move(Error));
	}
}
