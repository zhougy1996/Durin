#pragma once


// Turns an preprocessor token into a real string (see UBT_COMPILED_PLATFORM)
#define DURIN_STRINGIZE(Token) DURIN_PRIVATE_STRINGIZE(Token)
#define DURIN_PRIVATE_STRINGIZE(Token) #Token

// Concatenates two preprocessor tokens, performing macro expansion on them first
#define DURIN_JOIN(TokenA, TokenB) DURIN_PRIVATE_JOIN(TokenA, TokenB)
#define DURIN_PRIVATE_JOIN(TokenA, TokenB) TokenA##TokenB

// Concatenates the first two preprocessor tokens of a variadic list, after performing macro expansion on them
#define DURIN_JOIN_FIRST(Token, ...) DURIN_PRIVATE_JOIN_FIRST(Token, __VA_ARGS__)
#define DURIN_PRIVATE_JOIN_FIRST(Token, ...) Token##__VA_ARGS__

// Expands to the second argument or the third argument if the first argument is 1 or 0 respectively
#define DURIN_IF(OneOrZero, Token1, Token0) DURIN_JOIN(DURIN_PRIVATE_IF_, OneOrZero)(Token1, Token0)
#define DURIN_PRIVATE_IF_1(Token1, Token0) Token1
#define DURIN_PRIVATE_IF_0(Token1, Token0) Token0

// Expands to nothing - used as a placeholder
#define DURIN_EMPTY

// Expands to nothing when used as a function - used as a placeholder
#define DURIN_EMPTY_FUNCTION(...)

#define COMPILED_PLATFORM_HEADER(Suffix) DURIN_STRINGIZE(DURIN_JOIN(PLATFORM_HEADER_NAME/PLATFORM_HEADER_NAME, Suffix))


// Removes a single layer of parentheses from a macro argument if they are present - used to allow
// brackets to be optionally added when the argument contains commas, e.g.:
//
// #define DEFINE_VARIABLE(Type, Name) DURIN_REMOVE_OPTIONAL_PARENS(Type) Name;
//
// DEFINE_VARIABLE(int, IntVar)                  // expands to: int IntVar;
// DEFINE_VARIABLE((TPair<int, float>), PairVar) // expands to: TPair<int, float> PairVar;
#define DURIN_REMOVE_OPTIONAL_PARENS(...) DURIN_JOIN_FIRST(DURIN_PRIVATE_PREPROCESSOR_REMOVE_OPTIONAL_PARENS, DURIN_PRIVATE_PREPROCESSOR_REMOVE_OPTIONAL_PARENS __VA_ARGS__)
#define DURIN_PRIVATE_PREPROCESSOR_REMOVE_OPTIONAL_PARENS(...) DURIN_PRIVATE_PREPROCESSOR_REMOVE_OPTIONAL_PARENS __VA_ARGS__
#define DURIN_PRIVATE_PREPROCESSOR_REMOVE_OPTIONAL_PARENSDURIN_PRIVATE_PREPROCESSOR_REMOVE_OPTIONAL_PARENS