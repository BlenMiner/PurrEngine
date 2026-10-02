#pragma once

#include "common.h"

typedef enum tok_kind {
    T_EOF,
    T_IDENT,
    T_INT,
    T_FLOAT,
    T_STRING, // Text in double quotes; the token's text excludes them.
    // Text with values in it, $"score {score} of {max:D3}", as tokens whose text
    // is all they cover: T_INTERP is `$"score {`, then the value's own tokens,
    // then T_INTERP_PART `} of {`, ... and the last part ends with the quote,
    // `}"`. A format after a value is T_INTERP_FORMAT, `:D3`. Text with no
    // values is one T_INTERP, `$"plain"`.
    T_INTERP,
    T_INTERP_PART,
    T_INTERP_FORMAT,

    // Keywords
    T_COMPONENT,
    T_SINGLETON,
    T_SYSTEM,
    T_MUT,
    T_VAR,
    T_WITH,
    T_WITHOUT,
    T_IF,
    T_ELSE,
    T_RETURN,
    T_TRUE,
    T_FALSE,
    T_SWITCH,
    T_CASE,
    T_DEFAULT,
    T_BREAK,
    T_WHILE,
    T_FOR,
    T_FOREACH,
    T_PARALLEL,
    T_CONTINUE,
    T_THIS, // The entity the code runs for
    T_FAIL, // fail error;: ends a function that `fails`
    T_TRY,  // try call: its value, or its error passed on
    T_AWAIT, // await call: an async call's value, once it's done
    T_IS,   // x is Type name
    T_NULL, // The nothing of a T?

    // Punctuation
    T_LBRACE,
    T_RBRACE,
    T_LPAREN,
    T_RPAREN,
    T_LBRACKET,
    T_RBRACKET,
    T_SEMI,
    T_COMMA,
    T_DOT,
    T_QUESTION,
    T_COALESCE, // ??
    T_COLON,

    T_ASSIGN,
    T_PLUS_ASSIGN,
    T_MINUS_ASSIGN,
    T_STAR_ASSIGN,
    T_SLASH_ASSIGN,
    T_PERCENT_ASSIGN,
    T_SHL_ASSIGN,
    T_SHR_ASSIGN,
    T_AMP_ASSIGN,
    T_PIPE_ASSIGN,
    T_CARET_ASSIGN,

    T_EQ,
    T_NE,
    T_LT,
    T_LE,
    T_GT,
    T_GE,

    T_PLUS,
    T_MINUS,
    T_STAR,
    T_SLASH,
    T_PERCENT,
    T_SHL,
    T_SHR,
    T_AMP,
    T_PIPE,
    T_CARET,
    T_TILDE,
    T_NOT,
    T_AND,
    T_OR,
    T_PLUS_PLUS,   // i++, a statement
    T_MINUS_MINUS, // i--
} tok_kind;

typedef struct token {
    tok_kind kind;
    str text;
    loc at;
} token;

// Returns an array terminated by a T_EOF token, or NULL after reporting errors.
token *lex(const source *src);

// Like lex, but returns the tokens even after errors, skipping what it couldn't
// read. For editors, which need tokens while code is half typed.
token *lex_all(const source *src);

const char *tok_kind_name(tok_kind kind);

// The binary operator behind a compound assignment: T_PLUS for T_PLUS_ASSIGN,
// and for T_PLUS_PLUS, which adds 1.
tok_kind compound_op(tok_kind assign);
