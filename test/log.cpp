// Copyright 2020, 2021, 2022 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
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

#include <heyoka/expression.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/math/log.hpp>
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
#if LLVM_VERSION_MAJOR == 13 || LLVM_VERSION_MAJOR == 14
    std::numeric_limits<long double>::digits == 64
#else
    false
#endif
    ;

TEST_CASE("log s11n")
{
    std::stringstream ss;

    auto [x] = make_vars("x");

    auto ex = log(x);

    {
        boost::archive::binary_oarchive oa(ss);

        oa << ex;
    }

    ex = 0_dbl;

    {
        boost::archive::binary_iarchive ia(ss);

        ia >> ex;
    }

    REQUIRE(ex == log(x));
}

TEST_CASE("log diff")
{
    auto [x, y] = make_vars("x", "y");

    REQUIRE(diff(log(x * x - y), x) == (2. * x) / (x * x - y));
    REQUIRE(diff(log(x * x - y), y) == -1_dbl / (x * x - y));

    REQUIRE(diff(log(par[0] * par[0] - y), par[0]) == (2. * par[0]) / (par[0] * par[0] - y));
    REQUIRE(diff(log(x * x - par[1]), par[1]) == -1_dbl / (x * x - par[1]));
}

TEST_CASE("cfunc")
{
    auto tester = [](auto fp_x, unsigned opt_level, bool high_accuracy, bool compact_mode) {
        using std::log;

        using fp_t = decltype(fp_x);

        auto [x] = make_vars("x");

        std::uniform_real_distribution<double> rdist(.1, 10.);

        auto gen = [&rdist]() { return static_cast<fp_t>(rdist(rng)); };

        std::vector<fp_t> outs, ins, pars;

        for (auto batch_size : {1u, 2u, 4u, 5u}) {
            if (batch_size != 1u && std::is_same_v<fp_t, long double> && skip_batch_ld) {
                continue;
            }

            outs.resize(batch_size * 3u);
            ins.resize(batch_size);
            pars.resize(batch_size);

            std::generate(ins.begin(), ins.end(), gen);
            std::generate(pars.begin(), pars.end(), gen);

            llvm_state s{kw::opt_level = opt_level};

            add_cfunc<fp_t>(s, "cfunc", {log(x), log(expression{fp_t(.5)}), log(par[0])}, kw::batch_size = batch_size,
                            kw::high_accuracy = high_accuracy, kw::compact_mode = compact_mode);

            if (opt_level == 0u && compact_mode) {
                REQUIRE(boost::contains(s.get_ir(), "heyoka.llvm_c_eval.log."));
            }

            s.compile();

            auto *cf_ptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("cfunc"));

            cf_ptr(outs.data(), ins.data(), pars.data());

            for (auto i = 0u; i < batch_size; ++i) {
                REQUIRE(outs[i] == approximately(log(ins[i]), fp_t(100)));
                REQUIRE(outs[i + batch_size] == approximately(log(static_cast<fp_t>(.5)), fp_t(100)));
                REQUIRE(outs[i + 2u * batch_size] == approximately(log(pars[i]), fp_t(100)));
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
