#pragma once

namespace Doge
{
	namespace FFileHelper
	{
		CORE_API bool FileExists(std::string_view FileName);

		CORE_API bool LoadFileToArray(std::vector<uint8>& Result, std::string_view FileName);

		CORE_API bool LoadFileToArray(std::vector<uint32>& Result, std::string_view FileName);

		CORE_API bool LoadFileToString(std::string& Result, std::string_view FileName);
	}
}