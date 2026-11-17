#include "ShaderFormat.h"

namespace Durin::ShaderFormat
{
	auto ApplyGraphicsStageParameterSpaces(FShaderCode& Shader) -> std::expected<void, std::string>
	{
		if (Shader.Frequency != EShaderFrequency::Vertex && Shader.Frequency != EShaderFrequency::Fragment)
			return std::unexpected("Graphics parameter spaces require a graphics stage");
		if (Shader.CodeFormat != EShaderCodeFormat::Spirv15 || Shader.Code.size() < 20 || Shader.Code.size() % 4)
			return std::unexpected("Graphics parameter spaces require SPIR-V intermediate code");
		const uint32 Space = Shader.Frequency == EShaderFrequency::Fragment ? 1 : 0;
		std::vector<uint32> Words(Shader.Code.size() / 4);
		std::memcpy(Words.data(), Shader.Code.data(), Shader.Code.size());
		if (Words[0] != 0x07230203) return std::unexpected("Invalid SPIR-V header");
		for (size_t I = 5; I < Words.size();)
		{
			const uint32 Count = Words[I] >> 16;
			const uint32 Op = Words[I] & 0xffff;
			if (!Count || Count > Words.size() - I) return std::unexpected("Invalid SPIR-V instruction");
			// OpDecorate DescriptorSet. All default-space variables move together.
			if (Op == 71 && Count == 4 && Words[I + 2] == 34 && Words[I + 3] == 0) Words[I + 3] = Space;
			I += Count;
		}
		auto Reflection = Shader.Reflection;
		std::set<std::pair<uint32, uint32>> Locations;
		for (auto& B : Reflection.ResourceBindings)
		{
			if (B.SetIndex == 0) B.SetIndex = Space;
			if (!Locations.emplace(B.SetIndex, B.BindingIndex).second)
				return std::unexpected("Explicit descriptor space conflicts with the graphics stage root layout");
		}
		FByteBuffer Bytes(Shader.Code.size());
		std::memcpy(Bytes.data(), Words.data(), Bytes.size());
		Shader.Code = FSharedByteBuffer::Take(std::move(Bytes));
		Shader.CodeHash = FXxHash128::HashBuffer(Shader.Code);
		Shader.Reflection = std::move(Reflection);
		Shader.ShaderIdentity = {};
		return {};
	}
}
