/*
Copyright (c) 2014 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include <string>
#include "util/sexpr/option_declarations.h"
#include "util/sstream.h"
#include "kernel/abstract.h"
#include "library/annotation.h"
#include "library/placeholder.h"
#include "library/explicit.h"
#include "library/aliases.h"
#include "library/scoped_ext.h"
#include "library/typed_expr.h"
#include "library/choice.h"
#include "library/constants.h"
#include "library/quote.h"
#include "library/string.h"
#include "library/trace.h"
#include "library/kernel_serializer.h"
#include "library/tactic/elaborate.h"
#include "library/tactic/smt/hinst_lemmas.h"
#include "library/equations_compiler/equations.h"
#include "frontends/lean/builtin_exprs.h"
#include "frontends/lean/decl_cmds.h"
#include "frontends/lean/token_table.h"
#include "frontends/lean/calc.h"
#include "frontends/lean/parser.h"
#include "frontends/lean/util.h"
#include "frontends/lean/tokens.h"
#include "frontends/lean/match_expr.h"
#include "frontends/lean/decl_util.h"
#include "frontends/lean/brackets.h"
#include "frontends/lean/tactic_notation.h"
#include "frontends/lean/elaborator.h"

#ifndef LEAN_DEFAULT_PARSER_CHECKPOINT_HAVE
#define LEAN_DEFAULT_PARSER_CHECKPOINT_HAVE true
#endif

/* Temporary hack for parser_state refactoring */
#define parser_state parser

namespace lean {
static name * g_parser_checkpoint_have = nullptr;

bool get_parser_checkpoint_have(options const & opts) {
    return opts.get_bool(*g_parser_checkpoint_have, LEAN_DEFAULT_PARSER_CHECKPOINT_HAVE);
}

using namespace notation; // NOLINT

static name * g_no_universe_annotation = nullptr;

bool is_sort_wo_universe(expr const & e) {
    return is_annotation(e, *g_no_universe_annotation);
}

expr mk_sort_wo_universe(parser_state & p, pos_info const & pos, bool is_type) {
    expr r = p.save_pos(mk_sort(is_type ? mk_level_one() : mk_level_zero()), pos);
    return p.save_pos(mk_annotation(*g_no_universe_annotation, r), pos);
}

static expr parse_Type(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    if (p.curr_is_token(get_llevel_curly_tk())) {
        p.next();
        level l = mk_succ(p.parse_level());
        p.check_token_next(get_rcurly_tk(), "invalid Type expression, '}' expected");
        return p.save_pos(mk_sort(l), pos);
    } else {
        return mk_sort_wo_universe(p, pos, true);
    }
}

static expr parse_Sort(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    if (p.curr_is_token(get_llevel_curly_tk())) {
        p.next();
        level l = p.parse_level();
        p.check_token_next(get_rcurly_tk(), "invalid Sort expression, '}' expected");
        return p.save_pos(mk_sort(l), pos);
    } else {
        return mk_sort_wo_universe(p, pos, false);
    }
}

static expr parse_Type_star(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    return p.save_pos(mk_sort(mk_succ(mk_level_placeholder())), pos);
}

static expr parse_Sort_star(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    return p.save_pos(mk_sort(mk_level_placeholder()), pos);
}

static name * g_let_match_name = nullptr;

static expr parse_let(parser_state & p, pos_info const & pos, bool in_do_block);
static expr parse_do(parser_state & p, bool has_braces);
static expr parse_let_body(parser_state & p, pos_info const & pos, bool in_do_block) {
    if (in_do_block) {
        if (p.curr_is_token(get_in_tk())) {
            p.next();
            return p.parse_expr();
        } else {
            p.check_token_next(get_comma_tk(), "invalid 'do' block 'let' declaration, ',' or 'in' expected");
            if (p.curr_is_token(get_let_tk())) {
                p.next();
                return parse_let(p, pos, in_do_block);
            } else {
                return parse_do(p, false);
            }
        }
    } else {
        if (p.curr_is_token(get_comma_tk())) {
            p.next();
            return parse_let(p, pos, in_do_block);
        } else if (p.curr_is_token(get_in_tk())) {
            p.next();
            return p.parse_expr();
        } else {
            throw parser_error("invalid let declaration, 'in' or ',' expected", p.pos());
        }
    }
}

// Distribute mk_typed_expr over choice expression.
// see issue #768
static expr mk_typed_expr_distrib_choice(parser_state & p, expr const & type, expr const & value, pos_info const & pos) {
    if (is_choice(value)) {
        buffer<expr> new_choices;
        for (unsigned i = 0; i < get_num_choices(value); i++) {
            new_choices.push_back(mk_typed_expr_distrib_choice(p, type, get_choice(value, i), pos));
        }
        return p.save_pos(mk_choice(new_choices.size(), new_choices.data()), pos);
    } else {
        return p.save_pos(mk_typed_expr(type, value), pos);
    }
}

static expr parse_let(parser_state & p, pos_info const & pos, bool in_do_block) {
    parser_state::local_scope scope1(p);
    if (!in_do_block && p.parse_local_notation_decl()) {
        return parse_let_body(p, pos, in_do_block);
    } else if (p.curr_is_identifier()) {
        auto id_pos     = p.pos();
        name id         = p.check_atomic_id_next("invalid let declaration, atomic identifier expected");
        expr type;
        expr value;
        if (p.curr_is_token(get_assign_tk())) {
            p.next();
            type  = p.save_pos(mk_expr_placeholder(), id_pos);
            value = p.parse_expr();
        } else if (p.curr_is_token(get_colon_tk())) {
            p.next();
            type = p.parse_expr();
            p.check_token_next(get_assign_tk(), "invalid declaration, ':=' expected");
            value = p.parse_expr();
        } else {
            parser_state::local_scope scope2(p);
            buffer<expr> ps;
            unsigned rbp = 0;
            auto lenv = p.parse_binders(ps, rbp);
            if (p.curr_is_token(get_colon_tk())) {
                p.next();
                type  = p.parse_scoped_expr(ps, lenv);
                type  = Pi(ps, type, p);
            } else {
                type  = p.save_pos(mk_expr_placeholder(), id_pos);
            }
            p.check_token_next(get_assign_tk(), "invalid let declaration, ':=' expected");
            value = p.parse_scoped_expr(ps, lenv);
            value = Fun(ps, value, p);
        }
        expr x = p.save_pos(mk_local(id, type), id_pos);
        p.add_local_expr(id, x);
        expr b = parse_let_body(p, pos, in_do_block);
        return p.save_pos(mk_let(id, type, value, abstract_local(b, x)), pos);
    } else {
        buffer<expr> new_locals;
        expr lhs = p.parse_pattern(new_locals);
        p.check_token_next(get_assign_tk(), "invalid let declaration, ':=' expected");
        expr value = p.parse_expr();
        for (expr const & l : new_locals)
            p.add_local(l);
        expr body  = parse_let_body(p, pos, in_do_block);
        match_definition_scope match_scope;
        expr fn = p.save_pos(mk_local(mk_fresh_name(), *g_let_match_name, mk_expr_placeholder(), mk_rec_info(true)), pos);
        expr eqn = Fun(fn, Fun(new_locals, p.save_pos(mk_equation(p.rec_save_pos(mk_app(fn, lhs), pos), body), pos), p), p);
        equations_header h = mk_equations_header(match_scope.get_name());
        expr eqns  = p.save_pos(mk_equations(h, 1, &eqn), pos);
        return p.save_pos(mk_app(eqns, value), pos);
    }
}

static expr parse_let_expr(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    bool in_do_block = false;
    return parse_let(p, pos, in_do_block);
}

static name * g_do_match_name = nullptr;

static expr fix_do_action_lhs(parser_state & p, expr const & lhs, expr const & type, pos_info const & lhs_pos,
                              buffer<expr> & new_locals) {
    // Hack
    if ((is_placeholder(lhs)) ||
        (is_constant(lhs) && !inductive::is_intro_rule(p.env(), const_name(lhs))) ||
        (is_local(lhs))) {
        expr new_lhs;
        if (is_placeholder(lhs)) {
            new_lhs = mk_local("_x", type);
        } else if (is_constant(lhs)) {
            new_lhs = mk_local(name(const_name(lhs).get_string()), type);
        } else {
            new_lhs = mk_local(local_pp_name(lhs), type);
        }
        new_lhs = p.save_pos(new_lhs, lhs_pos);
        new_locals.clear();
        new_locals.push_back(new_lhs);
        return new_lhs;
    } else {
        return lhs;
    }
}

static std::tuple<optional<expr>, expr, expr, optional<expr>> parse_do_action(parser_state & p, buffer<expr> & new_locals) {
    auto lhs_pos = p.pos();
    optional<expr> lhs = some(p.parse_pattern_or_expr());
    expr type, curr;
    optional<expr> else_case;
    if (p.curr_is_token(get_colon_tk())) {
        p.next();
        type = p.parse_expr();
        lhs  = fix_do_action_lhs(p, *lhs, type, lhs_pos, new_locals);
        if (!is_local(*lhs)) {
            throw parser_error("invalid 'do' block, unexpected ':' the left hand side is a pattern", lhs_pos);
        }
        p.check_token_next(get_larrow_tk(), "invalid 'do' block, '←' expected");
        curr = p.parse_expr();
    } else if (p.curr_is_token(get_larrow_tk())) {
        p.next();
        type = p.save_pos(mk_expr_placeholder(), lhs_pos);
        lhs  = fix_do_action_lhs(p, *lhs, type, lhs_pos, new_locals);
        if (!is_local(*lhs)) {
            bool skip_main_fn = false;
            lhs = p.patexpr_to_pattern(*lhs, skip_main_fn, new_locals);
        }
        curr = p.parse_expr();
        if (p.curr_is_token(get_bar_tk())) {
            p.next();
            else_case = p.parse_expr();
        }
    } else {
        curr = p.patexpr_to_expr(*lhs);
        type = p.save_pos(mk_expr_placeholder(), lhs_pos);
        lhs  = none_expr();
    }
    return std::make_tuple(lhs, type, curr, else_case);
}

static expr mk_bind_fn() {
    return mk_no_info(mk_constant(get_has_bind_bind_name()));
}

static name * g_do_failure_eq = nullptr;

/* Mark for automatic failure equation when pattern matching in do-expressions. */
expr mark_do_failure_eq(expr const & e) {
    return mk_annotation(*g_do_failure_eq, e);
}

bool is_do_failure_eq(expr const & e) {
    auto it = e;
    while (is_lambda(it))
        it = binding_body(it);
    if (!is_equation(it)) return false;
    return is_annotation(equation_rhs(it), *g_do_failure_eq);
}

static expr parse_do(parser_state & p, bool has_braces) {
    parser_state::local_scope scope(p);
    buffer<expr>               es;
    buffer<pos_info>           ps;
    buffer<optional<expr>>     lhss;
    buffer<optional<expr>>     else_cases;
    buffer<list<expr>>         lhss_locals;
    while (true) {
        if (p.curr_is_token(get_let_tk())) {
            auto pos = p.pos();
            p.next();
            bool in_do_block = true;
            es.push_back(parse_let(p, pos, in_do_block));
            break;
        } else {
            auto lhs_pos = p.pos();
            ps.push_back(lhs_pos);
            buffer<expr> new_locals;
            optional<expr> lhs, else_case;
            expr type, curr;
            std::tie(lhs, type, curr, else_case) = parse_do_action(p, new_locals);
            es.push_back(curr);
            if (p.curr_is_token(get_comma_tk())) {
                p.next();
                for (expr const & l : new_locals)
                    p.add_local(l);
                if (lhs && !is_local(*lhs)) {
                    // if lhs is a pattern, we need to save the locals to create the match
                    lhss_locals.push_back(to_list(new_locals));
                } else {
                    lhss_locals.push_back(list<expr>());
                }
                lhss.push_back(lhs);
                else_cases.push_back(else_case);
            } else {
                if (lhs) {
                    throw parser_error("the last statement in a 'do' block must be an expression", lhs_pos);
                }
                break;
            }
        }
    }
    if (has_braces) {
        p.check_token_next(get_rcurly_tk(), "invalid 'do' block, '}' expected");
    }
    lean_assert(!es.empty());
    lean_assert(es.size() == lhss.size() + 1);
    if (es.size() == 1)
        return es[0];
    unsigned i = es.size();
    --i;
    expr r = es[i];
    while (i > 0) {
        --i;
        if (auto lhs = lhss[i]) {
            if (is_local(*lhs)) {
                r = p.rec_save_pos(mk_app(p.save_pos(mk_bind_fn(), ps[i]), es[i], Fun(*lhs, r, p)), ps[i]);
            } else {
                // must introduce a "fake" match
                auto pos   = ps[i];
                match_definition_scope match_scope;
                expr fn = p.save_pos(mk_local(mk_fresh_name(), *g_do_match_name, mk_expr_placeholder(), mk_rec_info(true)), pos);
                buffer<expr> locals;
                to_buffer(lhss_locals[i], locals);
                buffer<expr> eqs;
                eqs.push_back(Fun(fn, Fun(locals, p.save_pos(mk_equation(p.rec_save_pos(mk_app(fn, *lhs), pos), r), pos), p), p));
                expr else_case;
                bool ignore_if_unused;
                if (optional<expr> r = else_cases[i]) {
                    else_case        = *r;
                    ignore_if_unused = false;
                } else {
                    else_case        = p.save_pos(mark_do_failure_eq(p.save_pos(mk_constant(get_match_failed_name()), pos)), pos);
                    ignore_if_unused = true;
                }
                // add case
                //    _ := else_case
                expr x = mk_local(mk_fresh_name(), "_x", mk_expr_placeholder(), binder_info());
                expr else_eq = Fun(fn, Fun(x, p.save_pos(mk_equation(p.rec_save_pos(mk_app(fn, x), pos),
                                                                     else_case,
                                                                     ignore_if_unused),
                                                         pos), p), p);
                eqs.push_back(else_eq);
                equations_header h = mk_equations_header(match_scope.get_name());
                expr eqns  = p.save_pos(mk_equations(h, eqs.size(), eqs.data()), pos);
                expr local = mk_local("_p", mk_expr_placeholder());
                expr match = p.mk_app(eqns, local, pos);
                r = p.rec_save_pos(mk_app(p.save_pos(mk_bind_fn(), ps[i]),
                                          es[i],
                                          p.save_pos(Fun(local, match, p), pos)),
                                   pos);
            }
        } else {
            r = p.rec_save_pos(mk_app(p.save_pos(mk_bind_fn(), ps[i]),
                                      es[i],
                                      p.save_pos(mk_lambda("_x", mk_expr_placeholder(), r), p.pos_of(r))),
                               ps[i]);
        }
    }
    return r;
}

static expr parse_do_expr(parser_state & p, unsigned, expr const *, pos_info const &) {
    bool has_braces = false;
    if (p.curr_is_token(get_lcurly_tk())) {
        has_braces = true;
        p.next();
    }
    return parse_do(p, has_braces);
}

static expr parse_unit(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    return p.save_pos(mk_constant(get_unit_star_name()), pos);
}

static expr parse_proof(parser_state & p);

static expr parse_proof(parser_state & p) {
    if (p.curr_is_token(get_from_tk())) {
        // parse: 'from' expr
        p.next();
        return p.parse_expr();
    } else if (p.curr_is_token(get_begin_tk())) {
        auto pos = p.pos();
        return parse_begin_end_expr(p, pos);
    } else if (p.curr_is_token(get_lcurly_tk())) {
        auto pos = p.pos();
        return parse_curly_begin_end_expr(p, pos);
    } else if (p.curr_is_token(get_by_tk())) {
        auto pos = p.pos();
        return parse_by(p, 0, nullptr, pos);
    } else {
        throw parser_error("invalid expression, 'by', 'begin', '{', or 'from' expected", p.pos());
    }
}

static expr parse_have_core(parser_state & p, pos_info const & pos, optional<expr> const & prev_local) {
    auto id_pos       = p.pos();
    name id;
    expr prop;
    if (p.curr_is_identifier()) {
        id = p.get_name_val();
        p.next();
        if (p.curr_is_token(get_colon_tk())) {
            p.next();
            prop      = p.parse_expr();
        } else {
            expr left = p.id_to_expr(id, id_pos);
            id        = get_this_tk();
            unsigned rbp = 0;
            while (rbp < p.curr_lbp()) {
                left = p.parse_led(left);
            }
            prop      = left;
        }
    } else {
        id            = get_this_tk();
        prop          = p.parse_expr();
    }
    expr proof;
    p.check_token_next(get_comma_tk(), "invalid 'have' declaration, ',' expected");
    if (prev_local) {
        parser_state::local_scope scope(p);
        p.add_local(*prev_local);
        auto proof_pos = p.pos();
        proof = parse_proof(p);
        proof = Fun(*prev_local, proof, p);
        proof = p.save_pos(mk_app(proof, *prev_local), proof_pos);
    } else {
        proof = parse_proof(p);
    }
    p.check_token_next(get_comma_tk(), "invalid 'have' declaration, ',' expected");
    parser_state::local_scope scope(p);
    expr l = p.save_pos(mk_local(id, prop), pos);
    p.add_local(l);
    expr body;
    if (p.curr_is_token(get_then_tk())) {
        auto then_pos = p.pos();
        p.next();
        p.check_token_next(get_have_tk(), "invalid 'then' declaration, 'have' expected");
        body  = parse_have_core(p, then_pos, some_expr(l));
    } else {
        body  = p.parse_expr();
    }
    body = abstract(body, l);
    if (get_parser_checkpoint_have(p.get_options()))
        body = mk_checkpoint_annotation(body);
    expr r = p.save_pos(mk_have_annotation(p.save_pos(mk_lambda(id, prop, body), pos)), pos);
    return p.mk_app(r, proof, pos);
}

static expr parse_have(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    return parse_have_core(p, pos, none_expr());
}

static expr parse_suppose(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    auto id_pos = p.pos();
    name id;
    expr prop;
    if (p.curr_is_identifier()) {
        id = p.get_name_val();
        p.next();
        if (p.curr_is_token(get_colon_tk())) {
            p.next();
            prop      = p.parse_expr();
        } else {
            expr left = p.id_to_expr(id, id_pos);
            id        = get_this_tk();
            unsigned rbp = 0;
            while (rbp < p.curr_lbp()) {
                left = p.parse_led(left);
            }
            prop      = left;
        }
    } else {
        id    = get_this_tk();
        prop  = p.parse_expr();
    }
    p.check_token_next(get_comma_tk(), "invalid 'suppose', ',' expected");
    parser_state::local_scope scope(p);
    expr l = p.save_pos(mk_local(id, prop), id_pos);
    p.add_local(l);
    expr body = p.parse_expr();
    return p.save_pos(Fun(l, body, p), pos);
}

static expr parse_show(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    expr prop  = p.parse_expr();
    p.check_token_next(get_comma_tk(), "invalid 'show' declaration, ',' expected");
    expr proof = parse_proof(p);
    expr b = p.save_pos(mk_lambda(get_this_tk(), prop, Var(0)), pos);
    expr r = p.mk_app(b, proof, pos);
    return p.save_pos(mk_show_annotation(r), pos);
}

static expr parse_suffices(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    auto prop_pos = p.pos();
    name id;
    expr from;
    if (p.curr_is_identifier()) {
        id = p.get_name_val();
        p.next();
        if (p.curr_is_token(get_colon_tk())) {
            p.next();
            from = p.parse_expr();
        } else {
            expr left = p.id_to_expr(id, prop_pos);
            id        = get_this_tk();
            unsigned rbp = 0;
            while (rbp < p.curr_lbp()) {
                left = p.parse_led(left);
            }
            from = left;
        }
    } else {
        id    = get_this_tk();
        from  = p.parse_expr();
    }
    expr local = p.save_pos(mk_local(id, from), prop_pos);
    p.check_token_next(get_comma_tk(), "invalid 'suffices' declaration, ',' expected");
    expr body;
    {
        parser_state::local_scope scope(p);
        p.add_local(local);
        body = parse_proof(p);
    }
    expr proof = p.save_pos(Fun(local, body, p), pos);
    p.check_token_next(get_comma_tk(), "invalid 'suffices' declaration, ',' expected");
    expr rest  = p.parse_expr();
    expr r = p.mk_app(proof, rest, pos);
    return p.save_pos(mk_suffices_annotation(r), pos);
}

static expr * g_not  = nullptr;
static unsigned g_then_else_prec = 0;

static expr parse_ite(parser_state & p, expr const & c, pos_info const & pos) {
    if (!p.env().find(get_ite_name()))
        throw parser_error("invalid use of 'if-then-else' expression, environment does not contain 'ite' definition", pos);
    p.check_token_next(get_then_tk(), "invalid 'if-then-else' expression, 'then' expected");
    expr t = p.parse_expr(g_then_else_prec);
    p.check_token_next(get_else_tk(), "invalid 'if-then-else' expression, 'else' expected");
    expr e = p.parse_expr(g_then_else_prec);
    return p.save_pos(mk_app(mk_constant(get_ite_name()), c, t, e), pos);
}

static expr parse_dite(parser_state & p, name const & H_name, expr const & c, pos_info const & pos) {
    p.check_token_next(get_then_tk(), "invalid 'if-then-else' expression, 'then' expected");
    expr t, e;
    {
        parser_state::local_scope scope(p);
        expr H = mk_local(H_name, c);
        p.add_local(H);
        auto pos = p.pos();
        t = p.save_pos(Fun(H, p.parse_expr(g_then_else_prec), p), pos);
    }
    p.check_token_next(get_else_tk(), "invalid 'if-then-else' expression, 'else' expected");
    {
        parser_state::local_scope scope(p);
        expr H = mk_local(H_name, mk_app(*g_not, c));
        p.add_local(H);
        auto pos = p.pos();
        e = p.save_pos(Fun(H, p.parse_expr(g_then_else_prec), p), pos);
    }
    return p.save_pos(mk_app(p.save_pos(mk_constant(get_dite_name()), pos), c, t, e), pos);
}

static expr parse_if_then_else(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    pair<optional<name>, expr> ie = p.parse_qualified_expr();
    if (ie.first)
        return parse_dite(p, *ie.first, ie.second, pos);
    else
        return parse_ite(p, ie.second, pos);
}

static expr parse_calc_expr(parser_state & p, unsigned, expr const *, pos_info const &) {
    return parse_calc(p);
}

static expr parse_explicit_core(parser_state & p, pos_info const & pos, bool partial) {
    if (!p.curr_is_identifier())
        throw parser_error(sstream() << "invalid '" << (partial ? "@@" : "@") << "', identifier expected", p.pos());
    expr fn = p.parse_id(/* allow_field_notation */ false);
    if (is_choice(fn)) {
        sstream s;
        s << "invalid '" << (partial ? "@@" : "@") << "', function is overloaded, use fully qualified names (overloads: ";
        for (unsigned i = 0; i < get_num_choices(fn); i++) {
            if (i > 0) s << ", ";
            expr const & c = get_choice(fn, i);
            if (is_constant(c))
                s << const_name(c);
            else if (is_local(c))
                s << local_pp_name(c);
            else
                s << "[other]";
        }
        s << ")";
        throw parser_error(s, pos);
    } else if (!is_as_atomic(fn) && !is_constant(fn) && !is_local(fn)) {
        throw parser_error(sstream() << "invalid '" << (partial ? "@@" : "@") << "', function must be a constant or variable", pos);
    }
    if (partial)
        return p.save_pos(mk_partial_explicit(fn), pos);
    else
        return p.save_pos(mk_explicit(fn), pos);
}

static expr parse_explicit_expr(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    return parse_explicit_core(p, pos, false);
}

static expr parse_partial_explicit_expr(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    return parse_explicit_core(p, pos, true);
}

static expr parse_sorry(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    return p.mk_sorry(pos);
}

static expr parse_pattern(parser_state & p, unsigned, expr const * args, pos_info const & pos) {
    return p.save_pos(mk_pattern_hint(args[0]), pos);
}

static expr parse_lazy_quoted_pexpr(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    if (p.in_quote())
        throw parser_error("invalid nested quoted expression", pos);
    parser_state::quote_scope scope(p, true);
    expr e = p.parse_expr();
    if (p.curr_is_token(get_colon_tk())) {
        p.next();
        expr t = p.parse_expr();
        e = mk_typed_expr_distrib_choice(p, t, e, pos);
    }
    p.check_token_next(get_rparen_tk(), "invalid quoted expression, `)` expected");
    return p.save_pos(mk_pexpr_quote(e), pos);
}

static expr parse_quoted_pexpr(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    if (p.in_quote())
        throw parser_error("invalid nested quoted expression", pos);
    parser_state::quote_scope scope(p, true, id_behavior::ErrorIfUndef);
    expr e = p.parse_expr();
    if (p.curr_is_token(get_colon_tk())) {
        p.next();
        expr t = p.parse_expr();
        e = mk_typed_expr_distrib_choice(p, t, e, pos);
    }
    p.check_token_next(get_rparen_tk(), "invalid quoted expression, `)` expected");
    return p.save_pos(mk_pexpr_quote(e), pos);
}

static expr parse_quoted_expr(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    if (p.in_quote())
        throw parser_error("invalid nested quoted expression", pos);
    expr e;
    {
        parser_state::quote_scope scope(p, true, id_behavior::ErrorIfUndef);
        e = p.parse_expr();
        if (p.curr_is_token(get_colon_tk())) {
            p.next();
            expr t = p.parse_expr();
            e = mk_typed_expr_distrib_choice(p, t, e, pos);
        }
        p.check_token_next(get_rparen_tk(), "invalid quoted expression, `)` expected");
    }
    e = mk_quote_core(e, true);
    if (!p.in_pattern()) {
        e = elaborate_quote(e, p.env(), p.get_options(), /* in_pattern */ false);
        // note: when `p.in_pattern()`, quote will be elaborated in `parser::patexpr_to_{pattern,expr}`
    }
    return p.save_pos(e, pos);
}

static expr parse_antiquote_expr(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    if (!p.in_quote())
        throw parser_error("invalid antiquotation, occurs outside of quoted expressions", pos);
    parser_state::quote_scope scope(p, false);
    expr e = p.parse_expr(get_max_prec());
    return p.save_pos(mk_antiquote(e), pos);
}

static expr parse_quoted_name(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    bool resolve = false;
    name id;
    if (p.curr_is_token(get_placeholder_tk())) {
        p.next();
        id = "_";
    } else {
        if (p.curr_is_token(get_backtick_tk())) {
            p.next();
            resolve = true;
        }
        if (p.curr_is_keyword() || p.curr_is_command()) {
            if (resolve)
                throw parser_error("invalid resolved quote symbol, identifier is a keyword/command", pos);
            id = p.get_token_info().token();
            p.next();
        } else {
            id = p.check_id_next("invalid quoted name, identifier expected");
        }
    }
    if (resolve) {
        parser::error_if_undef_scope scope(p);
        bool resolve_only = true;
        expr e = p.id_to_expr(id, pos, resolve_only);
        if (is_constant(e)) {
            id = const_name(e);
        } else if (is_local(e)) {
            id = local_pp_name(e);
        } else if (is_choice(e)) {
            sstream ss;
            ss << "invalid resolved quoted symbol, it is ambiguous, possible interpretations:";
            for (unsigned i = 0; i < get_num_choices(e); i++)
                ss << " " << get_choice(e, i);
            ss << " (solution: use fully qualified names)";
            throw parser_error(ss, pos);
        } else {
            throw parser_error("invalid quoted symbol, failed to resolve it "
                               "(solution: use `<identifier> to bypass name resolution)", pos);
        }
    }
    lean_assert(id.is_string());
    expr e  = quote(id);
    return p.rec_save_pos(e, pos);
}

static name * g_anonymous_constructor = nullptr;

expr mk_anonymous_constructor(expr const & e) { return mk_annotation(*g_anonymous_constructor, e); }

bool is_anonymous_constructor(expr const & e) { return is_annotation(e, *g_anonymous_constructor); }

expr const & get_anonymous_constructor_arg(expr const & e) {
    lean_assert(is_anonymous_constructor(e));
    return get_annotation_arg(e);
}

static expr parse_constructor_core(parser_state & p, pos_info const & pos) {
    buffer<expr> args;
    while (!p.curr_is_token(get_rangle_tk())) {
        args.push_back(p.parse_expr());
        if (p.curr_is_token(get_comma_tk())) {
            p.next();
        } else {
            break;
        }
    }
    p.check_token_next(get_rangle_tk(), "invalid constructor, `⟩` expected");
    expr fn = p.save_pos(mk_expr_placeholder(), pos);
    return p.save_pos(mk_anonymous_constructor(p.save_pos(mk_app(fn, args), pos)), pos);
}

static expr parse_constructor(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    return parse_constructor_core(p, pos);
}

static expr parse_lambda_core(parser_state & p, pos_info const & pos);

static expr parse_lambda_binder(parser_state & p, pos_info const & pos) {
    parser_state::local_scope scope1(p);
    buffer<expr> locals;
    auto new_env = p.parse_binders(locals, 0);
    for (expr const & local : locals)
        p.add_local(local);
    parser_state::local_scope scope2(p, new_env);
    expr body;
    if (p.curr_is_token(get_comma_tk())) {
        p.next();
        body = p.parse_expr();
    } else if (p.curr_is_token(get_langle_tk())) {
        body = parse_lambda_core(p, pos);
    } else {
        throw parser_error("invalid lambda expression, ',' or '⟨' expected", p.pos());
    }
    bool use_cache = false;
    return p.rec_save_pos(Fun(locals, body, use_cache), pos);
}

static name * g_lambda_match_name = nullptr;

static expr parse_lambda_constructor(parser_state & p, pos_info const & ini_pos) {
    lean_assert(p.curr_is_token(get_langle_tk()));
    parser_state::local_scope scope(p);
    auto pos = p.pos();
    p.next();
    buffer<expr> locals;
    expr pattern = p.parse_pattern([&](parser_state & p) { return parse_constructor_core(p, pos); }, locals);
    for (expr const & local : locals)
        p.add_local(local);
    expr body;
    if (p.curr_is_token(get_comma_tk())) {
        p.next();
        body = p.parse_expr();
    } else {
        body = parse_lambda_core(p, ini_pos);
    }
    match_definition_scope match_scope;
    expr fn  = p.save_pos(mk_local(mk_fresh_name(), *g_lambda_match_name, mk_expr_placeholder(), mk_rec_info(true)), pos);
    expr eqn = Fun(fn, Fun(locals, p.save_pos(mk_equation(p.rec_save_pos(mk_app(fn, pattern), pos), body), pos), p), p);
    equations_header h = mk_equations_header(match_scope.get_name());
    expr x = p.rec_save_pos(mk_local(mk_fresh_name(), "_x", mk_expr_placeholder(), binder_info()), pos);
    bool use_cache = false;
    return p.rec_save_pos(Fun(x, mk_app(mk_equations(h, 1, &eqn), x), use_cache), pos);
}

static expr parse_lambda_core(parser_state & p, pos_info const & pos) {
    if (p.curr_is_token(get_langle_tk())) {
        return parse_lambda_constructor(p, pos);
    } else {
        return parse_lambda_binder(p, pos);
    }
}

static expr parse_lambda(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    return parse_lambda_core(p, pos);
}

static void consume_rparen(parser_state & p) {
    p.check_token_next(get_rparen_tk(), "invalid expression, `)` expected");
}

static name * g_infix_function = nullptr;

bool is_infix_function(expr const & e) {
    return is_annotation(e, *g_infix_function);
}

expr mk_infix_function(expr const & e) {
    return mk_annotation(*g_infix_function, e);
}

/* Check whether notation such as (<) (+ 10) is applicable.
   Like Haskell,
     (<) is notation for (fun x y, x < y)
     (+ 10) is notation for (fun x, x + 10)

   This kind of notation is applicable when
   1- The current token is a keyword.
   2- There is no entry for this token in the nud table
   3- There is an entry for this token in the led table,
      and exactly one Expr transition to an accepting state.

   If the notation is applicable, this function returns the accepting list.
*/
static list<notation::accepting> is_infix_paren_notation(parser_state & p) {
    if (p.curr_is_keyword() &&
        !p.nud().find(p.get_token_info().value())) {
        list<pair<transition, parse_table>> ts = p.led().find(p.get_token_info().value());
        if (ts && !tail(ts) && head(ts).second.is_accepting()) {
            notation::action const & act = head(ts).first.get_action();
            if (act.kind() == notation::action_kind::Expr) {
                return head(ts).second.is_accepting();
            }
        }
    }
    return list<notation::accepting>();
}

static expr parse_infix_paren(parser_state & p, list<notation::accepting> const & accs, pos_info const & pos) {
    expr args[2];
    buffer<expr> vars;
    args[0] = mk_local(mk_fresh_name(), "_x", mk_expr_placeholder(), binder_info());
    vars.push_back(args[0]);
    p.next();
    if (p.curr_is_token(get_rparen_tk())) {
        p.next();
        args[1] = mk_local(mk_fresh_name(), "_y", mk_expr_placeholder(), binder_info());
        vars.push_back(args[1]);
    } else {
        args[1] = p.parse_expr();
        consume_rparen(p);
    }
    buffer<expr> cs;
    for (notation::accepting const & acc : accs) {
        expr r = p.copy_with_new_pos(acc.get_expr(), pos);
        r = p.save_pos(mk_infix_function(Fun(vars, instantiate_rev(r, 2, args), p)), pos);
        cs.push_back(r);
    }
    return p.save_pos(mk_choice(cs.size(), cs.data()), pos);
}

static expr parse_lparen(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    if (auto accs = is_infix_paren_notation(p))
        return parse_infix_paren(p, accs, pos);
    expr e = p.parse_expr();
    if (p.curr_is_token(get_comma_tk())) {
        buffer<expr> args;
        args.push_back(e);
        while (p.curr_is_token(get_comma_tk())) {
            p.next();
            args.push_back(p.parse_expr());
        }
        consume_rparen(p);
        expr r = args.back();
        unsigned i = args.size() - 1;
        while (i > 0) {
            --i;
            r = p.save_pos(mk_app(p.save_pos(mk_constant(get_prod_mk_name()), pos),
                                  args[i], r), pos);
        }
        return r;
    } else if (p.curr_is_token(get_colon_tk())) {
        p.next();
        expr t = p.parse_expr();
        consume_rparen(p);
        return mk_typed_expr_distrib_choice(p, t, e, pos);
    } else {
        consume_rparen(p);
        return e;
    }
}

static expr parse_lambda_cons(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    list<pair<transition, parse_table>> ts = p.led().find(get_dcolon_tk());
    if (!ts || tail(ts) || !head(ts).second.is_accepting())
        throw parser_error("invalid '(::)' notation, infix operator '::' has not been defined yet or is the prefix of another notation declaration", pos);
    notation::action const & act = head(ts).first.get_action();
    if (act.kind() != notation::action_kind::Expr)
        throw parser_error("invalid '(::)' notation, declaration for operator '::' is not compatible with the `(::)` syntactic sugar", pos);
    expr args[2];
    buffer<expr> vars;
    args[0] = mk_local(mk_fresh_name(), "_x", mk_expr_placeholder(), binder_info());
    vars.push_back(args[0]);
    args[1] = mk_local(mk_fresh_name(), "_y", mk_expr_placeholder(), binder_info());
    vars.push_back(args[1]);
    buffer<expr> cs;
    for (notation::accepting const & acc : head(ts).second.is_accepting()) {
        expr r = p.copy_with_new_pos(acc.get_expr(), pos);
        r = p.save_pos(mk_infix_function(Fun(vars, instantiate_rev(r, 2, args), p)), pos);
        cs.push_back(r);
    }
    return p.save_pos(mk_choice(cs.size(), cs.data()), pos);
}

static expr parse_inaccessible(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    if (!p.in_pattern())
        throw parser_error("inaccesible pattern notation `.(t)` can only be used in patterns", pos);
    expr e = p.parse_expr();
    p.check_token_next(get_rparen_tk(), "invalid inaccesible pattern, `)` expected");
    return p.save_pos(mk_inaccessible(e), pos);
}

static expr parse_atomic_inaccessible(parser_state & p, unsigned, expr const *, pos_info const & pos) {
    if (!p.in_pattern())
        throw parser_error("inaccesible pattern notation `._` can only be used in patterns", pos);
    return p.save_pos(mk_inaccessible(p.save_pos(mk_expr_placeholder(), pos)), pos);
}

parse_table init_nud_table() {
    action Expr(mk_expr_action());
    action Skip(mk_skip_action());
    action Binders(mk_binders_action());
    expr x0 = mk_var(0);
    parse_table r;
    r = r.add({transition("by", mk_ext_action_core(parse_by))}, x0);
    r = r.add({transition("have", mk_ext_action(parse_have))}, x0);
    r = r.add({transition("suppose", mk_ext_action(parse_suppose))}, x0);
    r = r.add({transition("show", mk_ext_action(parse_show))}, x0);
    r = r.add({transition("suffices", mk_ext_action(parse_suffices))}, x0);
    r = r.add({transition("if", mk_ext_action(parse_if_then_else))}, x0);
    r = r.add({transition("(", mk_ext_action(parse_lparen))}, x0);
    r = r.add({transition("⟨", mk_ext_action(parse_constructor))}, x0);
    r = r.add({transition("{", mk_ext_action(parse_curly_bracket))}, x0);
    r = r.add({transition(".(", mk_ext_action(parse_inaccessible))}, x0);
    r = r.add({transition("._", mk_ext_action(parse_atomic_inaccessible))}, x0);
    r = r.add({transition("`(", mk_ext_action(parse_lazy_quoted_pexpr))}, x0);
    r = r.add({transition("``(", mk_ext_action(parse_quoted_pexpr))}, x0);
    r = r.add({transition("```(", mk_ext_action(parse_quoted_expr))}, x0);
    r = r.add({transition("`[", mk_ext_action(parse_interactive_tactic_block))}, x0);
    r = r.add({transition("`", mk_ext_action(parse_quoted_name))}, x0);
    r = r.add({transition("%%", mk_ext_action(parse_antiquote_expr))}, x0);
    r = r.add({transition("(:", Expr), transition(":)", mk_ext_action(parse_pattern))}, x0);
    r = r.add({transition("()", mk_ext_action(parse_unit))}, x0);
    r = r.add({transition("(::)", mk_ext_action(parse_lambda_cons))}, x0);
    r = r.add({transition("fun", mk_ext_action(parse_lambda))}, x0);
    r = r.add({transition("Pi", Binders), transition(",", mk_scoped_expr_action(x0, 0, false))}, x0);
    r = r.add({transition("Type", mk_ext_action(parse_Type))}, x0);
    r = r.add({transition("Type*", mk_ext_action(parse_Type_star))}, x0);
    r = r.add({transition("Sort", mk_ext_action(parse_Sort))}, x0);
    r = r.add({transition("Sort*", mk_ext_action(parse_Sort_star))}, x0);
    r = r.add({transition("let", mk_ext_action(parse_let_expr))}, x0);
    r = r.add({transition("calc", mk_ext_action(parse_calc_expr))}, x0);
    r = r.add({transition("@", mk_ext_action(parse_explicit_expr))}, x0);
    r = r.add({transition("@@", mk_ext_action(parse_partial_explicit_expr))}, x0);
    r = r.add({transition("begin", mk_ext_action_core(parse_begin_end))}, x0);
    r = r.add({transition("sorry", mk_ext_action(parse_sorry))}, x0);
    r = r.add({transition("match", mk_ext_action(parse_match))}, x0);
    r = r.add({transition("do", mk_ext_action(parse_do_expr))}, x0);
    return r;
}

static expr parse_field(parser_state & p, unsigned, expr const * args, pos_info const & pos) {
    try {
        if (p.curr_is_numeral()) {
            pos_info num_pos = p.pos();
            unsigned fidx = p.parse_small_nat();
            if (fidx == 0)
                throw parser_error("invalid projection, index must be greater than 0", num_pos);
            return p.save_pos(mk_field_notation(args[0], fidx), pos);
        } else {
            name field = p.check_id_next("identifier or numeral expected");
            return p.save_pos(mk_field_notation(args[0], field), pos);
        }
    } catch (break_at_pos_exception & ex) {
        if (!p.get_complete()) {
            // info is stored at position of notation token
            ex.m_token_info.m_pos = pos;
        }
        expr lhs = args[0];
        expr lhs_type;
        try {
            metavar_context mctx;
            bool check_unassigned = false;
            lhs = p.elaborate({}, mctx, lhs, check_unassigned).first;
            type_checker tc(p.env(), true, false);
            lhs_type = tc.infer(lhs);
        } catch (exception &) {
            /* failed to elaborate or infer type */
            throw ex;
        }
        expr fn = get_app_fn(lhs_type);
        if (is_constant(fn)) {
            ex.m_token_info.m_context = break_at_pos_exception::token_context::field;
            ex.m_token_info.m_param   = const_name(fn);
        }
        throw;
    }
}

parse_table init_led_table() {
    parse_table r(false);
    r = r.add({transition("->", mk_expr_action(get_arrow_prec()-1))},    mk_arrow(Var(1), Var(1)));
    r = r.add({transition("^.", mk_ext_action(parse_field))}, Var(0));
    return r;
}

static parse_table * g_nud_table = nullptr;
static parse_table * g_led_table = nullptr;

parse_table get_builtin_nud_table() {
    return *g_nud_table;
}

parse_table get_builtin_led_table() {
    return *g_led_table;
}

void initialize_builtin_exprs() {
    g_no_universe_annotation = new name("no_univ");
    register_annotation(*g_no_universe_annotation);

    g_do_failure_eq     = new name("do_failure_eq");
    register_annotation(*g_do_failure_eq);

    g_infix_function    = new name("infix_fn");
    register_annotation(*g_infix_function);

    g_not               = new expr(mk_constant(get_not_name()));
    g_nud_table         = new parse_table();
    *g_nud_table        = init_nud_table();
    g_led_table         = new parse_table();
    *g_led_table        = init_led_table();
    g_do_match_name     = new name("_do_match");
    g_let_match_name    = new name("_let_match");
    g_lambda_match_name = new name("_fun_match");

    g_parser_checkpoint_have = new name{"parser", "checkpoint_have"};
    register_bool_option(*g_parser_checkpoint_have, LEAN_DEFAULT_PARSER_CHECKPOINT_HAVE,
                         "(parser) introduces a checkpoint on have-expressions, checkpoints are like Prolog-cuts");

    g_anonymous_constructor = new name("anonymous_constructor");
    register_annotation(*g_anonymous_constructor);
}

void finalize_builtin_exprs() {
    delete g_do_failure_eq;
    delete g_infix_function;
    delete g_led_table;
    delete g_nud_table;
    delete g_not;
    delete g_do_match_name;
    delete g_let_match_name;
    delete g_lambda_match_name;
    delete g_parser_checkpoint_have;
    delete g_anonymous_constructor;
    delete g_no_universe_annotation;
}
}
