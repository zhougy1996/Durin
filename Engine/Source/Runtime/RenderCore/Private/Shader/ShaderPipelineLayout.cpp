#include "Shader/Shader.h"
#include "ShaderFormat.h"

namespace Durin
{
	namespace
	{
		struct FShaderBindingKey
		{
			uint32 SetIndex = 0;
			uint32 BindingIndex = 0;

			auto operator==(const FShaderBindingKey& Other) const -> bool
			{
				return SetIndex == Other.SetIndex && BindingIndex == Other.BindingIndex;
			}
		};

		struct FShaderBindingKeyHasher
		{
			auto operator()(const FShaderBindingKey& Key) const noexcept -> size_t
			{
				return std::hash<uint64>{}((static_cast<uint64>(Key.SetIndex) << 32) | Key.BindingIndex);
			}
		};

	} // namespace

	auto ShareShaderCode(const FShaderCode& Shader) -> std::shared_ptr<const FShaderCode>
	{
		if (!ShaderFormat::ValidateShaderCode(Shader)) return {};
		auto Published = Shader;
		Published.ShaderIdentity = ShaderFormat::ComputeShaderIdentity(Published);
		return std::make_shared<const FShaderCode>(std::move(Published));
	}

	auto MakeShaderCreateDesc(const FShaderCode& Shader) -> FRHIShaderCreateDesc
	{
		return MakeShaderCreateDesc(ShareShaderCode(Shader));
	}

	auto MakeShaderCreateDesc(std::shared_ptr<const FShaderCode> Code) -> FRHIShaderCreateDesc
	{
		requiref(Code && !Code->Code.IsEmpty(), "Shader code owner must be valid");
		auto Desc = FRHIShaderCreateDesc::Create(Code->DebugName.empty() ? nullptr : Code->DebugName.c_str(),
			Code->Frequency, Code->Code.GetBytes(), Code->CodeHash);
		Desc.SetEntryPoint(Code->BinaryEntryPoint.c_str());
		Desc.Target = Code->Target; Desc.CodeFormat = Code->CodeFormat;
		Desc.ComputeThreadGroupSize = Code->ComputeThreadGroupSize;
		Desc.ShaderIdentity = Code->ShaderIdentity;
		Desc.CodeOwner = std::move(Code);
		return Desc;
	}

	auto BuildPipelineLayoutFromReflection(
		std::span<const FShaderReflectionData> ReflectionData,
		FPipelineLayoutDesc& OutPipelineLayout) -> FShaderOperationResult
	{
		OutPipelineLayout = {};

		std::unordered_map<FShaderBindingKey, FShaderResourceBinding, FShaderBindingKeyHasher> MergedBindings;
		std::vector<FPushConstantRange> MergedPushConstants;
		uint32 MaxSetIndex = 0;

		for (const FShaderReflectionData& StageReflection : ReflectionData)
		{
			for (const FShaderResourceBinding& Binding : StageReflection.ResourceBindings)
			{
				FShaderBindingKey BindingKey{Binding.SetIndex, Binding.BindingIndex};
				MaxSetIndex = std::max(MaxSetIndex, Binding.SetIndex);

				auto FoundIt = MergedBindings.find(BindingKey);
				if (FoundIt == MergedBindings.end())
				{
					MergedBindings.emplace(BindingKey, Binding);
					continue;
				}

				FShaderResourceBinding& ExistingBinding = FoundIt->second;
				if (ExistingBinding.Type != Binding.Type)
				{
					return std::unexpected(FShaderError{.Code = EShaderError::BindingTypeConflict,
						.Expected = static_cast<uint64>(ExistingBinding.Type),
						.Actual = static_cast<uint64>(Binding.Type),
						.SetIndex = Binding.SetIndex,
						.BindingIndex = Binding.BindingIndex});
				}
				if (ExistingBinding.ArraySize != Binding.ArraySize)
				{
					return std::unexpected(FShaderError{.Code = EShaderError::BindingArraySizeConflict,
						.Expected = ExistingBinding.ArraySize,
						.Actual = Binding.ArraySize,
						.SetIndex = Binding.SetIndex,
						.BindingIndex = Binding.BindingIndex});
				}

				ExistingBinding.StageFlags |= Binding.StageFlags;
				if (ExistingBinding.Name.empty())
				{
					ExistingBinding.Name = Binding.Name;
				}
			}

			for (const FPushConstantRange& PushConstantRange : StageReflection.PushConstantRanges)
			{
				bool bMergedRange = false;
				for (FPushConstantRange& ExistingRange : MergedPushConstants)
				{
					const uint32 ExistingBegin = ExistingRange.Offset;
					const uint32 ExistingEnd = ExistingRange.Offset + ExistingRange.Size;
					const uint32 NewBegin = PushConstantRange.Offset;
					const uint32 NewEnd = PushConstantRange.Offset + PushConstantRange.Size;

					if (ExistingRange.Offset == PushConstantRange.Offset && ExistingRange.Size == PushConstantRange.Size)
					{
						ExistingRange.StageFlags |= PushConstantRange.StageFlags;
						bMergedRange = true;
						break;
					}

					if (NewBegin < ExistingEnd && ExistingBegin < NewEnd)
					{
						return std::unexpected(FShaderError{.Code = EShaderError::PushConstantOverlap,
							.ExistingBegin = ExistingBegin,
							.ExistingEnd = ExistingEnd,
							.NewBegin = NewBegin,
							.NewEnd = NewEnd});
					}
				}

				if (!bMergedRange)
				{
					MergedPushConstants.push_back(PushConstantRange);
				}
			}
		}

		OutPipelineLayout.BindingLayouts.clear();
		OutPipelineLayout.BindingLayouts.resize(MergedBindings.empty() ? 0 : MaxSetIndex + 1);
		for (const auto& [Key, Binding] : MergedBindings)
		{
			OutPipelineLayout.BindingLayouts[Key.SetIndex].BindingLayouts.emplace_back(
				Binding.StageFlags,
				Binding.BindingIndex,
				Binding.Type,
				Binding.ArraySize
			);
		}

		for (FBindingLayout& BindingLayout : OutPipelineLayout.BindingLayouts)
		{
			std::ranges::sort(BindingLayout.BindingLayouts, [](const FBindingLayoutItem& A, const FBindingLayoutItem& B) {
				return A.Slot < B.Slot;
			});
		}

		std::ranges::sort(MergedPushConstants, [](const FPushConstantRange& A, const FPushConstantRange& B) {
			if (A.Offset != B.Offset)
			{
				return A.Offset < B.Offset;
			}
			return A.Size < B.Size;
		});
		OutPipelineLayout.PushConstantRanges = std::move(MergedPushConstants);
		return {};
	}

	auto BuildPipelineLayoutFromShaders(
		std::span<const FShaderCode> CompiledShaders,
		FPipelineLayoutDesc& OutPipelineLayout) -> FShaderOperationResult
	{
		std::vector<FShaderReflectionData> ReflectionData;
		ReflectionData.reserve(CompiledShaders.size());
		for (const FShaderCode& CompiledShader : CompiledShaders)
		{
			ReflectionData.push_back(CompiledShader.Reflection);
		}

		return BuildPipelineLayoutFromReflection(ReflectionData, OutPipelineLayout);
	}

}
