#pragma once
#include "CoreMinimal.h"
#include "AssetTools/ContentMutation.h"
namespace Durin::ContentStorage
{
	class ASSETTOOLS_API FContentFolderOperation
	{
	public:
		explicit FContentFolderOperation(const FContentMutationPolicy& InPolicy) : Policy(InPolicy) {}
		auto CanCreateFolder(std::string_view Directory, std::string_view Name, std::string* Error = nullptr) const -> bool;
		auto CreateFolder(std::string_view Directory, std::string_view Name, std::string& PhysicalPath) -> bool;
	private:
		FContentMutationPolicy Policy;
		auto ValidateFolder(std::string_view Directory, std::string_view Name, std::string& PhysicalPath,
			std::optional<UnifiedError::FError>* OutError = nullptr) const -> bool;
	};
}
