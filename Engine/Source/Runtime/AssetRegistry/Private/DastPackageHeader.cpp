#include "AssetRegistry/PackageHeader.h"

#include "DObject/PackageFormat.h"
#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"

#include <fstream>

namespace Durin
{
	namespace
	{
		constexpr uint64 MaximumHeaderBytes = 16ull * 1024ull * 1024ull;
		constexpr uint64 MaximumFileBytes = 1024ull * 1024ull * 1024ull;
		constexpr FBinaryEnvelopeLimits EnvelopeLimits{
			MaximumHeaderBytes, MaximumFileBytes};

		auto Error(EAssetRegistryError Code, std::string Message) -> FAssetRegistryResult
		{
			return {Code, std::move(Message)};
		}

		auto ReaderError(const ObjectPackage::FPackageReaderDiagnostic& Diagnostic)
			-> FAssetRegistryResult
		{
			return Error(EAssetRegistryError::CorruptFile,
				std::format("DAST v9 Registry projection failed: {}",
					Diagnostic.Message));
		}
	}

	auto ReadAssetPackageHeaderBytes(FByteView FrontMatter,
		uint64 PhysicalFileBytes, uint64 PhysicalBulkBytes,
		const FPackagePath& PackagePath, FAssetPackageHeader& OutHeader)
		-> FAssetRegistryResult
	{
		OutHeader = {};
		FBinaryEnvelopePreamble Preamble;
		FBinaryEnvelopeDiagnostic EnvelopeDiagnostic;
		if (!ParseBinaryEnvelopePrefix(FrontMatter, PhysicalFileBytes,
			EnvelopeLimits, Preamble, &EnvelopeDiagnostic))
			return Error(EAssetRegistryError::CorruptFile,
				std::string(EnvelopeDiagnostic.Message));
		if (Preamble.FormatId != ObjectPackage::DastFormatId
			|| Preamble.FormatVersion != ObjectPackage::DastV9FormatVersion)
			return Error(EAssetRegistryError::UnsupportedVersion,
				std::format("Unsupported DAST package format version {}.",
					Preamble.FormatVersion));
		if (Preamble.HeaderBytes > FrontMatter.size())
			return Error(EAssetRegistryError::CorruptFile,
				"DAST v9 front matter is truncated.");
		if (!PackagePath.IsValid())
			return Error(EAssetRegistryError::InvalidPath,
				"DAST v9 Registry projection requires the mounted package identity.");

		ObjectPackage::FPackageV9RegistryData Registry;
		ObjectPackage::FPackageReaderDiagnostic ReaderDiagnostic;
		if (!ObjectPackage::ReadPackageV9Registry(
			FrontMatter.first(static_cast<size_t>(Preamble.HeaderBytes)),
			PhysicalFileBytes, PhysicalBulkBytes, PackagePath,
			Registry, &ReaderDiagnostic))
			return ReaderError(ReaderDiagnostic);

		FAssetPackageHeader Header{
			.PackagePath = Registry.PackagePath,
			.FormatVersion = Preamble.FormatVersion,
			.ObjectCount = Registry.ExportCount,
			.BulkSegmentExtent = Registry.ExternalBulkBytes,
			.BulkSegmentDigest = Registry.ExternalBulkHash,
			.BytesRead = Preamble.HeaderBytes};
		for (auto& Asset : Registry.TopLevelAssets)
			Header.TopLevelAssets.push_back({.AssetPath = std::move(Asset.AssetPath),
				.AssetClassName = std::move(Asset.ClassName),
				.RedirectDestination = std::move(Asset.RedirectDestination)});
		Header.Dependencies.assign(Registry.HardPackageReferences.begin(),
			Registry.HardPackageReferences.end());
		Header.SoftDependencies.assign(Registry.SoftPackageReferences.begin(),
			Registry.SoftPackageReferences.end());
		if (!Header.TopLevelAssets.empty())
		{
			const auto& First = Header.TopLevelAssets.front();
			Header.AssetClassName = First.AssetClassName;
			Header.EntryKind = First.RedirectDestination.IsValid()
				? EAssetRegistryEntryKind::Redirector : EAssetRegistryEntryKind::Asset;
			Header.RedirectDestination = First.RedirectDestination.GetPackagePath();
		}
		Header.SearchableNames = std::move(Registry.SearchableNames);
		OutHeader = std::move(Header);
		return {};
	}

	auto ReadAssetPackageHeader(std::string_view PhysicalPath,
		const FPackagePath& PackagePath, FAssetPackageHeader& OutHeader)
		-> FAssetRegistryResult
	{
		OutHeader = {};
		std::ifstream Stream(
			std::filesystem::path(PhysicalPath), std::ios::binary | std::ios::ate);
		if (!Stream)
			return Error(EAssetRegistryError::IoError,
				std::format("Failed to open asset package {}.", PhysicalPath));
		const auto End = Stream.tellg();
		if (End < 0)
			return Error(EAssetRegistryError::IoError,
				std::format("Failed to size asset package {}.", PhysicalPath));
		const uint64 FileSize = static_cast<uint64>(End);
		if (FileSize > MaximumFileBytes)
			return Error(EAssetRegistryError::CorruptFile,
				"Asset package exceeds the supported byte bound.");
		Stream.seekg(0);
		const uint64 InitialSize = std::min<uint64>(
			FileSize, BinaryEnvelopePreambleBytes);
		FByteBuffer Bytes(static_cast<size_t>(InitialSize));
		if (InitialSize != 0)
		{
			Stream.read(reinterpret_cast<char*>(Bytes.data()),
				static_cast<std::streamsize>(InitialSize));
			if (!Stream)
				return Error(EAssetRegistryError::IoError,
					std::format("Failed to read asset package {}.", PhysicalPath));
		}
		uint32 Magic = 0;
		if (Bytes.size() >= sizeof(Magic))
			std::memcpy(&Magic, Bytes.data(), sizeof(Magic));
		if (Magic == ObjectPackage::DastPackageMagic)
		{
			if (Bytes.size() < sizeof(uint32) * 2)
				return Error(EAssetRegistryError::CorruptFile, "Truncated asset header.");
			uint32 LegacyVersion = 0;
			std::memcpy(&LegacyVersion,
				Bytes.data() + sizeof(Magic), sizeof(LegacyVersion));
			return Error(EAssetRegistryError::UnsupportedVersion,
				std::format("Unsupported legacy DAST prefix version {}.",
					LegacyVersion));
		}

		uint64 HeaderBytes = InitialSize;
		if (Bytes.size() >= BinaryEnvelopePreambleBytes)
		{
			uint64 Declared = 0;
			if (!ReadLittleEndianAt(Bytes, 32, Declared)
				|| Declared < BinaryEnvelopePreambleBytes
				|| Declared > MaximumHeaderBytes || Declared > FileSize)
				return Error(EAssetRegistryError::CorruptFile,
					"Asset package declares an invalid front-matter extent.");
			HeaderBytes = Declared;
			if (Declared > Bytes.size())
			{
				const size_t Previous = Bytes.size();
				Bytes.resize(static_cast<size_t>(Declared));
				Stream.read(reinterpret_cast<char*>(Bytes.data() + Previous),
					static_cast<std::streamsize>(Declared - Previous));
				if (!Stream)
					return Error(EAssetRegistryError::IoError,
						std::format("Failed to read asset package {}.",
							PhysicalPath));
			}
		}

		uint64 BulkBytes = 0;
		std::filesystem::path BulkPath(PhysicalPath);
		BulkPath.replace_extension(".dbulk");
		std::error_code BulkEc;
		if (std::filesystem::exists(BulkPath, BulkEc))
		{
			const uintmax_t Extent = std::filesystem::file_size(BulkPath, BulkEc);
			if (BulkEc || Extent > ObjectPackage::DastV8MaximumBulkBytes)
				return Error(EAssetRegistryError::CorruptFile,
					"Asset package bulk segment exceeds the supported byte bound.");
			BulkBytes = static_cast<uint64>(Extent);
		}
		FAssetRegistryResult Result = ReadAssetPackageHeaderBytes(
			Bytes, FileSize, BulkBytes, PackagePath, OutHeader);
		if (Result) OutHeader.FileBytesRead = HeaderBytes;
		return Result;
	}
}
