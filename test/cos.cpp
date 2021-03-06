// Copyright 2020, 2021 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <sstream>

#include <heyoka/expression.hpp>
#include <heyoka/math/cos.hpp>
#include <heyoka/math/neg.hpp>

#include "catch.hpp"

using namespace heyoka;

TEST_CASE("cos neg simpl")
{
    auto [x, y] = make_vars("x", "y");

    REQUIRE(cos(-(x + y)) == cos(x + y));
    REQUIRE(cos(neg(x + y)) == cos(x + y));
    REQUIRE(cos(neg(neg(x + y))) == cos(x + y));
    REQUIRE(cos(neg(neg(par[0]))) == cos(par[0]));
}
