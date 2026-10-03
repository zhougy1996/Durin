#pragma once

#include "CoreMinimal.h"

#include "RHIShaderParameters.h"

namespace Durin::RHIShaderParameterValidationInternal
{
	struct FBindingElement
	{
		uint32 SetIndex = 0;
		const FBindingLayoutItem* Binding = nullptr;
		uint32 ArrayElement = 0;
	};

	inline auto CompareLocation(const FRHIShaderParameterResource& Resource,
		const FBindingElement& Expected) -> int32
	{
		if (Resource.SetIndex != Expected.SetIndex)
			return Resource.SetIndex < Expected.SetIndex ? -1 : 1;
		if (Resource.BindingIndex != Expected.Binding->Slot)
			return Resource.BindingIndex < Expected.Binding->Slot ? -1 : 1;
		if (Resource.ArrayElement != Expected.ArrayElement)
			return Resource.ArrayElement < Expected.ArrayElement ? -1 : 1;
		return 0;
	}

	namespace Detail
	{
		// Shared linear walk; the caller selects diagnostic or invariant failure handling.
		template <typename FVisitor, typename FFailure>
		auto WalkOrderedBindings(const FPipelineLayoutDesc& Layout,
			std::span<const FRHIShaderParameterResource> Resources,
			FVisitor&& Visitor, FFailure&& Failure,
			uint64* ValidationVisits) -> void
		{
			size_t ResourceIndex = 0;
			for (uint32 SetIndex = 0; SetIndex < Layout.BindingLayouts.size(); ++SetIndex)
			{
				for (const FBindingLayoutItem& Binding :
					Layout.BindingLayouts[SetIndex].BindingLayouts)
				{
					for (uint32 ArrayElement = 0; ArrayElement < Binding.ArraySize;
						++ArrayElement)
					{
						if (ValidationVisits) ++*ValidationVisits;
						const FBindingElement Expected{SetIndex, &Binding, ArrayElement};
						if (ResourceIndex >= Resources.size()
							|| CompareLocation(Resources[ResourceIndex], Expected) != 0
							|| !Resources[ResourceIndex].Resource
							|| Resources[ResourceIndex].Type != Binding.Type)
						{
							const auto Code = ResourceIndex >= Resources.size()
								|| CompareLocation(Resources[ResourceIndex], Expected) != 0
								? ERHIShaderBindingError::MissingBinding
								: !Resources[ResourceIndex].Resource ? ERHIShaderBindingError::NullResource
								: ERHIShaderBindingError::TypeMismatch;
							FRHIShaderBindingError Error{Code, static_cast<uint32>(ResourceIndex)};
							Error.SetIndex = SetIndex;
							Error.BindingIndex = Binding.Slot;
							Error.ArrayElement = ArrayElement;
							if (Code == ERHIShaderBindingError::TypeMismatch)
							{
								Error.ExpectedBindingType = Binding.Type;
								Error.ActualBindingType = Resources[ResourceIndex].Type;
							}
							Failure(Error);
							return;
						}
						Visitor(Expected, Resources[ResourceIndex]);
						++ResourceIndex;
					}
				}
			}
			if (ResourceIndex != Resources.size())
			{
				if (ValidationVisits) ++*ValidationVisits;
				FRHIShaderBindingError Error{ERHIShaderBindingError::UnexpectedBinding, static_cast<uint32>(ResourceIndex)};
				Error.SetIndex = Resources[ResourceIndex].SetIndex;
				Error.BindingIndex = Resources[ResourceIndex].BindingIndex;
				Error.ArrayElement = Resources[ResourceIndex].ArrayElement;
				Failure(Error);
			}
		}
	} // namespace Detail

	// Pure diagnostic validation; no visitor observes partially validated input.
	inline auto ValidateOrderedBindings(const FPipelineLayoutDesc& Layout,
		std::span<const FRHIShaderParameterResource> Resources,
		uint64* ValidationVisits = nullptr) -> std::expected<void, FRHIShaderBindingError>
	{
		std::expected<void, FRHIShaderBindingError> Result;
		Detail::WalkOrderedBindings(Layout, Resources, [](const auto&, const auto&) {},
			[&](const FRHIShaderBindingError& Error) { Result = std::unexpected(Error); }, ValidationVisits);
		return Result;
	}

	// Internal single-pass processing. Invalid input terminates the operation;
	// visitors may update state and are never offered a recoverable failure result.
	template <typename FVisitor>
	auto VisitOrderedBindingsChecked(const FPipelineLayoutDesc& Layout,
		std::span<const FRHIShaderParameterResource> Resources, FVisitor&& Visitor,
		uint64* ValidationVisits = nullptr) -> void
	{
		Detail::WalkOrderedBindings(Layout, Resources, std::forward<FVisitor>(Visitor),
			[](const FRHIShaderBindingError& Error) {
				requiref(false, "Invalid shader binding snapshot: {}", ToString(Error));
			}, ValidationVisits);
	}
} // namespace Durin::RHIShaderParameterValidationInternal
