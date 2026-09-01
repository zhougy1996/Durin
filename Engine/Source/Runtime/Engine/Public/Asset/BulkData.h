#pragma once

#include "EngineAPI.h"
#include "Asset/PackageResource.h"
#include "Serialization/Archive.h"

namespace Durin
{
	inline constexpr uint64 MaximumBulkDataBytes = 1024ull * 1024ull * 1024ull;

	enum class EBulkDataState : uint8
	{
		Empty, Attached, Loading, Resident, ReadLocked, WriteLocked, Detached, Failed, Retired,
	};

	// Carries only bounded runtime storage facts and a logical package resource.
	struct FBulkDataMetadata
	{
		uint64 LogicalSize = 0;
		FPackageResourceRange Range;
	};

	namespace AssetPrivate { struct FBulkDataState; }

	// Owns a lock-checked runtime allocation or a lazy package-resource range.
	class FBulkData
	{
	public:
		ENGINE_API FBulkData();
		ENGINE_API ~FBulkData();
		ENGINE_API FBulkData(const FBulkData& Other);
		ENGINE_API auto operator=(const FBulkData& Other) -> FBulkData&;
		ENGINE_API FBulkData(FBulkData&& Other) noexcept;
		ENGINE_API auto operator=(FBulkData&& Other) noexcept -> FBulkData&;

		ENGINE_API static auto TryCreateDetached(
			std::span<const std::byte> Bytes, FBulkData& OutValue,
			std::string* OutError = nullptr) -> bool;
		ENGINE_API static auto TryAttach(
			FBulkDataMetadata Metadata, FBulkData& OutValue,
			std::string* OutError = nullptr) -> bool;

		ENGINE_API auto GetState() const -> EBulkDataState;
		ENGINE_API auto GetMetadata() const -> FBulkDataMetadata;
		ENGINE_API auto HasData() const -> bool;
		ENGINE_API auto LockReadOnly(
			std::span<const std::byte>& OutBytes, std::string* OutError = nullptr) -> bool;
		ENGINE_API auto UnlockReadOnly(std::string* OutError = nullptr) -> bool;
		ENGINE_API auto LockReadWrite(
			std::span<std::byte>& OutBytes, std::string* OutError = nullptr) -> bool;
		ENGINE_API auto Resize(
			uint64 Size, std::span<std::byte>& OutBytes, std::string* OutError = nullptr) -> bool;
		ENGINE_API auto UnlockWrite(std::string* OutError = nullptr) -> bool;
		ENGINE_API auto Unload(std::string* OutError = nullptr) -> bool;
		ENGINE_API auto ReloadAsync() -> FPackageResourceRequest;
		ENGINE_API auto Serialize(
			FArchive& Ar, FArchiveBulkDataParameters Parameters = {}) -> void;

	private:
		explicit FBulkData(std::shared_ptr<AssetPrivate::FBulkDataState> InState)
			: State(std::move(InState)) {}

		std::shared_ptr<AssetPrivate::FBulkDataState> State;
	};
}
