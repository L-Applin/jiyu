
#include "lexer.h"
#include "compiler.h"


static bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\r' || c == '\n';
}

static char tolower(char c) {
    if (c >= 'A' && c <= 'Z') return c + 0x20;
    
    return c;
}

static bool is_letter(char c) {
    c = tolower(c);
    return c >= 'a' && c <= 'z';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// @TODO UTF8 identifier support?
static bool starts_identifier(char c) {
    return c == '_' || is_letter(c); 
}

static bool continues_identifier(char c) {
    return starts_identifier(c) || is_digit(c);
}

Token Lexer::make_token(Token::Type type, Span span) {
    Token t = Token(type, TextSpan(text, span));
    t.filename = filename;
    return t;
}

Token Lexer::make_eof_token() {
    return make_token(Token::END, Span(text.length, 0));
}

Token Lexer::make_string_token(Token::Type type, Span span, String string) {
    Token t = make_token(type, span);
    t.string = string;
    return t;
}

Token Lexer::make_integer_token(s64 value, Span span) {
    Token t = make_token(Token::INTEGER, span);
    t.integer = value;
    return t;
}

void Lexer::eat_whitespace() {
    while (current_char < text.length && is_whitespace(text[current_char])) {
        current_char++;
    }
}

Token Lexer::lex_token() {
    eat_whitespace();
    
    if (!(current_char < text.length)) return make_eof_token();
    
    if (starts_identifier(text[current_char]) || text[current_char] == '@') {
        auto start = current_char;
        current_char++;
        
        while (current_char < text.length && continues_identifier(text[current_char])) {
            current_char++;
        }
        
        string_length_type length = current_char - start;
        Token result = make_string_token(Token::IDENTIFIER, Span(start, length), text.substring(start, length));
        
        // @Cleanup find a faster way to implement these things
        if      (result.string == to_string("func"))   result.type = Token::KEYWORD_FUNC;
        else if (result.string == to_string("var"))    result.type = Token::KEYWORD_VAR;
        else if (result.string == to_string("let"))    result.type = Token::KEYWORD_LET;
        else if (result.string == to_string("typealias")) result.type = Token::KEYWORD_TYPEALIAS;
        
        else if (result.string == to_string("void"))   result.type = Token::KEYWORD_VOID;
        else if (result.string == to_string("string")) result.type = Token::KEYWORD_STRING;
        
        else if (result.string == to_string("int"))    result.type = Token::KEYWORD_INT;
        else if (result.string == to_string("uint"))   result.type = Token::KEYWORD_UINT;
        
        else if (result.string == to_string("uint8"))  result.type = Token::KEYWORD_UINT8;
        else if (result.string == to_string("uint16")) result.type = Token::KEYWORD_UINT16;
        else if (result.string == to_string("uint32")) result.type = Token::KEYWORD_UINT32;
        else if (result.string == to_string("uint64")) result.type = Token::KEYWORD_UINT64;
        else if (result.string == to_string("int8"))   result.type = Token::KEYWORD_INT8;
        else if (result.string == to_string("int16"))  result.type = Token::KEYWORD_INT16;
        else if (result.string == to_string("int32"))  result.type = Token::KEYWORD_INT32;
        else if (result.string == to_string("int64"))  result.type = Token::KEYWORD_INT64;
        else if (result.string == to_string("float"))  result.type = Token::KEYWORD_FLOAT;
        else if (result.string == to_string("double")) result.type = Token::KEYWORD_DOUBLE;
        else if (result.string == to_string("bool"))   result.type = Token::KEYWORD_BOOL;
        else if (result.string == to_string("true"))   result.type = Token::KEYWORD_TRUE;
        else if (result.string == to_string("false"))  result.type = Token::KEYWORD_FALSE;
        else if (result.string == to_string("if"))     result.type = Token::KEYWORD_IF;
        else if (result.string == to_string("else"))   result.type = Token::KEYWORD_ELSE;
        else if (result.string == to_string("while"))  result.type = Token::KEYWORD_WHILE;
        else if (result.string == to_string("break"))  result.type = Token::KEYWORD_BREAK;
        else if (result.string == to_string("continue")) result.type = Token::KEYWORD_CONTINUE;
        
        else if (result.string == to_string("return")) result.type = Token::KEYWORD_RETURN;
        
        else if (result.string == to_string("cast")) result.type = Token::KEYWORD_CAST;
        
        // @Cleanup we should probably have a "tag" token
        else if (result.string == to_string("@c_function")) result.type = Token::TAG_C_FUNCTION;
        
        else if (result.string == to_string("temporary_c_vararg")) result.type = Token::TEMPORARY_KEYWORD_C_VARARGS;
        
        return result;
    } else if (is_digit(text[current_char])) {
        auto start = current_char;
        
        while (current_char < text.length && is_digit(text[current_char])) current_char++;
        
        char *value_string = compiler->get_temp_c_string(text.substring(start, current_char - start));
        auto value = atoll(value_string);
        
        return make_integer_token(value, Span(start, current_char - start));
    } else if (text[current_char] == '\"') {
        auto start = current_char;
        current_char++;
        
        while (current_char < text.length  && text[current_char] != '\"') {
            if (text[current_char] == '\n') {
                // create a faux token for reporting
                Token t = make_string_token(Token::STRING, Span(start, current_char - start), text.substring(start, current_char - start));
                compiler->report_error(&t, "Newline found while lexing string constant!");
                
                // return the token so we dont report other errors related to lexing this string
                return t;
            } else if (text[current_char] == '\\') {
                // @Cleanup do we really have to do this??
                if (current_char + 1 < text.length) {
                    current_char ++; // pass these so that we can translate them later in the final string
                }
            }
            
            current_char++;
        }
        
        if (current_char < text.length && text[current_char] == '\"') {
            current_char++;
            
            auto length = current_char - start;
            
            String input = text.substring(start+1, length-2);
            String output_string = copy_string(input);
            output_string.length = 0;
            
            for (string_length_type i = 0; i < input.length; ++i) {
                if (input[i] == '\\') {
                    if (i + 1 < input.length) {
                        if (input[i + 1] == 'n') {
                            output_string.length++;
                            output_string.data[output_string.length-1] = '\n';
                            
                            ++i;
                            continue;
                        }
                    }
                }
                
                output_string.length++;
                output_string.data[output_string.length-1] = input[i];
            }
            
            return make_string_token(Token::STRING, Span(start, length), output_string);
        } else if (current_char >= text.length) {
            // create a faux token for reporting
            Token t = make_string_token(Token::STRING, Span(start, current_char - start), text.substring(start, current_char - start));
            compiler->report_error(&t, "End-of-file found while lexing string constant!");
            
            // return the token so we dont report other errors related to lexing this string
            return t;
        } else {
            assert(false);
        }
        
    } else if (text[current_char] == '-') {
        if (current_char+1 < text.length && text[current_char+1] == '>') {
            string_length_type start = current_char;
            current_char += 2;
            return make_token(Token::ARROW, Span(start, 2));
        }
    } else if (text[current_char] == '/') {
        if (current_char+1 < text.length && text[current_char+1] == '*') {
            string_length_type start = current_char;
            current_char += 2;
            s64 stack = 1;
            while (current_char < text.length && stack > 0) {
                if (text[current_char] == '*') {
                    if (current_char+1 < text.length && text[current_char+1] == '/') {
                        stack--;
                        current_char += 2;
                        continue;
                    }
                }
                
                if (text[current_char] == '/') {
                    if (current_char+1 < text.length && text[current_char+1] == '*') {
                        stack++;
                        current_char += 2;
                        continue;
                    }
                }
                
                current_char++;
            }
            
            string_length_type length = current_char - start;
            // :CommentTokens:
            // Returning a token here because we dont return pointers to tokens
            // and if we returned a recursive lex_token() call, we can get defeated quite quickly from
            // people spamming comment-after-comment.
            
            Token result = make_string_token(Token::COMMENT, Span(start, length), text.substring(start, length));
            return result;
        } else if (current_char+1 < text.length && text[current_char+1] == '/') {
            string_length_type start = current_char;
            current_char += 2;
            
            while (current_char < text.length && text[current_char] != '\n') current_char++;
            
            string_length_type length = current_char - start;
            
            // :CommentTokens:
            Token result = make_string_token(Token::COMMENT, Span(start, length), text.substring(start, length));
            return result;
        }
    } else if (text[current_char] == '<') {
        if (current_char+1 < text.length && text[current_char+1] == '<') {
            string_length_type start = current_char;
            current_char += 2;
            return make_token(Token::DEREFERENCE_OR_SHIFT, Span(start, 2));
        } else if (current_char+1 < text.length && text[current_char+1] == '=') {
            string_length_type start = current_char;
            current_char += 2;
            return make_token(Token::LE_OP, Span(start, 2));
        }
    } else if (text[current_char] == '>') {
        if (current_char+1 < text.length && text[current_char+1] == '>') {
            string_length_type start = current_char;
            current_char += 2;
            return make_token(Token::RIGHT_SHIFT, Span(start, 2));
        } else if (current_char+1 < text.length && text[current_char+1] == '=') {
            string_length_type start = current_char;
            current_char += 2;
            return make_token(Token::GE_OP, Span(start, 2));
        }
    } else if (text[current_char] == '=') {
        if (current_char+1 < text.length && text[current_char+1] == '=') {
            string_length_type start = current_char;
            current_char += 2;
            return make_token(Token::EQ_OP, Span(start, 2));
        }
    } else if (text[current_char] == '!') {
        if (current_char+1 < text.length && text[current_char+1] == '=') {
            string_length_type start = current_char;
            current_char += 2;
            return make_token(Token::NE_OP, Span(start, 2));
        }
    } else if (text[current_char] == '&') {
        if (current_char+1 < text.length && text[current_char+1] == '&') {
            string_length_type start = current_char;
            current_char += 2;
            return make_token(Token::AND_OP, Span(start, 2));
        }
    } else if (text[current_char] == '^') {
        if (current_char+1 < text.length && text[current_char+1] == '^') {
            string_length_type start = current_char;
            current_char += 2;
            return make_token(Token::XOR_OP, Span(start, 2));
        }
    } else if (text[current_char] == '|') {
        if (current_char+1 < text.length && text[current_char+1] == '|') {
            string_length_type start = current_char;
            current_char += 2;
            return make_token(Token::OR_OP, Span(start, 2));
        }
    }
    
    char c = text[current_char];
    auto start = current_char++;
    return make_token((Token::Type)c, Span(start, 1));
}

void Lexer::tokenize_text() {
    Token tok;
    do {
        tok = lex_token();
        
        // Ignore Token::COMMENT since in most cases we dont care about these
        if (tok.type == Token::COMMENT) continue;
        
        tokens.add(tok);
    } while (tok.type != Token::END);
}
