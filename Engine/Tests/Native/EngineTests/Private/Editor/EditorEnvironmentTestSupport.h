#pragma once

#include "CoreMinimal.h"

namespace Durin::Testing
{
    class FScopedEditorEnvironment
    {
    public:
        FScopedEditorEnvironment(std::string InName, const std::string& Value) : Name(std::move(InName))
        {
#if defined(_WIN32)
            char* Buffer = nullptr; size_t Size = 0;
            if (_dupenv_s(&Buffer, &Size, Name.c_str()) == 0 && Buffer)
            { Previous = Buffer; std::free(Buffer); }
#else
            if (const char* Buffer = std::getenv(Name.c_str())) Previous = Buffer;
#endif
            Set(Value.c_str());
        }
        ~FScopedEditorEnvironment() { Set(Previous ? Previous->c_str() : nullptr); }
    private:
        auto Set(const char* Value) -> void
        {
#if defined(_WIN32)
            _putenv_s(Name.c_str(), Value ? Value : "");
#else
            if (Value) setenv(Name.c_str(), Value, 1); else unsetenv(Name.c_str());
#endif
        }
        std::string Name;
        std::optional<std::string> Previous;
    };
}
