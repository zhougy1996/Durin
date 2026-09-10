#pragma once

namespace Durin
{
    class FRHICommandListImmediate;
    auto BeginSkyLightingSmokeFrame(FRHICommandListImmediate& Commands) -> void;
    auto AfterSkyLightingSmokeUpdate(FRHICommandListImmediate& Commands) -> void;
    auto EndSkyLightingSmokeFrame(FRHICommandListImmediate& Commands) -> void;
    struct FSkyLightingRuntimeSmokeState;
    auto BeginSkyLightingRuntimeSmoke() -> std::shared_ptr<FSkyLightingRuntimeSmokeState>;
    auto TickSkyLightingRuntimeSmoke(const std::shared_ptr<FSkyLightingRuntimeSmokeState>& State) -> bool;
    auto EndSkyLightingRuntimeSmoke(std::shared_ptr<FSkyLightingRuntimeSmokeState>& State) -> void;
}
