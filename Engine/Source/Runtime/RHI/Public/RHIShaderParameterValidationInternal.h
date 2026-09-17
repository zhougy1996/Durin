#pragma once

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

	// Walks canonical layout elements and sorted resources in lockstep. The
	// visitor receives each validated correspondence exactly once.
	template <typename FVisitor>
	auto VisitOrderedBindings(const FPipelineLayoutDesc& Layout,
		std::span<const FRHIShaderParameterResource> Resources,
		FVisitor&& Visitor,
		uint64* ValidationVisits = nullptr) -> FRHIOperationResult
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
						FRHIOperationResult Result{Code, static_cast<uint32>(ResourceIndex)};
						Result.Error.SetIndex = SetIndex;
						Result.Error.BindingIndex = Binding.Slot;
						Result.Error.ArrayElement = ArrayElement;
						if (Code == ERHIShaderBindingError::TypeMismatch)
						{
							Result.Error.ExpectedBindingType = Binding.Type;
							Result.Error.ActualBindingType = Resources[ResourceIndex].Type;
						}
						return Result;
					}
					Visitor(Expected, Resources[ResourceIndex]);
					++ResourceIndex;
				}
			}
		}
		if (ResourceIndex != Resources.size())
		{
			if (ValidationVisits) ++*ValidationVisits;
			FRHIOperationResult Result{ERHIShaderBindingError::UnexpectedBinding, static_cast<uint32>(ResourceIndex)};
			Result.Error.SetIndex = Resources[ResourceIndex].SetIndex;
			Result.Error.BindingIndex = Resources[ResourceIndex].BindingIndex;
			Result.Error.ArrayElement = Resources[ResourceIndex].ArrayElement;
			return Result;
		}
		return {};
	}
} // namespace Durin::RHIShaderParameterValidationInternal
