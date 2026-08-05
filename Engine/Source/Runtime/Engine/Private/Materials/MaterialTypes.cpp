#include "Materials/MaterialTypes.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_set>
#include <utility>

namespace Durin
{
	namespace
	{
		std::array<std::atomic<uint64>,
			static_cast<size_t>(EMaterialFallbackReason::Count)>
			GMaterialFallbackCounts{};

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

	FMaterialRenderRepresentation::FMaterialRenderRepresentation()
		: Layout(MakeDefaultMaterialRenderLayout())
		, UniformPayload(MakeErrorUniformPayload())
		, Resources(8)
		, bError(true)
	{
	}

	FMaterialRenderRepresentation::FMaterialRenderRepresentation(
		FMaterialRenderLayout InLayout,
		std::vector<std::byte> InUniformPayload,
		std::vector<FRHITextureReferenceRef> InResources,
		bool bInError)
		: Layout(std::move(InLayout))
		, UniformPayload(std::move(InUniformPayload))
		, Resources(std::move(InResources))
		, bError(bInError)
	{
	}

	auto FMaterialRenderRepresentation::TryCreate(
		FMaterialRenderRepresentationInput Input,
		FMaterialRenderRepresentation& OutRepresentation,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool
	{
		OutRepresentation = FMaterialRenderRepresentation();
		OutDiagnostic = {};
		if (!ValidateMaterialRenderLayout(Input.Layout, OutDiagnostic)) return false;
		if (Input.UniformPayload.size() != Input.Layout.UniformPayloadSize)
		{
			SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidPayloadSize,
				0,
				"Material render uniform payload size does not match its layout.");
			return false;
		}
		if (Input.Resources.size() != Input.Layout.ResourceFieldCount)
		{
			SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidResource,
				0,
				"Material render resource count does not match its layout.");
			return false;
		}

		std::vector<bool> Covered(Input.UniformPayload.size(), false);
		for (uint32 FieldIndex = 0; FieldIndex < Input.Layout.Fields.size(); ++FieldIndex)
		{
			const FMaterialRenderField& Field = Input.Layout.Fields[FieldIndex];
			if (Field.Storage != EMaterialRenderFieldStorage::Uniform) continue;
			for (uint32 ByteIndex = Field.Offset;
				ByteIndex < Field.Offset + Field.Size;
				++ByteIndex)
			{
				Covered[ByteIndex] = true;
			}
			for (uint32 ValueIndex = 0;
				ValueIndex < Field.Size / sizeof(float);
				++ValueIndex)
			{
				float Value = 0.0f;
				std::memcpy(
					&Value,
					Input.UniformPayload.data()
						+ Field.Offset + ValueIndex * sizeof(float),
					sizeof(Value));
				if (!std::isfinite(Value))
				{
					SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::NonFiniteValue,
						FieldIndex,
						"Material render uniform payload contains a non-finite value.");
					return false;
				}
			}
		}
		for (uint32 ByteIndex = 0; ByteIndex < Input.UniformPayload.size(); ++ByteIndex)
		{
			if (!Covered[ByteIndex] && Input.UniformPayload[ByteIndex] != std::byte{0})
			{
				SetValidationFailure(
					OutDiagnostic,
					EMaterialRenderValidationFailure::NonZeroPadding,
					0,
					"Material render uniform padding must be zero.");
				return false;
			}
		}

		OutRepresentation = FMaterialRenderRepresentation(
			std::move(Input.Layout),
			std::move(Input.UniformPayload),
			std::move(Input.Resources),
			false);
		return true;
	}

	auto FMaterialRenderRepresentation::GetLayout() const
		-> const FMaterialRenderLayout&
	{
		return Layout;
	}

	auto FMaterialRenderRepresentation::GetUniformPayload() const
		-> std::span<const std::byte>
	{
		return UniformPayload;
	}

	auto FMaterialRenderRepresentation::GetResources() const
		-> std::span<const FRHITextureReferenceRef>
	{
		return Resources;
	}

	auto FMaterialRenderRepresentation::IsError() const -> bool
	{
		return bError;
	}

	auto TryGetMaterialRenderV1Binding(
		const FMaterialRenderRepresentation& Representation,
		FMaterialRenderV1Binding& OutBinding,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool
	{
		OutBinding = FMaterialRenderV1Binding{};
		OutDiagnostic = {};

		static const FMaterialRenderLayout ExpectedLayout =
			MakeMaterialRenderLayoutV1();
		const FMaterialRenderLayout& Layout = Representation.GetLayout();
		if (Layout.Identity != ExpectedLayout.Identity)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::UnsupportedIdentity,
				0,
				"Material render binding layout identity is unsupported.");
		}
		if (Layout != ExpectedLayout)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidField,
				0,
				"Material render binding layout does not match the v1 contract.");
		}

		const std::span<const std::byte> Payload =
			Representation.GetUniformPayload();
		if (Payload.size() != ExpectedLayout.UniformPayloadSize)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidPayloadSize,
				0,
				"Material render binding payload size is invalid.");
		}
		const std::span<const FRHITextureReferenceRef> Resources =
			Representation.GetResources();
		if (Resources.size() != ExpectedLayout.ResourceFieldCount)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidResource,
				0,
				"Material render binding resource count is invalid.");
		}

		auto ReadFloat = [&Payload](uint32 Offset) {
			float Value = 0.0f;
			std::memcpy(&Value, Payload.data() + Offset, sizeof(Value));
			return Value;
		};
		OutBinding.BaseColor = FVector4f(
			ReadFloat(ExpectedLayout.Fields[0].Offset),
			ReadFloat(ExpectedLayout.Fields[0].Offset + 4),
			ReadFloat(ExpectedLayout.Fields[0].Offset + 8),
			ReadFloat(ExpectedLayout.Fields[1].Offset));
		OutBinding.SpecularStrength =
			ReadFloat(ExpectedLayout.Fields[2].Offset);
		OutBinding.Shininess = ReadFloat(ExpectedLayout.Fields[3].Offset);
		OutBinding.BaseColorTexture = Resources[0];
		return true;
	}

	auto TryGetMaterialRenderV2Binding(
		const FMaterialRenderRepresentation& Representation,
		FMaterialRenderV2Binding& OutBinding,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool
	{
		OutBinding = FMaterialRenderV2Binding{};
		OutDiagnostic = {};
		static const FMaterialRenderLayout ExpectedLayout = MakeMaterialRenderLayoutV2();
		const FMaterialRenderLayout& Layout = Representation.GetLayout();
		if (Layout.Identity != ExpectedLayout.Identity)
		{
			return SetValidationFailure(OutDiagnostic, EMaterialRenderValidationFailure::UnsupportedIdentity, 0,
				"Material render binding layout identity is not v2.");
		}
		if (Layout != ExpectedLayout)
		{
			return SetValidationFailure(OutDiagnostic, EMaterialRenderValidationFailure::InvalidField, 0,
				"Material render binding layout does not match the v2 contract.");
		}
		const auto Payload = Representation.GetUniformPayload();
		const auto Resources = Representation.GetResources();
		if (Payload.size() != 352) return SetValidationFailure(OutDiagnostic, EMaterialRenderValidationFailure::InvalidPayloadSize, 0, "Material render v2 payload size is invalid.");
		if (Resources.size() != 8) return SetValidationFailure(OutDiagnostic, EMaterialRenderValidationFailure::InvalidResource, 0, "Material render v2 resource count is invalid.");
		auto ReadFloat = [&Payload](uint32 Offset) { float Value = 0.0f; std::memcpy(&Value, Payload.data() + Offset, sizeof(Value)); return Value; };
		auto ReadVector = [&ReadFloat](uint32 Offset) { return FVector3f(ReadFloat(Offset), ReadFloat(Offset + 4), ReadFloat(Offset + 8)); };
		auto ReadVector2 = [&ReadFloat](uint32 Offset) { return FVector2f(ReadFloat(Offset), ReadFloat(Offset + 4)); };
		OutBinding.BaseColor = FVector4f(ReadFloat(0), ReadFloat(4), ReadFloat(8), ReadFloat(12));
		OutBinding.Emissive = ReadVector(16);
		OutBinding.Metallic = ReadFloat(28);
		OutBinding.Normal = ReadVector(32);
		OutBinding.Roughness = ReadFloat(44);
		OutBinding.AmbientOcclusion = ReadFloat(48);
		OutBinding.OpacityMask = ReadFloat(52);
		for (uint32 Role = 0; Role < 8; ++Role)
		{
			OutBinding.UVChannels[Role] = ReadFloat(64 + Role * 4);
			OutBinding.UVScales[Role] = ReadVector2(96 + Role * 16);
			OutBinding.UVOffsets[Role] = ReadVector2(224 + Role * 16);
			OutBinding.Textures[Role] = Resources[Role];
		}
		return true;
	}

	auto EncodeMaterialSamplerState(const FMaterialSamplerState& State) -> float
	{
		const uint32 Packed = static_cast<uint32>(State.MinFilter)
			| (static_cast<uint32>(State.MagFilter) << 3)
			| (static_cast<uint32>(State.AddressU) << 4)
			| (static_cast<uint32>(State.AddressV) << 6);
		return static_cast<float>(Packed);
	}

	auto TryDecodeMaterialSamplerState(
		float Encoded,
		FMaterialSamplerState& OutState) -> bool
	{
		OutState = {};
		if (!std::isfinite(Encoded) || Encoded < 0.0f
			|| Encoded != std::floor(Encoded) || Encoded > 255.0f)
		{
			return false;
		}
		const uint32 Packed = static_cast<uint32>(Encoded);
		const uint32 MinFilter = Packed & 0x7u;
		const uint32 MagFilter = (Packed >> 3) & 0x1u;
		const uint32 AddressU = (Packed >> 4) & 0x3u;
		const uint32 AddressV = (Packed >> 6) & 0x3u;
		if (MinFilter > static_cast<uint32>(EMaterialSamplerMinFilter::LinearMipmapLinear)
			|| AddressU > static_cast<uint32>(EMaterialSamplerAddressMode::ClampToEdge)
			|| AddressV > static_cast<uint32>(EMaterialSamplerAddressMode::ClampToEdge))
		{
			return false;
		}
		OutState.MinFilter = static_cast<EMaterialSamplerMinFilter>(MinFilter);
		OutState.MagFilter = static_cast<EMaterialSamplerMagFilter>(MagFilter);
		OutState.AddressU = static_cast<EMaterialSamplerAddressMode>(AddressU);
		OutState.AddressV = static_cast<EMaterialSamplerAddressMode>(AddressV);
		return true;
	}

	auto TryGetMaterialRenderV3Binding(
		const FMaterialRenderRepresentation& Representation,
		FMaterialRenderV3Binding& OutBinding,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool
	{
		OutBinding = FMaterialRenderV3Binding{};
		OutDiagnostic = {};
		static const FMaterialRenderLayout ExpectedLayout =
			MakeDefaultMaterialRenderLayout();
		const FMaterialRenderLayout& Layout = Representation.GetLayout();
		if (Layout.Identity != ExpectedLayout.Identity)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::UnsupportedIdentity,
				0,
				"Material render binding layout identity is not v3.");
		}
		if (Layout != ExpectedLayout)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidField,
				0,
				"Material render binding layout does not match the v3 contract.");
		}
		const auto Payload = Representation.GetUniformPayload();
		const auto Resources = Representation.GetResources();
		if (Payload.size() != 416)
		{
			return SetValidationFailure(OutDiagnostic, EMaterialRenderValidationFailure::InvalidPayloadSize, 0,
				"Material render v3 payload size is invalid.");
		}
		if (Resources.size() != 8)
		{
			return SetValidationFailure(OutDiagnostic, EMaterialRenderValidationFailure::InvalidResource, 0,
				"Material render v3 resource count is invalid.");
		}
		auto ReadFloat = [&Payload](uint32 Offset) {
			float Value = 0.0f;
			std::memcpy(&Value, Payload.data() + Offset, sizeof(Value));
			return Value;
		};
		auto ReadVector = [&ReadFloat](uint32 Offset) {
			return FVector3f(ReadFloat(Offset), ReadFloat(Offset + 4), ReadFloat(Offset + 8));
		};
		auto ReadVector2 = [&ReadFloat](uint32 Offset) {
			return FVector2f(ReadFloat(Offset), ReadFloat(Offset + 4));
		};
		OutBinding.BaseColor = FVector4f(ReadFloat(0), ReadFloat(4), ReadFloat(8), ReadFloat(12));
		OutBinding.Emissive = ReadVector(16);
		OutBinding.Metallic = ReadFloat(28);
		OutBinding.Normal = ReadVector(32);
		OutBinding.Roughness = ReadFloat(44);
		OutBinding.AmbientOcclusion = ReadFloat(48);
		OutBinding.OpacityMask = ReadFloat(52);
		for (uint32 Role = 0; Role < 8; ++Role)
		{
			OutBinding.UVChannels[Role] = ReadFloat(64 + Role * 4);
			OutBinding.UVScales[Role] = ReadVector2(96 + Role * 16);
			OutBinding.UVOffsets[Role] = ReadVector2(224 + Role * 16);
			OutBinding.UVRotations[Role] = ReadFloat(352 + Role * 4);
			if (!TryDecodeMaterialSamplerState(
				ReadFloat(384 + Role * 4), OutBinding.Samplers[Role]))
			{
				return SetValidationFailure(OutDiagnostic, EMaterialRenderValidationFailure::InvalidField, 40 + Role,
					"Material render v3 sampler state is invalid.");
			}
			OutBinding.Textures[Role] = Resources[Role];
		}
		return true;
	}

	FMaterialRenderRepresentationBuilder::FMaterialRenderRepresentationBuilder(
		const FMaterialRenderRepresentation& Source)
		: Input{.Layout = Source.GetLayout()}
	{
		Input.UniformPayload.assign(
			Source.GetUniformPayload().begin(),
			Source.GetUniformPayload().end());
		Input.Resources.assign(
			Source.GetResources().begin(),
			Source.GetResources().end());
	}

	auto FMaterialRenderRepresentationBuilder::FindField(
		const FGuid& ParameterId) const -> const FMaterialRenderField*
	{
		const auto It = std::ranges::find(
			Input.Layout.Fields,
			ParameterId,
			&FMaterialRenderField::ParameterId);
		return It == Input.Layout.Fields.end() ? nullptr : &*It;
	}

	auto FMaterialRenderRepresentationBuilder::RejectField(
		const FGuid& ParameterId) -> bool
	{
		bInvalid = true;
		InvalidParameterId = ParameterId;
		return false;
	}

	auto FMaterialRenderRepresentationBuilder::SetScalar(
		const FGuid& ParameterId,
		float Value
	) -> bool
	{
		const FMaterialRenderField* Field = FindField(ParameterId);
		if (Field == nullptr) return false;
		if (Field->Storage != EMaterialRenderFieldStorage::Uniform
			|| Field->Type != EMaterialRenderValueType::Scalar)
		{
			return RejectField(ParameterId);
		}
		if (std::ranges::find(
				MaterialParameters::SamplerStateIds, ParameterId)
			!= MaterialParameters::SamplerStateIds.end())
		{
			FMaterialSamplerState State;
			if (!TryDecodeMaterialSamplerState(Value, State))
			{
				Value = EncodeMaterialSamplerState({});
			}
		}
		WriteFloat(Input.UniformPayload, Field->Offset, Value);
		return true;
	}

	auto FMaterialRenderRepresentationBuilder::SetVector(
		const FGuid& ParameterId,
		const FVector3& Value
	) -> bool
	{
		const FMaterialRenderField* Field = FindField(ParameterId);
		if (Field == nullptr) return false;
		if (Field->Storage != EMaterialRenderFieldStorage::Uniform
			|| Field->Type != EMaterialRenderValueType::Vector3)
		{
			return RejectField(ParameterId);
		}
		WriteFloat(Input.UniformPayload, Field->Offset, static_cast<float>(Value.x));
		WriteFloat(Input.UniformPayload, Field->Offset + 4, static_cast<float>(Value.y));
		WriteFloat(Input.UniformPayload, Field->Offset + 8, static_cast<float>(Value.z));
		return true;
	}

	auto FMaterialRenderRepresentationBuilder::SetVector2(
		const FGuid& ParameterId,
		const FVector2& Value
	) -> bool
	{
		const FMaterialRenderField* Field = FindField(ParameterId);
		if (Field == nullptr) return false;
		// The current render protocol reserves a Vector3 slot for UV transforms.
		// Preserve that protocol while keeping the authored parameter dimension exact.
		if (Field->Storage != EMaterialRenderFieldStorage::Uniform
			|| Field->Type != EMaterialRenderValueType::Vector3)
		{
			return RejectField(ParameterId);
		}
		WriteFloat(Input.UniformPayload, Field->Offset, static_cast<float>(Value.x));
		WriteFloat(Input.UniformPayload, Field->Offset + 4, static_cast<float>(Value.y));
		WriteFloat(Input.UniformPayload, Field->Offset + 8, 0.0f);
		return true;
	}

	auto FMaterialRenderRepresentationBuilder::SetTexture(
		const FGuid& ParameterId,
		const FRHITextureReferenceRef& Value
	) -> bool
	{
		const FMaterialRenderField* Field = FindField(ParameterId);
		if (Field == nullptr) return false;
		if (Field->Storage != EMaterialRenderFieldStorage::Resource
			|| Field->Type != EMaterialRenderValueType::Texture2D
			|| Field->CompactIndex >= Input.Resources.size())
		{
			return RejectField(ParameterId);
		}
		Input.Resources[Field->CompactIndex] = Value;
		return true;
	}

	auto FMaterialRenderRepresentationBuilder::Build(
		FMaterialRenderRepresentation& OutRepresentation,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool
	{
		if (bInvalid)
		{
			OutRepresentation = FMaterialRenderRepresentation();
			OutDiagnostic = {
				.Failure = EMaterialRenderValidationFailure::InvalidField,
				.FieldIndex = 0,
				.Message = std::format(
					"Material render parameter {} does not match the selected layout.",
					InvalidParameterId.ToString()),
			};
			return false;
		}
		return FMaterialRenderRepresentation::TryCreate(
			Input,
			OutRepresentation,
			OutDiagnostic);
	}

	auto UpgradeMaterialParameterSchemaVersion(
		FMaterialParameterSchemaVersion& InOutVersion,
		std::string& OutWarning,
		std::string& OutError
	) -> bool
	{
		OutWarning.clear();
		OutError.clear();
		if (InOutVersion == 0)
		{
			InOutVersion = 1;
			OutWarning = "Material parameter schema version was missing; assumed version 1 and upgraded to the current version.";
		}
		if (InOutVersion == 1)
		{
			InOutVersion = 2;
			if (OutWarning.empty()) OutWarning = "Material parameter schema version 1 was upgraded to version 2; base SpecularStrength and Shininess values were discarded while instance overrides remain orphans.";
		}
		if (InOutVersion == 2)
		{
			InOutVersion = 3;
			if (OutWarning.empty()) OutWarning = "Material parameter schema version 2 was upgraded to version 3; UV transform values were migrated to Vector2.";
		}
		if (InOutVersion == 3)
		{
			InOutVersion = 4;
			if (OutWarning.empty()) OutWarning = "Material parameter schema version 3 was upgraded to version 4; UV rotation and per-texture sampler defaults were added.";
			return true;
		}
		if (InOutVersion == CurrentMaterialParameterSchemaVersion) return true;
		OutError = std::format(
			"Material parameter schema version {} is unsupported; expected {}.",
			InOutVersion,
			CurrentMaterialParameterSchemaVersion);
		return false;
	}

	auto GetErrorMaterialRenderData() -> const FMaterialRenderData&
	{
		static const FMaterialRenderData ErrorMaterial;
		return ErrorMaterial;
	}

	auto RecordMaterialFallbackReason(EMaterialFallbackReason Reason) -> void
	{
		const size_t Index = static_cast<size_t>(Reason);
		if (Index < GMaterialFallbackCounts.size())
		{
			GMaterialFallbackCounts[Index].fetch_add(
				1, std::memory_order_relaxed);
		}
	}

	auto GetMaterialFallbackDiagnosticsSnapshot()
		-> FMaterialFallbackDiagnosticsSnapshot
	{
		FMaterialFallbackDiagnosticsSnapshot Result;
		for (size_t Index = 0; Index < GMaterialFallbackCounts.size(); ++Index)
		{
			Result.Counts[Index] = GMaterialFallbackCounts[Index].load(
				std::memory_order_relaxed);
		}
		return Result;
	}

	auto ResetMaterialFallbackDiagnosticsForTests() -> void
	{
		for (std::atomic<uint64>& Count : GMaterialFallbackCounts)
		{
			Count.store(0, std::memory_order_relaxed);
		}
	}

	auto FMaterialParameterValue::MakeScalar(float Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.ScalarValue = Value;
		return Result;
	}

	auto FMaterialParameterValue::MakeVector(const FVector3& Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.VectorValue = Value;
		return Result;
	}

	auto FMaterialParameterValue::MakeVector2(const FVector2& Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.Vector2Value = Value;
		return Result;
	}

	auto FMaterialParameterValue::MakeTexture(DTexture2D* Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.TextureValue = Value;
		return Result;
	}

	namespace MaterialParameters
	{
		auto BaseColorName() -> const FName&
		{
			static const FName Name("BaseColor");
			return Name;
		}

		auto BaseColorTextureName() -> const FName&
		{
			static const FName Name("BaseColorTexture");
			return Name;
		}

		auto OpacityName() -> const FName&
		{
			static const FName Name("Opacity");
			return Name;
		}

		auto SpecularStrengthName() -> const FName&
		{
			static const FName Name("SpecularStrength");
			return Name;
		}

		auto ShininessName() -> const FName&
		{
			static const FName Name("Shininess");
			return Name;
		}

#define DURIN_DEFINE_MATERIAL_PARAMETER_NAME(FunctionName, Literal) \
		auto FunctionName() -> const FName& { static const FName Name(Literal); return Name; }
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(NormalName, "Normal")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(NormalTextureName, "NormalTexture")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(MetallicName, "Metallic")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(MetallicTextureName, "MetallicTexture")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(RoughnessName, "Roughness")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(RoughnessTextureName, "RoughnessTexture")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(AmbientOcclusionName, "AmbientOcclusion")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(AmbientOcclusionTextureName, "AmbientOcclusionTexture")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(EmissiveName, "Emissive")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(EmissiveTextureName, "EmissiveTexture")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(OpacityTextureName, "OpacityTexture")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(OpacityMaskName, "OpacityMask")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(OpacityMaskTextureName, "OpacityMaskTexture")
#undef DURIN_DEFINE_MATERIAL_PARAMETER_NAME
	}

	namespace
	{
		auto MakeDefinition(
			FGuid Id,
			FName Name,
			EMaterialParameterType Type,
			FMaterialParameterValue Value,
			std::string DisplayName,
			int32 SortOrder,
			EMaterialParameterPresentation Presentation,
			bool bHasRange = false,
			float MinimumValue = 0.0f,
			float MaximumValue = 0.0f,
			ETextureUsage TextureUsage = ETextureUsage::Color,
			FName GroupName = FName("Surface")
		) -> FMaterialParameterDefinition
		{
			FMaterialParameterDefinition Result;
			Result.Id = Id;
			Result.Name = Name;
			Result.Type = Type;
			Result.Value = std::move(Value);
			Result.DisplayName = std::move(DisplayName);
			Result.GroupName = GroupName;
			Result.SortOrder = SortOrder;
			Result.Presentation = Presentation;
			Result.bHasRange = bHasRange;
			Result.MinimumValue = MinimumValue;
			Result.MaximumValue = MaximumValue;
			Result.TextureUsage = TextureUsage;
			return Result;
		}

		auto HasCanonicalMetadata(
			const FMaterialParameterDefinition& Definition,
			const FMaterialParameterDefinition& Canonical
		) -> bool
		{
			return Definition.Id == Canonical.Id
				&& Definition.Name == Canonical.Name
				&& Definition.Type == Canonical.Type
				&& Definition.DisplayName == Canonical.DisplayName
				&& Definition.GroupName == Canonical.GroupName
				&& Definition.SortOrder == Canonical.SortOrder
				&& Definition.Presentation == Canonical.Presentation
				&& Definition.bHasRange == Canonical.bHasRange
				&& Definition.MinimumValue == Canonical.MinimumValue
				&& Definition.MaximumValue == Canonical.MaximumValue
				&& Definition.TextureUsage == Canonical.TextureUsage;
		}
	}

	auto MakeCanonicalMaterialParameterDefinitions() -> std::vector<FMaterialParameterDefinition>
	{
		using namespace MaterialParameters;
		std::vector<FMaterialParameterDefinition> Result;
		Result.reserve(56);
		const std::array ConstantIds{BaseColorId, NormalId, MetallicId, RoughnessId, AmbientOcclusionId, EmissiveId, OpacityId, OpacityMaskId};
		const std::array ConstantNames{&BaseColorName(), &NormalName(), &MetallicName(), &RoughnessName(), &AmbientOcclusionName(), &EmissiveName(), &OpacityName(), &OpacityMaskName()};
		const std::array TextureNames{&BaseColorTextureName(), &NormalTextureName(), &MetallicTextureName(), &RoughnessTextureName(), &AmbientOcclusionTextureName(), &EmissiveTextureName(), &OpacityTextureName(), &OpacityMaskTextureName()};
		const std::array RoleNames{"BaseColor", "Normal", "Metallic", "Roughness", "AmbientOcclusion", "Emissive", "Opacity", "OpacityMask"};
		const std::array DisplayNames{"Base Color", "Normal", "Metallic", "Roughness", "Ambient Occlusion", "Emissive", "Opacity", "Opacity Mask"};
		const std::array GroupNames{"Surface/Base", "Surface/Normal", "Surface/Metallic", "Surface/Roughness", "Surface/Ambient Occlusion", "Surface/Emissive", "Surface/Opacity", "Surface/Opacity Mask"};
		const std::array TextureUsages{ETextureUsage::Color, ETextureUsage::Normal, ETextureUsage::DataMask, ETextureUsage::DataMask, ETextureUsage::DataMask, ETextureUsage::Color, ETextureUsage::DataMask, ETextureUsage::DataMask};
		for (size_t Role = 0; Role < 8; ++Role)
		{
			const bool bVector = Role == 0 || Role == 1 || Role == 5;
			FMaterialParameterValue ConstantValue;
			float Minimum = 0.0f;
			float Maximum = 1.0f;
			if (Role == 0) ConstantValue = FMaterialParameterValue::MakeVector({0.95, 0.62, 0.22});
			else if (Role == 1) { ConstantValue = FMaterialParameterValue::MakeVector({0.0, 0.0, 1.0}); Minimum = -1.0f; }
			else if (Role == 3) ConstantValue = FMaterialParameterValue::MakeScalar(0.5f);
			else if (Role == 5) { ConstantValue = FMaterialParameterValue::MakeVector(FVector3(0.0)); Maximum = 64.0f; }
			else if (Role == 2) ConstantValue = FMaterialParameterValue::MakeScalar(0.0f);
			else ConstantValue = FMaterialParameterValue::MakeScalar(1.0f);
			const int32 Sort = static_cast<int32>(Role * 7);
			const FName Group(GroupNames[Role]);
			Result.push_back(MakeDefinition(ConstantIds[Role], *ConstantNames[Role], bVector ? EMaterialParameterType::Vector : EMaterialParameterType::Scalar,
				ConstantValue, DisplayNames[Role], Sort, (Role == 0 || Role == 5) ? EMaterialParameterPresentation::Color : EMaterialParameterPresentation::Drag,
				true, Minimum, Maximum, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(TextureIds[Role], *TextureNames[Role], EMaterialParameterType::Texture,
				FMaterialParameterValue::MakeTexture(nullptr), std::string(DisplayNames[Role]) + " Texture", Sort + 1,
				EMaterialParameterPresentation::AssetPicker, false, 0.0f, 0.0f, TextureUsages[Role], Group));
			Result.push_back(MakeDefinition(UVChannelIds[Role], FName(std::string(RoleNames[Role]) + "UVChannel"), EMaterialParameterType::Scalar,
				FMaterialParameterValue::MakeScalar(0.0f), "UV Channel", Sort + 2, EMaterialParameterPresentation::Integer,
				true, 0.0f, 3.0f, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(UVScaleIds[Role], FName(std::string(RoleNames[Role]) + "UVScale"), EMaterialParameterType::Vector2,
				FMaterialParameterValue::MakeVector2({1.0, 1.0}), "UV Scale", Sort + 3, EMaterialParameterPresentation::Drag,
				true, -1024.0f, 1024.0f, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(UVOffsetIds[Role], FName(std::string(RoleNames[Role]) + "UVOffset"), EMaterialParameterType::Vector2,
				FMaterialParameterValue::MakeVector2(FVector2(0.0)), "UV Offset", Sort + 4, EMaterialParameterPresentation::Drag,
				true, -1024.0f, 1024.0f, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(UVRotationIds[Role], FName(std::string(RoleNames[Role]) + "UVRotation"), EMaterialParameterType::Scalar,
				FMaterialParameterValue::MakeScalar(0.0f), "UV Rotation (Radians)", Sort + 5, EMaterialParameterPresentation::Drag,
				true, -1024.0f, 1024.0f, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(SamplerStateIds[Role], FName(std::string(RoleNames[Role]) + "SamplerState"), EMaterialParameterType::Scalar,
				FMaterialParameterValue::MakeScalar(EncodeMaterialSamplerState({})), "Sampler State", Sort + 6, EMaterialParameterPresentation::Integer,
				true, 0.0f, 255.0f, ETextureUsage::Color, Group));
		}
		return Result;
	}

	auto GetCanonicalMaterialParameterDefinitions() -> std::span<const FMaterialParameterDefinition>
	{
		static const std::vector<FMaterialParameterDefinition> Definitions = MakeCanonicalMaterialParameterDefinitions();
		return Definitions;
	}

	auto ValidateCanonicalMaterialParameterDefinitions(
		std::span<const FMaterialParameterDefinition> Definitions,
		std::string& OutError
	) -> bool
	{
		OutError.clear();
		const std::span Canonical = GetCanonicalMaterialParameterDefinitions();
		if (Definitions.size() != Canonical.size())
		{
			OutError = std::format("Material parameter schema contains {} definitions; expected {}.", Definitions.size(), Canonical.size());
			return false;
		}

		std::unordered_set<FGuid> Ids;
		std::unordered_set<FName> Names;
		for (size_t Index = 0; Index < Definitions.size(); ++Index)
		{
			const FMaterialParameterDefinition& Definition = Definitions[Index];
			if (!Definition.Id.IsValid())
			{
				OutError = std::format("Material parameter definition {} has an invalid GUID.", Index);
				return false;
			}
			if (!Ids.insert(Definition.Id).second)
			{
				OutError = std::format("Material parameter schema contains duplicate GUID {}.", Definition.Id.ToString());
				return false;
			}
			if (Definition.Name.IsNone())
			{
				OutError = std::format("Material parameter definition {} has a None name.", Index);
				return false;
			}
			if (!Names.insert(Definition.Name).second)
			{
				OutError = std::format("Material parameter schema contains duplicate name '{}'.", Definition.Name.ToString());
				return false;
			}
			if (!HasCanonicalMetadata(Definition, Canonical[Index]))
			{
				OutError = std::format("Material parameter definition {} ('{}') does not match the canonical identity, type, order, or metadata.",
					Index, Definition.Name.ToString());
				return false;
			}
		}
		return true;
	}

	auto ValidateMaterialStaticProperties(
		const FMaterialStaticProperties& Properties,
		std::string& OutError
	) -> bool
	{
		OutError.clear();
		switch (Properties.BlendMode)
		{
		case EMaterialBlendMode::Opaque:
		case EMaterialBlendMode::Masked:
		case EMaterialBlendMode::Translucent:
			break;
		default:
			OutError = "Material blend mode is invalid.";
			return false;
		}
		switch (Properties.ShadingModel)
		{
		case EMaterialShadingModel::Lit:
		case EMaterialShadingModel::Unlit:
			break;
		default:
			OutError = "Material shading model is invalid.";
			return false;
		}
		switch (Properties.DepthWritePolicy)
		{
		case EMaterialDepthWritePolicy::Automatic:
		case EMaterialDepthWritePolicy::Enabled:
		case EMaterialDepthWritePolicy::Disabled:
			break;
		default:
			OutError = "Material depth-write policy is invalid.";
			return false;
		}
		if (!std::isfinite(Properties.OpacityMaskThreshold)
			|| Properties.OpacityMaskThreshold < 0.0f
			|| Properties.OpacityMaskThreshold > 1.0f)
		{
			OutError = "Material opacity-mask threshold must be finite and in the inclusive range [0, 1].";
			return false;
		}
		return true;
	}
}
