#include "ShaderCacheStore.h"

#include "Hash/XxHash.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Misc/StringConvert.h"
#include "Shader/ShaderCompilerCore.h"
#include "Shader/ShaderPaths.h"

#include <unordered_set>

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

		auto ReflectionPath(std::string_view VirtualShaderPath, std::string_view EntryPoint, const FShaderVariantKey& VariantKey) -> std::string
		{
			const std::string FileName = String::SanitizeFileName(EntryPoint, "Shader") + ".reflect.json";
			return (std::filesystem::path(FShaderPaths::CacheDirectory(VirtualShaderPath, VariantKey.Hex)) / FileName).generic_string();
		}

		auto BuildBinaryCachePaths(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, std::vector<std::string>& OutCachePaths) -> bool
		{
			OutCachePaths.clear();
			OutCachePaths.reserve(Options.EntryPoints.size());

			std::unordered_set<std::string> UniqueCachePaths;
			UniqueCachePaths.reserve(Options.EntryPoints.size());

			for (const char8* EntryPoint : Options.EntryPoints)
			{
				OutCachePaths.push_back(FShaderPaths::BinaryPath(VirtualShaderPath, EntryPointToString(EntryPoint), VariantKey.Hex));
				const std::string& CachePath = OutCachePaths.back();
				if (!UniqueCachePaths.insert(CachePath).second)
				{
					DURIN_WARN("Shader binary cache skipped because multiple entry points map to the same file: {}", CachePath);
					return false;
				}
			}

			return true;
		}

		constexpr uint32 GShaderReflectionVersion = 4;

		auto SaveShaderReflection(const std::string& FilePath, const FCompiledShader& CompiledShader) -> bool
		{
			FJsonDocument Document;
			FJsonNodeRef Root = Document.GetMutableRoot();
			Root.EnsureObject();
			Root.SetUIntValue("Version", GShaderReflectionVersion);
			Root.SetUIntValue("Frequency", static_cast<uint32>(CompiledShader.Frequency));
			Root.SetStringValue("SourceEntryPoint", CompiledShader.SourceEntryPoint);
			Root.SetStringValue("BinaryEntryPoint", CompiledShader.BinaryEntryPoint);
			Root.SetStringValue("DebugName", CompiledShader.DebugName);
			Root.SetStringValue("Hash", CompiledShader.Hash.ToString());

			FJsonNodeRef ResourceBindings = Root.AddArray("ResourceBindings");
			for (const FShaderResourceBinding& Binding : CompiledShader.Reflection.ResourceBindings)
			{
				FJsonNodeRef BindingNode = ResourceBindings.AppendObject();
				BindingNode.SetStringValue("Name", Binding.Name);
				BindingNode.SetUIntValue("StageFlags", static_cast<uint32>(Binding.StageFlags));
				BindingNode.SetUIntValue("SetIndex", Binding.SetIndex);
				BindingNode.SetUIntValue("BindingIndex", Binding.BindingIndex);
				BindingNode.SetUIntValue("Type", static_cast<uint32>(Binding.Type));
				BindingNode.SetUIntValue("ArraySize", Binding.ArraySize);
			}

			FJsonNodeRef PushConstantRanges = Root.AddArray("PushConstantRanges");
			for (const FPushConstantRange& Range : CompiledShader.Reflection.PushConstantRanges)
			{
				FJsonNodeRef RangeNode = PushConstantRanges.AppendObject();
				RangeNode.SetUIntValue("StageFlags", static_cast<uint32>(Range.StageFlags));
				RangeNode.SetUIntValue("Offset", Range.Offset);
				RangeNode.SetUIntValue("Size", Range.Size);
			}

			return Document.SaveToFile(FilePath);
		}

		auto LoadShaderReflection(const std::string& FilePath, FCompiledShader& OutCompiledShader) -> bool
		{
			FJsonDocument Document;
			if (!Document.LoadFromFile(FilePath))
			{
				return false;
			}

			const FJsonNodeView Root = Document.GetRootView();
			if (!Root.IsObject() || Root.GetUIntValue("Version") != GShaderReflectionVersion)
			{
				return false;
			}

			const std::string HashString = Root.GetStringValue("Hash");
			if (!String::IsHex(HashString, 32))
			{
				return false;
			}

			OutCompiledShader.Frequency = static_cast<EShaderFrequency>(Root.GetUIntValue("Frequency"));
			OutCompiledShader.SourceEntryPoint = Root.GetStringValue("SourceEntryPoint");
			OutCompiledShader.BinaryEntryPoint = Root.GetStringValue("BinaryEntryPoint");
			if (OutCompiledShader.BinaryEntryPoint.empty())
			{
				OutCompiledShader.BinaryEntryPoint = "main";
			}
			OutCompiledShader.DebugName = Root.GetStringValue("DebugName");
			OutCompiledShader.Hash = FXxHash128::FromString(HashString);
			OutCompiledShader.Reflection = {};

			const FJsonNodeView ResourceBindings = Root.GetView("ResourceBindings");
			if (!ResourceBindings.IsArray())
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

				FShaderResourceBinding Binding;
				Binding.Name = BindingView.GetStringValue("Name");
				Binding.StageFlags = static_cast<EShaderStageFlags>(BindingView.GetUIntValue("StageFlags"));
				Binding.SetIndex = static_cast<uint32>(BindingView.GetUIntValue("SetIndex"));
				Binding.BindingIndex = static_cast<uint32>(BindingView.GetUIntValue("BindingIndex"));
				Binding.Type = static_cast<ERHIBindingType>(BindingView.GetUIntValue("Type"));
				Binding.ArraySize = static_cast<uint32>(BindingView.GetUIntValue("ArraySize", 1));
				OutCompiledShader.Reflection.ResourceBindings.push_back(std::move(Binding));
			}

			const FJsonNodeView PushConstantRanges = Root.GetView("PushConstantRanges");
			if (!PushConstantRanges.IsArray())
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

				FPushConstantRange Range{};
				Range.StageFlags = static_cast<EShaderStageFlags>(RangeView.GetUIntValue("StageFlags"));
				Range.Offset = static_cast<uint32>(RangeView.GetUIntValue("Offset"));
				Range.Size = static_cast<uint32>(RangeView.GetUIntValue("Size"));
				OutCompiledShader.Reflection.PushConstantRanges.push_back(Range);
			}

			return true;
		}

		// Parser constants must match the JSON document serialization schema.
		constexpr uint32 GShaderMetaVersion = 4;
	}

	FShaderCacheStore::FShaderCacheStore() = default;
	FShaderCacheStore::~FShaderCacheStore() = default;

	auto FShaderCacheStore::LoadMetaData(std::string_view VirtualShaderPath, FShaderMetaData& OutMetaData) -> bool
	{
		const std::string MetaPath = FShaderPaths::MetaPath(VirtualShaderPath);
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
			|| Root.GetUIntValue("Version") != GShaderMetaVersion)
		{
			return false;
		}

		OutMetaData = {};

		const std::string SourceTreeSignature = Root.GetStringValue("SourceTreeSignature");
		if (!String::IsHex(SourceTreeSignature, 32))
		{
			return false;
		}
		OutMetaData.SourceTreeSignature = FXxHash128::FromString(SourceTreeSignature);

		return true;
	}

	auto FShaderCacheStore::SaveMetaData(std::string_view VirtualShaderPath, const FShaderMetaData& MetaData) -> bool
	{
		FJsonDocument Document;
		FJsonNodeRef Root = Document.GetMutableRoot();
		Root.EnsureObject();
		Root.SetUIntValue("Version", GShaderMetaVersion);
		Root.SetStringValue("SourceTreeSignature", MetaData.SourceTreeSignature.ToString());
		return Document.SaveToFile(FShaderPaths::MetaPath(VirtualShaderPath));
	}

	auto FShaderCacheStore::TryLoad(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, FShaderCompilerOutput& OutOutput) -> bool
	{
		const uint32 EntryPointCount = static_cast<uint32>(Options.EntryPoints.size());
		if (!ValidateEntryPointCounts(Options, "load"))
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
			std::vector<uint8> ShaderBytes;
			if (!FFileHelper::LoadFileToArray(ShaderBytes, CachePaths[EntryPointIndex]))
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

			const std::string SidecarPath = ReflectionPath(VirtualShaderPath, EntryPointToString(Options.EntryPoints[EntryPointIndex]), VariantKey);
			if (!LoadShaderReflection(SidecarPath, CompiledShader))
			{
				return false;
			}
		}

		OutOutput.bSucceeded = true;
		return true;
	}

	auto FShaderCacheStore::Save(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, const FShaderCompilerOutput& Output) -> bool
	{
		if (Output.CompiledShaders.size() != Options.EntryPoints.size())
		{
			DURIN_WARN("Shader cache save skipped because compiler output count does not match requested entry point count.");
			return false;
		}
		if (!ValidateEntryPointCounts(Options, "save"))
		{
			return false;
		}

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

			if (!FFileHelper::SaveArrayToFile(*CompiledShader.Code, CachePaths[EntryPointIndex]))
			{
				DURIN_WARN("Failed to write shader cache artifact: {}", CachePaths[EntryPointIndex]);
				return false;
			}

			const std::string SidecarPath = ReflectionPath(VirtualShaderPath, EntryPointToString(Options.EntryPoints[EntryPointIndex]), VariantKey);
			if (!SaveShaderReflection(SidecarPath, CompiledShader))
			{
				DURIN_WARN("Failed to write shader reflection cache artifact: {}", SidecarPath);
				return false;
			}
		}

		return true;
	}
}
