#include "ShaderCookInput.h"
#include "ShaderBuilder.h"
#include "ShaderBuildVersion.h"
#include "ShaderCaptureLimits.h"
#include "Shader/ShaderCompiledOutput.h"
#include "Shader/ShaderData.h"
#include "ShaderBuild/ShaderPaths.h"
#include "Misc/FileHelper.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	namespace
	{
		using namespace ShaderCaptureLimits;
		constexpr size_t ReadChunkBytes = 4 * 1024 * 1024;

		auto CaptureLimitFailure(std::string_view Kind,
			uint64 Maximum, uint64 Actual, std::string_view Path = {}) -> FShaderCompletion
		{
			return FShaderCompletion::Failure(std::format("Shader capture {} limit exceeded for '{}': maximum {}, actual {}.", Kind, Path, Maximum, Actual));
		}
	}

	auto BuildCookShaderInputIdentity(FShaderBuilder& Builder, std::string& OutIdentity, const std::function<bool()>& IsCancelled) -> FShaderCompletion
	{
		OutIdentity.clear();
		if (IsCancelled && IsCancelled()) return FShaderCompletion::Canceled();
		const auto Mounts = FShaderPaths::GetRegisteredMountPoints();
		if (Mounts.size() > MaximumMounts)
			return CaptureLimitFailure("mount count", MaximumMounts, Mounts.size());
		uint64 Entries = 0;
		std::map<std::string, FXxHash128> Files;
		std::vector<std::string> SearchRoots;
		uint64 TotalBytes = 0;
		std::error_code Error;
		for (const auto& Mount : Mounts)
		{
			const std::filesystem::path Root(Mount.SourceDir);
			SearchRoots.push_back(Mount.VirtualRoot);
			auto FailurePath = Root;
			std::filesystem::recursive_directory_iterator It(Root, Error), End;
			if (Error) { return std::unexpected(FFileError{EFileOperation::Inspect, Error, FailurePath}.ToString()); }
			for (; It != End; It.increment(Error))
			{
				if (IsCancelled && IsCancelled()) { return FShaderCompletion::Canceled(); }
				if (++Entries > MaximumDirectoryEntries)
					return CaptureLimitFailure("directory entry count", MaximumDirectoryEntries, Entries, Root.generic_string());
				if (Error) { return std::unexpected(FFileError{EFileOperation::Inspect, Error, FailurePath}.ToString()); }
				FailurePath = It->path();
				if (It->is_symlink(Error))
				{ return std::unexpected(std::format("Shader capture requires regular source files, not symbolic links: '{}'.", (It->path().generic_string()))); }
				if (Error) { return std::unexpected(FFileError{EFileOperation::Inspect, Error, FailurePath}.ToString()); }
				if (!It->is_regular_file(Error))
				{
					if (Error) { return std::unexpected(FFileError{EFileOperation::Inspect, Error, FailurePath}.ToString()); }
					continue;
				}
				const auto Name = (std::filesystem::path(Mount.VirtualRoot)
					/ It->path().lexically_relative(Root)).lexically_normal().generic_string();
				auto File = FFileHelper::OpenRead(It->path());
				if (!File) { return std::unexpected((File.error()).ToString()); }
				const auto Size = (*File)->GetSize();
				if (Files.size() >= MaximumFiles)
					return CaptureLimitFailure("file count", MaximumFiles, Files.size() + 1, Name);
				if (Name.size() > MaximumPathBytes)
					return CaptureLimitFailure("path bytes", MaximumPathBytes, Name.size(), Name);
				if (Size > MaximumFileBytes)
					return CaptureLimitFailure("file bytes", MaximumFileBytes, Size, Name);
				if (Size > MaximumTotalBytes - TotalBytes)
					return CaptureLimitFailure("total bytes", MaximumTotalBytes, TotalBytes + Size, Name);
				FByteBuffer Bytes(static_cast<size_t>(Size));
				for (size_t Offset = 0; Offset < Bytes.size(); Offset += ReadChunkBytes)
				{
					if (IsCancelled && IsCancelled()) { return FShaderCompletion::Canceled(); }
					if (auto Read = (*File)->ReadAt(Offset, std::span(Bytes).subspan(Offset, std::min(ReadChunkBytes, Bytes.size() - Offset))); !Read)
					{ return std::unexpected((Read.error()).ToString()); }
				}
				TotalBytes += Size;
				if (!Files.emplace(Name, FXxHash128::HashBuffer(Bytes)).second)
				{ return std::unexpected(std::format("Shader capture contains duplicate logical file '{}'.", (Name))); }
			}
			if (Error) { return std::unexpected(FFileError{EFileOperation::Inspect, Error, FailurePath}.ToString()); }
		}
		FBinaryWriter Identity;
		Identity.WriteU64(Builder.GetBuildVersion());
		Identity.WriteU32(ShaderBuildPrivate::LibraryProductionRevision);
		const auto Descriptor = GetShaderBuildDescriptor();
		Identity.WriteU32(Descriptor.Version);
		Identity.WriteU32(Descriptor.ConstantsSchema);
		Identity.WriteU32(Descriptor.OutputSchema);
		Identity.WriteU32(ShaderCompiledOutput::PayloadSchemaVersion);
		Identity.WriteU32(ShaderCookedLibrarySchemaVersion);
		Identity.WriteString(Builder.GetCompilerEnvironmentIdentity());
		Identity.WriteU32(static_cast<uint32>(SearchRoots.size()));
		for (const auto& Root : SearchRoots) Identity.WriteString(Root);
		Identity.WriteU32(static_cast<uint32>(Files.size()));
		for (const auto& [Name, Digest] : Files)
		{
			Identity.WriteString(Name); Identity.WriteHash128(Digest);
		}
		const auto Digest = FXxHash128::HashBuffer(Identity.GetBytes());
		if (IsCancelled && IsCancelled()) return FShaderCompletion::Canceled();
		OutIdentity = std::format("{:016x}{:016x}", Digest.HashHigh, Digest.HashLow);
		return {};
	}
}
