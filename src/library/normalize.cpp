/*
Copyright (c) 2014 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include <string>
#include "util/interrupt.h"
#include "util/name_generator.h"
#include "kernel/type_checker.h"
#include "kernel/instantiate.h"
#include "kernel/abstract.h"
#include "kernel/free_vars.h"
#include "kernel/inductive/inductive.h"
#include "library/reducible.h"
#include "library/util.h"
#include "library/scoped_ext.h"
#include "library/kernel_serializer.h"

namespace lean {
/**
   \brief unfold hints instruct the normalizer (and simplifier) that
   a function application. We have two kinds of hints:
   - unfold_c (f a_1 ... a_i ... a_n) should be unfolded
     when argument a_i is a constructor.
   - unfold_f (f a_1 ... a_i ... a_n) should be unfolded when it is fully applied.
*/
struct unfold_hint_entry {
    bool     m_unfold_c; //!< true if it is an unfold_c hint
    bool     m_add;      //!< add/remove hint
    name     m_decl_name;
    unsigned m_arg_idx;
    unfold_hint_entry():m_unfold_c(false), m_add(false), m_arg_idx(0) {}
    unfold_hint_entry(bool unfold_c, bool add, name const & n, unsigned idx):
        m_unfold_c(unfold_c), m_add(add), m_decl_name(n), m_arg_idx(idx) {}
};

unfold_hint_entry mk_add_unfold_c_entry(name const & n, unsigned idx) { return unfold_hint_entry(true, true, n, idx); }
unfold_hint_entry mk_erase_unfold_c_entry(name const & n) { return unfold_hint_entry(true, false, n, 0); }
unfold_hint_entry mk_add_unfold_f_entry(name const & n) { return unfold_hint_entry(false, true, n, 0); }
unfold_hint_entry mk_erase_unfold_f_entry(name const & n) { return unfold_hint_entry(false, false, n, 0); }

static name * g_unfold_hint_name = nullptr;
static std::string * g_key = nullptr;

struct unfold_hint_state {
    name_map<unsigned>  m_unfold_c;
    name_set            m_unfold_f;
};

struct unfold_hint_config {
    typedef unfold_hint_state state;
    typedef unfold_hint_entry entry;

    static void add_entry(environment const &, io_state const &, state & s, entry const & e) {
        if (e.m_unfold_c) {
            if (e.m_add)
                s.m_unfold_c.insert(e.m_decl_name, e.m_arg_idx);
            else
                s.m_unfold_c.erase(e.m_decl_name);
        } else {
            if (e.m_add)
                s.m_unfold_f.insert(e.m_decl_name);
            else
                s.m_unfold_f.erase(e.m_decl_name);
        }
    }
    static name const & get_class_name() {
        return *g_unfold_hint_name;
    }
    static std::string const & get_serialization_key() {
        return *g_key;
    }
    static void  write_entry(serializer & s, entry const & e) {
        s << e.m_unfold_c << e.m_add << e.m_decl_name << e.m_arg_idx;
    }
    static entry read_entry(deserializer & d) {
        entry e;
        d >> e.m_unfold_c >> e.m_add >> e.m_decl_name >> e.m_arg_idx;
        return e;
    }
    static optional<unsigned> get_fingerprint(entry const & e) {
        return some(e.m_decl_name.hash());
    }
};

template class scoped_ext<unfold_hint_config>;
typedef scoped_ext<unfold_hint_config> unfold_hint_ext;

environment add_unfold_c_hint(environment const & env, name const & n, unsigned idx, bool persistent) {
    declaration const & d = env.get(n);
    if (!d.is_definition() || d.is_opaque())
        throw exception("invalid unfold-c hint, declaration must be a non-opaque definition");
    return unfold_hint_ext::add_entry(env, get_dummy_ios(), mk_add_unfold_c_entry(n, idx), persistent);
}

optional<unsigned> has_unfold_c_hint(environment const & env, name const & d) {
    unfold_hint_state const & s = unfold_hint_ext::get_state(env);
    if (auto it = s.m_unfold_c.find(d))
        return optional<unsigned>(*it);
    else
        return optional<unsigned>();
}

environment erase_unfold_c_hint(environment const & env, name const & n, bool persistent) {
    return unfold_hint_ext::add_entry(env, get_dummy_ios(), mk_erase_unfold_c_entry(n), persistent);
}

environment add_unfold_f_hint(environment const & env, name const & n, bool persistent) {
    declaration const & d = env.get(n);
    if (!d.is_definition() || d.is_opaque())
        throw exception("invalid unfold-f hint, declaration must be a non-opaque definition");
    return unfold_hint_ext::add_entry(env, get_dummy_ios(), mk_add_unfold_f_entry(n), persistent);
}

bool has_unfold_f_hint(environment const & env, name const & d) {
    unfold_hint_state const & s = unfold_hint_ext::get_state(env);
    return s.m_unfold_f.contains(d);
}

environment erase_unfold_f_hint(environment const & env, name const & n, bool persistent) {
    return unfold_hint_ext::add_entry(env, get_dummy_ios(), mk_erase_unfold_f_entry(n), persistent);
}

void initialize_normalize() {
    g_unfold_hint_name = new name("unfold-hints");
    g_key = new std::string("unfoldh");
    unfold_hint_ext::initialize();
}

void finalize_normalize() {
    unfold_hint_ext::finalize();
    delete g_unfold_hint_name;
    delete g_key;
}

class normalize_fn {
    type_checker   &                  m_tc;
    name_generator                    m_ngen;
    std::function<bool(expr const &)> m_pred;  // NOLINT
    bool                              m_save_cnstrs;
    constraint_seq                    m_cnstrs;
    bool                              m_use_eta;

    expr normalize_binding(expr const & e) {
        expr d = normalize(binding_domain(e));
        expr l = mk_local(m_ngen.next(), binding_name(e), d, binding_info(e));
        expr b = abstract(normalize(instantiate(binding_body(e), l)), l);
        return update_binding(e, d, b);
    }

    optional<unsigned> has_unfold_c_hint(expr const & f) {
        if (!is_constant(f))
            return optional<unsigned>();
        return ::lean::has_unfold_c_hint(m_tc.env(), const_name(f));
    }

    bool has_unfold_f_hint(expr const & f) {
        return is_constant(f) &&  ::lean::has_unfold_f_hint(m_tc.env(), const_name(f));
    }

    expr normalize_app(expr const & e) {
        buffer<expr> args;
        bool modified = false;
        expr f = get_app_rev_args(e, args);
        for (expr & a : args) {
            expr new_a = normalize(a);
            if (new_a != a)
                modified = true;
            a = new_a;
        }
        if (has_unfold_f_hint(f)) {
            if (!is_pi(m_tc.whnf(m_tc.infer(e).first).first)) {
                if (optional<expr> r = unfold_app(m_tc.env(), mk_rev_app(f, args)))
                    return normalize(*r);
            }
        }
        if (auto idx = has_unfold_c_hint(f)) {
            if (*idx < args.size() && is_constructor_app(m_tc.env(), args[args.size() - *idx - 1])) {
                if (optional<expr> r = unfold_app(m_tc.env(), mk_rev_app(f, args)))
                    return normalize(*r);
            }
        }
        if (!modified)
            return e;
        expr r = mk_rev_app(f, args);
        if (is_constant(f) && inductive::is_elim_rule(m_tc.env(), const_name(f))) {
            return normalize(r);
        } else {
            return r;
        }
    }

    expr try_eta(expr const & e) {
        lean_assert(is_lambda(e));
        expr const & b = binding_body(e);
        if (is_lambda(b)) {
            expr new_b = try_eta(b);
            if (is_eqp(b, new_b)) {
                return e;
            } else if (is_app(new_b) && is_var(app_arg(new_b), 0) && !has_free_var(app_fn(new_b), 0)) {
                return lower_free_vars(app_fn(new_b), 1);
            } else {
                return update_binding(e, binding_domain(e), new_b);
            }
        } else if (is_app(b) && is_var(app_arg(b), 0) && !has_free_var(app_fn(b), 0)) {
            return lower_free_vars(app_fn(b), 1);
        } else {
            return e;
        }
    }

    expr normalize(expr e) {
        check_system("normalize");
        if (!m_pred(e))
            return e;
        auto w = m_tc.whnf(e);
        e = w.first;
        if (m_save_cnstrs)
            m_cnstrs += w.second;
        switch (e.kind()) {
        case expr_kind::Var:  case expr_kind::Constant: case expr_kind::Sort:
        case expr_kind::Meta: case expr_kind::Local: case expr_kind::Macro:
            return e;
        case expr_kind::Lambda: {
            e = normalize_binding(e);
            if (m_use_eta)
                return try_eta(e);
            else
                return e;
        }
        case expr_kind::Pi:
            return normalize_binding(e);
        case expr_kind::App:
            return normalize_app(e);
        }
        lean_unreachable(); // LCOV_EXCL_LINE
    }

public:
    normalize_fn(type_checker & tc, bool save, bool eta):
        m_tc(tc), m_ngen(m_tc.mk_ngen()),
        m_pred([](expr const &) { return true; }),
        m_save_cnstrs(save), m_use_eta(eta) {}

    normalize_fn(type_checker & tc, std::function<bool(expr const &)> const & fn, bool eta): // NOLINT
        m_tc(tc), m_ngen(m_tc.mk_ngen()),
        m_pred(fn), m_save_cnstrs(true), m_use_eta(eta) {}

    expr operator()(expr const & e) {
        m_cnstrs = constraint_seq();
        return normalize(e);
    }

    expr operator()(level_param_names const & ls, expr const & e) {
        m_cnstrs = constraint_seq();
        return m_tc.with_params(ls, [&]() {
                return normalize(e);
            });
    }

    constraint_seq get_cnstrs() const { return m_cnstrs; }
};

expr normalize(environment const & env, expr const & e, bool eta) {
    auto tc          = mk_type_checker(env, true);
    bool save_cnstrs = false;
    return normalize_fn(*tc, save_cnstrs, eta)(e);
}

expr normalize(environment const & env, level_param_names const & ls, expr const & e, bool eta) {
    auto tc          = mk_type_checker(env, true);
    bool save_cnstrs = false;
    return normalize_fn(*tc, save_cnstrs, eta)(ls, e);
}

expr normalize(type_checker & tc, expr const & e, bool eta) {
    bool save_cnstrs = false;
    return normalize_fn(tc, save_cnstrs, eta)(e);
}

expr normalize(type_checker & tc, level_param_names const & ls, expr const & e, bool eta) {
    bool save_cnstrs = false;
    return normalize_fn(tc, save_cnstrs, eta)(ls, e);
}

expr normalize(type_checker & tc, expr const & e, constraint_seq & cs, bool eta) {
    bool save_cnstrs = false;
    normalize_fn fn(tc, save_cnstrs, eta);
    expr r = fn(e);
    cs += fn.get_cnstrs();
    return r;
}

expr normalize(type_checker & tc, expr const & e, std::function<bool(expr const &)> const & pred, // NOLINT
               constraint_seq & cs, bool eta) {
    normalize_fn fn(tc, pred, eta);
    expr r = fn(e);
    cs += fn.get_cnstrs();
    return r;
}
}
