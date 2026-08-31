#include "Materials/MaterialRenderTypes.h"

namespace Durin
{
	namespace
	{
		auto SetValidationFailure(FMaterialRenderValidationDiagnostic& OutDiagnostic,
			EMaterialRenderValidationFailure Failure, uint32 FieldIndex, std::string Message) -> bool
		{
			OutDiagnostic.Failure = Failure;
			OutDiagnostic.FieldIndex = FieldIndex;
			OutDiagnostic.Message = std::move(Message);
			return false;
		}

		auto WriteFloat(FByteArray& Bytes, uint32 Offset, float Value) -> void
		{
			std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
		}

		auto MakeErrorUniformPayload() -> FByteArray
		{
			FByteArray Result(416, std::byte{0});
			WriteFloat(Result, 0, 1.0f); WriteFloat(Result, 8, 1.0f);
			WriteFloat(Result, 12, 1.0f); WriteFloat(Result, 40, 1.0f);
			WriteFloat(Result, 44, 0.5f); WriteFloat(Result, 48, 1.0f);
			WriteFloat(Result, 52, 1.0f);
			for (uint32 Role = 0; Role < 8; ++Role)
			{
				WriteFloat(Result, 96 + Role * 16, 1.0f);
				WriteFloat(Result, 100 + Role * 16, 1.0f);
				WriteFloat(Result, 384 + Role * 4, 13.0f);
			}
			return Result;
		}
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
		FByteArray InUniformPayload,
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

	auto TryGetMaterialRenderBinding(
		const FMaterialRenderRepresentation& Representation,
		FMaterialRenderBinding& OutBinding,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool
	{
		OutBinding = FMaterialRenderBinding{};
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
		if (MaterialParameters::FindBuiltinParameterRole(ParameterId,
				MaterialParameters::EMaterialBuiltinParameterKind::SamplerState)
				!= MaterialParameters::EMaterialBuiltinParameterRole::Count)
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

}
