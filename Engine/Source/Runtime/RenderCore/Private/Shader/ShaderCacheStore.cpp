#include "ShaderCacheStore.h"

#include "Hash/XxHash.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Misc/StringConvert.h"
#include "Shader/ShaderCompilerCore.h"
#include "Shader/ShaderPaths.h"

#include <atomic>
#include <unordered_set>

#ifdef _WIN32
#include "Windows/WindowsPlatform.h"
#else
#include <unistd.h>
#endif

namespace Durin
{
	namespace
	{
		auto EntryPointToString(const char8* EntryPoint) -> std::string
		{
			return EntryPoint != nullptr ? std::string(EntryPoint) : std::string();
		}

		auto ValidateEntryPointCounts(const FShaderCompileOptions& Options, std::string_view OperationName) -> bool
		{
			if (Options.EntryPoints.size() != Options.Frequencies.size())
			{
				DURIN_WARN("Shader cache {} skipped because entry point count does not match shader frequency count.", OperationName);
				return false;
			}

			return true;
		}

		auto BuildBinaryCachePaths(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, std::vector<std::string>& OutCachePaths) -> bool
		{
			OutCachePaths.clear();
			OutCachePaths.reserve(Options.EntryPoints.size());

			std::unordered_set<std::string> UniqueCachePaths;
			UniqueCachePaths.reserve(Options.EntryPoints.size());

			for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
			{
				OutCachePaths.push_back(FShaderPaths::BinaryPath(VirtualShaderPath, EntryPointToString(Options.EntryPoints[Index]), Options.Frequencies[Index], VariantKey.Hex));
				const std::string& CachePath = OutCachePaths.back();
				if (!UniqueCachePaths.insert(CachePath).second)
				{
					DURIN_WARN("Shader binary cache skipped because multiple entry points map to the same file: {}", CachePath);
					return false;
				}
			}

			return true;
		}

		auto MakeTemporaryPath(const std::filesystem::path& TargetPath) -> std::filesystem::path
		{
			static std::atomic_uint64_t Counter = 0;
			const uint64 Suffix = Counter.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
			const uint64 ProcessId = static_cast<uint64>(GetCurrentProcessId());
#else
			const uint64 ProcessId = static_cast<uint64>(getpid());
#endif
			return TargetPath.parent_path() / std::format(".tmp.{}.{:016x}", ProcessId, Suffix);
		}

		auto ReplaceFileAtomically(const std::filesystem::path& TemporaryPath, const std::filesystem::path& TargetPath) -> bool
		{
#ifdef _WIN32
			if (!MoveFileExW(TemporaryPath.c_str(), TargetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				DURIN_WARN("Failed to atomically replace shader cache file {} (error {})", TargetPath.generic_string(), GetLastError());
				return false;
			}
#else
			std::error_code ErrorCode;
			std::filesystem::rename(TemporaryPath, TargetPath, ErrorCode);
			if (ErrorCode)
			{
				DURIN_WARN("Failed to atomically replace shader cache file {}: {}", TargetPath.generic_string(), ErrorCode.message());
				return false;
			}
#endif
			return true;
		}

		auto SaveBytesAtomically(std::span<const std::byte> Bytes, const std::filesystem::path& TargetPath) -> bool
		{
			const std::filesystem::path TemporaryPath = MakeTemporaryPath(TargetPath);
			if (!FFileHelper::SaveArrayToFile(Bytes, TemporaryPath))
			{
				return false;
			}

			if (ReplaceFileAtomically(TemporaryPath, TargetPath))
			{
				return true;
			}

			std::error_code Ignored;
			std::filesystem::remove(TemporaryPath, Ignored);
			return false;
		}

		auto SaveJsonAtomically(FJsonDocument& Document, const std::filesystem::path& TargetPath) -> bool
		{
			const std::string JsonText = Document.ToString();
			return !JsonText.empty() && SaveBytesAtomically(std::as_bytes(std::span(JsonText)), TargetPath);
		}

		constexpr uint32 GSpirvMagicNumber = 0x07230203u;
		constexpr size_t GMaximumShaderBytecodeSize = 64u * 1024u * 1024u;
		constexpr size_t GMaximumReflectionFileSize = 8u * 1024u * 1024u;
		constexpr size_t GMaximumReflectionEntries = 65536u;
		constexpr uint32 GMaximumDescriptorIndex = 65535u;
		constexpr uint32 GMaximumPushConstantBytes = 65536u;

		auto IsValidCacheKey(std::string_view Key) -> bool
		{
			return StringUtils::IsHex(Key, 32);
		}

		auto IsValidFrequency(uint64 Value) -> bool
		{
			return Value <= static_cast<uint64>(EShaderFrequency::RayMiss);
		}

		auto IsValidStageFlags(uint64 Value) -> bool
		{
			constexpr uint64 ValidMask = static_cast<uint64>(EShaderStageFlags::Vertex)
				| static_cast<uint64>(EShaderStageFlags::Fragment)
				| static_cast<uint64>(EShaderStageFlags::Compute)
				| static_cast<uint64>(EShaderStageFlags::Geometry);
			return (Value & ~ValidMask) == 0;
		}

		auto IsValidBindingType(uint64 Value) -> bool
		{
			return Value <= static_cast<uint64>(ERHIBindingType::StorageImage);
		}

		// Version 6 requires strict artifact/request validation on load.
		constexpr uint32 GShaderReflectionVersion = 6;

		auto SaveShaderReflection(const std::string& FilePath, const FCompiledShader& CompiledShader) -> bool
		{
			FJsonDocument Document;
			FJsonNodeRef Root = Document.GetMutableRoot();
			Root.EnsureObject();
			Root.SetChildValue("Version", GShaderReflectionVersion);
			Root.SetChildValue("Frequency", static_cast<uint32>(CompiledShader.Frequency));
			Root.SetChildValue("SourceEntryPoint", CompiledShader.SourceEntryPoint);
			Root.SetChildValue("BinaryEntryPoint", CompiledShader.BinaryEntryPoint);
			Root.SetChildValue("DebugName", CompiledShader.DebugName);
			Root.SetChildValue("Hash", CompiledShader.Hash.ToString());

			FJsonNodeRef ResourceBindings = Root.AddArray("ResourceBindings");
			for (const FShaderResourceBinding& Binding : CompiledShader.Reflection.ResourceBindings)
			{
				FJsonNodeRef BindingNode = ResourceBindings.AppendObject();
				BindingNode.SetChildValue("Name", Binding.Name);
				BindingNode.SetChildValue("StageFlags", static_cast<uint32>(Binding.StageFlags));
				BindingNode.SetChildValue("SetIndex", Binding.SetIndex);
				BindingNode.SetChildValue("BindingIndex", Binding.BindingIndex);
				BindingNode.SetChildValue("Type", static_cast<uint32>(Binding.Type));
				BindingNode.SetChildValue("ArraySize", Binding.ArraySize);
			}

			FJsonNodeRef PushConstantRanges = Root.AddArray("PushConstantRanges");
			for (const FPushConstantRange& Range : CompiledShader.Reflection.PushConstantRanges)
			{
				FJsonNodeRef RangeNode = PushConstantRanges.AppendObject();
				RangeNode.SetChildValue("StageFlags", static_cast<uint32>(Range.StageFlags));
				RangeNode.SetChildValue("Offset", Range.Offset);
				RangeNode.SetChildValue("Size", Range.Size);
			}

			return SaveJsonAtomically(Document, FilePath);
		}

		auto LoadShaderReflection(
			const std::string& FilePath,
			std::string_view ExpectedEntryPoint,
			EShaderFrequency ExpectedFrequency,
			const FXxHash128& ExpectedHash,
			FCompiledShader& OutCompiledShader
		) -> bool
		{
			std::error_code FileError;
			const uintmax_t FileSize = std::filesystem::file_size(FilePath, FileError);
			if (FileError || FileSize == 0 || FileSize > GMaximumReflectionFileSize)
			{
				return false;
			}

			FJsonDocument Document;
			if (!Document.LoadFromFile(FilePath))
			{
				return false;
			}

			const FJsonNodeView Root = Document.GetRootView();
			if (!Root.IsObject() || Root.GetView("Version").GetUInt() != GShaderReflectionVersion)
			{
				return false;
			}

			const FJsonNodeView FrequencyNode = Root.GetView("Frequency");
			const FJsonNodeView SourceEntryPointNode = Root.GetView("SourceEntryPoint");
			const FJsonNodeView BinaryEntryPointNode = Root.GetView("BinaryEntryPoint");
			const FJsonNodeView DebugNameNode = Root.GetView("DebugName");
			const FJsonNodeView HashNode = Root.GetView("Hash");
			if (!FrequencyNode.IsUInt()
				|| !SourceEntryPointNode.IsString()
				|| !BinaryEntryPointNode.IsString()
				|| !DebugNameNode.IsString()
				|| !HashNode.IsString())
			{
				return false;
			}

			const uint64 FrequencyValue = FrequencyNode.GetUInt();
			const std::string SourceEntryPoint = SourceEntryPointNode.GetString();
			const std::string HashString = HashNode.GetString();
			if (!IsValidFrequency(FrequencyValue)
				|| static_cast<EShaderFrequency>(FrequencyValue) != ExpectedFrequency
				|| SourceEntryPoint != ExpectedEntryPoint
				|| !StringUtils::IsHex(HashString, 32)
				|| FXxHash128::FromString(HashString) != ExpectedHash)
			{
				return false;
			}

			OutCompiledShader.Frequency = ExpectedFrequency;
			OutCompiledShader.SourceEntryPoint = SourceEntryPoint;
			OutCompiledShader.BinaryEntryPoint = BinaryEntryPointNode.GetString();
			if (OutCompiledShader.BinaryEntryPoint.empty())
			{
				OutCompiledShader.BinaryEntryPoint = "main";
			}
			OutCompiledShader.DebugName = DebugNameNode.GetString();
			OutCompiledShader.Hash = ExpectedHash;
			OutCompiledShader.Reflection = {};

			const FJsonNodeView ResourceBindings = Root.GetView("ResourceBindings");
			if (!ResourceBindings.IsArray() || ResourceBindings.Num() > GMaximumReflectionEntries)
			{
				return false;
			}

			for (size_t Index = 0; Index < ResourceBindings.Num(); ++Index)
			{
				const FJsonNodeView BindingView = ResourceBindings.GetView(Index);
				if (!BindingView.IsObject())
				{
					return false;
				}

				const FJsonNodeView NameNode = BindingView.GetView("Name");
				const FJsonNodeView StageFlagsNode = BindingView.GetView("StageFlags");
				const FJsonNodeView SetIndexNode = BindingView.GetView("SetIndex");
				const FJsonNodeView BindingIndexNode = BindingView.GetView("BindingIndex");
				const FJsonNodeView TypeNode = BindingView.GetView("Type");
				const FJsonNodeView ArraySizeNode = BindingView.GetView("ArraySize");
				if (!NameNode.IsString() || !StageFlagsNode.IsUInt() || !SetIndexNode.IsUInt()
					|| !BindingIndexNode.IsUInt() || !TypeNode.IsUInt() || !ArraySizeNode.IsUInt())
				{
					return false;
				}

				const uint64 StageFlags = StageFlagsNode.GetUInt();
				const uint64 SetIndex = SetIndexNode.GetUInt();
				const uint64 BindingIndex = BindingIndexNode.GetUInt();
				const uint64 Type = TypeNode.GetUInt();
				const uint64 ArraySize = ArraySizeNode.GetUInt();
				if (!IsValidStageFlags(StageFlags) || SetIndex > GMaximumDescriptorIndex
					|| BindingIndex > GMaximumDescriptorIndex || !IsValidBindingType(Type)
					|| ArraySize == 0 || ArraySize > GMaximumReflectionEntries)
				{
					return false;
				}

				FShaderResourceBinding Binding;
				Binding.Name = NameNode.GetString();
				Binding.StageFlags = static_cast<EShaderStageFlags>(StageFlags);
				Binding.SetIndex = static_cast<uint32>(SetIndex);
				Binding.BindingIndex = static_cast<uint32>(BindingIndex);
				Binding.Type = static_cast<ERHIBindingType>(Type);
				Binding.ArraySize = static_cast<uint32>(ArraySize);
				OutCompiledShader.Reflection.ResourceBindings.push_back(std::move(Binding));
			}

			const FJsonNodeView PushConstantRanges = Root.GetView("PushConstantRanges");
			if (!PushConstantRanges.IsArray() || PushConstantRanges.Num() > GMaximumReflectionEntries)
			{
				return false;
			}

			for (size_t Index = 0; Index < PushConstantRanges.Num(); ++Index)
			{
				const FJsonNodeView RangeView = PushConstantRanges.GetView(Index);
				if (!RangeView.IsObject())
				{
					return false;
				}

				const FJsonNodeView StageFlagsNode = RangeView.GetView("StageFlags");
				const FJsonNodeView OffsetNode = RangeView.GetView("Offset");
				const FJsonNodeView SizeNode = RangeView.GetView("Size");
				if (!StageFlagsNode.IsUInt() || !OffsetNode.IsUInt() || !SizeNode.IsUInt())
				{
					return false;
				}

				const uint64 StageFlags = StageFlagsNode.GetUInt();
				const uint64 Offset = OffsetNode.GetUInt();
				const uint64 Size = SizeNode.GetUInt();
				if (!IsValidStageFlags(StageFlags) || Size == 0 || Offset > GMaximumPushConstantBytes
					|| Size > GMaximumPushConstantBytes || Offset + Size > GMaximumPushConstantBytes)
				{
					return false;
				}

				FPushConstantRange Range{};
				Range.StageFlags = static_cast<EShaderStageFlags>(StageFlags);
				Range.Offset = static_cast<uint32>(Offset);
				Range.Size = static_cast<uint32>(Size);
				OutCompiledShader.Reflection.PushConstantRanges.push_back(Range);
			}

			return true;
		}

		// Parser constants must match the dependency-manifest serialization schema.
		constexpr uint32 GShaderMetaVersion = 5;
		constexpr size_t GMaximumShaderDependencies = 4096;
		constexpr size_t GMaximumDependencyPathLength = 32768;
	}

	FShaderCacheStore::FShaderCacheStore(FShaderCacheRetentionPolicy InRetentionPolicy)
		: RetentionPolicy(InRetentionPolicy)
	{
	}
	FShaderCacheStore::~FShaderCacheStore() = default;

	auto FShaderCacheStore::LoadMetaData(std::string_view VirtualShaderPath, const FShaderDependencyKey& DependencyKey, FShaderMetaData& OutMetaData) -> bool
	{
		if (!IsValidCacheKey(DependencyKey.Hex)) return false;
		const std::string MetaPath = FShaderPaths::MetaPath(VirtualShaderPath, DependencyKey.Hex);
		if (!FFileHelper::FileExists(MetaPath))
		{
			return false;
		}

		FJsonDocument Document;
		if (!Document.LoadFromFile(MetaPath))
		{
			return false;
		}

		const FJsonNodeView Root = Document.GetRootView();
		if (!Root.IsObject()
			|| Root.GetView("Version").GetUInt() != GShaderMetaVersion)
		{
			return false;
		}

		OutMetaData = {};

		const std::string SourceTreeSignature = Root.GetView("SourceTreeSignature").GetString();
		if (!StringUtils::IsHex(SourceTreeSignature, 32))
		{
			return false;
		}
		OutMetaData.SourceTreeSignature = FXxHash128::FromString(SourceTreeSignature);

		const FJsonNodeView Dependencies = Root.GetView("Dependencies");
		if (!Dependencies.IsArray() || Dependencies.Num() == 0 || Dependencies.Num() > GMaximumShaderDependencies)
		{
			return false;
		}

		OutMetaData.Dependencies.reserve(Dependencies.Num());
		for (size_t Index = 0; Index < Dependencies.Num(); ++Index)
		{
			const FJsonNodeView DependencyNode = Dependencies.GetView(Index);
			const FJsonNodeView PathNode = DependencyNode.GetView("Path");
			const FJsonNodeView LastWriteTimeNode = DependencyNode.GetView("LastWriteTime");
			const FJsonNodeView FileSizeNode = DependencyNode.GetView("FileSize");
			const FJsonNodeView ContentHashNode = DependencyNode.GetView("ContentHash");
			if (!DependencyNode.IsObject() || !PathNode.IsString() || !LastWriteTimeNode.IsInt()
				|| !FileSizeNode.IsUInt() || !ContentHashNode.IsString())
			{
				return false;
			}

			FFileFingerprint Fingerprint;
			Fingerprint.NormalizedPath = PathNode.GetString();
			const std::string ContentHash = ContentHashNode.GetString();
			if (Fingerprint.NormalizedPath.empty() || Fingerprint.NormalizedPath.size() > GMaximumDependencyPathLength
				|| !StringUtils::IsHex(ContentHash, 16))
			{
				return false;
			}
			using FFileTimeRep = std::filesystem::file_time_type::duration::rep;
			Fingerprint.LastWriteTime = std::filesystem::file_time_type(
				std::filesystem::file_time_type::duration(static_cast<FFileTimeRep>(LastWriteTimeNode.GetInt())));
			Fingerprint.FileSize = FileSizeNode.GetUInt();
			Fingerprint.ContentHash = FXxHash64::FromString(ContentHash);
			OutMetaData.Dependencies.push_back(std::move(Fingerprint));
		}

		return true;
	}

	auto FShaderCacheStore::SaveMetaData(std::string_view VirtualShaderPath, const FShaderDependencyKey& DependencyKey, const FShaderMetaData& MetaData) -> bool
	{
		if (!IsValidCacheKey(DependencyKey.Hex) || MetaData.Dependencies.empty() || MetaData.Dependencies.size() > GMaximumShaderDependencies)
		{
			return false;
		}
		FJsonDocument Document;
		FJsonNodeRef Root = Document.GetMutableRoot();
		Root.EnsureObject();
		Root.SetChildValue("Version", GShaderMetaVersion);
		Root.SetChildValue("SourceTreeSignature", MetaData.SourceTreeSignature.ToString());
		FJsonNodeRef Dependencies = Root.AddArray("Dependencies");
		for (const FFileFingerprint& Fingerprint : MetaData.Dependencies)
		{
			FJsonNodeRef DependencyNode = Dependencies.AppendObject();
			DependencyNode.SetChildValue("Path", Fingerprint.NormalizedPath);
			DependencyNode.SetChildValue("LastWriteTime", static_cast<int64>(Fingerprint.LastWriteTime.time_since_epoch().count()));
			DependencyNode.SetChildValue("FileSize", Fingerprint.FileSize);
			DependencyNode.SetChildValue("ContentHash", Fingerprint.ContentHash.ToString());
		}
		return SaveJsonAtomically(Document, FShaderPaths::MetaPath(VirtualShaderPath, DependencyKey.Hex));
	}

	auto FShaderCacheStore::TryLoad(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, FShaderCompilerOutput& OutOutput) -> bool
	{
		const uint32 EntryPointCount = static_cast<uint32>(Options.EntryPoints.size());
		if (!IsValidCacheKey(VariantKey.Hex) || !ValidateEntryPointCounts(Options, "load"))
		{
			return false;
		}

		std::vector<std::string> CachePaths;
		if (!BuildBinaryCachePaths(VirtualShaderPath, Options, VariantKey, CachePaths))
		{
			return false;
		}

		OutOutput.CompiledShaders.clear();
		OutOutput.CompiledShaders.resize(EntryPointCount);

		for (uint32 EntryPointIndex = 0; EntryPointIndex < EntryPointCount; ++EntryPointIndex)
		{
			const std::string EntryPoint = EntryPointToString(Options.EntryPoints[EntryPointIndex]);
			const EShaderFrequency Frequency = Options.Frequencies[EntryPointIndex];
			const std::string SidecarPath = FShaderPaths::ReflectionPath(VirtualShaderPath, EntryPoint, Frequency, VariantKey.Hex);
			// Missing artifacts are an expected cache miss, so avoid the generic file loader warning.
			if (!FFileHelper::FileExists(CachePaths[EntryPointIndex]) || !FFileHelper::FileExists(SidecarPath))
			{
				return false;
			}

			std::vector<uint8> ShaderBytes;
			if (!FFileHelper::LoadFileToArray(ShaderBytes, CachePaths[EntryPointIndex]))
			{
				return false;
			}

			if (ShaderBytes.size() < 5 * sizeof(uint32)
				|| ShaderBytes.size() > GMaximumShaderBytecodeSize
				|| ShaderBytes.size() % sizeof(uint32) != 0)
			{
				return false;
			}

			uint32 Magic = 0;
			std::memcpy(&Magic, ShaderBytes.data(), sizeof(Magic));
			if (Magic != GSpirvMagicNumber)
			{
				return false;
			}

			auto& CompiledShader = OutOutput.CompiledShaders[EntryPointIndex];
			CompiledShader.Code = std::make_shared<std::vector<std::byte>>();
			CompiledShader.Code->resize(ShaderBytes.size());
			if (!ShaderBytes.empty())
			{
				std::memcpy(CompiledShader.Code->data(), ShaderBytes.data(), ShaderBytes.size());
			}

			const FXxHash128 CodeHash = FXxHash128::HashBuffer(*CompiledShader.Code);
			if (!LoadShaderReflection(SidecarPath, EntryPoint, Frequency, CodeHash, CompiledShader))
			{
				return false;
			}
		}

		OutOutput.bSucceeded = true;
		return true;
	}

	auto FShaderCacheStore::Save(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, const FShaderCompilerOutput& Output) -> bool
	{
		if (!IsValidCacheKey(VariantKey.Hex))
		{
			DURIN_WARN("Shader cache save skipped because the variant key is invalid.");
			return false;
		}
		if (Output.CompiledShaders.size() != Options.EntryPoints.size())
		{
			DURIN_WARN("Shader cache save skipped because compiler output count does not match requested entry point count.");
			return false;
		}
		if (!ValidateEntryPointCounts(Options, "save"))
		{
			return false;
		}
		// Keep publication and retention in one critical section so cleanup cannot observe another in-process writer's partial variant.
		std::lock_guard RetentionLock(RetentionMutex);

		std::vector<std::string> CachePaths;
		if (!BuildBinaryCachePaths(VirtualShaderPath, Options, VariantKey, CachePaths))
		{
			return false;
		}

		for (uint32 EntryPointIndex = 0; EntryPointIndex < Output.CompiledShaders.size(); ++EntryPointIndex)
		{
			const FCompiledShader& CompiledShader = Output.CompiledShaders[EntryPointIndex];
			if (!CompiledShader.Code)
			{
				DURIN_WARN("Shader cache save skipped because compiled shader code is null.");
				return false;
			}
			if (CompiledShader.SourceEntryPoint != EntryPointToString(Options.EntryPoints[EntryPointIndex])
				|| CompiledShader.Frequency != Options.Frequencies[EntryPointIndex]
				|| CompiledShader.Code->size() < 5 * sizeof(uint32)
				|| CompiledShader.Code->size() > GMaximumShaderBytecodeSize
				|| CompiledShader.Code->size() % sizeof(uint32) != 0
				|| FXxHash128::HashBuffer(*CompiledShader.Code) != CompiledShader.Hash)
			{
				DURIN_WARN("Shader cache save skipped because compiler output identity or bytecode hash is invalid.");
				return false;
			}

			uint32 Magic = 0;
			std::memcpy(&Magic, CompiledShader.Code->data(), sizeof(Magic));
			if (Magic != GSpirvMagicNumber)
			{
				DURIN_WARN("Shader cache save skipped because compiler output is not valid SPIR-V.");
				return false;
			}

			if (!SaveBytesAtomically(*CompiledShader.Code, CachePaths[EntryPointIndex]))
			{
				DURIN_WARN("Failed to write shader cache artifact: {}", CachePaths[EntryPointIndex]);
				return false;
			}

			const std::string SidecarPath = FShaderPaths::ReflectionPath(
				VirtualShaderPath,
				EntryPointToString(Options.EntryPoints[EntryPointIndex]),
				Options.Frequencies[EntryPointIndex],
				VariantKey.Hex
			);
			if (!SaveShaderReflection(SidecarPath, CompiledShader))
			{
				DURIN_WARN("Failed to write shader reflection cache artifact: {}", SidecarPath);
				return false;
			}
		}

		EnforceRetention(VirtualShaderPath, VariantKey.Hex);
		return true;
	}

	auto FShaderCacheStore::EnforceRetention(std::string_view VirtualShaderPath, std::string_view ProtectedVariantKey) -> void
	{
		struct FVariantDirectory
		{
			std::filesystem::path Path;
			std::string Name;
			uint64 Bytes = 0;
			std::filesystem::file_time_type LastWriteTime{};
		};

		std::error_code ErrorCode;
		std::filesystem::path ShaderRoot = std::filesystem::absolute(FShaderPaths::ShaderDirectory(VirtualShaderPath), ErrorCode).lexically_normal();
		if (ErrorCode) return;
		// ShaderDirectory uses a trailing separator; remove its empty filename before direct-child comparisons.
		if (ShaderRoot.filename().empty()) ShaderRoot = ShaderRoot.parent_path();

		std::vector<FVariantDirectory> Variants;
		uint64 TotalBytes = 0;
		for (std::filesystem::directory_iterator It(ShaderRoot, ErrorCode), End; !ErrorCode && It != End; It.increment(ErrorCode))
		{
			const std::filesystem::path Candidate = std::filesystem::absolute(It->path(), ErrorCode).lexically_normal();
			if (ErrorCode) break;
			const std::string Name = Candidate.filename().generic_string();
			const std::filesystem::file_status Status = It->symlink_status(ErrorCode);
			if (ErrorCode) break;
			if (!std::filesystem::is_directory(Status) || Candidate.parent_path() != ShaderRoot || !IsValidCacheKey(Name)) continue;

			FVariantDirectory Variant{.Path = Candidate, .Name = Name, .LastWriteTime = It->last_write_time(ErrorCode)};
			if (ErrorCode) break;
			for (std::filesystem::recursive_directory_iterator FileIt(Candidate, ErrorCode), FileEnd; !ErrorCode && FileIt != FileEnd; FileIt.increment(ErrorCode))
			{
				if (FileIt->is_regular_file(ErrorCode)) Variant.Bytes += FileIt->file_size(ErrorCode);
			}
			if (ErrorCode) break;
			TotalBytes += Variant.Bytes;
			Variants.push_back(std::move(Variant));
		}
		if (ErrorCode) return;

		std::ranges::sort(Variants, [](const FVariantDirectory& Left, const FVariantDirectory& Right) {
			return Left.LastWriteTime != Right.LastWriteTime ? Left.LastWriteTime < Right.LastWriteTime : Left.Name < Right.Name;
		});
		size_t RemainingVariants = Variants.size();
		const size_t AllowedVariants = std::max<size_t>(1, RetentionPolicy.MaxVariantsPerShader);
		for (const FVariantDirectory& Variant : Variants)
		{
			if (RemainingVariants <= AllowedVariants && TotalBytes <= RetentionPolicy.MaxBytesPerShader) break;
			if (Variant.Name == ProtectedVariantKey) continue;
			std::error_code RemoveError;
			std::filesystem::remove_all(Variant.Path, RemoveError);
			if (RemoveError)
			{
				DURIN_WARN("Failed to prune shader cache variant {}: {}", Variant.Path.generic_string(), RemoveError.message());
				continue;
			}
			TotalBytes -= Variant.Bytes;
			--RemainingVariants;
		}
	}
}
