// Copyright 2020, 2021, 2022, 2023 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <numeric>
#include <ostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <boost/container/container_fwd.hpp>
#include <boost/container_hash/hash.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include <boost/safe_numerics/safe_integer.hpp>

#include <fmt/core.h>
#include <fmt/ranges.h>

#include <oneapi/tbb/parallel_sort.h>

#include <heyoka/config.hpp>
#include <heyoka/detail/fast_unordered.hpp>
#include <heyoka/detail/func_cache.hpp>
#include <heyoka/detail/logging_impl.hpp>
#include <heyoka/detail/string_conv.hpp>
#include <heyoka/detail/type_traits.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/func.hpp>
#include <heyoka/math/sum.hpp>
#include <heyoka/number.hpp>
#include <heyoka/param.hpp>
#include <heyoka/s11n.hpp>
#include <heyoka/variable.hpp>

HEYOKA_BEGIN_NAMESPACE

namespace detail
{

expression diff(funcptr_map<expression> &func_map, const expression &e, const std::string &s)
{
    return std::visit(
        [&func_map, &s](const auto &arg) {
            using type = uncvref_t<decltype(arg)>;

            if constexpr (std::is_same_v<type, number>) {
                return std::visit(
                    [](const auto &v) { return expression{number{static_cast<uncvref_t<decltype(v)>>(0)}}; },
                    arg.value());
            } else if constexpr (std::is_same_v<type, param>) {
                return 0_dbl;
            } else if constexpr (std::is_same_v<type, variable>) {
                if (s == arg.name()) {
                    return 1_dbl;
                } else {
                    return 0_dbl;
                }
            } else {
                const auto f_id = arg.get_ptr();

                if (auto it = func_map.find(f_id); it != func_map.end()) {
                    // We already performed diff on the current function,
                    // fetch the result from the cache.
                    return it->second;
                }

                auto ret = arg.diff(func_map, s);

                // Put the return value in the cache.
                [[maybe_unused]] const auto [_, flag] = func_map.emplace(f_id, ret);
                // NOTE: an expression cannot contain itself.
                assert(flag);

                return ret;
            }
        },
        e.value());
}

expression diff(funcptr_map<expression> &func_map, const expression &e, const param &p)
{
    return std::visit(
        [&func_map, &p](const auto &arg) {
            using type = uncvref_t<decltype(arg)>;

            if constexpr (std::is_same_v<type, number>) {
                return std::visit(
                    [](const auto &v) { return expression{number{static_cast<uncvref_t<decltype(v)>>(0)}}; },
                    arg.value());
            } else if constexpr (std::is_same_v<type, param>) {
                if (p.idx() == arg.idx()) {
                    return 1_dbl;
                } else {
                    return 0_dbl;
                }
            } else if constexpr (std::is_same_v<type, variable>) {
                return 0_dbl;
            } else {
                const auto f_id = arg.get_ptr();

                if (auto it = func_map.find(f_id); it != func_map.end()) {
                    // We already performed diff on the current function,
                    // fetch the result from the cache.
                    return it->second;
                }

                auto ret = arg.diff(func_map, p);

                // Put the return value in the cache.
                [[maybe_unused]] const auto [_, flag] = func_map.emplace(f_id, ret);
                // NOTE: an expression cannot contain itself.
                assert(flag);

                return ret;
            }
        },
        e.value());
}

} // namespace detail

expression diff(const expression &e, const std::string &s)
{
    detail::funcptr_map<expression> func_map;

    return detail::diff(func_map, e, s);
}

expression diff(const expression &e, const param &p)
{
    detail::funcptr_map<expression> func_map;

    return detail::diff(func_map, e, p);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
expression diff(const expression &e, const expression &x)
{
    return std::visit(
        [&e](const auto &v) -> expression {
            if constexpr (std::is_same_v<detail::uncvref_t<decltype(v)>, variable>) {
                return diff(e, v.name());
            } else if constexpr (std::is_same_v<detail::uncvref_t<decltype(v)>, param>) {
                return diff(e, v);
            } else {
                throw std::invalid_argument(
                    "Derivatives are currently supported only with respect to variables and parameters");
            }
        },
        x.value());
}

namespace detail
{

// Function decomposition for symbolic differentiation.
std::pair<std::vector<expression>, std::vector<expression>::size_type>
diff_decompose(const std::vector<expression> &v_ex_)
{
    // Determine the list of variables and params.
    const auto vars = get_variables(v_ex_);
    const auto nvars = vars.size();

    const auto params = get_params(v_ex_);
    const auto npars = params.size();

    // Cache the number of outputs.
    const auto nouts = v_ex_.size();
    assert(nouts > 0u);

    // Create the map for renaming variables and params to u_i.
    // The variables will precede the params. The renaming will be
    // done in alphabetical order for the variables and in index order
    // for the params.
    std::unordered_map<expression, expression> repl_map;
    {
        boost::safe_numerics::safe<std::size_t> u_idx = 0;

        for (const auto &var : vars) {
            [[maybe_unused]] const auto eres
                = repl_map.emplace(var, fmt::format("u_{}", static_cast<std::size_t>(u_idx++)));
            assert(eres.second);
        }

        for (const auto &p : params) {
            [[maybe_unused]] const auto eres
                = repl_map.emplace(p, fmt::format("u_{}", static_cast<std::size_t>(u_idx++)));
            assert(eres.second);
        }
    }

    // NOTE: split prods into binary mults. In reverse-mode AD,
    // n-ary products slow down performance because the complexity
    // of each adjoint increases linearly with the number of arguments,
    // leading to quadratic complexity in the reverse pass. By contrast,
    // the adjoints of binary products have fixed complexity and the number
    // of binary multiplications necessary to reconstruct an n-ary product
    // increases only linearly with the number of arguments.
    auto v_ex = detail::split_prods_for_decompose(v_ex_, 2u);

    // Unfix: fix() calls are not necessary any more, they will just increase
    // the decomposition's size and mess up the derivatives.
    // NOTE: unfix is the last step, as we want to keep expressions
    // fixed in the previous preprocessing steps.
    v_ex = unfix(v_ex);

#if !defined(NDEBUG)

    // Save copy for checking in debug mode.
    const auto v_ex_verify = v_ex;

#endif

    // Rename variables and params.
    v_ex = subs(v_ex, repl_map);

    // Init the decomposition. It begins with a list
    // of the original variables and params of the function.
    std::vector<expression> ret;
    ret.reserve(boost::safe_numerics::safe<decltype(ret.size())>(nvars) + npars);
    for (const auto &var : vars) {
        // NOTE: transform into push_back() once get_variables() returns
        // expressions rather than strings.
        ret.emplace_back(var);
    }
    for (const auto &par : params) {
        ret.push_back(par);
    }

    // Prepare the outputs vector.
    std::vector<expression> outs;
    outs.reserve(nouts);

    // Log the construction runtime in trace mode.
    spdlog::stopwatch sw;

    // Run the decomposition.
    detail::funcptr_map<std::vector<expression>::size_type> func_map;
    for (const auto &ex : v_ex) {
        // Decompose the current component.
        if (const auto dres = detail::decompose(func_map, ex, ret)) {
            // NOTE: if the component was decomposed
            // (that is, it is not constant or a single variable),
            // then the output is a u variable.
            // NOTE: all functions are forced to return
            // a non-empty dres
            // in the func API, so the only entities that
            // can return an empty dres are consts or
            // variables.
            outs.emplace_back(fmt::format("u_{}", *dres));
        } else {
            // NOTE: params have been turned into variables,
            // thus here the only 2 possibilities are variable
            // and number.
            assert(std::holds_alternative<variable>(ex.value()) || std::holds_alternative<number>(ex.value()));

            outs.push_back(ex);
        }
    }

    assert(outs.size() == nouts);

    // Append the definitions of the outputs.
    ret.insert(ret.end(), outs.begin(), outs.end());

    get_logger()->trace("diff decomposition construction runtime: {}", sw);

#if !defined(NDEBUG)

    // Verify the decomposition.
    // NOTE: nvars + npars is implicitly converted to std::vector<expression>::size_type here.
    // This is fine, as the decomposition must contain at least nvars + npars items.
    verify_function_dec(v_ex_verify, ret, nvars + npars, true);

#endif

    // Simplify the decomposition.
    // NOTE: nvars + npars is implicitly converted to std::vector<expression>::size_type here.
    // This is fine, as the decomposition must contain at least nvars + npars items.
    ret = function_decompose_cse(ret, nvars + npars, nouts);

#if !defined(NDEBUG)

    // Verify the decomposition.
    verify_function_dec(v_ex_verify, ret, nvars + npars, true);

#endif

    // Run the breadth-first topological sort on the decomposition.
    // NOTE: nvars + npars is implicitly converted to std::vector<expression>::size_type here.
    // This is fine, as the decomposition must contain at least nvars + npars items.
    ret = function_sort_dc(ret, nvars + npars, nouts);

#if !defined(NDEBUG)

    // Verify the decomposition.
    verify_function_dec(v_ex_verify, ret, nvars + npars, true);

#endif

    return {std::move(ret), nvars + npars};
}

namespace
{

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto diff_make_adj_dep(const std::vector<expression> &dc, std::vector<expression>::size_type nvars,
                       [[maybe_unused]] std::vector<expression>::size_type nouts)
{
    // NOTE: the shortest possible dc is for a scalar
    // function identically equal to a number. In this case,
    // dc will have a size of 1.
    assert(!dc.empty());
    assert(nvars < dc.size());
    assert(nouts >= 1u);
    assert(nouts <= dc.size());

    using idx_t = std::vector<expression>::size_type;

    // Do an initial pass to create the adjoints, the
    // vectors of direct and reverse dependencies,
    // and the substitution map.
    std::vector<fast_umap<std::uint32_t, expression>> adj;
    adj.resize(boost::numeric_cast<decltype(adj.size())>(dc.size()));

    std::vector<std::vector<std::uint32_t>> dep;
    dep.resize(boost::numeric_cast<decltype(dep.size())>(dc.size()));

    std::vector<std::vector<std::uint32_t>> revdep;
    revdep.resize(boost::numeric_cast<decltype(revdep.size())>(dc.size()));

    std::unordered_map<std::string, expression> subs_map;

    for (idx_t i = 0; i < nvars; ++i) {
        assert(subs(dc[i], subs_map) == dc[i]);

        // NOTE: no adjoints or direct/reverse dependencies needed for the initial definitions,
        // we only need to fill in subs_map.
        [[maybe_unused]] const auto flag = subs_map.emplace(fmt::format("u_{}", i), dc[i]).second;

        assert(flag);
    }

    for (idx_t i = nvars; i < dc.size(); ++i) {
        auto &cur_adj_dict = adj[i];
        auto &cur_dep = dep[i];

        for (const auto &var : get_variables(dc[i])) {
            const auto idx = uname_to_index(var);

            assert(cur_adj_dict.count(idx) == 0u);
            cur_adj_dict[idx] = diff(dc[i], var);

            assert(idx < revdep.size());
            revdep[idx].push_back(boost::numeric_cast<std::uint32_t>(i));

            cur_dep.push_back(idx);
        }

        subs_map.emplace(fmt::format("u_{}", i), subs(dc[i], subs_map));
    }

    // Sort the vectors of reverse dependencies.
    // NOTE: this is not strictly necessary for the correctness
    // of the algorithm. It will just ensure that when we eventually
    // compute the derivative of the output wrt a subexpression, the
    // summation over the reverse dependencies happens in index order.
    for (auto &rvec : revdep) {
        std::sort(rvec.begin(), rvec.end());

        // Check that there are no duplicates.
        assert(std::adjacent_find(rvec.begin(), rvec.end()) == rvec.end());
    }

#if !defined(NDEBUG)

    // Sanity checks in debug mode.
    for (idx_t i = 0; i < nvars; ++i) {
        // No adjoints for the vars/params definitions.
        assert(adj[i].empty());

        // Each var/param must be a dependency for some
        // other subexpression.
        assert(!revdep[i].empty());

        // No direct dependencies in the
        // initial definitions.
        assert(dep[i].empty());
    }

    for (idx_t i = nvars; i < dc.size() - nouts; ++i) {
        // The only possibility for an adjoint dict to be empty
        // is if all the subexpression arguments are numbers.
        // NOTE: params have been turned into variables, so that
        // get_variables() will also list param args.
        assert(!adj[i].empty() || get_variables(dc[i]).empty());

        // Every subexpression must be a dependency for some other subexpression.
        assert(!revdep[i].empty());

        // Every subexpression must depend on another subexpression,
        // unless all the subexpression arguments are numbers.
        assert(!dep[i].empty() || get_variables(dc[i]).empty());
    }

    // Each output:
    // - cannot be the dependency for any subexpression,
    // - must depend on 1 subexpression, unless the output
    //   itself is a number,
    // - must either be a number or have only 1 element in the adjoints dict, and:
    //   - the key of such element cannot be another output,
    //   - the value of such element must be the constant 1_dbl
    //     (this comes from the derivative of a variables wrt itself
    //     returning 1_dbl).
    for (idx_t i = dc.size() - nouts; i < dc.size(); ++i) {
        if (adj[i].empty()) {
            assert(std::holds_alternative<number>(dc[i].value()));
        } else {
            assert(adj[i].size() == 1u);
            assert(adj[i].begin()->first < dc.size() - nouts);
            assert(adj[i].begin()->second == 1_dbl);
        }

        assert(revdep[i].empty());

        if (std::holds_alternative<number>(dc[i].value())) {
            assert(dep[i].empty());
        } else {
            assert(dep[i].size() == 1u);
        }
    }

#endif

    return std::tuple{std::move(adj), std::move(dep), std::move(revdep), std::move(subs_map)};
}

// This is an alternative version of dtens_sv_idx_t that uses a dictionary
// for storing the index/order pairs instead of a sorted vector. Using a dictionary
// allows for faster/easier manipulation.
using dtens_ss_idx_t = std::pair<std::uint32_t, fast_umap<std::uint32_t, std::uint32_t>>;

// Helper to turn a dtens_sv_idx_t into a dtens_ss_idx_t.
void vidx_v2s(dtens_ss_idx_t &output, const dtens_sv_idx_t &input)
{
    // Assign the component.
    output.first = input.first;

    // Assign the index/order pairs.
    output.second.clear();
    for (const auto &p : input.second) {
        [[maybe_unused]] auto [_, flag] = output.second.insert(p);
        assert(flag);
    }
}

// Helper to turn a dtens_ss_idx_t into a dtens_sv_idx_t.
dtens_sv_idx_t vidx_s2v(const dtens_ss_idx_t &input)
{
    // Init retval.
    dtens_sv_idx_t retval{input.first, {input.second.begin(), input.second.end()}};

    // Sort the index/order pairs.
    std::sort(retval.second.begin(), retval.second.end(),
              [](const auto &p1, const auto &p2) { return p1.first < p2.first; });

    return retval;
} // LCOV_EXCL_LINE

// Hasher for the local maps of derivatives used in the
// forward/reverse mode implementations.
struct diff_map_hasher {
    std::size_t operator()(const dtens_ss_idx_t &s) const noexcept
    {
        // Use as seed the component index.
        std::size_t seed = std::hash<std::uint32_t>{}(s.first);

        // Compose via additions the hashes of the index/order pairs.
        // NOTE: it is important that we use here a commutative operation
        // for the composition so that the final hash is independent of the order
        // in which the pairs are stored in the dictionary.
        for (const auto &p : s.second) {
            std::size_t p_hash = std::hash<std::uint32_t>{}(p.first);
            boost::hash_combine(p_hash, std::hash<std::uint32_t>{}(p.second));

            seed += p_hash;
        }

        return seed;
    }
};

// Forward-mode implementation of diff_tensors().
template <typename DiffMap, typename Dep, typename Adj>
void diff_tensors_forward_impl(
    // The map of derivatives. It will be updated after all the
    // derivatives have been computed.
    DiffMap &diff_map,
    // The number of derivatives in the previous-order tensor.
    std::vector<expression>::size_type cur_nouts,
    // The decomposition of the previous-order tensor.
    const std::vector<expression> &dc,
    // The direct and reverse dependencies for the
    // subexpressions in dc.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const Dep &dep, const Dep &revdep,
    // The adjoints of the subexpressions in dc.
    const Adj &adj,
    // The total number of variables in dc (this accounts also
    // for params, as they are turned into variables during the
    // construciton of the decomposition).
    std::vector<expression>::size_type nvars,
    // The diff arguments.
    const std::vector<expression> &args,
    // Iterator in diff_map pointing to the first
    // derivative for the previous order.
    typename DiffMap::iterator prev_begin,
    // The current derivative order.
    std::uint32_t cur_order)
{
    assert(dc.size() > nvars);
    assert(cur_order > 0u);

    // Local data structures used to temporarily store the derivatives,
    // which will eventually be added to diff_map.
    // For derivative orders > 1, the algorithm we employ
    // will produce several times the same derivative, and thus
    // we need to store the derivatives in a dictionary in order
    // to prevent duplicates. For order-1 derivatives, no duplicate
    // derivatives will be produced and thus we can use a plain vector,
    // which can be quite a bit faster.
    using diff_map_t = fast_umap<dtens_ss_idx_t, expression, diff_map_hasher>;
    using diff_vec_t = std::vector<std::pair<dtens_ss_idx_t, expression>>;
    using local_diff_t = std::variant<diff_map_t, diff_vec_t>;
    auto local_diff = (cur_order == 1u) ? local_diff_t(diff_vec_t{}) : local_diff_t(diff_map_t{});

    // Helpers to ease the access to the active member of the local_diff variant.
    // NOTE: if used incorrectly, these will throw at runtime.
    auto local_dmap = [&local_diff]() -> diff_map_t & { return std::get<diff_map_t>(local_diff); };
    auto local_dvec = [&local_diff]() -> diff_vec_t & { return std::get<diff_vec_t>(local_diff); };

    // This is used as a temporary variable in several places below.
    dtens_ss_idx_t tmp_v_idx;

    // These two containers will be used to store the list of subexpressions
    // which depend on an input. They are used in the forward pass
    // to avoid iterating over those subexpressions which do not depend on
    // an input. We need two containers (with identical content)
    // because we need both ordered iteration AND fast lookup.
    fast_uset<std::uint32_t> in_deps;
    std::vector<std::uint32_t> sorted_in_deps;

    // A stack to be used when filling up in_deps/sorted_in_deps.
    std::deque<std::uint32_t> stack;

    // Create a dictionary mapping an input to its position
    // in the decomposition. This is used to locate diff arguments
    // in the decomposition.
    fast_umap<expression, std::vector<expression>::size_type, std::hash<expression>> input_idx_map;
    for (std::vector<expression>::size_type i = 0; i < nvars; ++i) {
        const auto &cur_in = dc[i];
        assert(input_idx_map.count(cur_in) == 0u);
        input_idx_map[cur_in] = i;
    }

    // Run the forward pass for each diff argument. The derivatives
    // wrt the diff argument will be stored into diffs.
    std::vector<expression> diffs(dc.size());
    for (decltype(args.size()) diff_arg_idx = 0; diff_arg_idx < args.size(); ++diff_arg_idx) {
        const auto &cur_diff_arg = args[diff_arg_idx];

        // Check if the current diff argument is one of the inputs.
        if (input_idx_map.count(cur_diff_arg) == 0u) {
            // The diff argument is not one of the inputs:
            // set the derivatives of all outputs wrt to the
            // diff argument to zero.
            auto out_it = prev_begin;

            for (std::vector<expression>::size_type out_idx = 0; out_idx < cur_nouts; ++out_idx, ++out_it) {
                assert(out_it != diff_map.end());

                vidx_v2s(tmp_v_idx, out_it->first);
                tmp_v_idx.second[static_cast<std::uint32_t>(diff_arg_idx)] += 1u;

                if (cur_order == 1u) {
                    local_dvec().emplace_back(tmp_v_idx, 0_dbl);
                } else {
                    // NOTE: use try_emplace() so that if the derivative
                    // has already been computed, nothing happens.
                    local_dmap().try_emplace(tmp_v_idx, 0_dbl);
                }
            }

            // Move to the next diff argument.
            continue;
        }

        // The diff argument is one of the inputs. Fetch its index.
        const auto input_idx = input_idx_map.find(cur_diff_arg)->second;

        // Seed the stack and in_deps/sorted_in_deps with the
        // dependees of the current input.
        stack.assign(revdep[input_idx].begin(), revdep[input_idx].end());
        sorted_in_deps.assign(revdep[input_idx].begin(), revdep[input_idx].end());
        in_deps.clear();
        in_deps.insert(revdep[input_idx].begin(), revdep[input_idx].end());

        // Build in_deps/sorted_in_deps by traversing
        // the decomposition forward.
        while (!stack.empty()) {
            // Pop the first element from the stack.
            const auto cur_idx = stack.front();
            stack.pop_front();

            // Push into the stack and in_deps/sorted_in_deps
            // the dependees of cur_idx.
            for (const auto next_idx : revdep[cur_idx]) {
                // NOTE: if next_idx is already in in_deps,
                // it means that it was visited already and thus
                // it does not need to be put in the stack.
                if (in_deps.count(next_idx) == 0u) {
                    stack.push_back(next_idx);
                    sorted_in_deps.push_back(next_idx);
                    in_deps.insert(next_idx);
                }
            }
        }

        // Sort sorted_in_deps in ascending order.
        std::sort(sorted_in_deps.begin(), sorted_in_deps.end());

        // sorted_in_deps cannot have duplicate values.
        assert(std::adjacent_find(sorted_in_deps.begin(), sorted_in_deps.end()) == sorted_in_deps.end());
        // sorted_in_deps either must be empty, or its last index
        // must refer to an output (i.e., the current input must be
        // a dependency for some output).
        assert(sorted_in_deps.empty() || *sorted_in_deps.rbegin() >= diffs.size() - cur_nouts);
        assert(sorted_in_deps.size() == in_deps.size());

        // Set the seed value for the current input.
        diffs[input_idx] = 1_dbl;

        // Set the derivatives of all outputs to zero, so that if
        // an output does not depend on the current input then the
        // derivative of that output wrt the current input is pre-emptively
        // set to zero.
        std::fill(diffs.data() + diffs.size() - cur_nouts, diffs.data() + diffs.size(), 0_dbl);

        // Run the forward pass.
        //
        // NOTE: we need to tread lightly here. The forward pass consists
        // of iteratively building up an expression from a seed value via
        // multiplications and summations involving the adjoints. In order
        // for the resulting expression to be able to be evaluated with
        // optimal performance by heyoka, several automated simplifications in sums
        // and products need to be disabled. One such example is the flattening
        // of nested products, which destroys heyoka's ability to identify and
        // eliminate redundant subexpressions via CSE. In order to disable automatic
        // simplifications, we use fix(), with the caveat that fix() is *not*
        // applied to number expressions (so that constant
        // folding can still take place). Perhaps in the future it will be possible
        // to enable further simplifications involving pars/vars, but, for now, let
        // us play it conservatively.
        for (const auto cur_idx : sorted_in_deps) {
            std::vector<expression> tmp_sum;

            for (const auto d_idx : dep[cur_idx]) {
                assert(d_idx < diffs.size());
                assert(cur_idx < adj.size());
                // NOTE: the dependency must point
                // to a subexpression *before* the current one.
                assert(d_idx < cur_idx);
                assert(adj[cur_idx].count(d_idx) == 1u);

                // NOTE: if the current subexpression depends
                // on another subexpression which neither is
                // the current input nor depends on the current input,
                // then the derivative is zero.
                if (d_idx != input_idx && in_deps.count(d_idx) == 0u) {
                    tmp_sum.push_back(0_dbl);
                } else {
                    auto new_term = fix_nn(fix_nn(diffs[d_idx]) * fix_nn(adj[cur_idx].find(d_idx)->second));
                    tmp_sum.push_back(std::move(new_term));
                }
            }

            assert(!tmp_sum.empty());

            diffs[cur_idx] = fix_nn(sum(tmp_sum));
        }

        // Add the derivatives of all outputs wrt the current input
        // to the local map.
        auto out_it = prev_begin;

        for (std::vector<expression>::size_type out_idx = 0; out_idx < cur_nouts; ++out_idx, ++out_it) {
            assert(out_it != diff_map.end());

            vidx_v2s(tmp_v_idx, out_it->first);
            tmp_v_idx.second[static_cast<std::uint32_t>(diff_arg_idx)] += 1u;

            if (cur_order == 1u) {
                auto cur_der = diffs[diffs.size() - cur_nouts + out_idx];
                local_dvec().emplace_back(tmp_v_idx, std::move(cur_der));
            } else {
                // Check if we already computed this derivative.
                if (const auto it = local_dmap().find(tmp_v_idx); it == local_dmap().end()) {
                    // The derivative is new.
                    auto cur_der = diffs[diffs.size() - cur_nouts + out_idx];

                    [[maybe_unused]] const auto [_, flag] = local_dmap().try_emplace(tmp_v_idx, std::move(cur_der));
                    assert(flag);
                }
            }
        }
    }

    // Merge the local map into diff_map.
    if (cur_order == 1u) {
        for (auto &p : local_dvec()) {
            diff_map.emplace_back(vidx_s2v(p.first), std::move(p.second));
        }
    } else {
        for (auto &p : local_dmap()) {
            diff_map.emplace_back(vidx_s2v(p.first), std::move(p.second));
        }
    }
}

// Reverse-mode implementation of diff_tensors().
template <typename DiffMap, typename Dep, typename Adj>
void diff_tensors_reverse_impl(
    // The map of derivatives. It will be updated after all the
    // derivatives have been computed.
    DiffMap &diff_map,
    // The number of derivatives in the previous-order tensor.
    std::vector<expression>::size_type cur_nouts,
    // The decomposition of the previous-order tensor.
    const std::vector<expression> &dc,
    // The direct and reverse dependencies for the
    // subexpressions in dc.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const Dep &dep, const Dep &revdep,
    // The adjoints of the subexpressions in dc.
    const Adj &adj,
    // The total number of variables in dc (this accounts also
    // for params, as they are turned into variables during the
    // construciton of the decomposition).
    std::vector<expression>::size_type nvars,
    // The diff arguments.
    const std::vector<expression> &args,
    // Iterator in diff_map pointing to the first
    // derivative for the previous order.
    typename DiffMap::iterator prev_begin,
    // The current derivative order.
    std::uint32_t cur_order)
{
    assert(dc.size() > nvars);
    assert(cur_order > 0u);

    // Local data structures used to temporarily store the derivatives,
    // which will eventually be added to diff_map.
    // For derivative orders > 1, the algorithm we employ
    // will produce several times the same derivative, and thus
    // we need to store the derivatives in a dictionary in order
    // to prevent duplicates. For order-1 derivatives, no duplicate
    // derivatives will be produced and thus we can use a plain vector,
    // which can be quite a bit faster.
    using diff_map_t = fast_umap<dtens_ss_idx_t, expression, diff_map_hasher>;
    using diff_vec_t = std::vector<std::pair<dtens_ss_idx_t, expression>>;
    using local_diff_t = std::variant<diff_map_t, diff_vec_t>;
    auto local_diff = (cur_order == 1u) ? local_diff_t(diff_vec_t{}) : local_diff_t(diff_map_t{});

    // Helpers to ease the access to the active member of the local_diff variant.
    // NOTE: if used incorrectly, these will throw at runtime.
    // NOTE: currently local_dmap is never used because the heuristic
    // for deciding between forward and reverse mode prevents reverse mode
    // from being used for order > 1.
    auto local_dmap = [&local_diff]() -> diff_map_t & { return std::get<diff_map_t>(local_diff); }; // LCOV_EXCL_LINE
    auto local_dvec = [&local_diff]() -> diff_vec_t & { return std::get<diff_vec_t>(local_diff); };

    // Cache the number of diff arguments.
    const auto nargs = args.size();

    // This is used as a temporary variable in several places below.
    dtens_ss_idx_t tmp_v_idx;

    // These two containers will be used to store the list of subexpressions
    // on which an output depends. They are used in the reverse pass
    // to avoid iterating over those subexpressions on which the output
    // does not depend (recall that the decomposition contains the subexpressions
    // for ALL outputs). We need two containers (with identical content)
    // because we need both ordered iteration AND fast lookup.
    fast_uset<std::uint32_t> out_deps;
    std::vector<std::uint32_t> sorted_out_deps;

    // A stack to be used when filling up out_deps/sorted_out_deps.
    std::deque<std::uint32_t> stack;

    // Run the reverse pass for each output. The derivatives
    // wrt the output will be stored into diffs.
    std::vector<expression> diffs(dc.size());
    for (std::vector<expression>::size_type i = 0; i < cur_nouts; ++i) {
        // Compute the index of the current output in the decomposition.
        const auto out_idx = boost::numeric_cast<std::uint32_t>(diffs.size() - cur_nouts + i);

        // Seed the stack and out_deps/sorted_out_deps with the
        // current output's dependency.
        stack.assign(dep[out_idx].begin(), dep[out_idx].end());
        sorted_out_deps.assign(dep[out_idx].begin(), dep[out_idx].end());
        out_deps.clear();
        out_deps.insert(dep[out_idx].begin(), dep[out_idx].end());

#if !defined(NDEBUG)

        // NOTE: an output can only have 0 or 1 dependencies.
        if (stack.empty()) {
            assert(std::holds_alternative<number>(dc[out_idx].value()));
        } else {
            assert(stack.size() == 1u);
        }

#endif

        // Build out_deps/sorted_out_deps by traversing
        // the decomposition backwards.
        while (!stack.empty()) {
            // Pop the first element from the stack.
            const auto cur_idx = stack.front();
            stack.pop_front();

            // Push into the stack and out_deps/sorted_out_deps
            // the dependencies of cur_idx.
            for (const auto next_idx : dep[cur_idx]) {
                // NOTE: if next_idx is already in out_deps,
                // it means that it was visited already and thus
                // it does not need to be put in the stack.
                if (out_deps.count(next_idx) == 0u) {
                    stack.push_back(next_idx);
                    sorted_out_deps.push_back(next_idx);
                    out_deps.insert(next_idx);
                }
            }
        }

        // Sort sorted_out_deps in decreasing order.
        std::sort(sorted_out_deps.begin(), sorted_out_deps.end(), std::greater{});

        // sorted_out_deps cannot have duplicate values.
        assert(std::adjacent_find(sorted_out_deps.begin(), sorted_out_deps.end()) == sorted_out_deps.end());
        // sorted_out_deps either must be empty, or its last index
        // must refer to a variable/param (i.e., the current output
        // must have a var/param as last element in the chain of dependencies).
        assert(sorted_out_deps.empty() || *sorted_out_deps.rbegin() < nvars);
        assert(sorted_out_deps.size() == out_deps.size());

        // Set the seed value for the current output.
        diffs[out_idx] = 1_dbl;

        // Set the derivatives wrt all vars/params for the current output
        // to zero, so that if the current output does not depend on a
        // var/param then the derivative wrt that var/param is pre-emptively
        // set to zero.
        std::fill(diffs.data(), diffs.data() + nvars, 0_dbl);

        // Run the reverse pass on all subexpressions which
        // the current output depends on.
        //
        // NOTE: we need to tread lightly here. The reverse pass consists
        // of iteratively building up an expression from a seed value via
        // multiplications and summations involving the adjoints. In order
        // for the resulting expression to be able to be evaluated with
        // optimal performance by heyoka, several automated simplifications in sums
        // and products need to be disabled. One such example is the flattening
        // of nested products, which destroys heyoka's ability to identify and
        // eliminate redundant subexpressions via CSE. In order to disable automatic
        // simplifications, we use fix(), with the caveat that fix() is *not*
        // applied to number expressions (so that constant
        // folding can still take place). Perhaps in the future it will be possible
        // to enable further simplifications involving pars/vars, but, for now, let
        // us play it conservatively.
        for (const auto cur_idx : sorted_out_deps) {
            std::vector<expression> tmp_sum;

            for (const auto rd_idx : revdep[cur_idx]) {
                assert(rd_idx < diffs.size());
                assert(rd_idx < adj.size());
                // NOTE: the reverse dependency must point
                // to a subexpression *after* the current one.
                assert(rd_idx > cur_idx);
                assert(adj[rd_idx].count(cur_idx) == 1u);

                // NOTE: if the current subexpression is a dependency
                // for another subexpression which is neither the current output
                // nor one of its dependencies, then the derivative is zero.
                if (rd_idx != out_idx && out_deps.count(rd_idx) == 0u) {
                    tmp_sum.push_back(0_dbl);
                } else {
                    auto new_term = fix_nn(fix_nn(diffs[rd_idx]) * fix_nn(adj[rd_idx].find(cur_idx)->second));
                    tmp_sum.push_back(std::move(new_term));
                }
            }

            assert(!tmp_sum.empty());

            diffs[cur_idx] = fix_nn(sum(tmp_sum));
        }

        // Create a dict mapping the vars/params in the decomposition
        // to the derivatives of the current output wrt them. This is used
        // to fetch from diffs only the derivatives we are interested in
        // (since there may be vars/params in the decomposition wrt which
        // the derivatives are not requested).
        fast_umap<expression, expression, std::hash<expression>> dmap;
        for (std::vector<expression>::size_type j = 0; j < nvars; ++j) {
            [[maybe_unused]] const auto [_, flag] = dmap.try_emplace(dc[j], diffs[j]);
            assert(flag);
        }

        // Add the derivatives to the local map.
        for (decltype(args.size()) j = 0; j < nargs; ++j) {
            // Compute the indices vector for the current derivative.
            vidx_v2s(tmp_v_idx, prev_begin->first);
            // NOTE: no need to overflow check here, because no derivative
            // order can end up being larger than the total diff order which
            // is representable by std::uint32_t.
            tmp_v_idx.second[static_cast<std::uint32_t>(j)] += 1u;

            if (cur_order == 1u) {
                // Check if the diff argument is present in the
                // decomposition: if it is, we will calculate the derivative and add it.
                // Otherwise, we set the derivative to zero and add it.
                expression cur_der = 0_dbl;

                if (const auto it_dmap = dmap.find(args[j]); it_dmap != dmap.end()) {
                    cur_der = it_dmap->second;
                }

                local_dvec().emplace_back(tmp_v_idx, std::move(cur_der));
            } else {
                // LCOV_EXCL_START
                // Check if we already computed this derivative.
                if (const auto it = local_dmap().find(tmp_v_idx); it == local_dmap().end()) {
                    // The derivative is new. If the diff argument is present in the
                    // decomposition, then we will calculate the derivative and add it.
                    // Otherwise, we set the derivative to zero and add it.
                    expression cur_der = 0_dbl;

                    if (const auto it_dmap = dmap.find(args[j]); it_dmap != dmap.end()) {
                        cur_der = it_dmap->second;
                    }

                    [[maybe_unused]] const auto [_, flag] = local_dmap().try_emplace(tmp_v_idx, std::move(cur_der));
                    assert(flag);
                }
                // LCOV_EXCL_STOP
            }
        }

        // Update prev_begin as we move to the next output.
        ++prev_begin;
        assert(prev_begin != diff_map.end() || i + 1u == cur_nouts);
    }

    // Merge the local map into diff_map.
    if (cur_order == 1u) {
        for (auto &p : local_dvec()) {
            diff_map.emplace_back(vidx_s2v(p.first), std::move(p.second));
        }
    } else {
        // LCOV_EXCL_START
        for (auto &p : local_dmap()) {
            diff_map.emplace_back(vidx_s2v(p.first), std::move(p.second));
        }
        // LCOV_EXCL_STOP
    }
}

// Utility function to check that a dtens_sv_idx_t is well-formed.
void sv_sanity_check([[maybe_unused]] const dtens_sv_idx_t &v)
{
    // Check sorting according to the derivative indices.
    auto cmp = [](const auto &p1, const auto &p2) { return p1.first < p2.first; };
    assert(std::is_sorted(v.second.begin(), v.second.end(), cmp));

    // Check no duplicate derivative indices.
    auto no_dup = [](const auto &p1, const auto &p2) { return p1.first == p2.first; };
    assert(std::adjacent_find(v.second.begin(), v.second.end(), no_dup) == v.second.end());

    // Check no zero derivative orders.
    auto nz_order = [](const auto &p) { return p.second != 0u; };
    assert(std::all_of(v.second.begin(), v.second.end(), nz_order));
}

// Implementation of dtens_sv_idx_cmp::operator().
bool dtens_sv_idx_cmp_impl(const dtens_sv_idx_t &v1, const dtens_sv_idx_t &v2)
{
    // Sanity checks on the inputs.
    sv_sanity_check(v1);
    sv_sanity_check(v2);

    // Compute the total derivative order for both v1 and v2.
    // NOTE: here we have to use safe_numerics because this comparison operator
    // might end up being invoked on a user-supplied dtens_sv_idx_t, whose total degree
    // may overflow. The dtens_sv_idx_t in dtens, by contrast, are guaranteed to never
    // overflow when computing the total degree.
    using su32 = boost::safe_numerics::safe<std::uint32_t>;

    // The accumulator.
    auto acc = [](const auto &val, const auto &p) { return val + p.second; };

    const auto deg1 = std::accumulate(v1.second.begin(), v1.second.end(), su32(0), acc);
    const auto deg2 = std::accumulate(v2.second.begin(), v2.second.end(), su32(0), acc);

    if (deg1 < deg2) {
        return true;
    }

    if (deg1 > deg2) {
        return false;
    }

    // The total derivative order is the same, look at
    // the component index next.
    if (v1.first < v2.first) {
        return true;
    }

    if (v1.first > v2.first) {
        return false;
    }

    // Component and total derivative order are the same,
    // resort to reverse lexicographical compare on the
    // derivative orders.
    auto it1 = v1.second.begin(), it2 = v2.second.begin();
    const auto end1 = v1.second.end(), end2 = v2.second.end();
    for (; it1 != end1 && it2 != end2; ++it1, ++it2) {
        const auto [idx1, n1] = *it1;
        const auto [idx2, n2] = *it2;

        if (idx2 > idx1) {
            return true;
        }

        if (idx1 > idx2) {
            return false;
        }

        if (n1 > n2) {
            return true;
        }

        if (n2 > n1) {
            return false;
        }

        assert(std::equal(v1.second.begin(), it1 + 1, v2.second.begin()));
    }

    if (it1 == end1 && it2 == end2) {
        assert(v1.second == v2.second);
        return false;
    }

    if (it1 == end1) {
        return false;
    }

    assert(it2 == end2);

    return true;
}

#if !defined(NDEBUG)

// Same comparison as the previous function, but in dense format.
// Used only for debug.
bool dtens_v_idx_cmp_impl(const dtens::v_idx_t &v1, const dtens::v_idx_t &v2)
{
    assert(v1.size() == v2.size());
    assert(!v1.empty());

    // Compute the total derivative order for both
    // vectors.
    boost::safe_numerics::safe<std::uint32_t> deg1 = 0, deg2 = 0;
    const auto size = v1.size();
    for (decltype(v1.size()) i = 1; i < size; ++i) {
        deg1 += v1[i];
        deg2 += v2[i];
    }

    if (deg1 < deg2) {
        return true;
    }

    if (deg1 > deg2) {
        return false;
    }

    // The total derivative order is the same, look at
    // the component index next.
    if (v1[0] < v2[0]) {
        return true;
    }

    if (v1[0] > v2[0]) {
        return false;
    }

    // Component and total derivative order are the same,
    // resort to reverse lexicographical compare on the
    // derivative orders.
    return std::lexicographical_compare(v1.begin() + 1, v1.end(), v2.begin() + 1, v2.end(), std::greater{});
}

#endif

} // namespace

bool dtens_sv_idx_cmp::operator()(const dtens_sv_idx_t &v1, const dtens_sv_idx_t &v2) const
{
    auto ret = dtens_sv_idx_cmp_impl(v1, v2);

#if !defined(NDEBUG)

    // Convert to dense and re-run the same comparison.
    auto to_dense = [](const dtens_sv_idx_t &v) {
        dtens::v_idx_t dv{v.first};

        std::uint32_t cur_d_idx = 0;
        for (auto it = v.second.begin(); it != v.second.end(); ++cur_d_idx) {
            if (cur_d_idx == it->first) {
                dv.push_back(it->second);
                ++it;
            } else {
                dv.push_back(0);
            }
        }

        return dv;
    }; // LCOV_EXCL_LINE

    auto dv1 = to_dense(v1);
    auto dv2 = to_dense(v2);
    dv1.resize(std::max(dv1.size(), dv2.size()));
    dv2.resize(std::max(dv1.size(), dv2.size()));

    assert(ret == dtens_v_idx_cmp_impl(dv1, dv2));

#endif

    return ret;
}

} // namespace detail

// NOLINTNEXTLINE(bugprone-exception-escape)
struct dtens::impl {
    detail::dtens_map_t m_map;
    std::vector<expression> m_args;

    // Serialisation.
    void save(boost::archive::binary_oarchive &ar, unsigned) const
    {
        // NOTE: this is essentially a manual implementation of serialisation
        // for flat_map, which is currently missing. See:
        // https://stackoverflow.com/questions/69492511/boost-serialize-writing-a-general-map-serialization-function

        // Serialise the size.
        const auto size = m_map.size();
        ar << size;

        // Serialise the elements.
        for (const auto &p : m_map) {
            ar << p;
        }

        // Serialise m_args.
        ar << m_args;
    }

    // NOTE: as usual, we assume here that the archive contains
    // a correctly-serialised instance. In particular, we are assuming
    // that the elements in ar are sorted correctly.
    void load(boost::archive::binary_iarchive &ar, unsigned)
    {
        try {
            // Reset m_map.
            m_map.clear();

            // Read the size.
            size_type size = 0;
            ar >> size;

            // Reserve space.
            // NOTE: this is important as it ensures that
            // the addresses of the inserted elements
            // do not change as we insert more elements.
            m_map.reserve(size);

            // Read the elements.
            for (size_type i = 0; i < size; ++i) {
                detail::dtens_map_t::value_type tmp_val;
                ar >> tmp_val;
                const auto it = m_map.insert(m_map.end(), std::move(tmp_val));
                assert(it == m_map.end() - 1);

                // Reset the object address.
                // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved)
                ar.reset_object_address(std::addressof(*it), &tmp_val);
            }

            assert(m_map.size() == size);

            // Deserialise m_args.
            ar >> m_args;

            // LCOV_EXCL_START
        } catch (...) {
            *this = impl{};
            throw;
        }
        // LCOV_EXCL_STOP
    }
    BOOST_SERIALIZATION_SPLIT_MEMBER()
};

namespace detail
{

namespace
{

auto diff_tensors_impl(const std::vector<expression> &v_ex, const std::vector<expression> &args, std::uint32_t order)
{
    spdlog::stopwatch sw;

    assert(std::all_of(args.begin(), args.end(), [](const auto &arg) {
        return std::holds_alternative<variable>(arg.value()) || std::holds_alternative<param>(arg.value());
    }));
    assert(std::unordered_set(args.begin(), args.end()).size() == args.size());

    // Cache the original number of outputs and the diff arguments.
    const auto orig_nouts = v_ex.size();
    const auto nargs = args.size();

    assert(orig_nouts > 0u);
    assert(nargs > 0u);

    // NOTE: check that nargs fits in a 32-bit int, so that
    // in the dtens API get_nvars() can safely return std::uint32_t.
    (void)(boost::numeric_cast<std::uint32_t>(nargs));

    // Map to associate a dtens_sv_idx_t to a derivative.
    // This will be kept manually sorted according to dtens_v_idx_cmp
    // and it will be turned into a flat map at the end.
    dtens_map_t::sequence_type diff_map;

    // Helper to locate a dtens_sv_idx_t in diff_map. If not present,
    // diff_map.end() will be returned.
    auto search_diff_map = [&diff_map](const dtens_sv_idx_t &v) {
        auto it = std::lower_bound(diff_map.begin(), diff_map.end(), v, [](const auto &item, const auto &vec) {
            return dtens_sv_idx_cmp{}(item.first, vec);
        });

        if (it != diff_map.end() && it->first == v) {
            return it;
        } else {
            return diff_map.end();
        }
    };

    // This is used as a temporary variable in several places below.
    dtens_sv_idx_t tmp_v_idx;

    // Vector that will store the previous-order derivatives in the loop below.
    // It will be used to construct the decomposition.
    std::vector<expression> prev_diffs;

    // Init diff_map with the order 0 derivatives
    // (i.e., the original function components).
    for (decltype(v_ex.size()) i = 0; i < orig_nouts; ++i) {
        tmp_v_idx.first = boost::numeric_cast<std::uint32_t>(i);

        assert(search_diff_map(tmp_v_idx) == diff_map.end());
        diff_map.emplace_back(tmp_v_idx, v_ex[i]);
    }

    // Iterate over the derivative orders.
    for (std::uint32_t cur_order = 0; cur_order < order; ++cur_order) {
        // Locate the iterator in diff_map corresponding to the beginning
        // of the previous-order derivatives.
        tmp_v_idx.first = 0;
        tmp_v_idx.second.clear();
        if (cur_order != 0u) {
            tmp_v_idx.second.emplace_back(0, cur_order);
        }

        const auto prev_begin = search_diff_map(tmp_v_idx);
        assert(prev_begin != diff_map.end());

        // Store the previous-order derivatives into a separate
        // vector so that we can construct the decomposition.
        prev_diffs.clear();
        std::transform(prev_begin, diff_map.end(), std::back_inserter(prev_diffs),
                       [](const auto &p) { return p.second; });

        // For the purposes of the diff decomposition, the number of outputs
        // is the number of previous derivatives.
        const auto cur_nouts = prev_diffs.size();

        // Run the decomposition on the derivatives of the previous order.
        const auto [dc, nvars] = diff_decompose(prev_diffs);

        // Create the adjoints, the direct/reverse dependencies and the substitution map.
        const auto [adj, dep, revdep, subs_map] = diff_make_adj_dep(dc, nvars, cur_nouts);

        // Store the current diff_map size in order to (later) determine
        // where the set of derivatives for the current order begins.
        const auto orig_diff_map_size = diff_map.size();

        spdlog::stopwatch sw_inner;

        // NOTE: in order to choose between forward and reverse mode, we adopt the standard approach
        // of comparing the number of inputs and outputs. A more accurate (yet more expensive) approach
        // would be to do the computation in both modes (e.g., in parallel) and pick the mode which
        // results in the shortest decomposition. Perhaps we can consider this for a future extension.
        if (cur_nouts >= args.size()) {
            diff_tensors_forward_impl(diff_map, cur_nouts, dc, dep, revdep, adj, nvars, args, prev_begin,
                                      cur_order + 1u);
        } else {
            diff_tensors_reverse_impl(diff_map, cur_nouts, dc, dep, revdep, adj, nvars, args, prev_begin,
                                      cur_order + 1u);
        }

        // Determine the range in diff_map for the current-order derivatives.
        auto *cur_begin = diff_map.data() + orig_diff_map_size;
        auto *cur_end = diff_map.data() + diff_map.size();

        // Sort the derivatives for the current order.
        oneapi::tbb::parallel_sort(
            cur_begin, cur_end, [](const auto &p1, const auto &p2) { return dtens_sv_idx_cmp{}(p1.first, p2.first); });

        // NOTE: the derivatives we just added to diff_map are still expressed in terms of u variables.
        // We need to apply the substitution map subs_map in order to recover the expressions in terms
        // of the original variables. It is important that we do this now (rather than when constructing
        // the derivatives in diff_tensors_*_impl()) because now we can do the substitution in a vectorised
        // fashion, which greatly reduces the internal redundancy of the resulting expressions.

        // Create the vector of expressions for the substitution.
        std::vector<expression> subs_ret;
        for (auto *it = cur_begin; it != cur_end; ++it) {
            subs_ret.push_back(it->second);
        }

        // Do the substitution.
        subs_ret = subs(subs_ret, subs_map);

        // Replace the original expressions in diff_map.
        decltype(subs_ret.size()) i = 0;
        for (auto *it = cur_begin; i < subs_ret.size(); ++i, ++it) {
            it->second = subs_ret[i];
        }

        get_logger()->trace("dtens diff runtime for order {}: {}", cur_order + 1u, sw_inner);
    }

    get_logger()->trace("dtens creation runtime: {}", sw);

    // NOTE: it is unclear at this time if it makes sense here to unfix() the derivatives.
    // This would have only a cosmetic purpose, as 1) if these derivatives are iterated to higher orders,
    // during decomposition we will be unfixing anyway and 2) unfixing does not
    // imply normalisation, thus we are not changing the symbolic structure
    // of the expressions and pessimising their evaluation.
    //
    // Keep also in mind that at this time the derivatives are not
    // *fully* fixed, in the sense that the entries in subs_map are not themselves fixed, and thus when we do the
    // substition to create the final expressions, we have inserting unfixed expressions in the result. This ultimately
    // does not matter as subs() does not do any normalisation by default, but it would matter if we wanted to
    // manipulate further the expression of the derivatives while relying on fix() being applied correctly everywhere.
    // In any case, if we decide to unfix() here at some point we should also consider unfixing
    // the expressions returned by models, for consistency.

    // Assemble and return the result.
    dtens_map_t retval;
    retval.adopt_sequence(boost::container::ordered_unique_range_t{}, std::move(diff_map));

    // Check sorting.
    assert(std::is_sorted(retval.begin(), retval.end(),
                          [](const auto &p1, const auto &p2) { return dtens_sv_idx_cmp{}(p1.first, p2.first); }));
    // Check the variable indices.
    assert(std::all_of(retval.begin(), retval.end(), [&nargs](const auto &p) {
        return p.first.second.empty() || p.first.second.back().first < nargs;
    }));
    // No duplicates in the indices vectors.
    assert(std::adjacent_find(retval.begin(), retval.end(),
                              [](const auto &p1, const auto &p2) { return p1.first == p2.first; })
           == retval.end());

    return retval;
}

} // namespace

dtens diff_tensors(const std::vector<expression> &v_ex, const std::variant<diff_args, std::vector<expression>> &d_args,
                   std::uint32_t order)
{
    if (v_ex.empty()) {
        throw std::invalid_argument("Cannot compute the derivatives of a function with zero components");
    }

    // Extract/build the diff arguments.
    std::vector<expression> args;

    if (std::holds_alternative<std::vector<expression>>(d_args)) {
        args = std::get<std::vector<expression>>(d_args);
    } else {
        switch (std::get<diff_args>(d_args)) {
            case diff_args::all: {
                // NOTE: this can be simplified once get_variables() returns
                // a list of expressions, rather than strings.
                for (const auto &var : get_variables(v_ex)) {
                    args.emplace_back(var);
                }

                const auto params = get_params(v_ex);
                args.insert(args.end(), params.begin(), params.end());

                break;
            }
            case diff_args::vars:
                for (const auto &var : get_variables(v_ex)) {
                    args.emplace_back(var);
                }

                break;
            case diff_args::params:
                args = get_params(v_ex);

                break;
            default:
                throw std::invalid_argument("An invalid diff_args enumerator was passed to diff_tensors()");
        }
    }

    // Handle empty args.
    if (args.empty()) {
        throw std::invalid_argument("Cannot compute derivatives with respect to an empty set of arguments");
    }

    // Ensure that every expression in args is either a variable
    // or a param.
    if (std::any_of(args.begin(), args.end(), [](const auto &arg) {
            return !std::holds_alternative<variable>(arg.value()) && !std::holds_alternative<param>(arg.value());
        })) {
        throw std::invalid_argument("Derivatives can be computed only with respect to variables and/or parameters");
    }

    // Check if there are repeated entries in args.
    const fast_uset<expression, std::hash<expression>> args_set(args.begin(), args.end());
    if (args_set.size() != args.size()) {
        throw std::invalid_argument(
            fmt::format("Duplicate entries detected in the list of variables/parameters with respect to which the "
                        "derivatives are to be computed: {}",
                        args));
    }

    return dtens{dtens::impl{diff_tensors_impl(v_ex, args, order), std::move(args)}};
}

} // namespace detail

dtens::subrange::subrange(const iterator &begin, const iterator &end) : m_begin(begin), m_end(end) {}

dtens::subrange::subrange(const subrange &) = default;

dtens::subrange::subrange(subrange &&) noexcept = default;

dtens::subrange &dtens::subrange::operator=(const subrange &) = default;

dtens::subrange &dtens::subrange::operator=(subrange &&) noexcept = default;

// NOLINTNEXTLINE(performance-trivially-destructible)
dtens::subrange::~subrange() = default;

dtens::iterator dtens::subrange::begin() const
{
    return m_begin;
}

dtens::iterator dtens::subrange::end() const
{
    return m_end;
}

dtens::dtens(impl x) : p_impl(std::make_unique<impl>(std::move(x))) {}

dtens::dtens() : dtens(impl{}) {}

dtens::dtens(const dtens &other) : dtens(*other.p_impl) {}

dtens::dtens(dtens &&) noexcept = default;

dtens &dtens::operator=(const dtens &other)
{
    if (&other != this) {
        *this = dtens(other);
    }

    return *this;
}

dtens &dtens::operator=(dtens &&) noexcept = default;

dtens::~dtens() = default;

dtens::iterator dtens::begin() const
{
    return p_impl->m_map.begin();
}

dtens::iterator dtens::end() const
{
    return p_impl->m_map.end();
}

std::uint32_t dtens::get_order() const
{
    // First we handle the empty case.
    if (p_impl->m_map.empty()) {
        return 0;
    }

    // We can fetch the total derivative
    // order from the last derivative
    // in the map (specifically, it is
    // the last element in the indices
    // vector of the last derivative).
    const auto &sv = (end() - 1)->first.second;
    if (sv.empty()) {
        // NOTE: an empty index/order vector
        // at the end means that the maximum
        // diff order is zero and that we are
        // only storing the original function
        // components in the dtens object.
        return 0;
    } else {
        return sv.back().second;
    }
}

dtens::iterator dtens::find(const v_idx_t &vidx) const
{
    // First we handle the empty case.
    if (p_impl->m_map.empty()) {
        return end();
    }

    // vidx must at least contain the function component index.
    if (vidx.empty()) {
        return end();
    }

    // The size of vidx must be consistent with the number
    // of diff args.
    if (vidx.size() - 1u != get_nvars()) {
        return end();
    }

    // Turn vidx into sparse format.
    detail::dtens_sv_idx_t s_vidx{vidx[0], {}};
    for (decltype(vidx.size()) i = 1; i < vidx.size(); ++i) {
        if (vidx[i] != 0u) {
            s_vidx.second.emplace_back(boost::numeric_cast<std::uint32_t>(i - 1u), vidx[i]);
        }
    }

    // Lookup.
    return p_impl->m_map.find(s_vidx);
}

const expression &dtens::operator[](const v_idx_t &vidx) const
{
    const auto it = find(vidx);

    if (it == end()) {
        throw std::out_of_range(
            fmt::format("Cannot locate the derivative corresponding to the indices vector {}", vidx));
    }

    return it->second;
}

dtens::size_type dtens::index_of(const v_idx_t &vidx) const
{
    return index_of(find(vidx));
}

dtens::size_type dtens::index_of(const iterator &it) const
{
    return p_impl->m_map.index_of(it);
}

// Get a range containing all derivatives of the given order for all components.
dtens::subrange dtens::get_derivatives(std::uint32_t order) const
{
    // First we handle the empty case. This will return
    // an empty range.
    if (p_impl->m_map.empty()) {
        return subrange{begin(), end()};
    }

    // Create the indices vector corresponding to the first derivative
    // of component 0 for the given order in the map.
    detail::dtens_sv_idx_t s_vidx{0, {}};
    if (order != 0u) {
        s_vidx.second.emplace_back(0, order);
    }

    // Locate the corresponding derivative in the map.
    // NOTE: this could be end() for invalid order.
    const auto b = p_impl->m_map.find(s_vidx);

#if !defined(NDEBUG)

    if (order <= get_order()) {
        assert(b != end());
    } else {
        assert(b == end());
    }

#endif

    // Modify s_vidx so that it now refers to the last derivative
    // for the last component at the given order in the map.
    // NOTE: get_nouts() can return zero only if the internal
    // map is empty, and we handled this corner case earlier.
    assert(get_nouts() > 0u);
    s_vidx.first = get_nouts() - 1u;
    if (order != 0u) {
        assert(get_nvars() > 0u);
        s_vidx.second[0].first = get_nvars() - 1u;
    }

    // NOTE: this could be end() for invalid order.
    auto e = p_impl->m_map.find(s_vidx);

#if !defined(NDEBUG)

    if (order <= get_order()) {
        assert(e != end());
    } else {
        assert(e == end());
    }

#endif

    // Need to move 1 past, if possible,
    // to produce a half-open range.
    if (e != end()) {
        ++e;
    }

    return subrange{b, e};
}

// Get a range containing all derivatives of the given order for a component.
dtens::subrange dtens::get_derivatives(std::uint32_t component, std::uint32_t order) const
{
    // First we handle the empty case. This will return
    // an empty range.
    if (p_impl->m_map.empty()) {
        return subrange{begin(), end()};
    }

    // Create the indices vector corresponding to the first derivative
    // for the given order and component in the map.
    detail::dtens_sv_idx_t s_vidx{component, {}};
    if (order != 0u) {
        s_vidx.second.emplace_back(0, order);
    }

    // Locate the corresponding derivative in the map.
    // NOTE: this could be end() for invalid component/order.
    const auto b = p_impl->m_map.find(s_vidx);

#if !defined(NDEBUG)

    if (component < get_nouts() && order <= get_order()) {
        assert(b != end());
    } else {
        assert(b == end());
    }

#endif

    // Modify vidx so that it now refers to the last derivative
    // for the given order and component in the map.
    assert(get_nvars() > 0u);
    if (order != 0u) {
        s_vidx.second[0].first = get_nvars() - 1u;
    }

    // NOTE: this could be end() for invalid component/order.
    auto e = p_impl->m_map.find(s_vidx);

#if !defined(NDEBUG)

    if (component < get_nouts() && order <= get_order()) {
        assert(e != end());
    } else {
        assert(e == end());
    }

#endif

    // Need to move 1 past, if possible,
    // to produce a half-open range.
    if (e != end()) {
        ++e;
    }

    return subrange{b, e};
}

std::vector<expression> dtens::get_gradient() const
{
    if (get_nouts() != 1u) {
        throw std::invalid_argument(fmt::format("The gradient can be requested only for a function with a single "
                                                "output, but the number of outputs is instead {}",
                                                get_nouts()));
    }

    if (get_order() == 0u) {
        throw std::invalid_argument("First-order derivatives are not available");
    }

    const auto sr = get_derivatives(0, 1);
    std::vector<expression> retval;
    retval.reserve(get_nvars());
    std::transform(sr.begin(), sr.end(), std::back_inserter(retval), [](const auto &p) { return p.second; });

    assert(retval.size() == get_nvars());

    return retval;
}

std::vector<expression> dtens::get_jacobian() const
{
    if (get_nouts() == 0u) {
        throw std::invalid_argument("Cannot return the Jacobian of a function with no outputs");
    }

    if (get_order() == 0u) {
        throw std::invalid_argument("First-order derivatives are not available");
    }

    const auto sr = get_derivatives(1);
    std::vector<expression> retval;
    retval.reserve(boost::safe_numerics::safe<decltype(retval.size())>(get_nvars()) * get_nouts());
    std::transform(sr.begin(), sr.end(), std::back_inserter(retval), [](const auto &p) { return p.second; });

    assert(retval.size() == boost::safe_numerics::safe<decltype(retval.size())>(get_nvars()) * get_nouts());

    return retval;
}

std::uint32_t dtens::get_nvars() const
{
    // NOTE: we ensure in the diff_tensors() implementation
    // that the number of diff variables is representable
    // by std::uint32_t.
    auto ret = static_cast<std::uint32_t>(get_args().size());

#if !defined(NDEBUG)

    if (p_impl->m_map.empty()) {
        assert(ret == 0u);
    }

#endif

    return ret;
}

namespace detail
{

namespace
{

// The indices vector corresponding
// to the first derivative of order 1
// of the first component.
// NOLINTNEXTLINE(cert-err58-cpp)
const dtens_sv_idx_t s_vidx_001{0, {{0, 1}}};

} // namespace

} // namespace detail

std::uint32_t dtens::get_nouts() const
{
    if (p_impl->m_map.empty()) {
        return 0;
    }

    // Try to find in the map the indices vector corresponding
    // to the first derivative of order 1 of the first component.
    const auto it = p_impl->m_map.find(detail::s_vidx_001);

    // NOTE: the number of outputs is always representable by
    // std::uint32_t, otherwise we could not index the function
    // components via std::uint32_t.
    if (it == end()) {
        // There are no derivatives in the map, which
        // means that the order must be zero and that the
        // size of the map gives directly the number of components.
        assert(get_order() == 0u);
        return static_cast<std::uint32_t>(p_impl->m_map.size());
    } else {
        assert(get_order() > 0u);
        return static_cast<std::uint32_t>(p_impl->m_map.index_of(it));
    }
}

dtens::size_type dtens::size() const
{
    return p_impl->m_map.size();
}

const std::vector<expression> &dtens::get_args() const
{
    return p_impl->m_args;
}

void dtens::save(boost::archive::binary_oarchive &ar, unsigned) const
{
    ar << p_impl;
}

void dtens::load(boost::archive::binary_iarchive &ar, unsigned)
{
    try {
        ar >> p_impl;
        // LCOV_EXCL_START
    } catch (...) {
        *this = dtens{};
        throw;
    }
    // LCOV_EXCL_STOP
}

std::ostream &operator<<(std::ostream &os, const dtens &dt)
{
    os << "Highest diff order: " << dt.get_order() << '\n';
    os << "Number of outputs : " << dt.get_nouts() << '\n';
    os << "Diff arguments    : " << fmt::format("{}", dt.get_args()) << '\n';

    return os;
}

HEYOKA_END_NAMESPACE
