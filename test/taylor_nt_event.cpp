// Copyright 2020, 2021 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <sstream>
#include <tuple>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/lexical_cast.hpp>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/expression.hpp>
#include <heyoka/math/sin.hpp>
#include <heyoka/math/square.hpp>
#include <heyoka/taylor.hpp>

#include "catch.hpp"
#include "test_utils.hpp"

using namespace heyoka;
using namespace heyoka_test;

const auto fp_types = std::tuple<double, long double
#if defined(HEYOKA_HAVE_REAL128)
                                 ,
                                 mppp::real128
#endif
                                 >{};

// A test case to check that the propagation codepath
// with events produces results identical results
// to the no-events codepath.
TEST_CASE("taylor nte match")
{
    auto tester = [](auto fp_x, unsigned opt_level, bool high_accuracy, bool compact_mode) {
        using fp_t = decltype(fp_x);

        auto [x, v] = make_vars("x", "v");

        using ev_t = typename taylor_adaptive<fp_t>::nt_event_t;

        auto ta_ev = taylor_adaptive<fp_t>{{prime(x) = v, prime(v) = -9.8 * sin(x)},
                                           {fp_t(-0.25), fp_t(0.)},
                                           kw::opt_level = opt_level,
                                           kw::high_accuracy = high_accuracy,
                                           kw::compact_mode = compact_mode,
                                           kw::nt_events = {ev_t(
                                               v, kw::callback = [](taylor_adaptive<fp_t> &, fp_t) {})}};

        auto ta = taylor_adaptive<fp_t>{{prime(x) = v, prime(v) = -9.8 * sin(x)},
                                        {fp_t(-0.25), fp_t(0.)},
                                        kw::opt_level = opt_level,
                                        kw::high_accuracy = high_accuracy,
                                        kw::compact_mode = compact_mode};

        for (auto i = 0; i < 200; ++i) {
            auto [oc_ev, h_ev] = ta_ev.step();
            auto [oc, h] = ta.step();

            REQUIRE(oc_ev == oc);
            REQUIRE(h_ev == h);

            REQUIRE(ta_ev.get_state() == ta.get_state());
            REQUIRE(ta_ev.get_time() == ta.get_time());
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

TEST_CASE("taylor nte")
{
    using Catch::Matchers::Message;

    auto [v] = make_vars("v");

    using ev_t = taylor_adaptive<double>::nt_event_t;

    std::ostringstream oss;
    oss << ev_t(
        v * v - 1e-10, kw::callback = [](taylor_adaptive<double> &, double) {});
    REQUIRE(boost::algorithm::contains(oss.str(), "direction::any"));
    REQUIRE(boost::algorithm::contains(oss.str(), "non-terminal"));
    oss.str("");

    oss << ev_t(
        v * v - 1e-10, kw::callback = [](taylor_adaptive<double> &, double) {},
        kw::direction = event_direction::positive);
    REQUIRE(boost::algorithm::contains(oss.str(), "event_direction::positive"));
    REQUIRE(boost::algorithm::contains(oss.str(), "non-terminal"));
    oss.str("");

    oss << ev_t(
        v * v - 1e-10, kw::callback = [](taylor_adaptive<double> &, double) {},
        kw::direction = event_direction::negative);
    REQUIRE(boost::algorithm::contains(oss.str(), "event_direction::negative"));
    REQUIRE(boost::algorithm::contains(oss.str(), "non-terminal"));
    oss.str("");

    // Check the assignment operators.
    ev_t ev0(
        v * v - 1e-10, kw::callback = [](taylor_adaptive<double> &, double) {}),
        ev1(
            v * v - 1e-10, kw::callback = [](taylor_adaptive<double> &, double) {},
            kw::direction = event_direction::negative),
        ev2(
            v * v - 1e-10, kw::callback = [](taylor_adaptive<double> &, double) {},
            kw::direction = event_direction::positive);
    ev0 = ev1;
    oss << ev0;
    REQUIRE(boost::algorithm::contains(oss.str(), "event_direction::negative"));
    REQUIRE(boost::algorithm::contains(oss.str(), "non-terminal"));
    oss.str("");

    ev0 = std::move(ev2);
    oss << ev0;
    REQUIRE(boost::algorithm::contains(oss.str(), "event_direction::positive"));
    REQUIRE(boost::algorithm::contains(oss.str(), "non-terminal"));
    oss.str("");

    // Failure modes.
    REQUIRE_THROWS_MATCHES(ev_t(v * v - 1e-10, kw::callback = ev_t::callback_t{}), std::invalid_argument,
                           Message("Cannot construct a non-terminal event with an empty callback"));
    REQUIRE_THROWS_MATCHES(ev_t(
                               v * v - 1e-10, kw::callback = [](taylor_adaptive<double> &, double) {},
                               kw::direction = event_direction{50}),
                           std::invalid_argument,
                           Message("Invalid value selected for the direction of a non-terminal event"));
}

TEST_CASE("taylor glancing blow test")
{
    std::cout << "Starting glancing blow test...\n";

    // NOTE: in this test two spherical particles are
    // "colliding" in a glancing fashion, meaning that
    // the polynomial representing the evolution in time
    // of the mutual distance has a repeated root at
    // t = collision time.

    auto tester = [](auto fp_x) {
        auto [x0, vx0, x1, vx1] = make_vars("x0", "vx0", "x1", "vx1");
        auto [y0, vy0, y1, vy1] = make_vars("y0", "vy0", "y1", "vy1");

        using fp_t = decltype(fp_x);

        using ev_t = typename taylor_adaptive<fp_t>::nt_event_t;

        auto counter = 0u;

        // First setup: one particle still, the other moving with uniform velocity.
        auto ta = taylor_adaptive<fp_t>{
            {prime(x0) = vx0, prime(y0) = vy0, prime(x1) = vx1, prime(y1) = vy1, prime(vx0) = 0_dbl, prime(vy0) = 0_dbl,
             prime(vx1) = 0_dbl, prime(vy1) = 0_dbl},
            {fp_t(0.), fp_t(0.), fp_t(-10.), fp_t(2), fp_t(0.), fp_t(0.), fp_t(1.), fp_t(0.)},
            kw::nt_events = {ev_t(
                square(x0 - x1) + square(y0 - y1) - 4., kw::callback = [&counter](taylor_adaptive<fp_t> &, fp_t t) {
                    REQUIRE((t - 10.) * (t - 10.) <= std::numeric_limits<fp_t>::epsilon());

                    ++counter;
                })}};

        for (auto i = 0; i < 20; ++i) {
            REQUIRE(std::get<0>(ta.step(fp_t(1.3))) == taylor_outcome::time_limit);
        }

        // Any number of events up to 2 is acceptable here.
        REQUIRE(counter <= 2u);

        counter = 0;

        // Second setup: one particle still, the other accelerating towards positive
        // x direction with constant acceleration.
        ta = taylor_adaptive<fp_t>{{prime(x0) = vx0, prime(y0) = vy0, prime(x1) = vx1, prime(y1) = vy1,
                                    prime(vx0) = 0_dbl, prime(vy0) = 0_dbl, prime(vx1) = .1_dbl, prime(vy1) = 0_dbl},
                                   {fp_t(0.), fp_t(0.), fp_t(-10.), fp_t(2), fp_t(0.), fp_t(0.), fp_t(1.), fp_t(0.)},
                                   kw::nt_events = {ev_t(
                                       square(x0 - x1) + square(y0 - y1) - 4.,
                                       kw::callback = [&counter](taylor_adaptive<fp_t> &, fp_t) { ++counter; })}};

        for (auto i = 0; i < 20; ++i) {
            REQUIRE(std::get<0>(ta.step(fp_t(1.3))) == taylor_outcome::time_limit);
        }

        REQUIRE(counter <= 2u);
    };

    tuple_for_each(fp_types, [&tester](auto x) { tester(x); });

    std::cout << "Glancing blow test finished\n";
}

TEST_CASE("taylor nte multizero")
{
    auto tester = [](auto fp_x, unsigned opt_level, bool high_accuracy, bool compact_mode) {
        using fp_t = decltype(fp_x);

        auto [x, v] = make_vars("x", "v");

        using ev_t = typename taylor_adaptive<fp_t>::nt_event_t;

        auto counter = 0u;

        // In this test, we define two events:
        // - the velocity is smaller in absolute
        //   value than a small limit,
        // - the velocity is exactly zero.
        // It is likely that both events are going to fire
        // in the same timestep, with the first event
        // firing twice. The sequence of events must
        // be 0 1 0 repeated a few times.

        fp_t cur_time(0);

        auto ta = taylor_adaptive<fp_t>{
            {prime(x) = v, prime(v) = -9.8 * sin(x)},
            {fp_t(0), fp_t(.25)},
            kw::opt_level = opt_level,
            kw::high_accuracy = high_accuracy,
            kw::compact_mode = compact_mode,
            kw::nt_events
            = {ev_t(
                   v * v - 1e-10, kw::callback =
                                      [&counter, &cur_time](taylor_adaptive<fp_t> &ta, fp_t t) {
                                          using std::abs;

                                          // Make sure the callbacks are called in order.
                                          REQUIRE(t > cur_time);

                                          // Ensure the state of ta has
                                          // been propagated until after the
                                          // event.
                                          REQUIRE(ta.get_time() > t);

                                          REQUIRE((counter % 3u == 0u || counter % 3u == 2u));

                                          ta.update_d_output(t);

                                          const auto v = ta.get_d_output()[1];
                                          REQUIRE(abs(v * v - 1e-10) < std::numeric_limits<fp_t>::epsilon());

                                          ++counter;

                                          cur_time = t;
                                      }),
               ev_t(
                   v, kw::callback = [&counter, &cur_time](taylor_adaptive<fp_t> &ta, fp_t t) {
                       using std::abs;

                       // Make sure the callbacks are called in order.
                       REQUIRE(t > cur_time);

                       // Ensure the state of ta has
                       // been propagated until after the
                       // event.
                       REQUIRE(ta.get_time() > t);

                       REQUIRE((counter % 3u == 1u));

                       ta.update_d_output(t);

                       const auto v = ta.get_d_output()[1];
                       REQUIRE(abs(v) <= std::numeric_limits<fp_t>::epsilon() * 100);

                       ++counter;

                       cur_time = t;
                   })}};

        REQUIRE(std::get<0>(ta.propagate_until(fp_t(4))) == taylor_outcome::time_limit);

        REQUIRE(counter == 12u);

        counter = 0;
        cur_time = 0;

        // We re-run the test, but this time we want to detect
        // only when the velocity goes from positive to negative.
        // Thus the sequence of events will be:
        // - 0 1 0
        // - 0 0
        // - 0 1 0
        // - 0 0
        ta = taylor_adaptive<fp_t>{
            {prime(x) = v, prime(v) = -9.8 * sin(x)},
            {fp_t(0), fp_t(.25)},
            kw::opt_level = opt_level,
            kw::high_accuracy = high_accuracy,
            kw::compact_mode = compact_mode,
            kw::nt_events
            = {ev_t(
                   v * v - 1e-10, kw::callback =
                                      [&counter, &cur_time](taylor_adaptive<fp_t> &ta, fp_t t) {
                                          using std::abs;

                                          // Make sure the callbacks are called in order.
                                          REQUIRE(t > cur_time);

                                          // Ensure the state of ta has
                                          // been propagated until after the
                                          // event.
                                          REQUIRE(ta.get_time() > t);

                                          REQUIRE((counter == 0u || (counter >= 2u && counter <= 6u)
                                                   || (counter >= 7u && counter <= 9u)));

                                          ta.update_d_output(t);

                                          const auto v = ta.get_d_output()[1];
                                          REQUIRE(abs(v * v - 1e-10) < std::numeric_limits<fp_t>::epsilon());

                                          ++counter;

                                          cur_time = t;
                                      }),
               ev_t(
                   v,
                   kw::callback =
                       [&counter, &cur_time](taylor_adaptive<fp_t> &ta, fp_t t) {
                           using std::abs;

                           // Make sure the callbacks are called in order.
                           REQUIRE(t > cur_time);

                           // Ensure the state of ta has
                           // been propagated until after the
                           // event.
                           REQUIRE(ta.get_time() > t);

                           REQUIRE((counter == 1u || counter == 6u));

                           ta.update_d_output(t);

                           const auto v = ta.get_d_output()[1];
                           REQUIRE(abs(v) <= std::numeric_limits<fp_t>::epsilon() * 100);

                           ++counter;

                           cur_time = t;
                       },
                   kw::direction = event_direction::negative)}};

        REQUIRE(std::get<0>(ta.propagate_until(fp_t(4))) == taylor_outcome::time_limit);

        REQUIRE(counter == 10u);
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

TEST_CASE("taylor nte multizero negative timestep")
{
    auto tester = [](auto fp_x, unsigned opt_level, bool high_accuracy, bool compact_mode) {
        using fp_t = decltype(fp_x);

        auto [x, v] = make_vars("x", "v");

        using ev_t = typename taylor_adaptive<fp_t>::nt_event_t;

        auto counter = 0u;

        fp_t cur_time(0);

        // In this test, we define two events:
        // - the velocity is smaller in absolute
        //   value than a small limit,
        // - the velocity is exactly zero.
        // It is likely that both events are going to fire
        // in the same timestep, with the first event
        // firing twice. The sequence of events must
        // be 0 1 0 repeated a few times.
        auto ta = taylor_adaptive<fp_t>{
            {prime(x) = v, prime(v) = -9.8 * sin(x)},
            {fp_t(0), fp_t(.25)},
            kw::opt_level = opt_level,
            kw::high_accuracy = high_accuracy,
            kw::compact_mode = compact_mode,
            kw::nt_events
            = {ev_t(
                   v * v - 1e-10, kw::callback =
                                      [&counter, &cur_time](taylor_adaptive<fp_t> &ta, fp_t t) {
                                          using std::abs;

                                          // Make sure the callbacks are called in order.
                                          REQUIRE(t < cur_time);

                                          // Ensure the state of ta has
                                          // been propagated until after the
                                          // event.
                                          REQUIRE(ta.get_time() < t);

                                          REQUIRE((counter % 3u == 0u || counter % 3u == 2u));

                                          ta.update_d_output(t);

                                          const auto v = ta.get_d_output()[1];
                                          REQUIRE(abs(v * v - 1e-10) < std::numeric_limits<fp_t>::epsilon());

                                          ++counter;

                                          cur_time = t;
                                      }),
               ev_t(
                   v, kw::callback = [&counter, &cur_time](taylor_adaptive<fp_t> &ta, fp_t t) {
                       using std::abs;

                       // Make sure the callbacks are called in order.
                       REQUIRE(t < cur_time);

                       // Ensure the state of ta has
                       // been propagated until after the
                       // event.
                       REQUIRE(ta.get_time() < t);

                       REQUIRE((counter % 3u == 1u));

                       ta.update_d_output(t);

                       const auto v = ta.get_d_output()[1];
                       REQUIRE(abs(v) <= std::numeric_limits<fp_t>::epsilon() * 100);

                       ++counter;

                       cur_time = t;
                   })}};

        REQUIRE(std::get<0>(ta.propagate_until(fp_t(-4))) == taylor_outcome::time_limit);

        REQUIRE(counter == 12u);
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

TEST_CASE("taylor nte basic")
{
    auto tester = [](auto fp_x, unsigned opt_level, bool high_accuracy, bool compact_mode) {
        using fp_t = decltype(fp_x);

        auto [x, v] = make_vars("x", "v");

        using ev_t = typename taylor_adaptive<fp_t>::nt_event_t;

        auto counter = 0u;

        auto ta = taylor_adaptive<fp_t>{
            {prime(x) = v, prime(v) = -9.8 * sin(x)},
            {fp_t(-0.25), fp_t(0.)},
            kw::opt_level = opt_level,
            kw::high_accuracy = high_accuracy,
            kw::compact_mode = compact_mode,
            kw::nt_events = {ev_t(
                v, kw::callback = [&counter](taylor_adaptive<fp_t> &, fp_t t) {
                    // Check that the first event detection happens at t == 0.
                    if (counter == 0u) {
                        REQUIRE(t == 0);
                    }

                    // Make sure the 3rd event detection corresponds
                    // to a full period.
                    if (counter == 2u) {
#if defined(HEYOKA_HAVE_REAL128)
                        if constexpr (std::is_same_v<fp_t, mppp::real128>) {
                            using namespace mppp::literals;

                            REQUIRE(t == approximately(2.01495830729551199828007207119092374_rq, fp_t(1000)));
                        } else {
#endif
                            REQUIRE(t
                                    == approximately(boost::lexical_cast<fp_t>("2.01495830729551199828007207119092374"),
                                                     fp_t(1000)));
#if defined(HEYOKA_HAVE_REAL128)
                        }
#endif
                    }

                    ++counter;
                })}};

        for (auto i = 0; i < 20; ++i) {
            REQUIRE(std::get<0>(ta.step()) == taylor_outcome::success);
        }

        REQUIRE(counter == 3u);
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

// Another test for event direction.
TEST_CASE("nt dir test")
{
    auto [x, v] = make_vars("x", "v");

    bool fwd = true;

    std::vector<double> tlist;

    std::vector<double>::reverse_iterator rit;

    auto ta = taylor_adaptive<double>{{prime(x) = v, prime(v) = -9.8 * sin(x)},
                                      {-0.25, 0.},
                                      kw::nt_events = {nt_event<double>(
                                          v,
                                          kw::callback =
                                              [&fwd, &tlist, &rit](taylor_adaptive<double> &, double t) {
                                                  if (fwd) {
                                                      tlist.push_back(t);
                                                  } else if (rit != tlist.rend()) {
                                                      REQUIRE(*rit == approximately(t));

                                                      ++rit;
                                                  }
                                              },
                                          kw::direction = event_direction::positive)}};

    ta.propagate_until(20);

    fwd = false;
    rit = tlist.rbegin();

    ta.propagate_until(0);
}
