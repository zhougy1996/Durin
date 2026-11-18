#pragma once

#include "CoreMinimal.h"

#include "Asset/PackageSchema.h"
#include "HAL/PlatformFile.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

namespace Durin::AssetPrivate
{
	class IAssetPackageByteSource
	{
	public:
		virtual ~IAssetPackageByteSource() = default;
		virtual auto GetSize() const -> uint64 = 0;
		virtual auto ReadAt(uint64 Offset, FMutableByteView Output,
			std::optional<UnifiedError::FError>* OutError = nullptr) -> bool = 0;
	};

	class FFileAssetPackageByteSource final : public IAssetPackageByteSource
	{
	public:
		explicit FFileAssetPackageByteSource(std::unique_ptr<IFileHandle> InHandle)
			: Handle(std::move(InHandle)) {}
		auto GetSize() const -> uint64 override { return Handle ? Handle->Size() : 0; }
		auto ReadAt(uint64 Offset, FMutableByteView Output,
			std::optional<UnifiedError::FError>* OutError = nullptr) -> bool override
		{
			if (OutError) OutError->reset();
			if (!Handle)
			{
				if (OutError) OutError->emplace(PackageReadErrors::IoError, "Package byte source has no file handle.");
				return false;
			}
			auto Read = Handle->ReadAt(reinterpret_cast<uint8*>((Output).data()), static_cast<int64>(std::span(Output).size_bytes()), static_cast<int64>(Offset));
			if (!Read && OutError) *OutError = UnifiedError::FError{PackageReadErrors::IoError, std::format("Package file read failed at offset {}, size {}.", Offset, Output.size_bytes())};
			return Read;
		}
	private:
		std::unique_ptr<IFileHandle> Handle;
	};

	class FBorrowedFileAssetPackageByteSource final : public IAssetPackageByteSource
	{
	public:
		explicit FBorrowedFileAssetPackageByteSource(IFileHandle& InHandle)
			: Handle(InHandle) {}
		auto GetSize() const -> uint64 override { return Handle.Size(); }
		auto ReadAt(uint64 Offset, FMutableByteView Output,
			std::optional<UnifiedError::FError>* OutError = nullptr) -> bool override
		{
			if (OutError) OutError->reset();
			auto Read = Handle.ReadAt(reinterpret_cast<uint8*>((Output).data()), static_cast<int64>(std::span(Output).size_bytes()), static_cast<int64>(Offset));
			if (!Read && OutError) *OutError = UnifiedError::FError{PackageReadErrors::IoError, std::format("Package file read failed at offset {}, size {}.", Offset, Output.size_bytes())};
			return Read;
		}
	private:
		IFileHandle& Handle;
	};

	class FMemoryAssetPackageByteSource final : public IAssetPackageByteSource
	{
	public:
		explicit FMemoryAssetPackageByteSource(FByteView InBytes)
			: Bytes(InBytes) {}
		auto GetSize() const -> uint64 override { return Bytes.size(); }
		auto ReadAt(uint64 Offset, FMutableByteView Output,
			std::optional<UnifiedError::FError>* OutError = nullptr) -> bool override
		{
			if (OutError) OutError->reset();
			if (Offset > Bytes.size() || Output.size_bytes() > Bytes.size() - Offset)
			{
				if (OutError) OutError->emplace(PackageReadErrors::CorruptFile, "Asset package byte range exceeds the source.");
				return false;
			}
			std::ranges::copy(Bytes.subspan(static_cast<size_t>(Offset), Output.size()), Output.begin());
			return true;
		}
	private:
		FByteView Bytes;
	};

	class FCountingAssetPackageByteSource final : public IAssetPackageByteSource
	{
	public:
		explicit FCountingAssetPackageByteSource(IAssetPackageByteSource& InInner,
			FPackageSchemaReadStats& InStats)
			: Inner(InInner), Stats(InStats) {}
		auto GetSize() const -> uint64 override { return Inner.GetSize(); }
		auto ReadAt(uint64 Offset, FMutableByteView Output,
			std::optional<UnifiedError::FError>* OutError = nullptr) -> bool override
		{
			if (!Inner.ReadAt(Offset, Output, OutError)) { Stats.bReadFailed = true; return false; }
			Stats.MetadataBytesRead += Output.size_bytes();
			return true;
		}
	private:
		IAssetPackageByteSource& Inner;
		FPackageSchemaReadStats& Stats;
	};
}
