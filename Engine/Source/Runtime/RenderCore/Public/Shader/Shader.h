#pragma once

#include "RHIResources.h"

namespace Durin
{
	struct FCompiledShader;
	struct FShaderCompilerOutput;
	using FShaderCode = std::vector<std::byte>;
	using FShaderCodeView = std::span<const std::byte>;

	class FShaderCodeResource
	{
	public:
		FShaderCodeResource(const std::shared_ptr<FShaderCode>& InCode, EShaderFrequency InFrequency)
			: Code(InCode)
			, Frequency(InFrequency)
		{
			checkf(InCode, "Shader code resource must not be null");
		}
		~FShaderCodeResource() = default;

		auto GetFrequency() const -> EShaderFrequency { return Frequency; }

		auto GetCodeView() const -> FShaderCodeView { return *Code; }

	private:
		const std::shared_ptr<FShaderCode> Code;
		EShaderFrequency Frequency;
	};

	class FShaderType
	{
	public:
		FShaderType(
			const char8* InName,
			FName InFName,
			const char8* InSourceFilePath,
			EShaderFrequency InFrequency,
			const char8* InEntryPoint
		)
			: Name(InName)
			, TypeName(InFName)
			, SourceFilePath(InSourceFilePath)

			, Frequency(InFrequency)
			, EntryPoint(InEntryPoint)
		{
		}

		auto GetName() const -> const char8* { return Name; }

		auto GetFName() const -> FName { return TypeName; }

		auto GetFrequency() const -> EShaderFrequency { return Frequency; }

		auto GetEntryPoint() const -> const char* { return EntryPoint; }

	private:
		const char8* Name;

		FName TypeName;

		const char8* SourceFilePath;

		EShaderFrequency Frequency;

		const char* EntryPoint;
	};

	class FShader
	{
	public:
	protected:
		FShaderRHIRef ShaderRHI;
	};

	class FShaderMapResourceCode
	{
	public:
		auto GetCodeView(uint32 ShaderIndex) const -> FShaderCodeView
		{
			check(ShaderIndex < ShaderCodeResources.size());
			return ShaderCodeResources[ShaderIndex].GetCodeView();
		}

		auto AddCompiledShader(const FCompiledShader& CompiledShader) -> void;

	private:
		std::vector<FXxHash64> ShaderHashes;
		std::vector<FShaderCodeResource> ShaderCodeResources;
	};

	class FShaderMapResource
	{
	public:
		FShaderMapResource(const std::shared_ptr<FShaderMapResourceCode>& InCode)
			: Code(InCode)
		{
		}
		~FShaderMapResource() = default;

		auto AddShaderCompilerOutput(const FShaderCompilerOutput& Output) -> void;

		auto GetShader(uint32 ShaderIndex, bool bRequired = true) const -> FRHIShader*
		{
			check(ShaderIndex < Shaders.size());
			return Shaders[ShaderIndex];
		}

		auto CreateRHIShader(uint32 ShaderIndex, bool bRequired = true) -> FRHIShader*;

	protected:
		auto ReleaseRHIShader(uint32 ShaderIndex) -> FRHIShader*;

		std::shared_ptr<FShaderMapResourceCode> Code;
		std::vector<FShaderRHIRef> Shaders;
	};

	class FShaderMapBase
	{
	public:
		auto GetResource() const -> FShaderMapResource* { return Resource.get(); }

	protected:
		std::shared_ptr<FShaderMapResource> Resource;
		std::shared_ptr<FShaderMapResourceCode> Code;
	};

	template<typename ShaderType>
	class TShaderRef
	{
	public:
		TShaderRef() = default;
		TShaderRef(ShaderType* InShader, FShaderMapBase* InShaderMap)
			: ShaderContent(InShader)
			, ShaderMap(InShaderMap)
		{
		}

		auto GetResource() const -> FShaderMapResource*
		{
			check(ShaderMap);
			return ShaderMap->GetResource();
		}

		auto GetShader() const -> ShaderType* { return ShaderContent; }
		auto GetShaderMap() const -> FShaderMapBase* { return ShaderMap; }

		auto GetRHIShader() const -> FRHIShader*
		{
			FShaderMapResource* Resource = GetResource();
			if (Resource)
			{
				return Resource->GetShader(0);
			}
			return nullptr;
		}

	private:
		ShaderType* ShaderContent = nullptr;
		FShaderMapBase* ShaderMap = nullptr;
	};
} // namespace Durin