#include "Materials/MaterialTypes.h"
#include "Materials/MaterialRenderTypes.h"

namespace Durin
{
	namespace
	{

		auto SetValidationFailure(
			FMaterialRenderValidationDiagnostic& OutDiagnostic,
			EMaterialRenderValidationFailure Failure,
			uint32 FieldIndex,
			std::string Message
		) -> bool
		{
			OutDiagnostic.Failure = Failure;
			OutDiagnostic.FieldIndex = FieldIndex;
			OutDiagnostic.Message = std::move(Message);
			return false;
		}

		auto GetUniformFieldSize(EMaterialRenderValueType Type) -> uint32
		{
			switch (Type)
			{
			case EMaterialRenderValueType::Scalar:
				return sizeof(float);
			case EMaterialRenderValueType::Vector3:
				return sizeof(float) * 3;
			case EMaterialRenderValueType::Vector4:
				return sizeof(float) * 4;
			case EMaterialRenderValueType::Texture2D:
				break;
			}
			return 0;
		}

		auto GetUniformFieldAlignment(EMaterialRenderValueType Type) -> uint32
		{
			switch (Type)
			{
			case EMaterialRenderValueType::Scalar:
				return sizeof(float);
			case EMaterialRenderValueType::Vector3:
			case EMaterialRenderValueType::Vector4:
				return 16;
			case EMaterialRenderValueType::Texture2D:
				break;
			}
			return 0;
		}

		auto IsUniformFieldType(EMaterialRenderValueType Type) -> bool
		{
			return Type != EMaterialRenderValueType::Texture2D;
		}

		auto WriteFloat(
			std::vector<std::byte>& Bytes,
			uint32 Offset,
			float Value
		) -> void
		{
			std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
		}

		auto MakeErrorUniformPayload() -> std::vector<std::byte>
		{
			std::vector<std::byte> Result(416, std::byte{0});
			WriteFloat(Result, 0, 1.0f);
			WriteFloat(Result, 4, 0.0f);
			WriteFloat(Result, 8, 1.0f);
			WriteFloat(Result, 12, 1.0f);
			WriteFloat(Result, 28, 0.0f);
			WriteFloat(Result, 40, 1.0f);
			WriteFloat(Result, 44, 0.5f);
			WriteFloat(Result, 48, 1.0f);
			WriteFloat(Result, 52, 1.0f);
			for (uint32 Role = 0; Role < 8; ++Role)
			{
				WriteFloat(Result, 96 + Role * 16, 1.0f);
				WriteFloat(Result, 100 + Role * 16, 1.0f);
				WriteFloat(Result, 384 + Role * 4, 13.0f);
			}
			return Result;
		}

		auto MakeCanonicalUniformPayload() -> std::vector<std::byte>
		{
			std::vector<std::byte> Result = MakeErrorUniformPayload();
			WriteFloat(Result, 0, 0.95f);
			WriteFloat(Result, 4, 0.62f);
			WriteFloat(Result, 8, 0.22f);
			return Result;
		}

		auto IsRangeOverlapping(
			const std::vector<std::pair<uint32, uint32>>& Ranges,
			uint32 Begin,
			uint32 End
		) -> bool
		{
			return std::ranges::any_of(Ranges, [Begin, End](const auto& Range) {
				return Begin < Range.second && Range.first < End;
			});
		}
	}

	auto MakeMaterialRenderLayoutV1() -> FMaterialRenderLayout
	{
		using namespace MaterialParameters;
		FMaterialRenderLayout Result;
		Result.Identity = {.Version = 1, .Id = MaterialRenderLayoutV1Id};
		Result.UniformPayloadSize = 32;
		Result.UniformFieldCount = 4;
		Result.ResourceFieldCount = 1;
		Result.Fields = {
			{
				.ParameterId = BaseColorId,
				.Storage = EMaterialRenderFieldStorage::Uniform,
				.Type = EMaterialRenderValueType::Vector3,
				.CompactIndex = 0,
				.Offset = 0,
				.Size = 12,
			},
			{
				.ParameterId = OpacityId,
				.Storage = EMaterialRenderFieldStorage::Uniform,
				.Type = EMaterialRenderValueType::Scalar,
				.CompactIndex = 1,
				.Offset = 12,
				.Size = 4,
			},
			{
				.ParameterId = SpecularStrengthId,
				.Storage = EMaterialRenderFieldStorage::Uniform,
				.Type = EMaterialRenderValueType::Scalar,
				.CompactIndex = 2,
				.Offset = 16,
				.Size = 4,
			},
			{
				.ParameterId = ShininessId,
				.Storage = EMaterialRenderFieldStorage::Uniform,
				.Type = EMaterialRenderValueType::Scalar,
				.CompactIndex = 3,
				.Offset = 20,
				.Size = 4,
			},
			{
				.ParameterId = BaseColorTextureId,
				.Storage = EMaterialRenderFieldStorage::Resource,
				.Type = EMaterialRenderValueType::Texture2D,
				.CompactIndex = 0,
				.Offset = 0,
				.Size = 0,
			},
		};
		return Result;
	}

	auto MakeMaterialRenderLayoutV2() -> FMaterialRenderLayout
	{
		using namespace MaterialParameters;
		FMaterialRenderLayout Result;
		Result.Identity = {.Version = 2, .Id = MaterialRenderLayoutV2Id};
		Result.UniformPayloadSize = 352;
		Result.UniformFieldCount = 32;
		Result.ResourceFieldCount = 8;
		auto AddUniform = [&Result](FGuid Id, EMaterialRenderValueType Type, uint16 Index, uint32 Offset) {
			Result.Fields.push_back({.ParameterId = Id, .Storage = EMaterialRenderFieldStorage::Uniform,
				.Type = Type, .CompactIndex = Index, .Offset = Offset, .Size = GetUniformFieldSize(Type)});
		};
		AddUniform(BaseColorId, EMaterialRenderValueType::Vector3, 0, 0);
		AddUniform(OpacityId, EMaterialRenderValueType::Scalar, 1, 12);
		AddUniform(EmissiveId, EMaterialRenderValueType::Vector3, 2, 16);
		AddUniform(MetallicId, EMaterialRenderValueType::Scalar, 3, 28);
		AddUniform(NormalId, EMaterialRenderValueType::Vector3, 4, 32);
		AddUniform(RoughnessId, EMaterialRenderValueType::Scalar, 5, 44);
		AddUniform(AmbientOcclusionId, EMaterialRenderValueType::Scalar, 6, 48);
		AddUniform(OpacityMaskId, EMaterialRenderValueType::Scalar, 7, 52);
		for (uint16 Role = 0; Role < 8; ++Role)
		{
			AddUniform(UVChannelIds[Role], EMaterialRenderValueType::Scalar, 8 + Role, 64 + Role * 4);
		}
		for (uint16 Role = 0; Role < 8; ++Role)
		{
			AddUniform(UVScaleIds[Role], EMaterialRenderValueType::Vector3, 16 + Role, 96 + Role * 16);
		}
		for (uint16 Role = 0; Role < 8; ++Role)
		{
			AddUniform(UVOffsetIds[Role], EMaterialRenderValueType::Vector3, 24 + Role, 224 + Role * 16);
		}
		for (uint16 Role = 0; Role < 8; ++Role)
		{
			Result.Fields.push_back({.ParameterId = TextureIds[Role], .Storage = EMaterialRenderFieldStorage::Resource,
				.Type = EMaterialRenderValueType::Texture2D, .CompactIndex = Role});
		}
		return Result;
	}

	auto MakeDefaultMaterialRenderLayout() -> FMaterialRenderLayout
	{
		using namespace MaterialParameters;
		FMaterialRenderLayout Result = MakeMaterialRenderLayoutV2();
		Result.Identity = {.Version = 3, .Id = MaterialRenderLayoutV3Id};
		Result.UniformPayloadSize = 416;
		Result.UniformFieldCount = 48;
		for (uint16 Role = 0; Role < 8; ++Role)
		{
			Result.Fields.insert(Result.Fields.begin() + 32 + Role, {
				.ParameterId = UVRotationIds[Role],
				.Storage = EMaterialRenderFieldStorage::Uniform,
				.Type = EMaterialRenderValueType::Scalar,
				.CompactIndex = static_cast<uint16>(32 + Role),
				.Offset = static_cast<uint32>(352 + Role * 4),
				.Size = sizeof(float),
			});
		}
		for (uint16 Role = 0; Role < 8; ++Role)
		{
			Result.Fields.insert(Result.Fields.begin() + 40 + Role, {
				.ParameterId = SamplerStateIds[Role],
				.Storage = EMaterialRenderFieldStorage::Uniform,
				.Type = EMaterialRenderValueType::Scalar,
				.CompactIndex = static_cast<uint16>(40 + Role),
				.Offset = static_cast<uint32>(384 + Role * 4),
				.Size = sizeof(float),
			});
		}
		return Result;
	}

	auto MakeCanonicalMaterialRenderRepresentation()
		-> FMaterialRenderRepresentation
	{
		FMaterialRenderRepresentationInput Input;
		Input.Layout = MakeDefaultMaterialRenderLayout();
		Input.UniformPayload = MakeCanonicalUniformPayload();
		Input.Resources.resize(Input.Layout.ResourceFieldCount);
		FMaterialRenderRepresentation Result;
		FMaterialRenderValidationDiagnostic Diagnostic;
		const bool bValid = FMaterialRenderRepresentation::TryCreate(
			std::move(Input), Result, Diagnostic);
		checkf(
			bValid,
			"Canonical material render seed must satisfy v3: %s",
			Diagnostic.Message.c_str());
		return Result;
	}

	auto ValidateMaterialRenderLayout(
		const FMaterialRenderLayout& Layout,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool
	{
		OutDiagnostic = {};
		if (Layout.Identity.Version != 1 && Layout.Identity.Version != 2
			&& Layout.Identity.Version != 3)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::UnsupportedVersion,
				0,
				std::format(
					"Material render layout version {} is unsupported; expected {}.",
					Layout.Identity.Version,
					CurrentMaterialRenderLayoutVersion));
		}
		const FGuid ExpectedIdentity = Layout.Identity.Version == 1
			? MaterialRenderLayoutV1Id
			: Layout.Identity.Version == 2
				? MaterialRenderLayoutV2Id
				: MaterialRenderLayoutV3Id;
		if (Layout.Identity.Id != ExpectedIdentity)
		{
			return SetValidationFailure(OutDiagnostic, EMaterialRenderValidationFailure::UnsupportedIdentity, 0,
				"Material render layout identity is unsupported for its version.");
		}
		if (Layout.UniformPayloadSize == 0
			|| Layout.UniformPayloadSize > MaterialRenderMaxUniformPayloadBytes
			|| Layout.UniformPayloadSize % 16 != 0
			|| Layout.Fields.size() > MaterialRenderMaxFieldCount
			|| Layout.UniformFieldCount > MaterialRenderMaxFieldCount
			|| Layout.ResourceFieldCount > MaterialRenderMaxResourceCount
			|| Layout.Fields.size()
				!= static_cast<size_t>(Layout.UniformFieldCount)
					+ Layout.ResourceFieldCount)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidCounts,
				0,
				"Material render layout counts or uniform payload size are invalid.");
		}

		std::unordered_set<FGuid> ParameterIds;
		std::vector<bool> UniformIndices(Layout.UniformFieldCount, false);
		std::vector<bool> ResourceIndices(Layout.ResourceFieldCount, false);
		std::vector<std::pair<uint32, uint32>> UniformRanges;
		UniformRanges.reserve(Layout.UniformFieldCount);
		for (uint32 FieldIndex = 0; FieldIndex < Layout.Fields.size(); ++FieldIndex)
		{
			const FMaterialRenderField& Field = Layout.Fields[FieldIndex];
			if (!Field.ParameterId.IsValid())
			{
				return SetValidationFailure(
					OutDiagnostic,
					EMaterialRenderValidationFailure::InvalidField,
					FieldIndex,
					"Material render field has an invalid parameter GUID.");
			}
			if (!ParameterIds.insert(Field.ParameterId).second)
			{
				return SetValidationFailure(
					OutDiagnostic,
					EMaterialRenderValidationFailure::DuplicateField,
					FieldIndex,
					"Material render layout contains a duplicate parameter GUID.");
			}

			if (Field.Storage == EMaterialRenderFieldStorage::Uniform)
			{
				if (!IsUniformFieldType(Field.Type)
					|| Field.CompactIndex >= UniformIndices.size()
					|| UniformIndices[Field.CompactIndex])
				{
					return SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::InvalidField,
						FieldIndex,
						"Material render uniform field type or compact index is invalid.");
				}
				UniformIndices[Field.CompactIndex] = true;
				const uint32 ExpectedSize = GetUniformFieldSize(Field.Type);
				const uint32 ExpectedAlignment = GetUniformFieldAlignment(Field.Type);
				if (Field.Size != ExpectedSize)
				{
					return SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::InvalidField,
						FieldIndex,
						"Material render uniform field size does not match its type.");
				}
				if (Field.Offset % ExpectedAlignment != 0)
				{
					return SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::InvalidAlignment,
						FieldIndex,
						"Material render uniform field offset is misaligned.");
				}
				if (Field.Offset > Layout.UniformPayloadSize
					|| Field.Size > Layout.UniformPayloadSize - Field.Offset)
				{
					return SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::InvalidOffset,
						FieldIndex,
						"Material render uniform field exceeds the payload.");
				}
				const uint32 End = Field.Offset + Field.Size;
				if (IsRangeOverlapping(UniformRanges, Field.Offset, End))
				{
					return SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::OverlappingFields,
						FieldIndex,
						"Material render uniform fields overlap.");
				}
				UniformRanges.emplace_back(Field.Offset, End);
			}
			else if (Field.Storage == EMaterialRenderFieldStorage::Resource)
			{
				if (Field.Type != EMaterialRenderValueType::Texture2D
					|| Field.CompactIndex >= ResourceIndices.size()
					|| ResourceIndices[Field.CompactIndex]
					|| Field.Offset != 0
					|| Field.Size != 0)
				{
					return SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::InvalidResource,
						FieldIndex,
						"Material render resource field type, index, or range is invalid.");
				}
				ResourceIndices[Field.CompactIndex] = true;
			}
			else
			{
				return SetValidationFailure(
					OutDiagnostic,
					EMaterialRenderValidationFailure::InvalidField,
					FieldIndex,
					"Material render field storage class is unsupported.");
			}
		}

		if (std::ranges::any_of(UniformIndices, [](bool Used) { return !Used; })
			|| std::ranges::any_of(ResourceIndices, [](bool Used) { return !Used; }))
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidCounts,
				0,
				"Material render compact indices are not contiguous.");
		}
		return true;
	}

}
