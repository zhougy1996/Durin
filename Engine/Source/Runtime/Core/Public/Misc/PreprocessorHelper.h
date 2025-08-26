#pragma once

// Turns an preprocessor token into a real string (see UBT_COMPILED_PLATFORM)
#define DOGE_STRINGIZE(Token) DOGE_PRIVATE_STRINGIZE(Token)
#define DOGE_PRIVATE_STRINGIZE(Token) #Token

// Concatenates two preprocessor tokens, performing macro expansion on them first
#define DOGE_JOIN(TokenA, TokenB) DOGE_PRIVATE_JOIN(TokenA, TokenB)
#define DOGE_PRIVATE_JOIN(TokenA, TokenB) TokenA##TokenB

// Expands to the second argument or the third argument if the first argument is 1 or 0 respectively
#define DOGE_IF(OneOrZero, Token1, Token0) DOGE_JOIN(DOGE_PRIVATE_IF_, OneOrZero)(Token1, Token0)
#define DOGE_PRIVATE_IF_1(Token1, Token0) Token1
#define DOGE_PRIVATE_IF_0(Token1, Token0) Token0

// Expands to nothing - used as a placeholder
#define DOGE_EMPTY

// Expands to nothing when used as a function - used as a placeholder
#define DOGE_EMPTY_FUNCTION(...)

// Removes a single layer of parentheses from a macro argument if they are present - used to allow
// brackets to be optionally added when the argument contains commas, e.g.:
//
// #define DEFINE_VARIABLE(Type, Name) DOGE_REMOVE_OPTIONAL_PARENS(Type) Name;
//
// DEFINE_VARIABLE(int, IntVar)                  // expands to: int IntVar;
// DEFINE_VARIABLE((TPair<int, float>), PairVar) // expands to: TPair<int, float> PairVar;
#define DOGE_REMOVE_OPTIONAL_PARENS(...) DOGE_JOIN_FIRST(DOGE_PRIVATE_PREPROCESSOR_REMOVE_OPTIONAL_PARENS, DOGE_PRIVATE_PREPROCESSOR_REMOVE_OPTIONAL_PARENS __VA_ARGS__)
#define DOGE_PRIVATE_PREPROCESSOR_REMOVE_OPTIONAL_PARENS(...) DOGE_PRIVATE_PREPROCESSOR_REMOVE_OPTIONAL_PARENS __VA_ARGS__
#define DOGE_PRIVATE_PREPROCESSOR_REMOVE_OPTIONAL_PARENSDOGE_PRIVATE_PREPROCESSOR_REMOVE_OPTIONAL_PARENS