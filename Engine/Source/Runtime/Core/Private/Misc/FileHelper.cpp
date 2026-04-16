#include "Misc/FileHelper.h"

namespace Doge
{
	namespace FFileHelper
	{
		bool FileExists(std::string_view FileName)
		{
			return std::filesystem::exists(FileName);
		}

		template<typename ElementType>
		static auto LoadFileToArrayInternal(std::vector<ElementType>& Result, std::string_view FileName) -> bool
		{
			std::filesystem::path FilePath(FileName);
			if (!std::filesystem::exists(FilePath))
			{
				DOGE_WARN("Failed to load file. File {} does not exist.", FileName);
				return false;
			}

			std::ifstream File(FilePath, std::ios::binary);
			if (!File.is_open())
			{
				return false;
			}

			const auto FileSize = std::filesystem::file_size(FileName);

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

		bool LoadFileToString(std::string& Result, std::string_view FileName)
		{
			std::filesystem::path FilePath(FileName);
			if (!std::filesystem::exists(FilePath))
			{
				DOGE_WARN("Failed to load file. File {} does not exist.", FileName);
				return false;
			}

			std::ifstream File(FilePath);
			if (!File.is_open())
			{
				return false;
			}

			std::stringstream StringStream;
			StringStream << File.rdbuf();
			Result = StringStream.str();

			File.close();
			return true;
		}

		bool SaveArrayToFile(const std::span<const std::byte>& Array, const std::filesystem::path& FilePath)
		{
			// Ensure the parent directory exists
			if (FilePath.has_parent_path())
			{
				std::error_code ErrorCode;
				std::filesystem::create_directories(FilePath.parent_path(), ErrorCode);
				if (ErrorCode)
				{
					DOGE_ERROR("Failed to create directories for path {}: {}", FilePath.parent_path().string(), ErrorCode.message());
					return false;
				}
			}

			// Open the file stream in binary mode
			std::ofstream File(FilePath, std::ios::binary | std::ios::out);

			if (!File.is_open())
			{
				DOGE_ERROR("Failed to open file for writing: {}", FilePath.string());
				return false;
			}

			File.write(reinterpret_cast<const char*>(Array.data()), Array.size_bytes());

			if (File.fail())
			{
				DOGE_ERROR("Failed to write data to file {}", FilePath.string());
				return false;
			}

			File.close();

			if (File.fail())
			{
				DOGE_ERROR("Failed to close file {} after writing", FilePath.string());
				return false;
			}

			return true;
		}

		bool SaveArrayToFile(const std::span<const uint32>& Array, const std::filesystem::path& FilePath)
		{
			return SaveArrayToFile(std::span{reinterpret_cast<const std::byte*>(Array.data()), Array.size() * sizeof(uint32)}, FilePath);
		}

	} // namespace FFileHelper
} // namespace Doge