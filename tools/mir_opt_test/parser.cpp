/*
 */
#include "test_desc.h"
#include <parse/lex.hpp>
#include <parse/common.hpp>
#include <parse/parseerror.hpp>

#include <hir/hir.hpp>
#include <mir/mir.hpp>

namespace {
    HIR::Function parse_function(TokenStream& lex, RcString& out_name);
    HIR::TypeRef parse_type(TokenStream& lex);
    MIR::LValue parse_lvalue(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map);

    bool consume_if(TokenStream& lex, eTokenType tok) {
        if( lex.lookahead(0) == tok ) {
            lex.getToken(); // eat token
            return true;
        }
        else {
            return false;
        }
    }
}

MirOptTestFile  MirOptTestFile::load_from_file(const helpers::path& p)
{
    Lexer   lex(p.str());
    Token   tok;

    MirOptTestFile  rv;
    rv.m_crate = HIR::CratePtr(::HIR::Crate());

    while(lex.lookahead(0) != TOK_EOF)
    {
        std::map<RcString, std::string>  attrs;
        while( consume_if(lex, TOK_HASH) )
        {
            //bool is_outer = consume_if(lex, TOK_EXCLAM);
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_OPEN);
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.istr();
            GET_CHECK_TOK(tok, lex, TOK_EQUAL);
            GET_CHECK_TOK(tok, lex, TOK_STRING);
            auto value = tok.str();
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);

            attrs.insert( std::make_pair(name, value) );
        }

        if( consume_if(lex, TOK_RWORD_FN) )
        {
            RcString    fcn_name;
            auto fcn_decl = parse_function(lex, fcn_name);

            auto vi = ::HIR::VisEnt<HIR::ValueItem> {
                HIR::Publicity::new_global(), ::HIR::ValueItem(mv$(fcn_decl))
                };
            rv.m_crate->m_root_module.m_value_items.insert(::std::make_pair(fcn_name,
                ::std::make_unique<decltype(vi)>(mv$(vi))
                ));

            // Attributes
            for(const auto& attr : attrs)
            {
                if( attr.first == "test" )
                {
                    MirOptTestFile::Test    t;
                    t.input_function = ::HIR::SimplePath("", { fcn_name });
                    t.output_template_function = ::HIR::SimplePath("", { RcString(attr.second) });

                    rv.m_tests.push_back(mv$(t));
                }
                else
                {
                    // Ignore?
                }
            }
        }
        //else if( lex.lookahead(0) == "INCLUDE" )
        //{
        //    auto path = lex.check_consume(TokenClass::String).strval;
        //}
        else
        {
            TODO(lex.point_span(), "Error");
        }
    }

    return rv;
}
namespace {
    HIR::Function parse_function(TokenStream& lex, RcString& out_name)
    {
        Token   tok;
        ::std::map<RcString, MIR::LValue::Storage>  val_name_map;
        val_name_map.insert(::std::make_pair("retval", MIR::LValue::Storage::new_Return()));
        ::std::map<RcString, unsigned>  real_bb_name_map;
        ::std::map<unsigned, RcString>  lookup_bb_name_map;

        ::HIR::Function fcn_decl;
        fcn_decl.m_code.m_mir = ::MIR::FunctionPointer(new ::MIR::Function());
        auto& mir_fcn = *fcn_decl.m_code.m_mir;

        // Name
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        auto fcn_name = tok.istr();
        DEBUG("fn " << fcn_name);

        // Arguments
        auto& args = fcn_decl.m_args;
        GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
        while( lex.lookahead(0) != TOK_PAREN_CLOSE )
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.istr();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            auto var_ty = parse_type(lex);

            auto arg_idx = static_cast<unsigned>(args.size());
            val_name_map.insert( ::std::make_pair(name, ::MIR::LValue::Storage::new_Argument(arg_idx)) );
            args.push_back( ::std::make_pair(HIR::Pattern(), mv$(var_ty)) );

            if( !consume_if(lex, TOK_COMMA) )
                break;
        }
        GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
        // Return type
        if( consume_if(lex, TOK_THINARROW) )
        {
            // Parse return type
            fcn_decl.m_return = parse_type(lex);
        }
        else
        {
            fcn_decl.m_return = HIR::TypeRef::new_unit();
        }

        // Body.
        GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);

        // 1. Variable list
        while( consume_if(lex, TOK_RWORD_LET) )
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.istr();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            auto var_ty = parse_type(lex);
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);

            auto var_idx = static_cast<unsigned>(mir_fcn.locals.size());
            val_name_map.insert( ::std::make_pair(name, ::MIR::LValue::Storage::new_Local(var_idx)) );
            mir_fcn.locals.push_back( mv$(var_ty) );
        }
        // 2. List of BBs arranged with 'ident: { STMTS; TERM }'
        while( lex.lookahead(0) != TOK_BRACE_CLOSE )
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.istr();
            real_bb_name_map.insert( ::std::make_pair(name, static_cast<unsigned>(mir_fcn.blocks.size())) );
            mir_fcn.blocks.push_back(::MIR::BasicBlock());
            auto& bb = mir_fcn.blocks.back();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
            while( lex.lookahead(0) != TOK_BRACE_CLOSE )
            {
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                if( tok.istr() == "DROP" )
                {
                    TODO(lex.point_span(), "MIR statement - " << tok);
                }
                else if( tok.istr() == "ASM" )
                {
                    TODO(lex.point_span(), "MIR statement - " << tok);
                }
                else if( tok.istr() == "SETDROP" )
                {
                    TODO(lex.point_span(), "MIR statement - " << tok);
                }
                else if( tok.istr() == "ASSIGN" )
                {
                    // parse a lvalue
                    auto dst = parse_lvalue(lex, val_name_map);
                    GET_CHECK_TOK(tok, lex, TOK_EQUAL);

                    MIR::RValue src;
                    GET_TOK(tok, lex);
                    switch(tok.type())
                    {
                    case TOK_RWORD_TRUE:    src = MIR::Constant::make_Bool({true });   break;
                    case TOK_RWORD_FALSE:   src = MIR::Constant::make_Bool({false});   break;
                    case TOK_IDENT:
                        TODO(lex.point_span(), "MIR assign - " << tok);
                        break;
                        // Tuple literal
                    case TOK_PAREN_OPEN: {
                        src = MIR::RValue::make_Tuple({});
                        auto& vals = src.as_Tuple().vals;
                        while( lex.lookahead(0) != TOK_PAREN_CLOSE )
                        {
                            vals.push_back(parse_lvalue(lex, val_name_map));
                            if( !consume_if(lex, TOK_COMMA) )
                                break;
                        }
                        GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
                    } break;
                    default:
                        TODO(lex.point_span(), "MIR assign - " << tok);
                    }

                    bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(dst), mv$(src) }));
                }
                else
                {
                    TODO(lex.point_span(), "MIR statement - " << tok);
                }
                GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            }
            GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);

            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            if( tok.istr() == "RETURN" )
            {
                bb.terminator = ::MIR::Terminator::make_Return({});
            }
            else
            {
                TODO(lex.point_span(), "MIR terminator - " << tok);
            }
        }
        GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);

        // 3. Convert BB names over
        for(auto& blk : mir_fcn.blocks)
        {
            TU_MATCH_HDRA( (blk.terminator), {)
                TU_ARMA(Goto, e) {
                e = real_bb_name_map.at( lookup_bb_name_map[e] );
            }
            }
        }

        DEBUG(fcn_decl.m_args << " -> " << fcn_decl.m_return);
        out_name = mv$(fcn_name);
        return fcn_decl;
    }

    HIR::Path parse_path(TokenStream& lex)
    {
        TODO(lex.point_span(), "parse_path");
    }
    HIR::TypeRef parse_type(TokenStream& lex)
    {
        Token   tok;
        GET_TOK(tok, lex);
        switch( tok.type() )
        {
        case TOK_PAREN_OPEN: {
            ::std::vector<HIR::TypeRef> tys;
            while( lex.lookahead(0) != TOK_PAREN_CLOSE )
            {
                tys.push_back( parse_type(lex) );
                if( !consume_if(lex, TOK_COMMA) )
                    break;
            }
            GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
            return HIR::TypeRef(mv$(tys));
            } break;
        default:
            TODO(lex.point_span(), "parse_type - " << tok);
        }
    }
    MIR::LValue::Storage parse_lvalue_root(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map)
    {
        if( lex.lookahead(0) == TOK_DOUBLE_COLON )
        {
            // Static
            return MIR::LValue::Storage::new_Static( parse_path(lex) );
        }
        else
        {
            Token   tok;
            GET_TOK(tok, lex);
            CHECK_TOK(tok, TOK_IDENT);
            return name_map.at(tok.istr()).clone();
        }
    }
    MIR::LValue parse_lvalue(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map)
    {
        Token   tok;
        std::vector<MIR::LValue::Wrapper>   wrappers;
        auto root = parse_lvalue_root(lex, name_map);

        while(true)
        {
            GET_TOK(tok, lex);
            switch( tok.type() )
            {
            case TOK_SQUARE_OPEN:
                TODO(lex.point_span(), "parse_lvalue - indexing");
                break;
            case TOK_DOT:
                TODO(lex.point_span(), "parse_lvalue - field");
                break;
            case TOK_HASH:
                TODO(lex.point_span(), "parse_lvalue - downcast");
                break;
            case TOK_STAR:
                wrappers.push_back(::MIR::LValue::Wrapper::new_Deref());
                break;
            default:
                lex.putback(mv$(tok));
                return MIR::LValue(mv$(root), mv$(wrappers));
            }
        }
    }
}