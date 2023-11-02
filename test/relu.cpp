// Copyright 2020, 2021, 2022, 2023 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <limits>
#include <random>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>

#include <llvm/Config/llvm-config.h>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#if defined(HEYOKA_HAVE_REAL)

#include <mp++/real.hpp>

#endif

#include <heyoka/expression.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/math/relu.hpp>
#include <heyoka/s11n.hpp>

#include "catch.hpp"
#include "test_utils.hpp"

static std::mt19937 rng;

using namespace heyoka;
using namespace heyoka_test;

const auto fp_types = std::tuple<double
#if !defined(HEYOKA_ARCH_PPC)
                                 ,
                                 long double
#endif
#if defined(HEYOKA_HAVE_REAL128)
                                 ,
                                 mppp::real128
#endif
                                 >{};

constexpr bool skip_batch_ld =
#if LLVM_VERSION_MAJOR >= 13 && LLVM_VERSION_MAJOR <= 17
    std::numeric_limits<long double>::digits == 64
#else
    false
#endif
    ;

template <typename T>
T cpp_relu(T x)
{
    return x > 0 ? x : T(0);
}

template <typename T>
T cpp_relup(T x)
{
    return x > 0 ? T(1) : T(0);
}

TEST_CASE("def ctor")
{
    {
        detail::relu_impl k;

        REQUIRE(k.args().size() == 1u);
        REQUIRE(k.args()[0] == 0_dbl);
    }

    {
        detail::relup_impl k;

        REQUIRE(k.args().size() == 1u);
        REQUIRE(k.args()[0] == 0_dbl);
    }
}

TEST_CASE("normalise")
{
    {
        auto ex = relu(fix(.1_dbl));
        ex = normalise(unfix(ex));
        REQUIRE(ex == .1_dbl);
    }

    {
        auto ex = relup(fix(-.1_dbl));
        ex = normalise(unfix(ex));
        REQUIRE(ex == 0_dbl);
    }
}

TEST_CASE("diff")
{
    auto [x, y] = make_vars("x", "y");

    REQUIRE(diff(relu(x), x) == relup(x));
    REQUIRE(diff(relup(x), x) == 0_dbl);

    REQUIRE(diff(relu(x * y), x) == y * relup(x * y));
    REQUIRE(diff(relu(x * y), par[0]) == 0_dbl);
    REQUIRE(diff(relu(x * par[0]), par[0]) == x * relup(x * par[0]));

    REQUIRE(diff(relup(x * y), x) == 0_dbl);
    REQUIRE(diff(relup(x * y), par[0]) == 0_dbl);
    REQUIRE(diff(relup(x * par[0]), par[0]) == 0_dbl);
}

TEST_CASE("constant fold")
{
    REQUIRE(relu(1.1_dbl) == 1.1_dbl);
    REQUIRE(relu(-1.1_dbl) == 0_dbl);

    REQUIRE(relup(1.1_dbl) == 1_dbl);
    REQUIRE(relup(-1.1_dbl) == 0_dbl);
}

TEST_CASE("s11n")
{
    {
        std::stringstream ss;

        auto [x, y] = make_vars("x", "y");

        auto ex = relu(x + y);

        {
            boost::archive::binary_oarchive oa(ss);

            oa << ex;
        }

        ex = 0_dbl;

        {
            boost::archive::binary_iarchive ia(ss);

            ia >> ex;
        }

        REQUIRE(ex == relu(x + y));
    }

    {
        std::stringstream ss;

        auto [x, y] = make_vars("x", "y");

        auto ex = relup(x + y);

        {
            boost::archive::binary_oarchive oa(ss);

            oa << ex;
        }

        ex = 0_dbl;

        {
            boost::archive::binary_iarchive ia(ss);

            ia >> ex;
        }

        REQUIRE(ex == relup(x + y));
    }
}

TEST_CASE("cfunc")
{
    auto tester = [](auto fp_x, unsigned opt_level, bool high_accuracy, bool compact_mode) {
        using fp_t = decltype(fp_x);

        auto [x] = make_vars("x");

        std::uniform_real_distribution<double> x_dist(-10, 10);

        std::vector<fp_t> outs, ins, pars;

        for (auto batch_size : {1u, 2u, 4u, 5u}) {
            if (batch_size != 1u && std::is_same_v<fp_t, long double> && skip_batch_ld) {
                continue;
            }

            outs.resize(batch_size * 4u);
            ins.resize(batch_size);
            pars.resize(batch_size * 2u);

            llvm_state s{kw::opt_level = opt_level};

            add_cfunc<fp_t>(s, "cfunc", {relu(x), relu(par[0]), relup(x), relup(par[1])}, kw::batch_size = batch_size,
                            kw::high_accuracy = high_accuracy, kw::compact_mode = compact_mode);

            if (opt_level == 0u && compact_mode) {
                REQUIRE(boost::contains(s.get_ir(), "heyoka.llvm_c_eval.relu."));
                REQUIRE(boost::contains(s.get_ir(), "heyoka.llvm_c_eval.relup."));
            }

            s.compile();

            auto *cf_ptr
                = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("cfunc"));

            for (auto niter = 0; niter < 100; ++niter) {
                for (auto i = 0u; i < batch_size; ++i) {
                    // Generate the xs.
                    ins[i] = x_dist(rng);

                    // Generate the pars.
                    pars[i] = x_dist(rng);
                    pars[i + batch_size] = x_dist(rng);
                }

                cf_ptr(outs.data(), ins.data(), pars.data(), nullptr);

                for (auto i = 0u; i < batch_size; ++i) {
                    REQUIRE(outs[i] == cpp_relu(ins[i]));
                    REQUIRE(outs[i + batch_size] == cpp_relu(pars[i]));
                    REQUIRE(outs[i + 2u * batch_size] == cpp_relup(ins[i]));
                    REQUIRE(outs[i + 3u * batch_size] == cpp_relup(pars[i + batch_size]));
                }
            }
        }
    };

    for (auto cm : {false, true}) {
        for (auto f : {false, true}) {
            tuple_for_each(fp_types, [&tester, f, cm](auto x) { tester(x, 0, f, cm); });
            tuple_for_each(fp_types, [&tester, f, cm](auto x) { tester(x, 1, f, cm); });
            tuple_for_each(fp_types, [&tester, f, cm](auto x) { tester(x, 2, f, cm); });
            tuple_for_each(fp_types, [&tester, f, cm](auto x) { tester(x, 3, f, cm); });
        }
    }
}

#if defined(HEYOKA_HAVE_REAL)

TEST_CASE("cfunc mp")
{
    using fp_t = mppp::real;

    const auto prec = 237;

    auto [x] = make_vars("x");

    std::uniform_real_distribution<double> x_dist(-10, 10);

    std::vector<fp_t> outs, ins, pars;

    outs.resize(4u, mppp::real{0, prec});
    ins.resize(1u);
    pars.resize(2u);

    for (auto compact_mode : {false, true}) {
        for (auto opt_level : {0u, 1u, 2u, 3u}) {
            llvm_state s{kw::opt_level = opt_level};

            add_cfunc<fp_t>(s, "cfunc", {relu(x), relu(par[0]), relup(x), relup(par[1])},
                            kw::compact_mode = compact_mode, kw::prec = prec);

            if (opt_level == 0u && compact_mode) {
                REQUIRE(boost::contains(s.get_ir(), "heyoka.llvm_c_eval.relu."));
                REQUIRE(boost::contains(s.get_ir(), "heyoka.llvm_c_eval.relup."));
            }

            s.compile();

            auto *cf_ptr
                = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("cfunc"));

            // Generate the x and pars.
            ins[0] = mppp::real{x_dist(rng), prec};
            pars[0] = mppp::real{x_dist(rng), prec};
            pars[1] = mppp::real{x_dist(rng), prec};

            cf_ptr(outs.data(), ins.data(), pars.data(), nullptr);

            REQUIRE(outs[0] == cpp_relu(ins[0]));
            REQUIRE(outs[1] == cpp_relu(pars[0]));
            REQUIRE(outs[2] == cpp_relup(ins[0]));
            REQUIRE(outs[3] == cpp_relup(pars[1]));
        }
    }
}

#endif
