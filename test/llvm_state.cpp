// Copyright 2020, 2021, 2022 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <initializer_list>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/expression.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/math/binary_op.hpp>
#include <heyoka/s11n.hpp>
#include <heyoka/taylor.hpp>

#include "catch.hpp"
#include "test_utils.hpp"

using namespace heyoka;
using namespace heyoka_test;

TEST_CASE("simd size")
{
    REQUIRE(recommended_simd_size<double>() > 0u);
    REQUIRE(recommended_simd_size<long double>() > 0u);

#if defined(HEYOKA_HAVE_REAL128)
    REQUIRE(recommended_simd_size<mppp::real128>() > 0u);
#endif

#if defined(__GNUC__)

#if defined(__amd64__) || defined(__aarch64__)
    REQUIRE(recommended_simd_size<double>() >= 2u);
#endif

#endif
}

TEST_CASE("empty state")
{
    llvm_state s;
    std::cout << s << '\n';
    std::cout << s.get_ir() << '\n';

    // Print also some info on the FP types.
    std::cout << "Double digits     : " << std::numeric_limits<double>::digits << '\n';
    std::cout << "Long double digits: " << std::numeric_limits<long double>::digits << '\n';
}

TEST_CASE("copy semantics")
{
    auto [x, y] = make_vars("x", "y");

    // Copy without compilation.
    {
        std::vector<double> jet{2, 3, 0, 0};

        llvm_state s{kw::mname = "sample state", kw::opt_level = 2u, kw::fast_math = true};

        REQUIRE(s.module_name() == "sample state");
        REQUIRE(s.opt_level() == 2u);
        REQUIRE(s.fast_math());
        REQUIRE(!s.is_compiled());

        taylor_add_jet<double>(s, "jet", {x * y, y * x}, 1, 1, true, false);

        auto s2 = s;

        REQUIRE(s2.module_name() == "sample state");
        REQUIRE(s2.opt_level() == 2u);
        REQUIRE(s2.fast_math());
        REQUIRE(!s2.is_compiled());

        s2.compile();

        auto jptr = reinterpret_cast<void (*)(double *, const double *, const double *)>(s2.jit_lookup("jet"));

        jptr(jet.data(), nullptr, nullptr);

        REQUIRE(jet[0] == 2);
        REQUIRE(jet[1] == 3);
        REQUIRE(jet[2] == 6);
        REQUIRE(jet[3] == 6);
    }

    // Compile, but don't generate code, and copy.
    {
        std::vector<double> jet{2, 3, 0, 0};

        llvm_state s{kw::mname = "sample state", kw::opt_level = 2u, kw::fast_math = true};

        taylor_add_jet<double>(s, "jet", {x * y, y * x}, 1, 1, true, false);

        s.compile();

        auto s2 = s;

        REQUIRE(s2.module_name() == "sample state");
        REQUIRE(s2.opt_level() == 2u);
        REQUIRE(s2.fast_math());
        REQUIRE(s2.is_compiled());

        auto jptr = reinterpret_cast<void (*)(double *, const double *, const double *)>(s2.jit_lookup("jet"));

        jptr(jet.data(), nullptr, nullptr);

        REQUIRE(jet[0] == 2);
        REQUIRE(jet[1] == 3);
        REQUIRE(jet[2] == 6);
        REQUIRE(jet[3] == 6);
    }

    // Compile, generate code, and copy.
    {
        std::vector<double> jet{2, 3, 0, 0};

        llvm_state s{kw::mname = "sample state", kw::opt_level = 2u, kw::fast_math = true};

        taylor_add_jet<double>(s, "jet", {x * y, y * x}, 1, 1, true, false);

        s.compile();

        auto jptr = reinterpret_cast<void (*)(double *, const double *, const double *)>(s.jit_lookup("jet"));

        auto s2 = s;

        REQUIRE(s2.module_name() == "sample state");
        REQUIRE(s2.opt_level() == 2u);
        REQUIRE(s2.fast_math());
        REQUIRE(s2.is_compiled());

        jptr = reinterpret_cast<void (*)(double *, const double *, const double *)>(s2.jit_lookup("jet"));

        jptr(jet.data(), nullptr, nullptr);

        REQUIRE(jet[0] == 2);
        REQUIRE(jet[1] == 3);
        REQUIRE(jet[2] == 6);
        REQUIRE(jet[3] == 6);
    }
}

TEST_CASE("get object code")
{
    using Catch::Matchers::Message;

    auto [x, y] = make_vars("x", "y");

    {
        llvm_state s{kw::mname = "sample state", kw::opt_level = 2u, kw::fast_math = true};

        taylor_add_jet<double>(s, "jet", {x * y, y * x}, 1, 1, true, false);

        REQUIRE_THROWS_MATCHES(
            s.get_object_code(), std::invalid_argument,
            Message("Cannot extract the object code from an llvm_state which has not been compiled yet"));

        s.compile();

        REQUIRE_THROWS_MATCHES(
            s.get_object_code(), std::invalid_argument,
            Message("Cannot extract the object code from an llvm_state if the binary code has not been generated yet"));

        s.jit_lookup("jet");

        REQUIRE(!s.get_object_code().empty());
    }
}

TEST_CASE("s11n")
{
    auto [x, y] = make_vars("x", "y");

    // Def-cted state, no compilation, no object file.
    {
        std::stringstream ss;

        llvm_state s;

        const auto orig_ir = s.get_ir();

        {
            boost::archive::binary_oarchive oa(ss);

            oa << s;
        }

        s = llvm_state{kw::mname = "sample state", kw::opt_level = 2u, kw::fast_math = true};

        {
            boost::archive::binary_iarchive ia(ss);

            ia >> s;
        }

        REQUIRE(!s.is_compiled());
        REQUIRE(s.get_ir() == orig_ir);
        REQUIRE(s.module_name() == "");
        REQUIRE(s.opt_level() == 3u);
        REQUIRE(s.fast_math() == false);
    }

    // Compiled state but without object file.
    {
        std::stringstream ss;

        llvm_state s;

        taylor_add_jet<double>(s, "jet", {x * y, y * x}, 1, 1, true, false);

        const auto orig_ir = s.get_ir();

        s.compile();

        {
            boost::archive::binary_oarchive oa(ss);

            oa << s;
        }

        s = llvm_state{kw::mname = "sample state", kw::opt_level = 2u, kw::fast_math = true};

        {
            boost::archive::binary_iarchive ia(ss);

            ia >> s;
        }

        REQUIRE(s.is_compiled());
        REQUIRE(s.get_ir() == orig_ir);
        REQUIRE(s.module_name() == "");
        REQUIRE(s.opt_level() == 3u);
        REQUIRE(s.fast_math() == false);
    }

    // Compiled state with object file.
    {
        std::stringstream ss;

        llvm_state s;

        taylor_add_jet<double>(s, "jet", {sub(2_dbl, 3_dbl), x + y}, 1, 1, true, false);

        const auto orig_ir = s.get_ir();

        s.compile();

        s.jit_lookup("jet");

        {
            boost::archive::binary_oarchive oa(ss);

            oa << s;
        }

        s = llvm_state{kw::mname = "sample state", kw::opt_level = 2u, kw::fast_math = true};

        {
            boost::archive::binary_iarchive ia(ss);

            ia >> s;
        }

        REQUIRE(s.is_compiled());
        REQUIRE(s.get_ir() == orig_ir);
        REQUIRE(s.module_name() == "");
        REQUIRE(s.opt_level() == 3u);
        REQUIRE(s.fast_math() == false);

        auto jptr = reinterpret_cast<void (*)(double *, const double *, const double *)>(s.jit_lookup("jet"));

        std::vector<double> jet{2, 3};
        jet.resize(4);

        jptr(jet.data(), nullptr, nullptr);

        REQUIRE(jet[0] == 2);
        REQUIRE(jet[1] == 3);
        REQUIRE(jet[2] == approximately(-1.));
        REQUIRE(jet[3] == approximately(5.));
    }
}

TEST_CASE("make_similar")
{
    auto [x, y] = make_vars("x", "y");

    llvm_state s{kw::mname = "sample state", kw::opt_level = 2u, kw::fast_math = true};
    taylor_add_jet<double>(s, "jet", {sub(2_dbl, 3_dbl), x + y}, 1, 1, true, false);

    s.compile();

    REQUIRE(s.module_name() == "sample state");
    REQUIRE(s.opt_level() == 2u);
    REQUIRE(s.fast_math());
    REQUIRE(s.is_compiled());

    auto s2 = s.make_similar();

    REQUIRE(s2.module_name() == "sample state");
    REQUIRE(s2.opt_level() == 2u);
    REQUIRE(s2.fast_math());
    REQUIRE(!s2.is_compiled());
    REQUIRE(s.get_ir() != s2.get_ir());
}
