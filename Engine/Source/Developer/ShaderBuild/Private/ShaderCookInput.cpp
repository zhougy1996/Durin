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

		auto CaptureLimitFailure(EShaderError Code, EShaderCaptureLimit Kind,
			uint64 Maximum, uint64 Actual, std::string_view Path = {}) -> FShaderOperationResult
		{
			return std::unexpected(FShaderError{.Code = Code, .ActualIdentity = std::string(Path),
				.CaptureLimit = FShaderCaptureLimitContext{Kind, Maximum, Actual}});
		}
	}

	auto BuildCookShaderInputIdentity(FShaderBuilder& Builder, std::string& OutIdentity, const std::function<bool()>& IsCancelled) -> FShaderOperationResult
	{
		OutIdentity.clear();
		const auto Mounts = FShaderPaths::GetRegisteredMountPoints();
		if (Mounts.size() > MaximumMounts)
			return CaptureLimitFailure(EShaderError::CaptureMountLimit,
				EShaderCaptureLimit::Mounts, MaximumMounts, Mounts.size());
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
			if (Error) { return std::unexpected(FShaderError::FromFileSystem(FailurePath, Error)); }
			for (; It != End; It.increment(Error))
			{
				if (IsCancelled && IsCancelled()) { return std::unexpected(FShaderError{.Code = EShaderError::Cancelled}); }
				if (++Entries > MaximumDirectoryEntries)
					return CaptureLimitFailure(EShaderError::CaptureDirectoryLimit,
						EShaderCaptureLimit::DirectoryEntries, MaximumDirectoryEntries, Entries, Root.generic_string());
				if (Error) { return std::unexpected(FShaderError::FromFileSystem(FailurePath, Error)); }
				FailurePath = It->path();
				if (It->is_symlink(Error))
				{ return std::unexpected(FShaderError{.Code = EShaderError::CaptureSymlink, .ActualIdentity = It->path().generic_string()}); }
				if (Error) { return std::unexpected(FShaderError::FromFileSystem(FailurePath, Error)); }
				if (!It->is_regular_file(Error))
				{
					if (Error) { return std::unexpected(FShaderError::FromFileSystem(FailurePath, Error)); }
					continue;
				}
				const auto Name = (std::filesystem::path(Mount.VirtualRoot)
					/ It->path().lexically_relative(Root)).lexically_normal().generic_string();
				auto File = FFileHelper::OpenRead(It->path());
				if (!File) { return std::unexpected(FShaderError{.Code = EShaderError::FileReadFailure, .FileError = File.error()}); }
				const auto Size = (*File)->GetSize();
				if (Files.size() >= MaximumFiles)
					return CaptureLimitFailure(EShaderError::CaptureInputLimit,
						EShaderCaptureLimit::Files, MaximumFiles, Files.size() + 1, Name);
				if (Name.size() > MaximumPathBytes)
					return CaptureLimitFailure(EShaderError::CaptureInputLimit,
						EShaderCaptureLimit::PathBytes, MaximumPathBytes, Name.size(), Name);
				if (Size > MaximumFileBytes)
					return CaptureLimitFailure(EShaderError::CaptureInputLimit,
						EShaderCaptureLimit::FileBytes, MaximumFileBytes, Size, Name);
				if (Size > MaximumTotalBytes - TotalBytes)
					return CaptureLimitFailure(EShaderError::CaptureInputLimit,
						EShaderCaptureLimit::TotalBytes, MaximumTotalBytes, TotalBytes + Size, Name);
				FByteBuffer Bytes(static_cast<size_t>(Size));
				for (size_t Offset = 0; Offset < Bytes.size(); Offset += ReadChunkBytes)
				{
					if (IsCancelled && IsCancelled()) { return std::unexpected(FShaderError{.Code = EShaderError::Cancelled}); }
					if (auto Read = (*File)->ReadAt(Offset, std::span(Bytes).subspan(Offset, std::min(ReadChunkBytes, Bytes.size() - Offset))); !Read)
					{ return std::unexpected(FShaderError{.Code = EShaderError::FileReadFailure, .FileError = Read.error()}); }
				}
				TotalBytes += Size;
				if (!Files.emplace(Name, FXxHash128::HashBuffer(Bytes)).second)
				{ return std::unexpected(FShaderError{.Code = EShaderError::CaptureDuplicateFile, .ActualIdentity = Name}); }
			}
			if (Error) { return std::unexpected(FShaderError::FromFileSystem(FailurePath, Error)); }
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
		OutIdentity = std::format("{:016x}{:016x}", Digest.HashHigh, Digest.HashLow);
		return {};
	}
}
