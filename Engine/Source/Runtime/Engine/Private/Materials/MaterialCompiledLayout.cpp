#include "Materials/MaterialCompiledLayout.h"

#include <algorithm>
#include <unordered_set>

namespace Durin
{
	namespace
	{
		auto HashLayout(const FMaterialRenderLayout& Layout) -> FGuid
		{
			FByteBuffer Bytes;
			auto Append = [&](uint32 Value) {
				for (uint32 Shift = 0; Shift < 32; Shift += 8)
					Bytes.push_back(static_cast<std::byte>((Value >> Shift) & 0xff));
			};
			Append(CompiledMaterialRenderLayoutVersion);
			Append(MaterialTextureBindingBase);
			Append(MaterialUniformControlBytes);
			Append(Layout.UniformPayloadSize);
			Append(Layout.UniformFieldCount);
			Append(Layout.ResourceFieldCount);
			for (const auto& Field : Layout.Fields)
			{
				Append(Field.ParameterId.A); Append(Field.ParameterId.B);
				Append(Field.ParameterId.C); Append(Field.ParameterId.D);
				Append(static_cast<uint32>(Field.Storage));
				Append(static_cast<uint32>(Field.Type));
				Append(Field.CompactIndex); Append(Field.Offset); Append(Field.Size);
			}
			const auto Hash = FXxHash128::HashBuffer(Bytes);
			return {static_cast<uint32>(Hash.HashLow), static_cast<uint32>(Hash.HashLow >> 32),
				static_cast<uint32>(Hash.HashHigh), static_cast<uint32>(Hash.HashHigh >> 32)};
		}
	}

	auto CompileMaterialLayout(std::span<const FMaterialCompilerParameterDeclaration> Parameters,
		const FMaterialCompilerResourceLimits& Limits) -> FMaterialLayoutBuildResult
	{
		FMaterialLayoutBuildResult Result;
		auto Reject = [&](EMaterialLayoutError Error, FGuid Id = {}) {
			Result.Validation = {.Error = Error, .ParameterId = Id};
			Result.Layout = {};
			return Result;
		};
		if (Parameters.size() > MaterialMaxParameterDefinitionCount)
			return Reject(EMaterialLayoutError::ResourceLimit);
		std::vector<FMaterialCompilerParameterDeclaration> Ordered(Parameters.begin(), Parameters.end());
		std::ranges::sort(Ordered, {}, &FMaterialCompilerParameterDeclaration::Id);
		auto& Layout = Result.Layout;
		Layout.UniformPayloadSize = MaterialUniformControlBytes;
		FGuid Previous;
		for (const auto& Parameter : Ordered)
		{
			if (!Parameter.Id.IsValid()) return Reject(EMaterialLayoutError::InvalidParameter);
			if (Parameter.Id == Previous) return Reject(EMaterialLayoutError::DuplicateParameter, Parameter.Id);
			Previous = Parameter.Id;
			FMaterialRenderField Field;
			Field.ParameterId = Parameter.Id;
			switch (Parameter.Type)
			{
			case EMaterialParameterType::Scalar: Field.Type = EMaterialRenderValueType::Scalar; Field.Size = 4; break;
			case EMaterialParameterType::Vector2: Field.Type = EMaterialRenderValueType::Vector2; Field.Size = 8; break;
			case EMaterialParameterType::Vector: Field.Type = EMaterialRenderValueType::Vector3; Field.Size = 12; break;
			case EMaterialParameterType::Vector4: Field.Type = EMaterialRenderValueType::Vector4; Field.Size = 16; break;
			case EMaterialParameterType::Texture: Field.Type = EMaterialRenderValueType::Texture2D; break;
			default: return Reject(EMaterialLayoutError::InvalidType, Parameter.Id);
			}
			if (Parameter.Type == EMaterialParameterType::Texture)
			{
				Field.Storage = EMaterialRenderFieldStorage::Resource;
				Field.CompactIndex = Layout.ResourceFieldCount++;
			}
			else
			{
				Field.CompactIndex = Layout.UniformFieldCount++;
				Field.Offset = Layout.UniformPayloadSize;
				Layout.UniformPayloadSize += 16;
			}
			Layout.Fields.push_back(Field);
		}
		const uint64 Resources = Layout.ResourceFieldCount;
		if (Resources > MaterialRenderMaxResourceCount
			|| Resources + 4 > Limits.SampledImages || Resources + 2 > Limits.Samplers
			|| Limits.UniformBuffers < 4 || 2 * Resources + 12 > Limits.StageResources
			|| Layout.UniformPayloadSize > MaterialRenderMaxUniformPayloadBytes
			|| Layout.UniformPayloadSize > Limits.UniformBufferBytes)
			return Reject(EMaterialLayoutError::ResourceLimit);
		Layout.Identity = {.Version = CompiledMaterialRenderLayoutVersion, .Id = HashLayout(Layout)};
		return Result;
	}

	auto ValidateCompiledMaterialLayout(const FMaterialRenderLayout& Layout,
		const FMaterialCompilerResourceLimits& Limits) -> FMaterialLayoutValidationResult
	{
		if (Layout.Identity.Version != CompiledMaterialRenderLayoutVersion)
			return {.Error = EMaterialLayoutError::InvalidIdentity};
		if (Layout.Fields.size() > MaterialMaxParameterDefinitionCount)
			return {.Error = EMaterialLayoutError::ResourceLimit};
		std::vector<FMaterialCompilerParameterDeclaration> Parameters;
		Parameters.reserve(Layout.Fields.size());
		for (uint32 Index = 0; Index < Layout.Fields.size(); ++Index)
		{
			const auto& Field = Layout.Fields[Index];
			EMaterialParameterType Type;
			switch (Field.Type)
			{
			case EMaterialRenderValueType::Scalar: Type = EMaterialParameterType::Scalar; break;
			case EMaterialRenderValueType::Vector2: Type = EMaterialParameterType::Vector2; break;
			case EMaterialRenderValueType::Vector3: Type = EMaterialParameterType::Vector; break;
			case EMaterialRenderValueType::Vector4: Type = EMaterialParameterType::Vector4; break;
			case EMaterialRenderValueType::Texture2D: Type = EMaterialParameterType::Texture; break;
			default: return {EMaterialLayoutError::InvalidType, Field.ParameterId, Index};
			}
			Parameters.push_back({Field.ParameterId, Type});
		}
		const auto Expected = CompileMaterialLayout(Parameters, Limits);
		if (!Expected) return Expected.Validation;
		for (uint32 Index = 0; Index < Layout.Fields.size(); ++Index)
			if (Layout.Fields[Index] != Expected.Layout.Fields[Index])
				return {EMaterialLayoutError::InvalidField, Layout.Fields[Index].ParameterId, Index};
		if (Layout.UniformPayloadSize != Expected.Layout.UniformPayloadSize
			|| Layout.UniformFieldCount != Expected.Layout.UniformFieldCount
			|| Layout.ResourceFieldCount != Expected.Layout.ResourceFieldCount)
			return {.Error = EMaterialLayoutError::InvalidField};
		if (Layout.Identity != Expected.Layout.Identity)
			return {.Error = EMaterialLayoutError::InvalidIdentity};
		return {};
	}

	auto GetMaterialLayoutErrorText(EMaterialLayoutError Error) -> std::string_view
	{
		switch (Error)
		{
		case EMaterialLayoutError::None: return {};
		case EMaterialLayoutError::InvalidParameter: return "The material layout contains an invalid parameter identity.";
		case EMaterialLayoutError::DuplicateParameter: return "The material layout contains a duplicate parameter identity.";
		case EMaterialLayoutError::InvalidType: return "The material layout contains an unsupported parameter type.";
		case EMaterialLayoutError::ResourceLimit: return "The material layout exceeds the target uniform or descriptor budget.";
		case EMaterialLayoutError::InvalidField: return "The material layout fields, counts or offsets do not match its deterministic schema.";
		case EMaterialLayoutError::InvalidIdentity: return "The material layout version or identity does not match its schema.";
		case EMaterialLayoutError::InvalidReflection: return "The compiled material reflection does not match the layout and pass contract.";
		}
		return "Unknown material layout error.";
	}
}
