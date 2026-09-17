#include "RDG.h"

namespace Durin
{
	auto FormatRDGError(const FRDGResult& Result) -> std::string
	{
		if (Result.IsSuccess()) return {};
		std::string Text;
		switch (Result.Reason)
		{
		case ERDGReason::Unspecified: Text = "unspecified render graph failure"; break;
		case ERDGReason::MetadataNull: Text = "metadata null"; break;
		case ERDGReason::MetadataNameEmpty: Text = "metadata name empty"; break;
		case ERDGReason::MetadataLayoutMismatch: Text = "metadata layout mismatch"; break;
		case ERDGReason::MetadataNestingLimit: Text = "metadata nesting limit"; break;
		case ERDGReason::MemberNameEmpty: Text = "member name empty"; break;
		case ERDGReason::MemberNameDuplicate: Text = "member name duplicate"; break;
		case ERDGReason::MemberLayoutEmpty: Text = "member layout empty"; break;
		case ERDGReason::MemberOffsetInvalid: Text = "member offset invalid"; break;
		case ERDGReason::NestedMetadataInvalid: Text = "nested metadata invalid"; break;
		case ERDGReason::UnexpectedNestedMetadata: Text = "unexpected nested metadata"; break;
		case ERDGReason::WrapperLayoutMismatch: Text = "wrapper layout mismatch"; break;
		case ERDGReason::OptionalLayoutMismatch: Text = "optional layout mismatch"; break;
		case ERDGReason::DeclarationSemanticsInvalid: Text = "declaration semantics invalid"; break;
		case ERDGReason::ShaderBindingNameEmpty: Text = "shader binding name empty"; break;
		case ERDGReason::ShaderBindingDuplicate: Text = "shader binding duplicate"; break;
		case ERDGReason::ShaderDeclarationIncompatible: Text = "shader declaration incompatible"; break;
		case ERDGReason::ShaderBindingAuthorityMissing: Text = "shader binding authority missing"; break;
		case ERDGReason::NestedShaderBindingDuplicate: Text = "nested shader binding duplicate"; break;
		case ERDGReason::ResourceHandleInvalid: Text = "resource handle invalid"; break;
		case ERDGReason::FinalAccessInvalid: Text = "final access invalid"; break;
		case ERDGReason::RequiredAccessInvalid: Text = "required access invalid"; break;
		case ERDGReason::PassAccessIncompatible: Text = "pass access incompatible"; break;
		case ERDGReason::UseAccessMismatch: Text = "use access mismatch"; break;
		case ERDGReason::ReadDiscardInvalid: Text = "read discard invalid"; break;
		case ERDGReason::ManagedResultAccessInvalid: Text = "managed result access invalid"; break;
		case ERDGReason::BufferRangeInvalid: Text = "buffer range invalid"; break;
		case ERDGReason::TextureRangeInvalid: Text = "texture range invalid"; break;
		case ERDGReason::UsesOverlap: Text = "uses overlap"; break;
		case ERDGReason::StructuralLimit: Text = "structural limit"; break;
		case ERDGReason::DependencyNotForward: Text = "dependency not forward"; break;
		case ERDGReason::ResourceNameEmpty: Text = "resource name empty"; break;
		case ERDGReason::PhysicalResourceMissing: Text = "physical resource missing"; break;
		case ERDGReason::ExternalFinalAccessMissing: Text = "external final access missing"; break;
		case ERDGReason::ResourceNameDuplicate: Text = "resource name duplicate"; break;
		case ERDGReason::PassNameEmpty: Text = "pass name empty"; break;
		case ERDGReason::PassNameDuplicate: Text = "pass name duplicate"; break;
		case ERDGReason::ProducerHandleInvalid: Text = "producer handle invalid"; break;
		case ERDGReason::ValueWriterCount: Text = "value writer count"; break;
		case ERDGReason::BufferProducerMissing: Text = "buffer producer missing"; break;
		case ERDGReason::ResourceProducerMissing: Text = "resource producer missing"; break;
		case ERDGReason::BuilderConsumed: Text = "builder consumed"; break;
		case ERDGReason::CompilationIncomplete: Text = "compilation incomplete"; break;
		case ERDGReason::PreparationIncomplete: Text = "preparation incomplete"; break;
		case ERDGReason::ParameterLayoutMismatch: Text = "parameter layout mismatch"; break;
		case ERDGReason::ValueStorageInvalid: Text = "value storage invalid"; break;
		case ERDGReason::ValueTypeNameChanged: Text = "value type name changed"; break;
		case ERDGReason::ValueTypeNameReused: Text = "value type name reused"; break;
		case ERDGReason::ExternalContractConflict: Text = "external contract conflict"; break;
		case ERDGReason::TextureExtractionHandleInvalid: Text = "texture extraction handle invalid"; break;
		case ERDGReason::TextureExtractionInvalid: Text = "texture extraction invalid"; break;
		case ERDGReason::TextureExtractionDuplicate: Text = "texture extraction duplicate"; break;
		case ERDGReason::BufferExtractionHandleInvalid: Text = "buffer extraction handle invalid"; break;
		case ERDGReason::BufferExtractionInvalid: Text = "buffer extraction invalid"; break;
		case ERDGReason::BufferExtractionDuplicate: Text = "buffer extraction duplicate"; break;
		case ERDGReason::ParameterAllocationInvalid: Text = "parameter allocation invalid"; break;
		case ERDGReason::ParameterAllocationSubmitted: Text = "parameter allocation submitted"; break;
		case ERDGReason::PassHandleInvalid: Text = "pass handle invalid"; break;
		case ERDGReason::ManualUseOnParameterizedPass: Text = "manual use on parameterized pass"; break;
		case ERDGReason::RootHandleInvalid: Text = "root handle invalid"; break;
		case ERDGReason::AsyncPassInvalid: Text = "async pass invalid"; break;
		case ERDGReason::ConsumerHandleInvalid: Text = "consumer handle invalid"; break;
		case ERDGReason::ValueDirectionInvalid: Text = "value direction invalid"; break;
		case ERDGReason::ValueHandleInvalid: Text = "value handle invalid"; break;
		case ERDGReason::StorageIncomplete: Text = "storage incomplete"; break;
		case ERDGReason::AllocatorFailure: Text = "allocator failure"; break;
		case ERDGReason::AllocationMissing: Text = "allocation missing"; break;
		case ERDGReason::TextureAllocationIncompatible: Text = "texture allocation incompatible"; break;
		case ERDGReason::BufferAllocationIncompatible: Text = "buffer allocation incompatible"; break;
		case ERDGReason::AllocatorMissing: Text = "allocator missing"; break;
		case ERDGReason::QueueTransferFailed: Text = "queue transfer failed"; break;
		case ERDGReason::RecordingIncomplete: Text = "recording incomplete"; break;
		case ERDGReason::AllocationBudgetExceeded: Text = "allocation budget exceeded"; break;
		case ERDGReason::AllocationRetrySuppressed: Text = "allocation retry suppressed"; break;
		case ERDGReason::AllocationKindInvalid: Text = "allocation kind invalid"; break;
		case ERDGReason::AllocationRetryDeferred: Text = "allocation retry deferred"; break;
		case ERDGReason::AllocationRetirementPending: Text = "allocation retirement pending"; break;
		case ERDGReason::PhysicalAllocationFailed: Text = "physical allocation failed"; break;
		case ERDGReason::AllocationPublicationFailed: Text = "allocation publication failed"; break;
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
				Text += std::format(": {} actual={} limit={}", Context.Dimension, Context.Actual, Context.Limit);
			else if constexpr (std::is_same_v<T, FRDGAllocationErrorContext>)
			{
				Text += std::format(": resource={}", Context.ResourceId);
				for (const auto* Buffer : {&Context.ActualBuffer, &Context.ExpectedBuffer})
					Text += std::format(" buffer=(size={},stride={},usage={})",
						Buffer->Size, Buffer->Stride, static_cast<uint64>(Buffer->Usage));
				for (const auto* Texture : {&Context.ActualTexture, &Context.ExpectedTexture})
					Text += std::format(" texture=(dimension={},extent={}x{},depth={},array={},mips={},samples={},format={},flags={})",
						static_cast<uint32>(Texture->Dimension), Texture->Extent.x, Texture->Extent.y,
						Texture->Depth, Texture->ArraySize, Texture->NumMips, Texture->NumSamples,
						static_cast<uint32>(Texture->Format), static_cast<uint64>(Texture->Flags));
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
