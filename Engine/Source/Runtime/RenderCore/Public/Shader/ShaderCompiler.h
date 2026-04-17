#pragma once

namespace Doge
{
	struct FShaderSource
	{
		// The relative shader file path. For example, "Engine/Shaders/MyShader.slang".
		std::string Filename;
		std::string EntryPoint;
	};

	class FShaderCompiler
	{
	public:
		FShaderCompiler() = default;
		virtual ~FShaderCompiler() = default;

		RENDERCORE_API virtual auto Compile(const char8* InShaderFilename, const char8* InEntryPoint, std::vector<uint32>& OutCode) -> bool = 0;

		RENDERCORE_API virtual auto Compile(const char8* InShaderFilename, const std::span<const char8*>& InEntryPoints, std::vector<std::vector<uint32>>& OutCodes) -> bool = 0;

		DOGE_NONCOPYABLE(FShaderCompiler);
	};

	RENDERCORE_API extern FShaderCompiler* GShaderCompiler;
} // namespace Doge