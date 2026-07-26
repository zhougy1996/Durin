#pragma once

#include "AssetCoreAPI.h"

namespace Durin::Asset
{
	enum class EDerivedDataObjectReadStatus : uint8
	{
		Hit,
		Missing,
		InvalidKey,
		TooLarge,
		ReadFailure
	};

	struct FDerivedDataObjectReadResult
	{
		EDerivedDataObjectReadStatus Status = EDerivedDataObjectReadStatus::Missing;
		std::string Message;

		explicit operator bool() const { return Status == EDerivedDataObjectReadStatus::Hit; }
	};

	struct FDerivedDataObjectCleanupResult
	{
		uint64 BytesBefore = 0;
		uint64 BytesAfter = 0;
		uint64 DeletedBytes = 0;
		uint32 DeletedObjects = 0;
		bool bBudgetSatisfied = true;
		std::string Message;
	};

	// Stores immutable objects beneath one validated family root in the project DDC.
	class FDerivedDataObjectStore
	{
	public:
		ASSETCORE_API explicit FDerivedDataObjectStore(
			std::filesystem::path InRelativeRoot,
			uint64 InMaximumObjectBytes,
			uint32 InKeyLength = 32);

		ASSETCORE_API auto GetRoot() const -> std::filesystem::path;
		ASSETCORE_API auto GetObjectPath(std::string_view Key, std::filesystem::path& OutPath, std::string* OutError = nullptr) const -> bool;
		ASSETCORE_API auto Read(std::string_view Key, std::vector<uint8>& OutBytes) const -> FDerivedDataObjectReadResult;
		ASSETCORE_API auto Write(std::string_view Key, std::span<const uint8> Bytes, std::string* OutError = nullptr) const -> bool;
		ASSETCORE_API auto CleanupToBudget(uint64 BudgetBytes, uint32 MaximumDeletes) const -> FDerivedDataObjectCleanupResult;

	private:
		auto IsValidKey(std::string_view Key) const -> bool;

		std::filesystem::path RelativeRoot;
		uint64 MaximumObjectBytes = 0;
		uint32 KeyLength = 0;
		bool bValidRoot = false;
	};
}
