#include "RDG.h"

namespace Durin
{
	namespace
	{
		auto LimitName(ERDGLimit Dimension) -> std::string_view
		{
			switch (Dimension)
			{
			case ERDGLimit::Passes: return "passes";
			case ERDGLimit::Resources: return "resources";
			case ERDGLimit::Uses: return "uses";
			case ERDGLimit::Dependencies: return "dependencies";
			case ERDGLimit::RangeCells: return "range-cells";
			case ERDGLimit::RangeCellCandidates: return "range-cell-candidates";
			case ERDGLimit::CellVisits: return "cell-visits";
			case ERDGLimit::TextureTransitions: return "texture-transitions";
			case ERDGLimit::BufferTransitions: return "buffer-transitions";
			case ERDGLimit::AllocationBytes: return "allocation-bytes";
			}
			return "unknown-limit";
		}
	}

	auto FRDGError::GetCategory() const -> ERDGErrorCategory
	{
		switch (Code)
		{
		case ERDGError::AllocatorFailure:
		case ERDGError::AllocationPublicationFailed:
		case ERDGError::PhysicalAllocationFailed:
		case ERDGError::AllocatorMissing:
		case ERDGError::QueueTransferFailed:
		case ERDGError::AllocationBudgetExceeded:
		case ERDGError::AllocationRetrySuppressed:
		case ERDGError::AllocationKindInvalid:
		case ERDGError::AllocationRetryDeferred:
		case ERDGError::AllocationRetirementPending:
			return ERDGErrorCategory::AllocationFailed;
		case ERDGError::StructuralLimit:
			return ERDGErrorCategory::SafetyLimitExceeded;
		case ERDGError::AllocationMissing:
			return ERDGErrorCategory::MissingAllocation;
		case ERDGError::BufferAllocationIncompatible:
		case ERDGError::TextureAllocationIncompatible:
			return ERDGErrorCategory::IncompatibleAllocation;
		case ERDGError::MetadataNull:
		case ERDGError::MetadataNameEmpty:
		case ERDGError::MetadataLayoutMismatch:
		case ERDGError::MetadataNestingLimit:
		case ERDGError::MemberNameEmpty:
		case ERDGError::MemberNameDuplicate:
		case ERDGError::MemberLayoutEmpty:
		case ERDGError::MemberOffsetInvalid:
		case ERDGError::NestedMetadataInvalid:
		case ERDGError::UnexpectedNestedMetadata:
		case ERDGError::WrapperLayoutMismatch:
		case ERDGError::OptionalLayoutMismatch:
		case ERDGError::DeclarationSemanticsInvalid:
		case ERDGError::ShaderBindingNameEmpty:
		case ERDGError::ShaderBindingDuplicate:
		case ERDGError::ShaderDeclarationIncompatible:
		case ERDGError::ShaderBindingAuthorityMissing:
		case ERDGError::NestedShaderBindingDuplicate:
		case ERDGError::ParameterLayoutMismatch:
			return ERDGErrorCategory::InvalidParameterMetadata;
		case ERDGError::ResourceHandleInvalid:
		case ERDGError::FinalAccessInvalid:
		case ERDGError::RequiredAccessInvalid:
		case ERDGError::PassAccessIncompatible:
		case ERDGError::UseAccessMismatch:
		case ERDGError::ReadDiscardInvalid:
		case ERDGError::ManagedResultAccessInvalid:
		case ERDGError::BufferRangeInvalid:
		case ERDGError::TextureRangeInvalid:
		case ERDGError::UsesOverlap:
		case ERDGError::ResourceNameEmpty:
		case ERDGError::PhysicalResourceMissing:
		case ERDGError::ExternalFinalAccessMissing:
		case ERDGError::ResourceNameDuplicate:
		case ERDGError::PassNameEmpty:
		case ERDGError::PassNameDuplicate:
		case ERDGError::ProducerHandleInvalid:
		case ERDGError::ValueWriterCount:
		case ERDGError::ValueStorageInvalid:
		case ERDGError::ValueTypeNameChanged:
		case ERDGError::ValueTypeNameReused:
		case ERDGError::ExternalContractConflict:
		case ERDGError::TextureExtractionHandleInvalid:
		case ERDGError::TextureExtractionInvalid:
		case ERDGError::TextureExtractionDuplicate:
		case ERDGError::BufferExtractionHandleInvalid:
		case ERDGError::BufferExtractionInvalid:
		case ERDGError::BufferExtractionDuplicate:
		case ERDGError::ParameterAllocationInvalid:
		case ERDGError::ParameterAllocationSubmitted:
		case ERDGError::PassHandleInvalid:
		case ERDGError::ManualUseOnParameterizedPass:
		case ERDGError::RootHandleInvalid:
		case ERDGError::AsyncPassInvalid:
		case ERDGError::ValueDirectionInvalid:
		case ERDGError::ValueHandleInvalid:
			return ERDGErrorCategory::InvalidDeclaration;
		case ERDGError::DependencyNotForward:
		case ERDGError::ConsumerHandleInvalid:
			return ERDGErrorCategory::InvalidDependency;
		case ERDGError::BufferProducerMissing:
		case ERDGError::ResourceProducerMissing:
			return ERDGErrorCategory::MissingProducer;
		case ERDGError::BuilderConsumed:
		case ERDGError::CompilationIncomplete:
		case ERDGError::PreparationIncomplete:
		case ERDGError::StorageIncomplete:
		case ERDGError::RecordingIncomplete:
		case ERDGError::ExecutionNotStarted:
			return ERDGErrorCategory::InvalidState;
		}
		return ERDGErrorCategory::InvalidState;
	}

	auto FormatRDGError(const FRDGError& Result) -> std::string
	{
		std::string Text;
		switch (Result.Code)
		{
		case ERDGError::ExecutionNotStarted: Text = "graph execution not started"; break;
		case ERDGError::MetadataNull: Text = "metadata null"; break;
		case ERDGError::MetadataNameEmpty: Text = "metadata name empty"; break;
		case ERDGError::MetadataLayoutMismatch: Text = "metadata layout mismatch"; break;
		case ERDGError::MetadataNestingLimit: Text = "metadata nesting limit"; break;
		case ERDGError::MemberNameEmpty: Text = "member name empty"; break;
		case ERDGError::MemberNameDuplicate: Text = "member name duplicate"; break;
		case ERDGError::MemberLayoutEmpty: Text = "member layout empty"; break;
		case ERDGError::MemberOffsetInvalid: Text = "member offset invalid"; break;
		case ERDGError::NestedMetadataInvalid: Text = "nested metadata invalid"; break;
		case ERDGError::UnexpectedNestedMetadata: Text = "unexpected nested metadata"; break;
		case ERDGError::WrapperLayoutMismatch: Text = "wrapper layout mismatch"; break;
		case ERDGError::OptionalLayoutMismatch: Text = "optional layout mismatch"; break;
		case ERDGError::DeclarationSemanticsInvalid: Text = "declaration semantics invalid"; break;
		case ERDGError::ShaderBindingNameEmpty: Text = "shader binding name empty"; break;
		case ERDGError::ShaderBindingDuplicate: Text = "shader binding duplicate"; break;
		case ERDGError::ShaderDeclarationIncompatible: Text = "shader declaration incompatible"; break;
		case ERDGError::ShaderBindingAuthorityMissing: Text = "shader binding authority missing"; break;
		case ERDGError::NestedShaderBindingDuplicate: Text = "nested shader binding duplicate"; break;
		case ERDGError::ResourceHandleInvalid: Text = "resource handle invalid"; break;
		case ERDGError::FinalAccessInvalid: Text = "final access invalid"; break;
		case ERDGError::RequiredAccessInvalid: Text = "required access invalid"; break;
		case ERDGError::PassAccessIncompatible: Text = "pass access incompatible"; break;
		case ERDGError::UseAccessMismatch: Text = "use access mismatch"; break;
		case ERDGError::ReadDiscardInvalid: Text = "read discard invalid"; break;
		case ERDGError::ManagedResultAccessInvalid: Text = "managed result access invalid"; break;
		case ERDGError::BufferRangeInvalid: Text = "buffer range invalid"; break;
		case ERDGError::TextureRangeInvalid: Text = "texture range invalid"; break;
		case ERDGError::UsesOverlap: Text = "uses overlap"; break;
		case ERDGError::StructuralLimit: Text = "structural limit"; break;
		case ERDGError::DependencyNotForward: Text = "dependency not forward"; break;
		case ERDGError::ResourceNameEmpty: Text = "resource name empty"; break;
		case ERDGError::PhysicalResourceMissing: Text = "physical resource missing"; break;
		case ERDGError::ExternalFinalAccessMissing: Text = "external final access missing"; break;
		case ERDGError::ResourceNameDuplicate: Text = "resource name duplicate"; break;
		case ERDGError::PassNameEmpty: Text = "pass name empty"; break;
		case ERDGError::PassNameDuplicate: Text = "pass name duplicate"; break;
		case ERDGError::ProducerHandleInvalid: Text = "producer handle invalid"; break;
		case ERDGError::ValueWriterCount: Text = "value writer count"; break;
		case ERDGError::BufferProducerMissing: Text = "buffer producer missing"; break;
		case ERDGError::ResourceProducerMissing: Text = "resource producer missing"; break;
		case ERDGError::BuilderConsumed: Text = "builder consumed"; break;
		case ERDGError::CompilationIncomplete: Text = "compilation incomplete"; break;
		case ERDGError::PreparationIncomplete: Text = "preparation incomplete"; break;
		case ERDGError::ParameterLayoutMismatch: Text = "parameter layout mismatch"; break;
		case ERDGError::ValueStorageInvalid: Text = "value storage invalid"; break;
		case ERDGError::ValueTypeNameChanged: Text = "value type name changed"; break;
		case ERDGError::ValueTypeNameReused: Text = "value type name reused"; break;
		case ERDGError::ExternalContractConflict: Text = "external contract conflict"; break;
		case ERDGError::TextureExtractionHandleInvalid: Text = "texture extraction handle invalid"; break;
		case ERDGError::TextureExtractionInvalid: Text = "texture extraction invalid"; break;
		case ERDGError::TextureExtractionDuplicate: Text = "texture extraction duplicate"; break;
		case ERDGError::BufferExtractionHandleInvalid: Text = "buffer extraction handle invalid"; break;
		case ERDGError::BufferExtractionInvalid: Text = "buffer extraction invalid"; break;
		case ERDGError::BufferExtractionDuplicate: Text = "buffer extraction duplicate"; break;
		case ERDGError::ParameterAllocationInvalid: Text = "parameter allocation invalid"; break;
		case ERDGError::ParameterAllocationSubmitted: Text = "parameter allocation submitted"; break;
		case ERDGError::PassHandleInvalid: Text = "pass handle invalid"; break;
		case ERDGError::ManualUseOnParameterizedPass: Text = "manual use on parameterized pass"; break;
		case ERDGError::RootHandleInvalid: Text = "root handle invalid"; break;
		case ERDGError::AsyncPassInvalid: Text = "async pass invalid"; break;
		case ERDGError::ConsumerHandleInvalid: Text = "consumer handle invalid"; break;
		case ERDGError::ValueDirectionInvalid: Text = "value direction invalid"; break;
		case ERDGError::ValueHandleInvalid: Text = "value handle invalid"; break;
		case ERDGError::StorageIncomplete: Text = "storage incomplete"; break;
		case ERDGError::AllocatorFailure: Text = "allocator failure"; break;
		case ERDGError::AllocationMissing: Text = "allocation missing"; break;
		case ERDGError::TextureAllocationIncompatible: Text = "texture allocation incompatible"; break;
		case ERDGError::BufferAllocationIncompatible: Text = "buffer allocation incompatible"; break;
		case ERDGError::AllocatorMissing: Text = "allocator missing"; break;
		case ERDGError::QueueTransferFailed: Text = "queue transfer failed"; break;
		case ERDGError::RecordingIncomplete: Text = "recording incomplete"; break;
		case ERDGError::AllocationBudgetExceeded: Text = "allocation budget exceeded"; break;
		case ERDGError::AllocationRetrySuppressed: Text = "allocation retry suppressed"; break;
		case ERDGError::AllocationKindInvalid: Text = "allocation kind invalid"; break;
		case ERDGError::AllocationRetryDeferred: Text = "allocation retry deferred"; break;
		case ERDGError::AllocationRetirementPending: Text = "allocation retirement pending"; break;
		case ERDGError::PhysicalAllocationFailed: Text = "physical allocation failed"; break;
		case ERDGError::AllocationPublicationFailed: Text = "allocation publication failed"; break;
		}
		std::visit([&]<typename T>(const T& Context) {
			if constexpr (std::is_same_v<T, FRDGMetadataErrorContext>)
				Text += std::format(": struct='{}' member='{}' other='{}' binding='{}' index={} size={}/{} alignment={}/{} offset={} array={} depth={}",
					Context.StructName, Context.MemberName, Context.OtherMemberName, Context.BindingName,
					Context.MemberIndex, Context.ActualSize, Context.ExpectedSize, Context.ActualAlignment,
					Context.ExpectedAlignment, Context.Offset, Context.ArraySize, Context.Depth);
			else if constexpr (std::is_same_v<T, FRDGUseErrorContext>)
				Text += std::format(": pass='{}'[{}] resource='{}'[{}] parameter='{}' kind={} domain={} mode={} use={} other='{}'[{}] access={} result={} byte-offset={} byte-size={} capacity={} texture=(aspects={},mips={}+{}/{},layers={}+{}/{})",
					Context.PassName, Context.PassIndex, Context.ResourceName, Context.ResourceIndex,
					Context.ParameterPath, static_cast<uint32>(Context.Kind), static_cast<uint32>(Context.PassType),
					static_cast<uint32>(Context.Use), Context.UseIndex, Context.OtherParameterPath, Context.OtherUseIndex,
					static_cast<uint64>(Context.Access), static_cast<uint64>(Context.ResultAccess),
					Context.BufferOffset, Context.BufferSize, Context.BufferCapacity,
					static_cast<uint32>(Context.TextureRange.Aspects), Context.TextureRange.FirstMip,
					Context.TextureRange.NumMips, Context.TextureMips, Context.TextureRange.FirstArrayLayer,
					Context.TextureRange.NumArrayLayers, Context.TextureLayers);
			else if constexpr (std::is_same_v<T, FRDGIdentityErrorContext>)
				Text += std::format(": name='{}' other='{}' type='{}' index={} other-index={} expected={} actual={}",
					Context.Name, Context.OtherName, Context.TypeName, Context.Index, Context.OtherIndex, Context.Expected, Context.Actual);
			else if constexpr (std::is_same_v<T, FRDGDependencyErrorContext>)
				Text += std::format(": producer={} consumer={}", Context.Producer, Context.Consumer);
			else if constexpr (std::is_same_v<T, FRDGLimitErrorContext>)
				Text += std::format(": {} actual={} limit={}", LimitName(Context.Dimension), Context.Actual, Context.Limit);
			else if constexpr (std::is_same_v<T, FRDGAllocationErrorContext>)
				Text += std::format(": resource={}", Context.ResourceId);
			else if constexpr (std::is_same_v<T, FRDGBufferAllocationErrorContext>)
			{
				Text += std::format(": resource={}", Context.ResourceId);
				auto Append = [&](std::string_view Label, const FRHIBufferDesc& Buffer) {
					Text += std::format(" {}=(size={},stride={},usage={})", Label,
						Buffer.Size, Buffer.Stride, static_cast<uint64>(Buffer.Usage));
				};
				Append("expected", Context.Expected);
				Append("actual", Context.Actual);
			}
			else if constexpr (std::is_same_v<T, FRDGTextureAllocationErrorContext>)
			{
				Text += std::format(": resource={}", Context.ResourceId);
				auto Append = [&](std::string_view Label, const FRHITextureDesc& Texture) {
					Text += std::format(" {}=(dimension={},extent={}x{},depth={},array={},mips={},samples={},format={},flags={})",
						Label, static_cast<uint32>(Texture.Dimension), Texture.Extent.x, Texture.Extent.y,
						Texture.Depth, Texture.ArraySize, Texture.NumMips, Texture.NumSamples,
						static_cast<uint32>(Texture.Format), static_cast<uint64>(Texture.Flags));
				};
				Append("expected", Context.Expected);
				Append("actual", Context.Actual);
			}
			else if constexpr (std::is_same_v<T, FRDGExternalConflictContext>)
			{
				for (const auto* Contract : {&Context.Canonical, &Context.Requested})
					Text += std::format(": name='{}' kind={} texture=(dimension={},extent={}x{},depth={},array={},mips={},samples={},format={},flags={}) buffer=(size={},stride={},usage={}) access={}/{}",
						Contract->Name, static_cast<uint32>(Contract->Kind), static_cast<uint32>(Contract->Texture.Dimension),
						Contract->Texture.Extent.x, Contract->Texture.Extent.y, Contract->Texture.Depth,
						Contract->Texture.ArraySize, Contract->Texture.NumMips, Contract->Texture.NumSamples,
						static_cast<uint32>(Contract->Texture.Format), static_cast<uint64>(Contract->Texture.Flags),
						Contract->Buffer.Size, Contract->Buffer.Stride, static_cast<uint64>(Contract->Buffer.Usage),
						static_cast<uint64>(Contract->InitialAccess), static_cast<uint64>(Contract->FinalAccess));
			}
		}, Result.Context);
		if (const auto* Resource = std::get_if<FRenderResourceCreateError>(&Result.Cause))
			Text += ": " + FormatRenderResourceCreateError(*Resource);
		else if (const auto* RHI = std::get_if<FRHICreationError>(&Result.Cause))
			Text += ": " + FormatRHICreationError(*RHI);
		return Text;
	}
}
