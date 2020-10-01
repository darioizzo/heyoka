// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <cmath>
#include <initializer_list>
#include <random>
#include <tuple>
#include <vector>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/expression.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/math_functions.hpp>
#include <heyoka/number.hpp>
#include <heyoka/taylor.hpp>

#include "catch.hpp"
#include "test_utils.hpp"

static std::mt19937 rng;

using namespace heyoka;
using namespace heyoka_test;

const auto fp_types = std::tuple<double, long double
#if defined(HEYOKA_HAVE_REAL128)
                                 ,
                                 mppp::real128
#endif
                                 >{};

template <typename T, typename U>
void compare_batch_scalar(std::initializer_list<U> sys, unsigned opt_level, bool high_accuracy)
{
    const auto batch_size = 23u;

    llvm_state s{kw::opt_level = opt_level};

    taylor_add_jet<T>(s, "jet_batch", sys, 3, batch_size, high_accuracy);
    taylor_add_jet<T>(s, "jet_scalar", sys, 3, 1, high_accuracy);

    s.compile();

    auto jptr_batch = reinterpret_cast<void (*)(T *)>(s.jit_lookup("jet_batch"));
    auto jptr_scalar = reinterpret_cast<void (*)(T *)>(s.jit_lookup("jet_scalar"));

    std::vector<T> jet_batch;
    jet_batch.resize(8 * batch_size);
    std::uniform_real_distribution<float> dist(.1f, 20.f);
    std::generate(jet_batch.begin(), jet_batch.end(), [&dist]() { return T{dist(rng)}; });

    std::vector<T> jet_scalar;
    jet_scalar.resize(8);

    jptr_batch(jet_batch.data());

    for (auto batch_idx = 0u; batch_idx < batch_size; ++batch_idx) {
        // Assign the initial values of x and y.
        for (auto i = 0u; i < 2u; ++i) {
            jet_scalar[i] = jet_batch[i * batch_size + batch_idx];
        }

        jptr_scalar(jet_scalar.data());

        for (auto i = 2u; i < 8u; ++i) {
            REQUIRE(jet_scalar[i] == approximately(jet_batch[i * batch_size + batch_idx]));
        }
    }
}

TEST_CASE("taylor log")
{
    auto tester = [](auto fp_x, unsigned opt_level, bool high_accuracy) {
        using std::log;

        using fp_t = decltype(fp_x);

        using Catch::Matchers::Message;

        auto x = "x"_var, y = "y"_var;

        // Number tests.
        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {log(expression{number{fp_t(2)}}), x + y}, 1, 1, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(4);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(log(fp_t{2})));
            REQUIRE(jet[3] == 5);
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {log(expression{number{fp_t(2)}}), x + y}, 1, 2, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-2}, fp_t{3}, fp_t{-3}};
            jet.resize(8);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -2);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == -3);

            REQUIRE(jet[4] == approximately(log(fp_t{2})));
            REQUIRE(jet[5] == approximately(log(fp_t{2})));

            REQUIRE(jet[6] == 5);
            REQUIRE(jet[7] == -5);
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {log(expression{number{fp_t(2)}}), x + y}, 2, 1, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(6);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(log(fp_t{2})));
            REQUIRE(jet[3] == 5);
            REQUIRE(jet[4] == 0);
            REQUIRE(jet[5] == approximately(fp_t{1} / 2 * (jet[3] + log(fp_t{2}))));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {log(expression{number{fp_t(2)}}), x + y}, 2, 2, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-2}, fp_t{3}, fp_t{-3}};
            jet.resize(12);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -2);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == -3);

            REQUIRE(jet[4] == approximately(log(fp_t{2})));
            REQUIRE(jet[5] == approximately(log(fp_t{2})));

            REQUIRE(jet[6] == 5);
            REQUIRE(jet[7] == -5);

            REQUIRE(jet[8] == 0);
            REQUIRE(jet[9] == 0);

            REQUIRE(jet[10] == approximately(fp_t{1} / 2 * (jet[6] + log(fp_t{2}))));
            REQUIRE(jet[11] == approximately(fp_t{1} / 2 * (jet[7] + log(fp_t{2}))));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {log(expression{number{fp_t(2)}}), x + y}, 3, 3, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-2}, fp_t{1}, fp_t{3}, fp_t{-3}, fp_t{0}};
            jet.resize(24);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -2);
            REQUIRE(jet[2] == 1);

            REQUIRE(jet[3] == 3);
            REQUIRE(jet[4] == -3);
            REQUIRE(jet[5] == 0);

            REQUIRE(jet[6] == approximately(log(fp_t{2})));
            REQUIRE(jet[7] == approximately(log(fp_t{2})));
            REQUIRE(jet[8] == approximately(log(fp_t{2})));

            REQUIRE(jet[9] == 5);
            REQUIRE(jet[10] == -5);
            REQUIRE(jet[11] == 1);

            REQUIRE(jet[12] == 0);
            REQUIRE(jet[13] == 0);
            REQUIRE(jet[14] == 0);

            REQUIRE(jet[15] == approximately(fp_t{1} / 2 * (jet[9] + log(fp_t{2}))));
            REQUIRE(jet[16] == approximately(fp_t{1} / 2 * (jet[10] + log(fp_t{2}))));
            REQUIRE(jet[17] == approximately(fp_t{1} / 2 * (jet[11] + log(fp_t{2}))));

            REQUIRE(jet[18] == 0);
            REQUIRE(jet[19] == 0);
            REQUIRE(jet[20] == 0);

            REQUIRE(jet[21] == approximately(fp_t{1} / 6 * (2 * jet[15])));
            REQUIRE(jet[22] == approximately(fp_t{1} / 6 * (2 * jet[16])));
            REQUIRE(jet[23] == approximately(fp_t{1} / 6 * (2 * jet[17])));
        }

        // Do the batch/scalar comparison.
        compare_batch_scalar<fp_t>({log(expression{number{fp_t(2)}}), x + y}, opt_level, high_accuracy);

        // Variable tests.
        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {log(y), log(x)}, 1, 1, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(4);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(log(fp_t{3})));
            REQUIRE(jet[3] == approximately(log(fp_t{2})));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {log(y), log(x)}, 1, 2, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{4}, fp_t{3}, fp_t{5}};
            jet.resize(8);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 4);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 5);

            REQUIRE(jet[4] == approximately(log(fp_t{3})));
            REQUIRE(jet[5] == approximately(log(fp_t{5})));

            REQUIRE(jet[6] == approximately(log(fp_t{2})));
            REQUIRE(jet[7] == approximately(log(fp_t{4})));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {log(y), log(x)}, 2, 1, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(6);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(log(fp_t{3})));
            REQUIRE(jet[3] == approximately(log(fp_t{2})));
            REQUIRE(jet[4] == approximately(fp_t{1} / 2 * jet[3] / jet[1]));
            REQUIRE(jet[5] == approximately(fp_t{1} / 2 * jet[2] / jet[0]));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {log(y), log(x)}, 2, 2, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{4}, fp_t{3}, fp_t{5}};
            jet.resize(12);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 4);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 5);

            REQUIRE(jet[4] == approximately(log(fp_t{3})));
            REQUIRE(jet[5] == approximately(log(fp_t{5})));

            REQUIRE(jet[6] == approximately(log(fp_t{2})));
            REQUIRE(jet[7] == approximately(log(fp_t{4})));

            REQUIRE(jet[8] == approximately(fp_t{1} / 2 * jet[6] / jet[2]));
            REQUIRE(jet[9] == approximately(fp_t{1} / 2 * jet[7] / jet[3]));

            REQUIRE(jet[10] == approximately(fp_t{1} / 2 * jet[4] / jet[0]));
            REQUIRE(jet[11] == approximately(fp_t{1} / 2 * jet[5] / jet[1]));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {log(y), log(x)}, 3, 3, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{4}, fp_t{3}, fp_t{3}, fp_t{5}, fp_t{6}};
            jet.resize(24);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 4);
            REQUIRE(jet[2] == 3);

            REQUIRE(jet[3] == 3);
            REQUIRE(jet[4] == 5);
            REQUIRE(jet[5] == 6);

            REQUIRE(jet[6] == approximately(log(fp_t{3})));
            REQUIRE(jet[7] == approximately(log(fp_t{5})));
            REQUIRE(jet[8] == approximately(log(fp_t{6})));

            REQUIRE(jet[9] == approximately(log(fp_t{2})));
            REQUIRE(jet[10] == approximately(log(fp_t{4})));
            REQUIRE(jet[11] == approximately(log(fp_t{3})));

            REQUIRE(jet[12] == approximately(fp_t{1} / 2 * jet[9] / jet[3]));
            REQUIRE(jet[13] == approximately(fp_t{1} / 2 * jet[10] / jet[4]));
            REQUIRE(jet[14] == approximately(fp_t{1} / 2 * jet[11] / jet[5]));

            REQUIRE(jet[15] == approximately(fp_t{1} / 2 * jet[6] / jet[0]));
            REQUIRE(jet[16] == approximately(fp_t{1} / 2 * jet[7] / jet[1]));
            REQUIRE(jet[17] == approximately(fp_t{1} / 2 * jet[8] / jet[2]));

            REQUIRE(jet[18]
                    == approximately(fp_t{1} / 6 * (2 * jet[15] * jet[3] - jet[9] * jet[9]) / (jet[3] * jet[3])));
            REQUIRE(jet[19]
                    == approximately(fp_t{1} / 6 * (2 * jet[16] * jet[4] - jet[10] * jet[10]) / (jet[4] * jet[4])));
            REQUIRE(jet[20]
                    == approximately(fp_t{1} / 6 * (2 * jet[17] * jet[5] - jet[11] * jet[11]) / (jet[5] * jet[5])));

            REQUIRE(jet[21]
                    == approximately(fp_t{1} / 6 * (2 * jet[12] * jet[0] - jet[6] * jet[6]) / (jet[0] * jet[0])));
            REQUIRE(jet[22]
                    == approximately(fp_t{1} / 6 * (2 * jet[13] * jet[1] - jet[7] * jet[7]) / (jet[1] * jet[1])));
            REQUIRE(jet[23]
                    == approximately(fp_t{1} / 6 * (2 * jet[14] * jet[2] - jet[8] * jet[8]) / (jet[2] * jet[2])));
        }

        // Do the batch/scalar comparison.
        compare_batch_scalar<fp_t>({log(y), log(x)}, opt_level, high_accuracy);
    };

    for (auto f : {false, true}) {
        tuple_for_each(fp_types, [&tester, f](auto x) { tester(x, 0, f); });
        tuple_for_each(fp_types, [&tester, f](auto x) { tester(x, 1, f); });
        tuple_for_each(fp_types, [&tester, f](auto x) { tester(x, 2, f); });
        tuple_for_each(fp_types, [&tester, f](auto x) { tester(x, 3, f); });
    }
}