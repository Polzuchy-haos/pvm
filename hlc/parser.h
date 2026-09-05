#ifndef HLC_PARSER_H_
#define HLC_PARSER_H_

#include "ast.h"
#include "lexer.h"

Program Parse(const std::vector<Token>& tokens, const std::string& fileName);

#endif /* HLC_PARSER_H_ */
