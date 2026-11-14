#pragma once
#include "CoreMinimal.h"
#include "AssetTools/ContentMutation.h"
namespace Durin::ContentStorage
{
	class ASSETTOOLS_API FContentFolderOperation : public FContentMutationOperation
	{
	public:
		using FContentMutationOperation::FContentMutationOperation;
		auto CanCreateFolder(std::string_view Directory, std::string_view Name, std::string* Error = nullptr) const -> bool;
		auto CreateFolder(std::string_view Directory, std::string_view Name, std::string& PhysicalPath) -> bool;
	private:
		auto ValidateFolder(std::string_view Directory, std::string_view Name, std::string& PhysicalPath) const -> FAssetWriteResult;
	};
}
