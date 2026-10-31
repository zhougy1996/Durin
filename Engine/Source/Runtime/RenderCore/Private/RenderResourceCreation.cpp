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
		Add(RetryDependencies);

		return Fingerprint;
	}

	auto FormatRenderResourceCreateError(const FRenderResourceCreateError& Error) -> std::string
	{
		if (!Error.Cause.empty()) return Error.Cause;
		switch (Error.Reason)
		{
		case ERenderResourceCreateErrorReason::Unspecified: return "Renderer resource creation failed.";
		case ERenderResourceCreateErrorReason::GlobalShaderUnavailable: return "Global shader set is unavailable.";
		case ERenderResourceCreateErrorReason::ShaderFailure: return "Shader preparation failed.";
		case ERenderResourceCreateErrorReason::PipelinePreparationRejected: return "Pipeline preparation or request admission was rejected.";
		case ERenderResourceCreateErrorReason::FullscreenGeometryUnavailable: return "Shared fullscreen geometry is unavailable.";
		}
		return "Unknown renderer resource failure.";
	}
}
