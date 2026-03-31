#include "Misc/FileHelper.h"

namespace Doge
{
	namespace FFileHelper
	{
		bool FileExists(std::string_view FileName)
		{
			return FFileSystem::exists(FileName);
		}

		template<typename ElementType>
		static auto LoadFileToArrayInternal(std::vector<ElementType>& Result, std::string_view FileName) -> bool
		{
			FPath FilePath(FileName);
			if (!FFileSystem::exists(FilePath))
			{
				DOGE_WARN("File {} does not exist.", FileName);
				return false;
			}

			std::ifstream File(FilePath, std::ios::binary);
			if (!File.is_open())
			{
				return false;
			}

			const auto FileSize = FFileSystem::file_size(FileName);

			if (FileSize > 0)
			{
				Result.resize((FileSize + sizeof(ElementType) - 1) / sizeof(ElementType));
				if (!File.read(reinterpret_cast<char*>(Result.data()), FileSize))
				{
					return false;
				}
			}

			return true;
		};

		bool LoadFileToArray(std::vector<uint8>& Result, std::string_view FileName)
		{
			return LoadFileToArrayInternal(Result, FileName);
		}

		bool LoadFileToArray(std::vector<uint32>& Result, std::string_view FileName)
		{
			return LoadFileToArrayInternal(Result, FileName);
		}
	} // namespace FFileHelper
}