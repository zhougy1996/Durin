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

	// Move-only read lease; retains storage even if the owning value is replaced.
	class FBulkDataReadScope
	{
	public:
		FBulkDataReadScope() = default;
		ENGINE_API ~FBulkDataReadScope();
		ENGINE_API FBulkDataReadScope(FBulkDataReadScope&& Other) noexcept;
		ENGINE_API auto operator=(FBulkDataReadScope&& Other) noexcept -> FBulkDataReadScope&;
		FBulkDataReadScope(const FBulkDataReadScope&) = delete;
		auto operator=(const FBulkDataReadScope&) -> FBulkDataReadScope& = delete;
		ENGINE_API auto GetBytes() const -> FByteView;
		ENGINE_API auto Reset() -> void;
		explicit operator bool() const { return State != nullptr; }

	private:
		explicit FBulkDataReadScope(std::shared_ptr<AssetPrivate::FBulkDataState> InState)
			: State(std::move(InState))
		{
		}
		std::shared_ptr<AssetPrivate::FBulkDataState> State;
		friend class FBulkData;
	};

	enum class EBulkReadStatus : uint8
	{
		Acquired,
		Empty,
		Busy,
		Retired,
		ReadFailed
	};

	// Admission state is separate from an underlying package I/O failure.
	struct [[nodiscard]] FBulkDataReadResult
	{
		EBulkReadStatus Status = EBulkReadStatus::Empty;
		FBulkDataReadScope Lock;
		FPackageResourceReadResult Error;
		explicit operator bool() const { return static_cast<bool>(Lock); }
	};

	// Exclusive mutation lease. Resize rejects oversized input without changing bytes.
	class FBulkDataWriteScope
	{
	public:
		FBulkDataWriteScope() = default;
		ENGINE_API ~FBulkDataWriteScope();
		ENGINE_API FBulkDataWriteScope(FBulkDataWriteScope&& Other) noexcept;
		ENGINE_API auto operator=(FBulkDataWriteScope&& Other) noexcept -> FBulkDataWriteScope&;
		FBulkDataWriteScope(const FBulkDataWriteScope&) = delete;
		auto operator=(const FBulkDataWriteScope&) -> FBulkDataWriteScope& = delete;
		ENGINE_API auto GetBytes() const -> FMutableByteView;
		[[nodiscard]] ENGINE_API auto TryResize(uint64 Size) -> bool;
		ENGINE_API auto Reset() -> void;

	private:
		explicit FBulkDataWriteScope(std::shared_ptr<AssetPrivate::FBulkDataState> InState)
			: State(std::move(InState))
		{
		}
		std::shared_ptr<AssetPrivate::FBulkDataState> State;
		friend class FBulkData;
	};

	enum class EBulkUnloadResult : uint8
	{
		Unloaded,
		Busy,
		NotResident
	};

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
			FByteView Bytes, FBulkData& OutValue,
			std::string* OutError = nullptr) -> bool;
		ENGINE_API static auto TryAttach(
			FBulkDataMetadata Metadata, FBulkData& OutValue,
			std::string* OutError = nullptr) -> bool;

		ENGINE_API auto GetState() const -> EBulkDataState;
		ENGINE_API auto GetMetadata() const -> FBulkDataMetadata;
		ENGINE_API auto HasData() const -> bool;
		ENGINE_API auto AcquireRead() -> FBulkDataReadResult;
		// Requires detached or empty storage with no active locks.
		ENGINE_API auto AcquireWrite() -> FBulkDataWriteScope;
		[[nodiscard]] ENGINE_API auto TryUnload() -> EBulkUnloadResult;
		ENGINE_API auto ReloadAsync() -> FPackageResourceRequest;
		ENGINE_API auto Serialize(
			FArchive& Ar, FArchiveBulkDataParameters Parameters = {}) -> void;

	private:
		explicit FBulkData(std::shared_ptr<AssetPrivate::FBulkDataState> InState)
			: State(std::move(InState)) {}

		std::shared_ptr<AssetPrivate::FBulkDataState> State;
	};
}
