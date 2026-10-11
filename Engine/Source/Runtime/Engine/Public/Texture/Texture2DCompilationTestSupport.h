#pragma once

#include "CoreMinimal.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Texture/Texture2DCompilationTypes.h"

namespace Durin
{
	namespace DerivedData { class ICache; }
	class DTexture2D;

	namespace AssetPrivate
	{
		// Replace only while compilation is quiescent; null restores the production session.
		ENGINE_API auto SetTexture2DAsyncCacheForTests(std::shared_ptr<DerivedData::ICache> Cache) -> void;
		// Bounded internal snapshot for qualification and regression tests.
		struct FTexture2DCompilationDiagnostic
		{
			uint64 RequestId = 0;
			// Latest-wins serial owned by the compiling manager; unrelated to DDC identity.
			uint64 RequestSerial = 0;
			std::string AssetIdentity;
			FTexture2DCompilationError Error;
			std::optional<FTexture2DBuildError> BuildCause;
			FTexture2DCompilationMetrics Metrics;
			uint64 QueuedNanoseconds = 0;
			ETexture2DCompilationPhase Phase = ETexture2DCompilationPhase::None;
			bool bSourceDecoderInvoked = false;
		};

		ENGINE_API auto GetTexture2DCompilationDiagnosticForTests(const DTexture2D& Texture)
			-> FTexture2DCompilationDiagnostic;
	}
}

#endif
