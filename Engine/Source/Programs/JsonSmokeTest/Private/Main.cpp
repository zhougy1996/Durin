#include "Json/Json.h"

#include <iostream>

int main()
{
    Durin::FJsonDocument Document;
    Durin::FJsonParseError Error;

    if (!Document.LoadFromFile(JSON_SMOKE_TEST_FILE, &Error))
    {
        std::cerr << "Failed to load sample JSON: " << Error.Message << "\n";
        return 1;
    }

    const Durin::FJsonValueView Root = Document.GetRootView();
    if (!Root.IsObject())
    {
        std::cerr << "Root is not an object\n";
        return 1;
    }

    if (Root.GetStringValue("name") != "yyjson smoke test")
    {
        std::cerr << "Unexpected name field\n";
        return 1;
    }

    if (Root.GetIntValue("version") != 1)
    {
        std::cerr << "Unexpected version field\n";
        return 1;
    }

    const auto Features = Root.GetView("features");
    if (!Features.IsArray() || Features.Num() != 3)
    {
        std::cerr << "Unexpected features array\n";
        return 1;
    }

    if (Features.GetView(0).GetString() != "parse" ||
        Features.GetView(1).GetString() != "load-file" ||
        Features.GetView(2).GetString() != "errors")
    {
        std::cerr << "Unexpected feature values\n";
        return 1;
    }

    const auto Flags = Root.GetView("flags");
    if (!Flags.IsObject() ||
        !Flags.GetBoolValue("enabled") ||
        Flags.GetDoubleValue("threshold") != 0.75)
    {
        std::cerr << "Unexpected flags object\n";
        return 1;
    }

    Durin::FJsonDocument InvalidDocument;
    if (InvalidDocument.Parse("{\"broken\": [1, 2, }", &Error))
    {
        std::cerr << "Invalid JSON unexpectedly parsed\n";
        return 1;
    }

    if (Error.Code == 0 || Error.Message.empty() || Error.Line == 0 || Error.Column == 0)
    {
        std::cerr << "Invalid JSON did not produce detailed parse errors\n";
        return 1;
    }

    std::cout << "JsonSmokeTest passed\n";
    return 0;
}
