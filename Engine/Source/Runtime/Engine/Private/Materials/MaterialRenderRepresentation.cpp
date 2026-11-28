#include "Materials/MaterialRenderTypes.h"
#include "Hash/XxHash.h"

namespace Durin
{
	struct FMaterialRenderRepresentation::FStorage
	{
		std::shared_ptr<const FMaterialRenderLayout> Layout;
		FByteBuffer UniformPayload;
		std::vector<FRHITextureReferenceRef> Resources;
		std::vector<FMaterialSamplerState> Samplers;
		std::vector<EMaterialTextureFallback> TextureFallbacks;
		uint64 RecordId = 0;
		uint64 ContentHash = 0;
	};
	namespace
	{
		auto SetValidationFailure(FMaterialRenderValidationDiagnostic& OutDiagnostic,
			std::string Message) -> bool
		{
			OutDiagnostic.Message = std::move(Message);
			return false;
		}

		auto WriteFloat(FByteBuffer& Bytes, uint32 Offset, float Value) -> void
		{
			std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
		}

	}
	FMaterialRenderRepresentation::FMaterialRenderRepresentation()
	{
		static const FMaterialRenderRepresentation Error(std::make_shared<const FMaterialRenderLayout>(MakeErrorMaterialRenderLayout()),
			FByteBuffer(MaterialUniformHeaderBytes, std::byte{0}), {}, {}, {});
		Storage = Error.Storage;
	}

	FMaterialRenderRepresentation::FMaterialRenderRepresentation(
		std::shared_ptr<const FMaterialRenderLayout> InLayout,
		FByteBuffer InUniformPayload,
		std::vector<FRHITextureReferenceRef> InResources,
		std::vector<FMaterialSamplerState> InSamplers,
		std::vector<EMaterialTextureFallback> InFallbacks)
	{
		static std::atomic<uint64> NextRecordId{1};
		const uint64 Id = NextRecordId.fetch_add(1, std::memory_order_relaxed);
		checkf(Id != 0, "Material publication record IDs exhausted.");
		FXxHash64Builder Hash;
		Hash.Update(InUniformPayload);
		Hash.UpdateValue(InLayout->Identity.Version);
		Hash.UpdateValue(InLayout->Identity.Id);
		for (const auto& Resource : InResources) Hash.UpdateValue(Resource.GetReference());
		for (const auto& Sampler : InSamplers)
		{
			Hash.UpdateValue(Sampler.MinFilter);
			Hash.UpdateValue(Sampler.MagFilter);
			Hash.UpdateValue(Sampler.AddressU);
			Hash.UpdateValue(Sampler.AddressV);
			Hash.UpdateValue(Sampler.AddressW);
		}
		for (const auto Fallback : InFallbacks) Hash.UpdateValue(Fallback);
		Storage = std::make_shared<const FStorage>(FStorage{
			std::move(InLayout), std::move(InUniformPayload), std::move(InResources),
			std::move(InSamplers), std::move(InFallbacks), Id, Hash.Finalize().HashValue});
	}

	auto FMaterialRenderRepresentation::TryCreate(FMaterialRenderLayout Layout, FMaterialRenderRepresentationInput Input,
		FMaterialRenderRepresentation& OutRepresentation, FMaterialRenderValidationDiagnostic& OutDiagnostic) -> bool
	{
		return TryCreateFromLayout(std::make_shared<const FMaterialRenderLayout>(std::move(Layout)),
			std::move(Input), OutRepresentation, OutDiagnostic);
	}

	auto FMaterialRenderRepresentation::TryCreateFromLayout(
		std::shared_ptr<const FMaterialRenderLayout> Layout,
		FMaterialRenderRepresentationInput Input,
		FMaterialRenderRepresentation& OutRepresentation,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool
	{
		OutRepresentation = FMaterialRenderRepresentation();
		OutDiagnostic = {};
		if (!ValidateMaterialRenderLayout(*Layout, OutDiagnostic)) return false;
		if (Input.UniformPayload.size() != Layout->UniformPayloadSize)
		{
			SetValidationFailure(OutDiagnostic, "Material render uniform payload size does not match its layout.");
			return false;
		}
		if (Input.Resources.size() != Layout->ResourceFieldCount)
		{
			SetValidationFailure(OutDiagnostic, "Material render resource count does not match its layout.");
			return false;
		}

		if (Layout->Identity.Version == CompiledMaterialRenderLayoutVersion)
		{
			if (Input.Samplers.size() != Input.Resources.size()
				|| Input.TextureFallbacks.size() != Input.Resources.size())
				return SetValidationFailure(OutDiagnostic, "Material sampler and fallback counts must match texture resources.");
			for (uint32 Index = 0; Index < Input.Resources.size(); ++Index)
				if (!IsValidMaterialSampling(Input.Samplers[Index], Input.TextureFallbacks[Index]))
					return SetValidationFailure(OutDiagnostic, std::format("Material sampling state or fallback is invalid. [index {}]", Index));
		}
		std::vector<bool> Covered(Input.UniformPayload.size(), false);
		for (uint32 FieldIndex = 0; FieldIndex < Layout->Fields.size(); ++FieldIndex)
		{
			const FMaterialRenderField& Field = Layout->Fields[FieldIndex];
			if (Field.Storage == EMaterialRenderFieldStorage::Resource)
			{
				if (Field.Type == EMaterialRenderValueType::TextureCube && Input.TextureFallbacks[Field.CompactIndex] == EMaterialTextureFallback::FlatRGNormal)
					return SetValidationFailure(OutDiagnostic, "Cube textures cannot use a 2D normal fallback.");
				continue;
			}
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
					SetValidationFailure(OutDiagnostic, std::format("Material render uniform payload contains a non-finite value. [index {}]", FieldIndex));
					return false;
				}
			}
		}
		for (uint32 ByteIndex = 0; ByteIndex < Input.UniformPayload.size(); ++ByteIndex)
		{
			if (!Covered[ByteIndex] && Input.UniformPayload[ByteIndex] != std::byte{0})
			{
				SetValidationFailure(OutDiagnostic, "Material render uniform padding must be zero.");
				return false;
			}
		}

		OutRepresentation = FMaterialRenderRepresentation(
			std::move(Layout),
			std::move(Input.UniformPayload),
			std::move(Input.Resources),
			std::move(Input.Samplers),
			std::move(Input.TextureFallbacks));
		return true;
	}

	auto FMaterialRenderRepresentation::GetLayout() const
		-> const FMaterialRenderLayout&
	{
		return *Storage->Layout;
	}

	auto FMaterialRenderRepresentation::GetUniformPayload() const
		-> FByteView
	{
		return Storage->UniformPayload;
	}

	auto FMaterialRenderRepresentation::GetResources() const
		-> std::span<const FRHITextureReferenceRef>
	{
		return Storage->Resources;
	}

	auto FMaterialRenderRepresentation::GetSamplers() const -> std::span<const FMaterialSamplerState>
	{ return Storage->Samplers; }
	auto FMaterialRenderRepresentation::GetTextureFallbacks() const -> std::span<const EMaterialTextureFallback>
	{ return Storage->TextureFallbacks; }
	auto FMaterialRenderRepresentation::GetRecordId() const -> uint64
	{ return Storage->RecordId; }
	auto FMaterialRenderRepresentation::GetContentHash() const -> uint64
	{ return Storage->ContentHash; }

	FMaterialRenderRepresentationBuilder::FMaterialRenderRepresentationBuilder(
		const FMaterialRenderRepresentation& Source)
		: Layout(Source.Storage->Layout)
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

	FMaterialRenderRepresentationBuilder::FMaterialRenderRepresentationBuilder(const FMaterialRenderLayout& InLayout)
		: FMaterialRenderRepresentationBuilder(std::make_shared<const FMaterialRenderLayout>(InLayout))
	{
	}

	FMaterialRenderRepresentationBuilder::FMaterialRenderRepresentationBuilder(std::shared_ptr<const FMaterialRenderLayout> InLayout)
		: Layout(std::move(InLayout))
	{
		const auto Valid = ValidateCompiledMaterialLayout(*Layout);
		bInvalid = !Valid;
		if (bInvalid) return;
		Input.UniformPayload.resize(Layout->UniformPayloadSize, std::byte{0});
		Input.Resources.resize(Layout->ResourceFieldCount);
		Input.Samplers.resize(Layout->ResourceFieldCount);
		Input.TextureFallbacks.resize(Layout->ResourceFieldCount, EMaterialTextureFallback::White);
	}

	auto FMaterialRenderRepresentationBuilder::SetParameter(const FMaterialFieldKey& Id, const FMaterialParameterValue& Value) -> bool
	{
		const auto* Field = FindField(Id);
		if (!Field) return RejectField(Id);
		return WriteBoundParameter(static_cast<uint32>(Field - Layout->Fields.data()), Value);
	}

	auto FMaterialRenderRepresentationBuilder::WriteBoundParameter(uint32 FieldIndex, const FMaterialParameterValue& Value) -> bool
	{
		if (bInvalid || FieldIndex >= Layout->Fields.size()) return RejectField({});
		const auto& Field = Layout->Fields[FieldIndex];
		const auto& Id = Field.ParameterId;
		if (Value.GetType() == EMaterialParameterType::Texture)
		{
			const auto& Texture = Value.GetTexture();
			if (!Value.IsRenderThreadValue() || Field.Storage != EMaterialRenderFieldStorage::Resource
				|| Field.Type != (Texture.Dimension == EMaterialTextureDimension::TextureCube ? EMaterialRenderValueType::TextureCube : EMaterialRenderValueType::Texture2D) || Field.CompactIndex >= Input.Resources.size()
				|| !Texture.IsValid()) return RejectField(Id);
			Input.Resources[Field.CompactIndex] = Texture.TextureReference;
			Input.Samplers[Field.CompactIndex] = Texture.SamplerState;
			Input.TextureFallbacks[Field.CompactIndex] = Texture.TextureFallback;
			return true;
		}
		FVector4 Vector(0);
		EMaterialRenderValueType Type = EMaterialRenderValueType::Scalar;
		uint32 Components = 1;
		switch (Value.GetType()) {
		case EMaterialParameterType::Scalar: Vector.x = Value.GetScalar(); break;
		case EMaterialParameterType::Vector2: Type = EMaterialRenderValueType::Vector2; Components = 2; Vector = FVector4(Value.GetVector2(), 0, 0); break;
		case EMaterialParameterType::Vector: Type = EMaterialRenderValueType::Vector3; Components = 3; Vector = FVector4(Value.GetVector(), 0); break;
		case EMaterialParameterType::Vector4: Type = EMaterialRenderValueType::Vector4; Components = 4; Vector = Value.GetVector4(); break;
		default: return RejectField(Id);
		}
		if (Field.Storage != EMaterialRenderFieldStorage::Uniform || Field.Type != Type) return RejectField(Id);
		for (uint32 Index = 0; Index < Components; ++Index) WriteFloat(Input.UniformPayload, Field.Offset + Index * 4, static_cast<float>(Vector[Index]));
		return true;
	}

	auto FMaterialRenderRepresentationBuilder::FindField(
		const FMaterialFieldKey& ParameterId) const -> const FMaterialRenderField*
	{
		const auto It = std::ranges::find(
			Layout->Fields,
			ParameterId,
			&FMaterialRenderField::ParameterId);
		return It == Layout->Fields.end() ? nullptr : &*It;
	}

	auto FMaterialRenderRepresentationBuilder::RejectField(
		const FMaterialFieldKey& ParameterId) -> bool
	{
		bInvalid = true;
		InvalidParameterId = ParameterId;
		return false;
	}

	auto FMaterialRenderRepresentationBuilder::SetScalar(
		const FMaterialFieldKey& ParameterId,
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
		const FMaterialFieldKey& ParameterId,
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
		const FMaterialFieldKey& ParameterId,
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

	auto FMaterialRenderRepresentationBuilder::SetVector4(const FMaterialFieldKey& ParameterId, const FVector4& Value) -> bool
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
		const FMaterialFieldKey& ParameterId,
		const FRHITextureReferenceRef& Value,
		FMaterialSamplerState Sampler,
		EMaterialTextureFallback Fallback
	) -> bool
	{
		const FMaterialRenderField* Field = FindField(ParameterId);
		if (Field == nullptr) return false;
		if (Field->Storage != EMaterialRenderFieldStorage::Resource
			|| (Field->Type != EMaterialRenderValueType::Texture2D && Field->Type != EMaterialRenderValueType::TextureCube)
			|| Field->CompactIndex >= Input.Resources.size())
		{
			return RejectField(ParameterId);
		}
		Input.Resources[Field->CompactIndex] = Value;
		if (Layout->Identity.Version == CompiledMaterialRenderLayoutVersion)
		{
			if (!IsValidMaterialSampling(Sampler, Fallback) || (Field->Type == EMaterialRenderValueType::TextureCube && Fallback == EMaterialTextureFallback::FlatRGNormal)) return RejectField(ParameterId);
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
				.Message = std::format("{} [parameter {}]", GetMaterialLayoutErrorText(EMaterialLayoutError::InvalidField), InvalidParameterId.ToString()),
			};
			return false;
		}
		return FMaterialRenderRepresentation::TryCreateFromLayout(
			Layout, Input,
			OutRepresentation,
			OutDiagnostic);
	}

}
