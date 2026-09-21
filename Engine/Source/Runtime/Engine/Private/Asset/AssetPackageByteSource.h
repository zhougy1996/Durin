#pragma once

#include "Asset/PackageSchema.h"
#include "Misc/FileIO.h"

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
		explicit FFileAssetPackageByteSource(std::unique_ptr<FFileIO::IFileHandle> InHandle)
			: Handle(std::move(InHandle)) {}
		auto GetSize() const -> uint64 override { return Handle ? Handle->GetSize() : 0; }
		auto ReadAt(uint64 Offset, FMutableByteView Output,
			std::string* OutError = nullptr) -> bool override
		{
			if (!Handle)
			{
				if (OutError) *OutError = "Package byte source has no file handle.";
				return false;
			}
			auto Read = Handle->ReadAt(Offset, Output);
			if (!Read && OutError) *OutError = Read.error().ToString();
			return Read.has_value();
		}
	private:
		std::unique_ptr<FFileIO::IFileHandle> Handle;
	};

	class FBorrowedFileAssetPackageByteSource final : public IAssetPackageByteSource
	{
	public:
		explicit FBorrowedFileAssetPackageByteSource(FFileIO::IFileHandle& InHandle)
			: Handle(InHandle) {}
		auto GetSize() const -> uint64 override { return Handle.GetSize(); }
		auto ReadAt(uint64 Offset, FMutableByteView Output,
			std::string* OutError = nullptr) -> bool override
		{
			auto Read = Handle.ReadAt(Offset, Output);
			if (!Read && OutError) *OutError = Read.error().ToString();
			return Read.has_value();
		}
	private:
		FFileIO::IFileHandle& Handle;
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
