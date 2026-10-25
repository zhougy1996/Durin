#include "Rendering/ViewportPresentation.h"
#include "RHICommandList.h"

namespace Durin
{
	auto EndDrawingViewportWithReceipts(FRHICommandListBase& Commands,
		const FViewportRHIRef& Viewport, bool bPresent, FConsumedViewportOutputs Outputs) -> void
	{
		if (!bPresent || Outputs.empty()) return Commands.EndDrawingViewport(Viewport, bPresent, false);
		struct FObservation
		{
			uint64 Before = 0;
			FConsumedViewportOutputs Outputs;
		};
		auto Observation = std::make_shared<FObservation>();
		Observation->Outputs = std::move(Outputs);
		const size_t PayloadBytes = sizeof(FObservation) + Observation->Outputs.capacity() * sizeof(FConsumedViewportOutputs::value_type);
		Commands.EnqueueLambda([Observation, Viewport] { Observation->Before = Viewport->GetPresentationSerial(); }, PayloadBytes);
		Commands.EndDrawingViewport(Viewport, true, false);
		Commands.EnqueueLambda([Observation, Viewport] {
			if (Viewport->GetPresentationSerial() > Observation->Before)
				for (const auto& [Receipt, Generation] : Observation->Outputs) Receipt->RecordPresented(Generation, Viewport);
		}, 0);
	}
}
