#include "RenderResourceCreation.h"

namespace Durin
{
	auto FRenderResourceCreateError::GetFingerprint() const -> size_t
	{
		size_t Fingerprint = 0;
		auto Add = [&]<typename T>(const T& Value) {
			Fingerprint ^= std::hash<T>{}(Value) + 0x9e3779b9 + (Fingerprint << 6) + (Fingerprint >> 2);
		};
		Add(Category); Add(Reason); Add(Context); Add(Identity);
		Add(RetryDependencies); Add(bRetainedFallback); Add(Cause.index());
		if (const auto* Shader = std::get_if<FShaderError>(&Cause))
			Add(Shader->GetSemanticFingerprint());
		else if (const auto* RHI = std::get_if<FRHICreationError>(&Cause))
			Add(RHI->GetSemanticFingerprint());
		return Fingerprint;
	}

	auto FormatRenderResourceCreateError(const FRenderResourceCreateError& Error) -> std::string
	{
		if (const auto* Shader = std::get_if<FShaderError>(&Error.Cause)) return FormatShaderError(*Shader);
		if (const auto* RHI = std::get_if<FRHICreationError>(&Error.Cause)) return FormatRHICreationError(*RHI);
		switch (Error.Reason)
		{
		case ERenderResourceCreateErrorReason::Unspecified: return "Renderer resource creation failed.";
		case ERenderResourceCreateErrorReason::GlobalShaderUnavailable: return "Global shader set is unavailable.";
		case ERenderResourceCreateErrorReason::ShaderFailure: return "Shader preparation failed.";
		case ERenderResourceCreateErrorReason::ShaderCreationFailed: return "RHI shader creation returned null.";
		case ERenderResourceCreateErrorReason::ResourceCreationFailed: return "RHI resource creation returned null.";
		case ERenderResourceCreateErrorReason::PipelineCreationFailed: return "RHI pipeline creation returned null.";
		case ERenderResourceCreateErrorReason::SamplerCreationFailed: return "RHI sampler creation returned null.";
		case ERenderResourceCreateErrorReason::FullscreenGeometryUnavailable: return "Shared fullscreen geometry is unavailable.";
		}
		return "Unknown renderer resource failure.";
	}
}
