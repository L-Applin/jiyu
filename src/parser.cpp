
#include "parser.h"
#include "compiler.h"


Ast *ast_init(Parser *parser, Ast *ast) {
    Token *token = parser->peek_token();
    ast->text_span = token->text_span;
    ast->filename = token->filename;
    return ast;
}

Token *Parser::next_token() {
    return &lexer->tokens[current_token++];
}

Token *Parser::peek_token() {
    return &lexer->tokens[current_token];
}

Ast_Scope *Parser::get_current_scope() {
    return scope_stack[scope_stack.count-1];
}

bool Parser::expect(Token::Type type) {
    Token *token = peek_token();
    
    if (token->type != type) {
        compiler->report_error(token, "Expected token of type '%d' but got '%d'.\n", type, token->type);
        return false;
    }
    
    return true;
}

bool Parser::expect_and_eat(Token::Type type) {
    Token *token = next_token();
    
    if (token->type != type) {
        compiler->report_error(token, "Expected token of type '%d' but got '%d'.\n", type, token->type);
        return false;
    }
    
    return true;
}

Ast_Identifier *Parser::parse_identifier() {
    if (!expect(Token::IDENTIFIER)) return nullptr;
    Ast_Identifier *ident = AST_NEW(Ast_Identifier);
    
    Token *token = next_token();
    String name = token->string;
    
    Atom *atom = compiler->make_atom(name);
    assert(atom);
    
    ident->name = atom;
    ident->enclosing_scope = get_current_scope();
    return ident;
}

Ast_Expression *Parser::parse_primary_expression() {
    Token *token = peek_token();
    
    if (token->type == Token::IDENTIFIER) {
        auto ident = parse_identifier();
        return ident;
    }
    
    if (token->type == Token::INTEGER) {
        Ast_Literal *lit = AST_NEW(Ast_Literal);
        next_token();
        
        lit->literal_type = Ast_Literal::INTEGER;
        lit->integer_value = token->integer;
        return lit;
    }
    
    if (token->type == Token::KEYWORD_TRUE || token->type == Token::KEYWORD_FALSE) {
        Ast_Literal *lit = AST_NEW(Ast_Literal);
        next_token();
        
        lit->literal_type = Ast_Literal::BOOL;
        lit->bool_value = (token->type == Token::KEYWORD_TRUE);
        return lit;
    }
    
    if (token->type == Token::STRING) {
        Ast_Literal *lit = AST_NEW(Ast_Literal);
        next_token();
        
        lit->literal_type = Ast_Literal::STRING;
        lit->string_value = token->string;
        return lit;
    }
    
    if (token->type == Token::KEYWORD_NULL) {
        Ast_Literal *lit = AST_NEW(Ast_Literal);
        next_token();
        
        lit->literal_type = Ast_Literal::NULLPTR;
        return lit;
    }
    
    if (token->type == '(') {
        next_token();
        
        auto expr = parse_expression();
        
        if (!expect_and_eat((Token::Type) ')')) return nullptr;
        
        return expr;
    }
    
    if (token->type == Token::KEYWORD_SIZEOF) {
        Ast_Sizeof *size = AST_NEW(Ast_Sizeof);
        next_token();
        
        if (!expect_and_eat((Token::Type) '(')) return size;
        
        size->target_type_inst = parse_type_inst();
        
        if (!expect_and_eat((Token::Type) ')')) return size;
        
        return size;
    }
    
    return nullptr;
}

Ast_Expression *Parser::parse_postfix_expression() {
    Ast_Expression *sub_expression = parse_primary_expression();
    if (!sub_expression) return nullptr;
    
    Token *token = peek_token();
    while (token->type != Token::END) {
        
        if (token->type == Token::LEFT_PAREN) {
            // transform this into a function call
            Ast_Function_Call *call = AST_NEW(Ast_Function_Call);
            next_token();
            
            // @HACK @HACK @HACK
            // @HACK @HACK @HACK
            // @HACK @HACK @HACK
            // @HACK @HACK @HACK
            assert(sub_expression->type == AST_IDENTIFIER);
            call->identifier = reinterpret_cast<Ast_Identifier *>(sub_expression);
            
            bool found_argument = false;
            token = peek_token();
            while (token->type != Token::END) {
                
                if (call->argument_list.count > 0 && token->type == ',') {
                    next_token();
                } else if (token->type == ')') break;
                
                auto expr = parse_expression();
                if (!expr) {
                    // @FixME report_error
                    compiler->report_error(expr, "Malformed expression found while parsing parameter list.\n");
                    return nullptr;
                }
                call->argument_list.add(expr);
                
                token = peek_token(); 
            }
            
            if (!expect_and_eat((Token::Type) ')')) return nullptr;
            
            sub_expression = call;
        } else if (token->type == Token::DOT) {
            Ast_Dereference *deref = AST_NEW(Ast_Dereference);
            next_token();
            
            // @TODO do other languages let you use anything other than an identifier for a field selection?
            auto right = parse_identifier();
            if (!right) return nullptr;
            
            deref->left = sub_expression;
            deref->field_selector = right;
            
            sub_expression = deref;
        } else if (token->type == '[') {
            Ast_Array_Dereference *deref = AST_NEW(Ast_Array_Dereference);
            
            next_token();
            deref->array_or_pointer_expression = sub_expression;
            deref->index_expression = parse_expression();
            
            if (!expect_and_eat((Token::Type) ']')) return nullptr;
            
            sub_expression = deref;
        } else {
            break;
        }
        
        
        token = peek_token();
    }
    
    return sub_expression;
}

Ast_Expression *Parser::parse_unary_expression() {
    Token *token = peek_token();
    
    if (token->type == Token::STAR ||
        token->type == Token::DEREFERENCE_OR_SHIFT ||
        token->type == Token::MINUS) {
        Ast_Unary_Expression *ref = AST_NEW(Ast_Unary_Expression);
        ref->operator_type = token->type;
        
        
        next_token();
        
        // we recurse through parse_unary_expression here, but we may be better off using a loop
        auto expression = parse_unary_expression();
        if (!expression) {
            compiler->report_error(token, "Malformed expression following unary operator '%d'.\n", token->type);
            return nullptr;
        }
        
        ref->expression = expression;
        return ref;
    } else if (token->type == Token::KEYWORD_CAST) {
        Ast_Cast *cast = AST_NEW(Ast_Cast);
        
        next_token();
        
        if (!expect_and_eat((Token::Type) '(')) return nullptr;
        
        cast->target_type_inst = parse_type_inst();
        
        if (!expect_and_eat((Token::Type) ')')) return nullptr;
        
        cast->expression = parse_unary_expression();
        if (!cast->expression) {
            compiler->report_error(cast, "Malformed expression following cast.\n", token->type);
            return nullptr;
        }
        
        return cast;
    }
    
    return parse_postfix_expression();
}

Ast_Expression *Parser::parse_multiplicative_expression() {
    auto sub_expression = parse_unary_expression();
    
    Token *token = peek_token();
    while (token->type != Token::END) {
        
        if (token->type == Token::STAR
            || token->type == Token::SLASH
            || token->type == Token::PERCENT) {
            Ast_Binary_Expression *bin = AST_NEW(Ast_Binary_Expression);
            next_token();
            
            bin->operator_type = token->type;
            bin->left = sub_expression;
            
            auto right = parse_unary_expression();
            if (!right) {
                compiler->report_error(token, "Malformed expression following '%c' operator.\n", token->type);
                return nullptr;
            }
            bin->right = right;
            
            sub_expression = bin;
        } else {
            break;
        }
        
        token = peek_token();
    }
    
    return sub_expression;
}

Ast_Expression *Parser::parse_additive_expression() {
    auto sub_expression = parse_multiplicative_expression();
    if (!sub_expression) return nullptr;
    
    Token *token = peek_token();
    while (token->type != Token::END) {
        
        if (token->type == Token::PLUS
            || token->type == Token::MINUS) {
            next_token();
            
            Ast_Binary_Expression *bin = AST_NEW(Ast_Binary_Expression);
            bin->operator_type = token->type;
            bin->left = sub_expression;
            
            auto right = parse_multiplicative_expression();
            if (!right) {
                compiler->report_error(token, "Malformed expression following '%c' operator.\n", token->type);
                return nullptr;
            }
            bin->right = right;
            
            sub_expression = bin;
        } else {
            break;
        }
        
        token = peek_token();
    }
    
    return sub_expression;
}

Ast_Expression *Parser::parse_shift_expression() {
    auto sub_expression = parse_additive_expression();
    
    Token *token = peek_token();
    while (token->type != Token::END) {
        
        if (token->type == Token::DEREFERENCE_OR_SHIFT
            || token->type == Token::RIGHT_SHIFT) {
            next_token();
            
            Ast_Binary_Expression *bin = AST_NEW(Ast_Binary_Expression);
            bin->operator_type = token->type;
            bin->left = sub_expression;
            
            auto right = parse_additive_expression();
            if (!right) {
                compiler->report_error(token, "Malformed expression following '%c' operator.\n", token->type);
                return nullptr;
            }
            bin->right = right;
            
            sub_expression = bin;
        } else {
            break;
        }
        
        token = peek_token();
    }
    
    return sub_expression;
}

Ast_Expression *Parser::parse_relational_expression() {
    auto sub_expression = parse_shift_expression();
    
    Token *token = peek_token();
    while (token->type != Token::END) {
        
        if (token->type == Token::LEFT_ANGLE
            || token->type == Token::RIGHT_ANGLE
            || token->type == Token::LE_OP
            || token->type == Token::GE_OP) {
            next_token();
            
            Ast_Binary_Expression *bin = AST_NEW(Ast_Binary_Expression);
            bin->operator_type = token->type;
            bin->left = sub_expression;
            
            auto right = parse_shift_expression();
            if (!right) {
                compiler->report_error(token, "Malformed expression following '%c' operator.\n", token->type);
                return nullptr;
            }
            bin->right = right;
            
            sub_expression = bin;
        } else {
            break;
        }
        
        token = peek_token();
    }
    
    return sub_expression;
}

Ast_Expression *Parser::parse_equality_expression() {
    auto sub_expression = parse_relational_expression();
    
    Token *token = peek_token();
    while (token->type != Token::END) {
        
        if (token->type == Token::EQ_OP
            || token->type == Token::NE_OP) {
            next_token();
            
            Ast_Binary_Expression *bin = AST_NEW(Ast_Binary_Expression);
            bin->operator_type = token->type;
            bin->left = sub_expression;
            
            auto right = parse_relational_expression();
            if (!right) {
                compiler->report_error(token, "Malformed expression following '%c' operator.\n", token->type);
                return nullptr;
            }
            bin->right = right;
            
            sub_expression = bin;
        } else {
            break;
        }
        
        token = peek_token();
    }
    
    return sub_expression;
}

Ast_Expression *Parser::parse_and_expression() {
    auto sub_expression = parse_equality_expression();
    
    Token *token = peek_token();
    while (token->type != Token::END) {
        
        if (token->type == Token::AMPERSAND) {
            next_token();
            
            Ast_Binary_Expression *bin = AST_NEW(Ast_Binary_Expression);
            bin->operator_type = token->type;
            bin->left = sub_expression;
            
            auto right = parse_equality_expression();
            if (!right) {
                compiler->report_error(token, "Malformed expression following '%c' operator.\n", token->type);
                return nullptr;
            }
            bin->right = right;
            
            sub_expression = bin;
        } else {
            break;
        }
        
        token = peek_token();
    }
    
    return sub_expression;
}

Ast_Expression *Parser::parse_exclusive_or_expression() {
    auto sub_expression = parse_and_expression();
    
    Token *token = peek_token();
    while (token->type != Token::END) {
        
        if (token->type == Token::CARET) {
            next_token();
            
            Ast_Binary_Expression *bin = AST_NEW(Ast_Binary_Expression);
            bin->operator_type = token->type;
            bin->left = sub_expression;
            
            auto right = parse_and_expression();
            if (!right) {
                compiler->report_error(token, "Malformed expression following '%c' operator.\n", token->type);
                return nullptr;
            }
            bin->right = right;
            
            sub_expression = bin;
        } else {
            break;
        }
        
        token = peek_token();
    }
    
    return sub_expression;
}

Ast_Expression *Parser::parse_inclusive_or_expression() {
    auto sub_expression = parse_exclusive_or_expression();
    
    Token *token = peek_token();
    while (token->type != Token::END) {
        
        if (token->type == Token::VERTICAL_BAR) {
            next_token();
            
            Ast_Binary_Expression *bin = AST_NEW(Ast_Binary_Expression);
            bin->operator_type = token->type;
            bin->left = sub_expression;
            
            auto right = parse_exclusive_or_expression();
            if (!right) {
                compiler->report_error(token, "Malformed expression following '%c' operator.\n", token->type);
                return nullptr;
            }
            bin->right = right;
            
            sub_expression = bin;
        } else {
            break;
        }
        
        token = peek_token();
    }
    
    return sub_expression;
}

Ast_Expression *Parser::parse_logical_and_expression() {
    auto sub_expression = parse_inclusive_or_expression();
    
    Token *token = peek_token();
    while (token->type != Token::END) {
        
        if (token->type == Token::AND_OP) {
            next_token();
            
            Ast_Binary_Expression *bin = AST_NEW(Ast_Binary_Expression);
            bin->operator_type = token->type;
            bin->left = sub_expression;
            
            auto right = parse_inclusive_or_expression();
            if (!right) {
                compiler->report_error(token, "Malformed expression following '%c' operator.\n", token->type);
                return nullptr;
            }
            bin->right = right;
            
            sub_expression = bin;
        } else {
            break;
        }
        
        token = peek_token();
    }
    
    return sub_expression;
}

Ast_Expression *Parser::parse_logical_xor_expression() {
    auto sub_expression = parse_logical_and_expression();
    
    Token *token = peek_token();
    while (token->type != Token::END) {
        
        if (token->type == Token::XOR_OP) {
            next_token();
            
            Ast_Binary_Expression *bin = AST_NEW(Ast_Binary_Expression);
            bin->operator_type = token->type;
            bin->left = sub_expression;
            
            auto right = parse_logical_and_expression();
            if (!right) {
                compiler->report_error(token, "Malformed expression following '%c' operator.\n", token->type);
                return nullptr;
            }
            bin->right = right;
            
            sub_expression = bin;
        } else {
            break;
        }
        
        token = peek_token();
    }
    
    return sub_expression;
}

Ast_Expression *Parser::parse_logical_or_expression() {
    auto sub_expression = parse_logical_xor_expression();
    
    Token *token = peek_token();
    while (token->type != Token::END) {
        
        if (token->type == Token::OR_OP) {
            next_token();
            
            Ast_Binary_Expression *bin = AST_NEW(Ast_Binary_Expression);
            bin->operator_type = token->type;
            bin->left = sub_expression;
            
            auto right = parse_logical_xor_expression();
            if (!right) {
                compiler->report_error(token, "Malformed expression following '%c' operator.\n", token->type);
                return nullptr;
            }
            bin->right = right;
            
            sub_expression = bin;
        } else {
            break;
        }
        
        token = peek_token();
    }
    
    return sub_expression;
}

Ast_Expression *Parser::parse_expression() {
    return parse_logical_or_expression();
}

Ast_Expression *Parser::parse_statement() {
    Token *token = peek_token();
    
    if (token->type == Token::KEYWORD_FUNC) {
        return parse_function();
    }
    
    if (token->type == Token::KEYWORD_TYPEALIAS) {
        Ast_Type_Alias *alias = AST_NEW(Ast_Type_Alias);
        next_token();
        alias->identifier = parse_identifier();
        
        if (!expect_and_eat(Token::EQUALS)) return nullptr;
        
        alias->internal_type_inst = parse_type_inst();
        
        if (!alias->internal_type_inst) {
            compiler->report_error(alias, "Could not parse aliasee following typealias.\n");
            return nullptr;
        }
        
        if (!expect_and_eat(Token::SEMICOLON)) return nullptr;
        
        return alias;
    }
    
    if (token->type == Token::KEYWORD_STRUCT) {
        Ast_Struct *_struct = AST_NEW(Ast_Struct);
        next_token();
        _struct->identifier = parse_identifier();
        _struct->member_scope.parent = get_current_scope();
        parse_scope(&_struct->member_scope, true);
        return _struct;
    }
    
    if (token->type == Token::KEYWORD_VAR) {
        auto var = parse_variable_declaration(true);
        if (!expect_and_eat(Token::SEMICOLON)) return nullptr;
        return var;
    }
    
    if (token->type == Token::KEYWORD_LET) {
        next_token();
        
        auto let = parse_variable_declaration(false);
        if (!expect_and_eat(Token::SEMICOLON)) return nullptr;
        
        let->is_let = true;
        return let;
    }
    
    if (token->type == Token::KEYWORD_IF) {
        Ast_If *_if = AST_NEW(Ast_If);
        next_token();
        
        _if->condition = parse_expression();
        if (!_if->condition) {
            // @Cleanup move to sema?
            compiler->report_error(_if, "'if' must be followed by an expression.\n");
            return _if;
        }
        
        _if->then_statement = parse_statement();
        
        token = peek_token();
        if (token->type == Token::KEYWORD_ELSE) {
            next_token();
            
            _if->else_statement = parse_statement();
        }
        
        return _if;
    }
    
    if (token->type == Token::KEYWORD_FOR) {
        Ast_For *_for = AST_NEW(Ast_For);
        next_token();
        
        _for->initial_iterator_expression = parse_expression();
        
        token = peek_token();
        if (token->type == Token::DOTDOT) {
            next_token();
            
            if (!_for->initial_iterator_expression) {
                compiler->report_error(token, ".. operator must be preceeded by an expression.\n");
                return _for;
            }
            
            _for->upper_range_expression = parse_expression();
        }
        
        _for->iterator_declaration_scope.parent = get_current_scope();
        _for->body.parent = &_for->iterator_declaration_scope;
        parse_scope(&_for->body, false, true);
        return _for;
    }
    
    if (token->type == Token::KEYWORD_WHILE) {
        Ast_While *loop = AST_NEW(Ast_While);
        next_token();
        
        loop->condition = parse_expression();
        
        if (!loop->condition) {
            compiler->report_error(loop, "'while' must be followed by an expression.\n");
            return loop;
        }
        
        loop->statement = parse_statement();
        return loop;
    }
    
    if (token->type == Token::KEYWORD_RETURN) {
        Ast_Return *ret = AST_NEW(Ast_Return);
        next_token();
        
        ret->owning_function = currently_parsing_function;
        ret->expression = parse_expression();
        
        if (!expect_and_eat(Token::SEMICOLON)) return nullptr;
        return ret;
    }
    
    if (token->type == '{') {
        auto parent = get_current_scope();
        Ast_Scope *scope = AST_NEW(Ast_Scope);
        scope->parent = parent;
        parse_scope(scope, true);
        return scope;
    }
    
    Ast_Expression *left = parse_expression();
    if (!left) return nullptr;
    
    token = peek_token();
    if (token->type == Token::EQUALS) {
        next_token();
        
        Ast_Expression *right = parse_expression();
        Ast_Binary_Expression *bin = AST_NEW(Ast_Binary_Expression);
        bin->operator_type = Token::EQUALS;
        bin->left = left;
        bin->right = right;
        
        left = bin;
    }
    
    if (!expect_and_eat(Token::SEMICOLON)) return nullptr;
    
    return left;
}

void Parser::parse_scope(Ast_Scope *scope, bool requires_braces, bool only_one_statement) {
    scope_stack.add(scope);
    
    if (requires_braces && !expect_and_eat((Token::Type) '{')) return;
    
    Token *token = peek_token();
    while (token->type != Token::END) {
        
        if (requires_braces && token->type == '}') break;
        
        Ast_Expression *stmt = parse_statement();
        if (stmt) {
            scope->statements.add(stmt);
            
            // @Cleanup have a is_declaration() function?
            if (stmt->type == AST_DECLARATION ||
                stmt->type == AST_FUNCTION    ||
                stmt->type == AST_TYPE_ALIAS  ||
                stmt->type == AST_STRUCT) {
                scope->declarations.add(stmt);
            }
        }
        
        if (compiler->errors_reported) return;
        
        if (only_one_statement) break;
        
        token = peek_token();
    }
    
    if (requires_braces && !expect_and_eat((Token::Type) '}')) return;
    
    scope_stack.pop();
}

Ast_Declaration *Parser::parse_variable_declaration(bool expect_var_keyword) {
    if (expect_var_keyword && !expect_and_eat(Token::KEYWORD_VAR)) return nullptr;
    
    Token *ident_token = peek_token(); // used for error below, @Cleanup we want to be able to report errors using an Ast
    Ast_Identifier *ident = parse_identifier();
    if (!ident) {
        compiler->report_error(ident_token, "Expected identifier for variable declaration.\n");
        return nullptr;
    }
    
    Ast_Declaration *decl = AST_NEW(Ast_Declaration);
    decl->identifier = ident;
    
    Token *token = peek_token();
    if (token->type == Token::COLON) {
        next_token();
        
        Ast_Type_Instantiation *type_inst = parse_type_inst();
        if (!type_inst) return nullptr;
        
        
        decl->type_inst = type_inst;
    }
    
    token = peek_token();
    if (token->type == Token::EQUALS) {
        next_token();
        
        Ast_Expression *expression = parse_expression();
        if (!expression) return nullptr;
        
        decl->initializer_expression = expression;
    }
    
    if (!decl->initializer_expression && !decl->type_inst) {
        // @TODO maybe this should be moved to semantic analysis
        compiler->report_error(ident_token, "Declared variable must be declared with a type or be initialized.\n");
        return nullptr;
    }
    
    
    return decl;
}


Ast_Type_Instantiation *Parser::wrap_primitive_type(Ast_Type_Info *info) {
    assert(info->type == Ast_Type_Info::VOID    ||
           info->type == Ast_Type_Info::BOOL    ||
           info->type == Ast_Type_Info::INTEGER ||
           info->type == Ast_Type_Info::FLOAT   ||
           info->type == Ast_Type_Info::STRING);
    
    auto type_inst = AST_NEW(Ast_Type_Instantiation);
    type_inst->builtin_primitive = info;
    return type_inst;
}

Ast_Type_Instantiation *Parser::parse_type_inst() {
    Token *token = peek_token();
    
    Ast_Type_Info *builtin_primitive = nullptr;
    switch (token->type) {
        case Token::KEYWORD_INT:    builtin_primitive = compiler->type_int32; break;  // @IntegerSize ??
        case Token::KEYWORD_UINT:   builtin_primitive = compiler->type_uint32; break; // @IntegerSize ??
        
        case Token::KEYWORD_INT8:   builtin_primitive = compiler->type_int8; break;
        case Token::KEYWORD_INT16:  builtin_primitive = compiler->type_int16; break;
        case Token::KEYWORD_INT32:  builtin_primitive = compiler->type_int32; break;
        case Token::KEYWORD_INT64:  builtin_primitive = compiler->type_int64; break;
        
        case Token::KEYWORD_UINT8:  builtin_primitive = compiler->type_uint8; break;
        case Token::KEYWORD_UINT16: builtin_primitive = compiler->type_uint16; break;
        case Token::KEYWORD_UINT32: builtin_primitive = compiler->type_uint32; break;
        case Token::KEYWORD_UINT64: builtin_primitive = compiler->type_uint64; break;
        
        case Token::KEYWORD_FLOAT:  builtin_primitive = compiler->type_float32; break;
        case Token::KEYWORD_DOUBLE: builtin_primitive = compiler->type_float64; break;
        
        case Token::KEYWORD_STRING: builtin_primitive = compiler->type_string; break;
        
        case Token::KEYWORD_VOID:   builtin_primitive = compiler->type_void; break;
        
        case Token::KEYWORD_BOOL:   builtin_primitive = compiler->type_bool; break;
    }
    
    if (builtin_primitive) {
        next_token();
        return wrap_primitive_type(builtin_primitive);
    }
    
    if (token->type == Token::STAR) {
        next_token();
        auto pointee = parse_type_inst();
        if (!pointee) {
            compiler->report_error(token, "Couldn't parse pointer element type.\n");
            return nullptr;
        }
        
        Ast_Type_Instantiation *type_inst = AST_NEW(Ast_Type_Instantiation);
        type_inst->pointer_to = pointee;
        return type_inst;
    }
    
    if (token->type == Token::IDENTIFIER) {
        Ast_Type_Instantiation *type_inst = AST_NEW(Ast_Type_Instantiation);
        
        auto ident = parse_identifier();
        type_inst->typename_identifier = ident;
        return type_inst;
    }
    
    if (token->type == '[') {
        Ast_Type_Instantiation *type_inst = AST_NEW(Ast_Type_Instantiation);
        next_token();
        
        token = peek_token();
        if (token->type == Token::DOTDOT) {
            type_inst->array_is_dynamic = true;
            next_token();
            
            if (!expect_and_eat((Token::Type) ']')) return type_inst;
        } else if (token->type == ']') {
            next_token();
        } else {
            type_inst->array_size_expression = parse_expression();
            
            if (!type_inst->array_size_expression) {
                compiler->report_error(type_inst, "Expected expression or '..' token within array size instantiation.\n");
            }
            
            if (!expect_and_eat((Token::Type) ']')) return type_inst;
        }
        
        type_inst->array_element_type = parse_type_inst();
        if (!type_inst->array_element_type) {
            compiler->report_error(type_inst, "Couldn't parse array element type.\n");
        }
        return type_inst;
    }
    
    return nullptr;
}

Ast_Function *Parser::parse_function() {
    expect_and_eat(Token::KEYWORD_FUNC);
    
    Ast_Function *function = AST_NEW(Ast_Function);
    function->arguments_scope.parent = get_current_scope();
    
    Ast_Function *old_function = currently_parsing_function;
    
    currently_parsing_function = function;
    
    
    Token *token = peek_token();
    if (token->type == Token::TAG_C_FUNCTION) {
        function->is_c_function = true;
        next_token();
    }
    
    Ast_Identifier *ident = parse_identifier();
    if (!ident) return nullptr;
    
    token = peek_token();
    if (token->type == Token::LEFT_ANGLE) {
        Ast_Scope *polymorphic_scope = AST_NEW(Ast_Scope);
        polymorphic_scope->is_template_argument_block = true;
        function->polymorphic_type_alias_scope = polymorphic_scope;
        next_token();
        
        scope_stack.add(polymorphic_scope);
        
        token = peek_token();
        while (token->type != Token::END) {
            Ast_Type_Alias *alias = AST_NEW(Ast_Type_Alias);
            alias->identifier = parse_identifier();
            
            if (!alias->identifier) {
                compiler->report_error(alias, "Expected identifier in template argument list but got something else.\n");
                return nullptr;
            }
            
            polymorphic_scope->declarations.add(alias);
            
            token = peek_token();
            if (token->type == Token::COMMA) {
                next_token();
                
                token = peek_token();
                continue;
            }
            
            if (!expect_and_eat(Token::RIGHT_ANGLE)) return nullptr;
            
            break;
        }
        
        scope_stack.pop();
        
        function->arguments_scope.parent = polymorphic_scope;
        
        function->is_template_function = true;
    }
    
    if (!expect_and_eat((Token::Type) '(')) return nullptr;
    
    token = peek_token();
    while (token->type != Token::END) {
        
        if (function->arguments.count > 0 && token->type == ',') {
            next_token();
            token = peek_token();
        } else  if (token->type == ')') break;
        
        // @Temporary
        // @Temporary
        // @Temporary
        if (token->type == Token::TEMPORARY_KEYWORD_C_VARARGS) {
            next_token();
            function->is_c_varargs = true;
            
            token = peek_token();
            if (token->type != ')') {
                compiler->report_error(token, "Expected ')' following 'temporary_c_vararg' declarator.\n");
                return nullptr;
            }
            break;
        }
        
        Ast_Declaration *decl = parse_variable_declaration(false);
        if (decl) {
            decl->is_let = true;
            function->arguments.add(decl);
            function->arguments_scope.declarations.add(decl);
        }
        
        if (compiler->errors_reported) return nullptr;
        
        token = peek_token();
    }
    
    if (!expect_and_eat((Token::Type) ')')) return nullptr;
    
    if (peek_token()->type == Token::ARROW) {
        token = peek_token();
        if (!expect_and_eat(Token::ARROW)) return nullptr;
        
        Ast_Type_Instantiation *type_inst = parse_type_inst();
        if (!type_inst) {
            compiler->report_error(token, "Could not parse type following '->'.\n");
            return nullptr;
        }
        
        // @Cleanup change this from a declaration to just the type-instantiation?
        Ast_Declaration *decl = AST_NEW(Ast_Declaration);
        // decl->identifier = nullptr;
        decl->type_inst = type_inst;
        
        function->return_decl = decl;
        
        token = peek_token();
    }
    
    
    if (peek_token()->type == '{') {
        auto parent = get_current_scope();
        Ast_Scope *scope = AST_NEW(Ast_Scope);
        scope->parent = &function->arguments_scope;
        parse_scope(scope, true);
        function->scope = scope;
    } else {
        if (!expect_and_eat(Token::SEMICOLON)) return nullptr;
    }
    
    function->identifier = ident;
    currently_parsing_function = old_function;
    return function;
}
