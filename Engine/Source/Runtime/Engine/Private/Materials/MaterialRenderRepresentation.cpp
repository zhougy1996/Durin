#include "Materials/MaterialRenderTypes.h"
#include "Hash/XxHash.h"
#include <atomic>

namespace Durin
{
	struct FMaterialRenderRepresentation::FStorage
	{
		FMaterialRenderLayout Layout;
		FByteBuffer UniformPayload;
		std::vector<FRHITextureReferenceRef> Resources;
		std::vector<FMaterialSamplerState> Samplers;
		std::vector<EMaterialTextureFallback> TextureFallbacks;
		bool bError = false;
		uint64 RecordId = 0;
		uint64 ContentHash = 0;
	};
	namespace
	{
		auto SetValidationFailure(FMaterialRenderValidationDiagnostic& OutDiagnostic,
			EMaterialRenderValidationFailure Failure, uint32 FieldIndex, FMaterialError Error) -> bool
		{
			OutDiagnostic.Failure = Failure;
			OutDiagnostic.FieldIndex = FieldIndex;
			OutDiagnostic.Error = std::move(Error);
			return false;
		}

		auto WriteFloat(FByteBuffer& Bytes, uint32 Offset, float Value) -> void
		{
			std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
		}

	}
	FMaterialRenderRepresentation::FMaterialRenderRepresentation()
	{
		static const FMaterialRenderRepresentation Error(MakeErrorMaterialRenderLayout(),
			FByteBuffer(MaterialUniformHeaderBytes, std::byte{0}), {}, {}, {}, true);
		Storage = Error.Storage;
	}

	FMaterialRenderRepresentation::FMaterialRenderRepresentation(
		FMaterialRenderLayout InLayout,
		FByteBuffer InUniformPayload,
		std::vector<FRHITextureReferenceRef> InResources,
		std::vector<FMaterialSamplerState> InSamplers,
		std::vector<EMaterialTextureFallback> InFallbacks,
		bool bInError)
	{
		static std::atomic<uint64> NextRecordId{1};
		const uint64 Id = NextRecordId.fetch_add(1, std::memory_order_relaxed);
		checkf(Id != 0, "Material publication record IDs exhausted.");
		FXxHash64Builder Hash;
		Hash.Update(InUniformPayload);
		Hash.UpdateValue(InLayout.Identity.Version);
		Hash.UpdateValue(InLayout.Identity.Id);
		Hash.UpdateValue(bInError);
		for (const auto& Resource : InResources) Hash.UpdateValue(Resource.GetReference());
		for (const auto& Sampler : InSamplers)
		{
			Hash.UpdateValue(Sampler.MinFilter);
			Hash.UpdateValue(Sampler.MagFilter);
			Hash.UpdateValue(Sampler.AddressU);
			Hash.UpdateValue(Sampler.AddressV);
		}
		for (const auto Fallback : InFallbacks) Hash.UpdateValue(Fallback);
		Storage = std::make_shared<const FStorage>(FStorage{
			std::move(InLayout), std::move(InUniformPayload), std::move(InResources),
			std::move(InSamplers), std::move(InFallbacks), bInError, Id, Hash.Finalize().HashValue});
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
				EMaterialRenderError::PayloadSizeMismatch);
			return false;
		}
		if (Input.Resources.size() != Input.Layout.ResourceFieldCount)
		{
			SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidResource,
				0,
				EMaterialRenderError::ResourceCountMismatch);
			return false;
		}

		if (Input.Layout.Identity.Version == CompiledMaterialRenderLayoutVersion)
		{
			if (Input.Samplers.size() != Input.Resources.size()
				|| Input.TextureFallbacks.size() != Input.Resources.size())
				return SetValidationFailure(OutDiagnostic, EMaterialRenderValidationFailure::InvalidCounts, 0,
					EMaterialRenderError::SamplerCountMismatch);
			for (uint32 Index = 0; Index < Input.Resources.size(); ++Index)
				if (!IsValidMaterialSampling(Input.Samplers[Index], Input.TextureFallbacks[Index]))
					return SetValidationFailure(OutDiagnostic, EMaterialRenderValidationFailure::InvalidResource, Index,
						EMaterialRenderError::SamplingStateFallbackInvalid);
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
						EMaterialRenderError::UniformPayloadContainsNonFiniteValue);
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
					EMaterialRenderError::NonZeroPadding);
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
		return Storage->Layout;
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

	auto FMaterialRenderRepresentation::IsError() const -> bool
	{
		return Storage->bError;
	}

	auto FMaterialRenderRepresentation::GetSamplers() const -> std::span<const FMaterialSamplerState>
	{ return Storage->Samplers; }
	auto FMaterialRenderRepresentation::GetTextureFallbacks() const -> std::span<const EMaterialTextureFallback>
	{ return Storage->TextureFallbacks; }
	auto FMaterialRenderRepresentation::GetRecordId() const -> uint64
	{ return Storage->RecordId; }
	auto FMaterialRenderRepresentation::GetContentHash() const -> uint64
	{ return Storage->ContentHash; }

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
			// TryCreate validates before immutable publication. Retain the publication
			// instead of validating and copying its layout and arrays for each draw.
			OutBinding.Owner = Representation.Storage;
			OutBinding.bError = Representation.IsError();
			OutBinding.LayoutIdentity = Representation.GetLayout().Identity;
			OutBinding.CompiledUniformPayload = Representation.GetUniformPayload();
			OutBinding.CompiledTextures = Representation.GetResources();
			OutBinding.CompiledSamplers = Representation.GetSamplers();
			OutBinding.CompiledTextureFallbacks = Representation.GetTextureFallbacks();
			return true;
		}
		return SetValidationFailure(OutDiagnostic,
			EMaterialRenderValidationFailure::UnsupportedVersion, 0,
			EMaterialRenderError::UnsupportedBindingLayout);
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
				.Error = FMaterialError(EMaterialLayoutError::InvalidField, InvalidParameterId),
			};
			return false;
		}
		return FMaterialRenderRepresentation::TryCreate(
			Input,
			OutRepresentation,
			OutDiagnostic);
	}

}
