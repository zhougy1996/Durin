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

		auto WriteFloat(FByteBuffer& Bytes, uint32 Offset, float Value) -> void
		{
			std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
		}

	}
	FMaterialRenderRepresentation::FMaterialRenderRepresentation()
		: Layout(MakeErrorMaterialRenderLayout())
		, UniformPayload(MaterialUniformControlBytes, std::byte{0})
		, bError(true)
	{
	}

	FMaterialRenderRepresentation::FMaterialRenderRepresentation(
		FMaterialRenderLayout InLayout,
		FByteBuffer InUniformPayload,
		std::vector<FRHITextureReferenceRef> InResources,
		std::vector<FMaterialSamplerState> InSamplers,
		std::vector<EMaterialTextureFallback> InFallbacks,
		bool bInError)
		: Layout(std::move(InLayout))
		, UniformPayload(std::move(InUniformPayload))
		, Resources(std::move(InResources))
		, Samplers(std::move(InSamplers))
		, TextureFallbacks(std::move(InFallbacks))
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

		if (Input.Layout.Identity.Version == CompiledMaterialRenderLayoutVersion)
		{
			if (Input.Samplers.size() != Input.Resources.size()
				|| Input.TextureFallbacks.size() != Input.Resources.size())
				return SetValidationFailure(OutDiagnostic, EMaterialRenderValidationFailure::InvalidCounts, 0,
					"Material sampler and fallback counts must match texture resources.");
			for (uint32 Index = 0; Index < Input.Resources.size(); ++Index)
				if (!IsValidMaterialSampling(Input.Samplers[Index], Input.TextureFallbacks[Index]))
					return SetValidationFailure(OutDiagnostic, EMaterialRenderValidationFailure::InvalidResource, Index,
						"Material sampling state or fallback is invalid.");
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
			std::move(Input.Samplers),
			std::move(Input.TextureFallbacks),
			false);
		return true;
	}

	auto FMaterialRenderRepresentation::GetLayout() const
		-> const FMaterialRenderLayout&
	{
		return Layout;
	}

	auto FMaterialRenderRepresentation::GetUniformPayload() const
		-> FByteView
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
		if (Representation.GetLayout().Identity.Version == CompiledMaterialRenderLayoutVersion)
		{
			if (!ValidateMaterialRenderLayout(Representation.GetLayout(), OutDiagnostic)) return false;
			OutBinding.bError = Representation.IsError();
			OutBinding.LayoutIdentity = Representation.GetLayout().Identity;
			OutBinding.CompiledUniformPayload.assign(Representation.GetUniformPayload().begin(), Representation.GetUniformPayload().end());
			OutBinding.CompiledTextures.assign(Representation.GetResources().begin(), Representation.GetResources().end());
			OutBinding.CompiledSamplers.assign(Representation.GetSamplers().begin(), Representation.GetSamplers().end());
			OutBinding.CompiledTextureFallbacks.assign(Representation.GetTextureFallbacks().begin(), Representation.GetTextureFallbacks().end());
			return true;
		}
		return SetValidationFailure(OutDiagnostic,
			EMaterialRenderValidationFailure::UnsupportedVersion, 0,
			"Only compiled material layout v4 can bind.");
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
		Input.Samplers.assign(Source.GetSamplers().begin(), Source.GetSamplers().end());
		Input.TextureFallbacks.assign(Source.GetTextureFallbacks().begin(), Source.GetTextureFallbacks().end());
	}

	FMaterialRenderRepresentationBuilder::FMaterialRenderRepresentationBuilder(const FMaterialRenderLayout& Layout)
	{
		Input.Layout = Layout;
		const auto Valid = ValidateCompiledMaterialLayout(Layout);
		bInvalid = !Valid;
		if (bInvalid) return;
		Input.UniformPayload.resize(Layout.UniformPayloadSize, std::byte{0});
		Input.Resources.resize(Layout.ResourceFieldCount);
		Input.Samplers.resize(Layout.ResourceFieldCount);
		Input.TextureFallbacks.resize(Layout.ResourceFieldCount, EMaterialTextureFallback::White);
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
		if (Field->Storage != EMaterialRenderFieldStorage::Uniform
			|| Field->Type != EMaterialRenderValueType::Vector2)
		{
			return RejectField(ParameterId);
		}
		WriteFloat(Input.UniformPayload, Field->Offset, static_cast<float>(Value.x));
		WriteFloat(Input.UniformPayload, Field->Offset + 4, static_cast<float>(Value.y));
		WriteFloat(Input.UniformPayload, Field->Offset + 8, 0.0f);
		return true;
	}

	auto FMaterialRenderRepresentationBuilder::SetVector4(const FGuid& ParameterId, const FVector4& Value) -> bool
	{
		const auto* Field = FindField(ParameterId);
		if (!Field) return false;
		if (Field->Storage != EMaterialRenderFieldStorage::Uniform || Field->Type != EMaterialRenderValueType::Vector4)
			return RejectField(ParameterId);
		WriteFloat(Input.UniformPayload, Field->Offset, static_cast<float>(Value.x));
		WriteFloat(Input.UniformPayload, Field->Offset + 4, static_cast<float>(Value.y));
		WriteFloat(Input.UniformPayload, Field->Offset + 8, static_cast<float>(Value.z));
		WriteFloat(Input.UniformPayload, Field->Offset + 12, static_cast<float>(Value.w));
		return true;
	}

	auto FMaterialRenderRepresentationBuilder::SetTexture(
		const FGuid& ParameterId,
		const FRHITextureReferenceRef& Value,
		FMaterialSamplerState Sampler,
		EMaterialTextureFallback Fallback
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
		if (Input.Layout.Identity.Version == CompiledMaterialRenderLayoutVersion)
		{
			if (!IsValidMaterialSampling(Sampler, Fallback)) return RejectField(ParameterId);
			Input.Samplers[Field->CompactIndex] = Sampler;
			Input.TextureFallbacks[Field->CompactIndex] = Fallback;
		}
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
