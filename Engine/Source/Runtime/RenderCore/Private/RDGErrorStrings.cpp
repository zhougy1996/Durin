#include "RDGExecution.h"

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

	auto ToString(ERDGMetadataError Reason) -> std::string_view
	{
		switch (Reason)
		{
		case ERDGMetadataError::MetadataNull: return "metadata null";
		case ERDGMetadataError::MetadataNameEmpty: return "metadata name empty";
		case ERDGMetadataError::MetadataLayoutMismatch: return "metadata layout mismatch";
		case ERDGMetadataError::MetadataNestingLimit: return "metadata nesting limit";
		case ERDGMetadataError::MemberNameEmpty: return "member name empty";
		case ERDGMetadataError::MemberNameDuplicate: return "member name duplicate";
		case ERDGMetadataError::MemberLayoutEmpty: return "member layout empty";
		case ERDGMetadataError::MemberOffsetInvalid: return "member offset invalid";
		case ERDGMetadataError::NestedMetadataInvalid: return "nested metadata invalid";
		case ERDGMetadataError::UnexpectedNestedMetadata: return "unexpected nested metadata";
		case ERDGMetadataError::WrapperLayoutMismatch: return "wrapper layout mismatch";
		case ERDGMetadataError::OptionalLayoutMismatch: return "optional layout mismatch";
		case ERDGMetadataError::DeclarationSemanticsInvalid: return "declaration semantics invalid";
		case ERDGMetadataError::ShaderBindingNameEmpty: return "shader binding name empty";
		case ERDGMetadataError::ShaderBindingDuplicate: return "shader binding duplicate";
		case ERDGMetadataError::ShaderDeclarationIncompatible: return "shader declaration incompatible";
		case ERDGMetadataError::ShaderBindingAuthorityMissing: return "shader binding authority missing";
		case ERDGMetadataError::NestedShaderBindingDuplicate: return "nested shader binding duplicate";
		case ERDGMetadataError::ParameterLayoutMismatch: return "parameter layout mismatch";
		}
		return "unknown RDG error";
	}

	auto ToString(ERDGUseError Reason) -> std::string_view
	{
		switch (Reason)
		{
		case ERDGUseError::ResourceHandleInvalid: return "resource handle invalid";
		case ERDGUseError::FinalAccessInvalid: return "final access invalid";
		case ERDGUseError::RequiredAccessInvalid: return "required access invalid";
		case ERDGUseError::PassAccessIncompatible: return "pass access incompatible";
		case ERDGUseError::UseAccessMismatch: return "use access mismatch";
		case ERDGUseError::ReadDiscardInvalid: return "read discard invalid";
		case ERDGUseError::ManagedResultAccessInvalid: return "managed result access invalid";
		case ERDGUseError::BufferRangeInvalid: return "buffer range invalid";
		case ERDGUseError::TextureRangeInvalid: return "texture range invalid";
		case ERDGUseError::UsesOverlap: return "uses overlap";
		case ERDGUseError::BufferProducerMissing: return "buffer producer missing";
		case ERDGUseError::ResourceProducerMissing: return "resource producer missing";
		}
		return "unknown RDG error";
	}

	auto ToString(ERDGIdentityError Reason) -> std::string_view
	{
		switch (Reason)
		{
		case ERDGIdentityError::ResourceNameEmpty: return "resource name empty";
		case ERDGIdentityError::PhysicalResourceMissing: return "physical resource missing";
		case ERDGIdentityError::ExternalFinalAccessMissing: return "external final access missing";
		case ERDGIdentityError::ResourceNameDuplicate: return "resource name duplicate";
		case ERDGIdentityError::PassNameEmpty: return "pass name empty";
		case ERDGIdentityError::PassNameDuplicate: return "pass name duplicate";
		case ERDGIdentityError::ValueWriterCount: return "value writer count";
		case ERDGIdentityError::ValueStorageInvalid: return "value storage invalid";
		case ERDGIdentityError::ValueTypeNameChanged: return "value type name changed";
		case ERDGIdentityError::ValueTypeNameReused: return "value type name reused";
		case ERDGIdentityError::TextureExtractionHandleInvalid: return "texture extraction handle invalid";
		case ERDGIdentityError::TextureExtractionInvalid: return "texture extraction invalid";
		case ERDGIdentityError::TextureExtractionDuplicate: return "texture extraction duplicate";
		case ERDGIdentityError::BufferExtractionHandleInvalid: return "buffer extraction handle invalid";
		case ERDGIdentityError::BufferExtractionInvalid: return "buffer extraction invalid";
		case ERDGIdentityError::BufferExtractionDuplicate: return "buffer extraction duplicate";
		case ERDGIdentityError::ParameterAllocationInvalid: return "parameter allocation invalid";
		case ERDGIdentityError::ParameterAllocationSubmitted: return "parameter allocation submitted";
		case ERDGIdentityError::PassHandleInvalid: return "pass handle invalid";
		case ERDGIdentityError::ManualUseOnParameterizedPass: return "manual use on parameterized pass";
		case ERDGIdentityError::RootHandleInvalid: return "root handle invalid";
		case ERDGIdentityError::AsyncPassInvalid: return "async pass invalid";
		case ERDGIdentityError::ValueDirectionInvalid: return "value direction invalid";
		case ERDGIdentityError::ValueHandleInvalid: return "value handle invalid";
		case ERDGIdentityError::FinalAccessInvalid: return "final access invalid";
		}
		return "unknown RDG error";
	}

	auto ToString(ERDGDependencyError Reason) -> std::string_view
	{
		switch (Reason)
		{
		case ERDGDependencyError::DependencyNotForward: return "dependency not forward";
		case ERDGDependencyError::ProducerHandleInvalid: return "producer handle invalid";
		case ERDGDependencyError::ConsumerHandleInvalid: return "consumer handle invalid";
		}
		return "unknown RDG error";
	}

	auto ToString(ERDGStateError Reason) -> std::string_view
	{
		switch (Reason)
		{
		case ERDGStateError::BuilderConsumed: return "builder consumed";
		case ERDGStateError::CompilationIncomplete: return "compilation incomplete";
		case ERDGStateError::PreparationIncomplete: return "preparation incomplete";
		case ERDGStateError::StorageIncomplete: return "storage incomplete";
		case ERDGStateError::RecordingIncomplete: return "recording incomplete";
		}
		return "unknown RDG error";
	}

	auto ToString(ERDGPreparationError Reason) -> std::string_view
	{
		switch (Reason)
		{
		case ERDGPreparationError::AllocatorMissing: return "allocator missing";
		case ERDGPreparationError::QueueTransferFailed: return "queue transfer failed";
		}
		return "unknown RDG error";
	}

	auto ToString(ERDGAllocationError Reason) -> std::string_view
	{
		switch (Reason)
		{
		case ERDGAllocationError::AllocatorFailure: return "allocator failure";
		case ERDGAllocationError::AllocationRetrySuppressed: return "allocation retry suppressed";
		case ERDGAllocationError::AllocationKindInvalid: return "allocation kind invalid";
		case ERDGAllocationError::AllocationRetryDeferred: return "allocation retry deferred";
		case ERDGAllocationError::AllocationRetirementPending: return "allocation retirement pending";
		case ERDGAllocationError::PhysicalAllocationFailed: return "physical allocation failed";
		case ERDGAllocationError::AllocationPublicationFailed: return "allocation publication failed";
		}
		return "unknown RDG error";
	}

	namespace
	{
		template<typename T>
		auto AppendContext(std::string& Text, const T& Context) -> void
		{
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
			else if constexpr (std::is_same_v<T, FRDGDependencyError>)
				Text += std::format(": producer={} consumer={}", Context.Producer, Context.Consumer);
			else if constexpr (std::is_same_v<T, FRDGLimitError>)
				Text += std::format(": {} actual={} limit={}", LimitName(Context.Dimension), Context.Actual, Context.Limit);
			else if constexpr (std::is_same_v<T, FRDGMissingAllocationError> || std::is_same_v<T, FRDGAllocationFailure>)
			{
				if (Context.ResourceId != UINT32_MAX) Text += std::format(": resource={}", Context.ResourceId);
			}
			else if constexpr (std::is_same_v<T, FRDGBufferAllocationError>)
			{
				if (Context.ResourceId != UINT32_MAX) Text += std::format(": resource={}", Context.ResourceId);
				auto Append = [&](std::string_view Label, const FRHIBufferDesc& Buffer) {
					Text += std::format(" {}=(size={},stride={},usage={})", Label,
						Buffer.Size, Buffer.Stride, static_cast<uint64>(Buffer.Usage));
				};
				Append("expected", Context.Expected);
				Append("actual", Context.Actual);
			}
			else if constexpr (std::is_same_v<T, FRDGTextureAllocationError>)
			{
				if (Context.ResourceId != UINT32_MAX) Text += std::format(": resource={}", Context.ResourceId);
				auto Append = [&](std::string_view Label, const FRHITextureDesc& Texture) {
					Text += std::format(" {}=(dimension={},extent={}x{},depth={},array={},mips={},samples={},format={},flags={})",
						Label, static_cast<uint32>(Texture.Dimension), Texture.Extent.x, Texture.Extent.y,
						Texture.Depth, Texture.ArraySize, Texture.NumMips, Texture.NumSamples,
						static_cast<uint32>(Texture.Format), static_cast<uint64>(Texture.Flags));
				};
				Append("expected", Context.Expected);
				Append("actual", Context.Actual);
			}
			else if constexpr (std::is_same_v<T, FRDGExternalConflictError>)
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
		}
		template<typename T>
		auto FormatDetail(const T& Error) -> std::string
		{
			std::string Text(ToString(Error.Reason));
			if constexpr (requires { Error.Context; }) AppendContext(Text, Error.Context);
			else AppendContext(Text, Error);
			return Text;
		}
	}

	auto ToString(const FRDGMetadataError& Error) -> std::string { return FormatDetail(Error); }
	auto ToString(const FRDGUseError& Error) -> std::string { return FormatDetail(Error); }
	auto ToString(const FRDGIdentityError& Error) -> std::string { return FormatDetail(Error); }
	auto ToString(const FRDGDependencyError& Error) -> std::string { return FormatDetail(Error); }
	auto ToString(const FRDGLimitError& Error) -> std::string
	{
		std::string Text = "structural limit";
		AppendContext(Text, Error);
		return Text;
	}
	auto ToString(const FRDGExternalConflictError& Error) -> std::string
	{
		std::string Text = "external contract conflict";
		AppendContext(Text, Error);
		return Text;
	}
	auto ToString(const FRDGMissingAllocationError& Error) -> std::string
	{
		std::string Text = "allocation missing";
		AppendContext(Text, Error);
		return Text;
	}
	auto ToString(const FRDGTextureAllocationError& Error) -> std::string
	{
		std::string Text = "texture allocation incompatible";
		AppendContext(Text, Error);
		return Text;
	}
	auto ToString(const FRDGBufferAllocationError& Error) -> std::string
	{
		std::string Text = "buffer allocation incompatible";
		AppendContext(Text, Error);
		return Text;
	}
	auto ToString(const FRDGAllocationBudgetError& Error) -> std::string
	{
		auto Text = std::format("allocation budget exceeded: allocation-bytes actual={} limit={}", Error.Actual, Error.Limit);
		return Text;
	}
	auto ToString(const FRDGAllocationFailure& Error) -> std::string
	{
		auto Text = FormatDetail(Error);
		if (Error.Cause.HasError()) Text += ": " + ToString(Error.Cause);
		return Text;
	}
	auto ToString(const FRDGCompileError& Error) -> std::string
	{ return std::visit([](const auto& Detail) { return std::string(ToString(Detail)); }, Error.Detail); }
	auto ToString(const FRDGAllocationError& Error) -> std::string
	{ return std::visit([](const auto& Detail) { return std::string(ToString(Detail)); }, Error.Detail); }
	auto ToString(const FRDGPreparationError& Error) -> std::string
	{ return std::visit([](const auto& Detail) { return std::string(ToString(Detail)); }, Error.Detail); }
	auto ToString(const FRDGExecutionError& Error) -> std::string
	{ return std::visit([](const auto& Detail) { return std::string(ToString(Detail)); }, Error.Detail); }
}
