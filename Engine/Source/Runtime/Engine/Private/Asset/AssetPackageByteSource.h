#pragma once

#include "Asset/PackageSchema.h"
#include "Misc/FileHelper.h"

namespace Durin::AssetPrivate
{
	class IAssetPackageByteSource
	{
	public:
		virtual ~IAssetPackageByteSource() = default;
		virtual auto GetSize() const -> uint64 = 0;
		virtual auto ReadAt(uint64 Offset, FMutableByteView Output,
			std::string* OutError = nullptr) -> bool = 0;
	};

	class FFileAssetPackageByteSource final : public IAssetPackageByteSource
	{
	public:
		explicit FFileAssetPackageByteSource(std::unique_ptr<FFileHelper::IFileHandle> InHandle)
			: Handle(std::move(InHandle)) {}
		auto GetSize() const -> uint64 override { return Handle ? Handle->GetSize() : 0; }
		auto ReadAt(uint64 Offset, FMutableByteView Output,
			std::string* OutError = nullptr) -> bool override
		{
			FFileHelper::FFileIoError Error;
			if (Handle && Handle->ReadAt(Offset, Output, &Error)) return true;
			if (OutError) *OutError = Error.ToString();
			return false;
		}
	private:
		std::unique_ptr<FFileHelper::IFileHandle> Handle;
	};

	class FBorrowedFileAssetPackageByteSource final : public IAssetPackageByteSource
	{
	public:
		explicit FBorrowedFileAssetPackageByteSource(FFileHelper::IFileHandle& InHandle)
			: Handle(InHandle) {}
		auto GetSize() const -> uint64 override { return Handle.GetSize(); }
		auto ReadAt(uint64 Offset, FMutableByteView Output,
			std::string* OutError = nullptr) -> bool override
		{
			FFileHelper::FFileIoError Error;
			if (Handle.ReadAt(Offset, Output, &Error)) return true;
			if (OutError) *OutError = Error.ToString();
			return false;
		}
	private:
		FFileHelper::IFileHandle& Handle;
	};

	class FMemoryAssetPackageByteSource final : public IAssetPackageByteSource
	{
	public:
		explicit FMemoryAssetPackageByteSource(FByteView InBytes)
			: Bytes(InBytes) {}
		auto GetSize() const -> uint64 override { return Bytes.size(); }
		auto ReadAt(uint64 Offset, FMutableByteView Output,
			std::string* OutError = nullptr) -> bool override
		{
			if (Offset > Bytes.size() || Output.size_bytes() > Bytes.size() - Offset)
			{
				if (OutError) *OutError = "Asset package byte range exceeds the source.";
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
			std::string* OutError = nullptr) -> bool override
		{
			if (!Inner.ReadAt(Offset, Output, OutError)) return false;
			Stats.MetadataBytesRead += Output.size_bytes();
			return true;
		}
	private:
		IAssetPackageByteSource& Inner;
		FPackageSchemaReadStats& Stats;
	};
}
