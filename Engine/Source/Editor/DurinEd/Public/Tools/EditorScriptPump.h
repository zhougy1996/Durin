#pragma once

#include "CoreMinimal.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
    class DEditorEngine;
    class FEditorToolService;

    // Synchronous script boundary, outside ToolService provider dispatch. The host
    // and feature dispatch scopes retain the editor and provider until return.
    class IEditorScriptPump : public IModularFeature
    {
    public:
        static constexpr std::string_view FeatureName = "EditorScriptPump";
        static constexpr uint32 FeatureVersion = 2;
        virtual auto Pump(DEditorEngine& Editor, const std::shared_ptr<FEditorToolService>& Endpoint) -> void = 0;
        virtual auto CloseEndpoint(const std::shared_ptr<FEditorToolService>& Endpoint) -> void {}
    };
}
