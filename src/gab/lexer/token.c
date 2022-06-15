#include "token.h"

#define GAB_TOKEN_PRINT(name)                                                  \
  case (TOKEN_##name): {                                                       \
    return s_u8_create_cstr(#name);                                            \
  }

s_u8 *gab_token_to_string(gab_token tok) {
  switch (tok) {
    GAB_TOKEN_PRINT(MATCH)
    GAB_TOKEN_PRINT(NEWLINE)
    GAB_TOKEN_PRINT(IF)
    GAB_TOKEN_PRINT(IS)
    GAB_TOKEN_PRINT(THEN)
    GAB_TOKEN_PRINT(ELSE)
    GAB_TOKEN_PRINT(DO)
    GAB_TOKEN_PRINT(FOR)
    GAB_TOKEN_PRINT(IN)
    GAB_TOKEN_PRINT(END)
    GAB_TOKEN_PRINT(DEF)
    GAB_TOKEN_PRINT(RETURN)
    GAB_TOKEN_PRINT(WHILE)
    GAB_TOKEN_PRINT(PLUS)
    GAB_TOKEN_PRINT(MINUS)
    GAB_TOKEN_PRINT(STAR)
    GAB_TOKEN_PRINT(SLASH)
    GAB_TOKEN_PRINT(COMMA)
    GAB_TOKEN_PRINT(COLON)
    GAB_TOKEN_PRINT(DOT)
    GAB_TOKEN_PRINT(EQUAL)
    GAB_TOKEN_PRINT(EQUAL_EQUAL)
    GAB_TOKEN_PRINT(BANG)
    GAB_TOKEN_PRINT(LESSER)
    GAB_TOKEN_PRINT(LESSER_EQUAL)
    GAB_TOKEN_PRINT(GREATER)
    GAB_TOKEN_PRINT(GREATER_EQUAL)
    GAB_TOKEN_PRINT(ARROW)
    GAB_TOKEN_PRINT(FAT_ARROW)
    GAB_TOKEN_PRINT(AND)
    GAB_TOKEN_PRINT(OR)
    GAB_TOKEN_PRINT(NOT)
    GAB_TOKEN_PRINT(LBRACE)
    GAB_TOKEN_PRINT(RBRACE)
    GAB_TOKEN_PRINT(LBRACK)
    GAB_TOKEN_PRINT(RBRACK)
    GAB_TOKEN_PRINT(LPAREN)
    GAB_TOKEN_PRINT(RPAREN)
    GAB_TOKEN_PRINT(PIPE)
    GAB_TOKEN_PRINT(SEMICOLON)
    GAB_TOKEN_PRINT(IDENTIFIER)
    GAB_TOKEN_PRINT(STRING)
    GAB_TOKEN_PRINT(NUMBER)
    GAB_TOKEN_PRINT(FALSE)
    GAB_TOKEN_PRINT(TRUE)
    GAB_TOKEN_PRINT(NULL)
    GAB_TOKEN_PRINT(EOF)
    GAB_TOKEN_PRINT(ERROR)
    GAB_TOKEN_PRINT(DOT_DOT)
    GAB_TOKEN_PRINT(INTERPOLATION)
    GAB_TOKEN_PRINT(INTERPOLATION_END)
    GAB_TOKEN_PRINT(QUESTION)
    GAB_TOKEN_PRINT(AMPERSAND)
    GAB_TOKEN_PRINT(PERCENT)
  }
}
