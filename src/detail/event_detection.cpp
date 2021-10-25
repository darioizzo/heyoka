// Copyright 2020, 2021 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/math/policies/policy.hpp>
#include <boost/math/tools/toms748_solve.hpp>
#include <boost/numeric/conversion/cast.hpp>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include <heyoka/detail/event_detection.hpp>
#include <heyoka/detail/fwd_decl.hpp>
#include <heyoka/detail/llvm_helpers.hpp>
#include <heyoka/detail/logging_impl.hpp>
#include <heyoka/detail/type_traits.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/number.hpp>
#include <heyoka/taylor.hpp>

#if defined(_MSC_VER) && !defined(__clang__)

// NOTE: MSVC has issues with the other "using"
// statement form.
using namespace fmt::literals;

#else

using fmt::literals::operator""_format;

#endif

namespace heyoka::detail
{

namespace
{

// The polynomial cache type.
template <typename T>
using poly_cache_t = std::vector<std::vector<std::vector<T>>>;

// Helper to fetch the per-thread poly cache.
template <typename T>
poly_cache_t<T> &get_poly_cache()
{
    thread_local poly_cache_t<T> ret;

    return ret;
}

// Extract a poly of order n from the cache (or create a new one).
template <typename T>
auto get_poly_from_cache(poly_cache_t<T> &cache, std::uint32_t n)
{
    // Look if we have inited the cache for order n.
    if (n >= cache.size()) {
        // The cache was never used for polynomials of order
        // n, add the necessary entries.

        // NOTE: no overflow check needed here, cause the order
        // is always overflow checked in the integrator machinery.
        cache.resize(boost::numeric_cast<decltype(cache.size())>(n + 1u));
    }

    auto &pcache = cache[n];

    if (pcache.empty()) {
        // No polynomials are available, create a new one.
        return std::vector<T>(boost::numeric_cast<typename std::vector<T>::size_type>(n + 1u));
    } else {
        // Extract an existing polynomial from the cache.
        auto retval = std::move(pcache.back());
        pcache.pop_back();

        return retval;
    }
}

// Insert a poly into the cache.
template <typename T>
void put_poly_in_cache(poly_cache_t<T> &cache, std::vector<T> &&v)
{
    // Fetch the order of the polynomial.
    // NOTE: the order is the size - 1.
    assert(!v.empty()); // LCOV_EXCL_LINE
    const auto n = v.size() - 1u;

    // Look if we have inited the cache for order n.
    if (n >= cache.size()) {
        // NOTE: this is currently never reached
        // because we always look in the cache
        // before the first invocation of this function.
        // LCOV_EXCL_START
        // The cache was never used for polynomials of order
        // n, add the necessary entries.
        if (n == std::numeric_limits<decltype(cache.size())>::max()) {
            throw std::overflow_error("An overflow was detected in the polynomial cache");
        }
        cache.resize(n + 1u);
        // LCOV_EXCL_STOP
    }

    // Move v in.
    cache[n].push_back(std::move(v));
}

// Given an input polynomial a(x), substitute
// x with x_1 * h and write to ret the resulting
// polynomial in the new variable x_1. Requires
// random-access iterators.
// NOTE: aliasing allowed.
template <typename OutputIt, typename InputIt, typename T>
void poly_rescale(OutputIt ret, InputIt a, const T &scal, std::uint32_t n)
{
    T cur_f(1);

    for (std::uint32_t i = 0; i <= n; ++i) {
        ret[i] = cur_f * a[i];
        cur_f *= scal;
    }
}

// Transform the polynomial a(x) into 2**n * a(x / 2).
// Requires random-access iterators.
// NOTE: aliasing allowed.
template <typename OutputIt, typename InputIt>
void poly_rescale_p2(OutputIt ret, InputIt a, std::uint32_t n)
{
    using value_type = typename std::iterator_traits<InputIt>::value_type;

    value_type cur_f(1);

    for (std::uint32_t i = 0; i <= n; ++i) {
        ret[n - i] = cur_f * a[n - i];
        cur_f *= 2;
    }
}

// Generic branchless sign function.
template <typename T>
int sgn(T val)
{
    return (T(0) < val) - (val < T(0));
}

// Evaluate the first derivative of a polynomial.
// Requires random-access iterator.
template <typename InputIt, typename T>
auto poly_eval_1(InputIt a, T x, std::uint32_t n)
{
    assert(n >= 2u); // LCOV_EXCL_LINE

    // Init the return value.
    auto ret1 = a[n] * n;

    for (std::uint32_t i = 1; i < n; ++i) {
        ret1 = a[n - i] * (n - i) + ret1 * x;
    }

    return ret1;
}

// Evaluate polynomial.
// Requires random-access iterator.
template <typename InputIt, typename T>
auto poly_eval(InputIt a, T x, std::uint32_t n)
{
    auto ret = a[n];

    for (std::uint32_t i = 1; i <= n; ++i) {
        ret = a[n - i] + ret * x;
    }

    return ret;
}

// A RAII helper to extract polys from the cache and
// return them to the cache upon destruction.
template <typename T>
class pwrap
{
    void back_to_cache()
    {
        // NOTE: the cache does not allow empty vectors.
        if (!v.empty()) {
            put_poly_in_cache(pc, std::move(v));
        }
    }

public:
    explicit pwrap(poly_cache_t<T> &cache, std::uint32_t n) : pc(cache), v(get_poly_from_cache<T>(pc, n)) {}

    pwrap(pwrap &&other) noexcept : pc(other.pc), v(std::move(other.v))
    {
        // Make sure we moved from a valid pwrap.
        assert(!v.empty()); // LCOV_EXCL_LINE
    }
    pwrap &operator=(pwrap &&other) noexcept
    {
        // Disallow self move.
        assert(this != &other); // LCOV_EXCL_LINE

        // Make sure the polyomial caches match.
        assert(&pc == &other.pc); // LCOV_EXCL_LINE

        // Make sure we are not moving from an
        // invalid pwrap.
        assert(!other.v.empty()); // LCOV_EXCL_LINE

        // Put the current v in the cache.
        back_to_cache();

        // Do the move-assignment.
        v = std::move(other.v);

        return *this;
    }

    // Delete copy semantics.
    pwrap(const pwrap &) = delete;
    pwrap &operator=(const pwrap &) = delete;

    ~pwrap()
    {
        // Put the current v in the cache.
        back_to_cache();
    }

    poly_cache_t<T> &pc;
    std::vector<T> v;
};

// Find the only existing root for the polynomial poly of the given order
// existing in [lb, ub).
template <typename T>
std::tuple<T, int> bracketed_root_find(const pwrap<T> &poly, std::uint32_t order, T lb, T ub)
{
    using std::isfinite;
    using std::nextafter;

    // NOTE: the Boost root finding routine searches in a closed interval,
    // but the goal here is to find a root in [lb, ub). Thus, we move ub
    // one position down so that it is not considered in the root finding routine.
    if (isfinite(lb) && isfinite(ub) && ub > lb) {
        ub = nextafter(ub, lb);
    }

    // NOTE: perhaps this should depend on T? E.g., we could use the number
    // of binary digits in the significand.
    constexpr boost::uintmax_t iter_limit = 100;
    auto max_iter = iter_limit;

    // Ensure that root finding does not throw on error,
    // rather it will write something to errno instead.
    // https://www.boost.org/doc/libs/1_75_0/libs/math/doc/html/math_toolkit/pol_tutorial/namespace_policies.html
    using boost::math::policies::domain_error;
    using boost::math::policies::errno_on_error;
    using boost::math::policies::evaluation_error;
    using boost::math::policies::overflow_error;
    using boost::math::policies::pole_error;
    using boost::math::policies::policy;

    using pol = policy<domain_error<errno_on_error>, pole_error<errno_on_error>, overflow_error<errno_on_error>,
                       evaluation_error<errno_on_error>>;

    // Clear out errno before running the root finding.
    errno = 0;

    // Run the root finder.
    const auto p = boost::math::tools::toms748_solve([d = poly.v.data(), order](T x) { return poly_eval(d, x, order); },
                                                     lb, ub, boost::math::tools::eps_tolerance<T>(), max_iter, pol{});
    const auto ret = (p.first + p.second) / 2;

    SPDLOG_LOGGER_DEBUG(get_logger(), "root finding iterations: {}", max_iter);

    if (errno > 0) {
        // Some error condition arose during root finding,
        // return zero and errno.
        return std::tuple{T(0), errno};
    }

    if (max_iter < iter_limit) {
        // Root finding terminated within the
        // iteration limit, return ret and success.
        return std::tuple{ret, 0};
    } else {
        // LCOV_EXCL_START
        // Root finding needed too many iterations,
        // return the (possibly wrong) result
        // and flag -1.
        return std::tuple{ret, -1};
        // LCOV_EXCL_STOP
    }
}

// Helper to fetch the per-thread working list used in
// poly root finding.
template <typename T>
auto &get_wlist()
{
    thread_local std::vector<std::tuple<T, T, pwrap<T>>> w_list;

    return w_list;
}

// Helper to fetch the per-thread list of isolating intervals.
template <typename T>
auto &get_isol()
{
    thread_local std::vector<std::tuple<T, T>> isol;

    return isol;
}

// Helper to detect events of terminal type.
template <typename>
struct is_terminal_event : std::false_type {
};

template <typename T>
struct is_terminal_event<t_event<T>> : std::true_type {
};

template <typename T>
constexpr bool is_terminal_event_v = is_terminal_event<T>::value;

// Helper to add a polynomial translation function
// to the state 's'.
template <typename T>
llvm::Function *add_poly_translator_1(llvm_state &s, std::uint32_t order, std::uint32_t batch_size)
{
    assert(order > 0u); // LCOV_EXCL_LINE

    // Overflow check: we need to be able to index
    // into the array of coefficients.
    // LCOV_EXCL_START
    if (order == std::numeric_limits<std::uint32_t>::max()
        || batch_size > std::numeric_limits<std::uint32_t>::max() / (order + 1u)) {
        throw std::overflow_error("Overflow detected while adding a polynomial translation function");
    }
    // LCOV_EXCL_STOP

    auto &builder = s.builder();
    auto &context = s.context();

    // Helper to fetch the (i, j) binomial coefficient from
    // a precomputed global array. The returned value is already
    // splatted.
    auto get_bc = [&, bc_ptr = llvm_add_bc_array<T>(s, order)](llvm::Value *i, llvm::Value *j) {
        auto idx = builder.CreateMul(i, builder.getInt32(order + 1u));
        idx = builder.CreateAdd(idx, j);

        auto val = builder.CreateLoad(builder.CreateInBoundsGEP(bc_ptr, {idx}));

        return vector_splat(builder, val, batch_size);
    };

    // Fetch the current insertion block.
    auto orig_bb = builder.GetInsertBlock();

    // The function arguments:
    // - the output pointer,
    // - the pointer to the poly coefficients (read-only).
    // No overlap is allowed.
    std::vector<llvm::Type *> fargs(2, llvm::PointerType::getUnqual(to_llvm_type<T>(context)));
    // The function does not return anything.
    auto *ft = llvm::FunctionType::get(builder.getVoidTy(), fargs, false);
    assert(ft != nullptr); // LCOV_EXCL_LINE
    // Now create the function.
    auto *f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "poly_translate_1", &s.module());
    // LCOV_EXCL_START
    if (f == nullptr) {
        throw std::invalid_argument("Unable to create a function for polynomial translation");
    }
    // LCOV_EXCL_STOP

    // Set the names/attributes of the function arguments.
    auto out_ptr = f->args().begin();
    out_ptr->setName("out_ptr");
    out_ptr->addAttr(llvm::Attribute::NoCapture);
    out_ptr->addAttr(llvm::Attribute::NoAlias);

    auto cf_ptr = f->args().begin() + 1;
    cf_ptr->setName("cf_ptr");
    cf_ptr->addAttr(llvm::Attribute::NoCapture);
    cf_ptr->addAttr(llvm::Attribute::NoAlias);
    cf_ptr->addAttr(llvm::Attribute::ReadOnly);

    // Create a new basic block to start insertion into.
    auto *bb = llvm::BasicBlock::Create(context, "entry", f);
    assert(bb != nullptr); // LCOV_EXCL_LINE
    builder.SetInsertPoint(bb);

    // Init the return values as zeroes.
    llvm_loop_u32(s, builder.getInt32(0), builder.getInt32(order + 1u), [&](llvm::Value *i) {
        auto ptr = builder.CreateInBoundsGEP(out_ptr, {builder.CreateMul(i, builder.getInt32(batch_size))});
        store_vector_to_memory(builder, ptr, vector_splat(builder, codegen<T>(s, number{0.}), batch_size));
    });

    // Do the translation.
    llvm_loop_u32(s, builder.getInt32(0), builder.getInt32(order + 1u), [&](llvm::Value *i) {
        auto ai = load_vector_from_memory(
            builder, builder.CreateInBoundsGEP(cf_ptr, {builder.CreateMul(i, builder.getInt32(batch_size))}),
            batch_size);

        llvm_loop_u32(s, builder.getInt32(0), builder.CreateAdd(i, builder.getInt32(1)), [&](llvm::Value *k) {
            auto tmp = builder.CreateFMul(ai, get_bc(i, k));

            auto ptr = builder.CreateInBoundsGEP(out_ptr, {builder.CreateMul(k, builder.getInt32(batch_size))});
            auto new_val = builder.CreateFAdd(load_vector_from_memory(builder, ptr, batch_size), tmp);
            store_vector_to_memory(builder, ptr, new_val);
        });
    });

    // Create the return value.
    builder.CreateRetVoid();

    // Verify the function.
    s.verify_function(f);

    // Restore the original insertion block.
    builder.SetInsertPoint(orig_bb);

    // NOTE: the optimisation pass will be run outside.
    return f;
}

// Add a function that, given an input polynomial of order n represented
// as an array of coefficients:
// - reverses it,
// - translates it by 1,
// - counts the sign changes in the coefficients
//   of the resulting polynomial.
template <typename T>
llvm::Function *add_poly_rtscc(llvm_state &s, std::uint32_t n, std::uint32_t batch_size)
{
    assert(batch_size > 0u); // LCOV_EXCL_LINE

    // Overflow check: we need to be able to index
    // into the array of coefficients.
    // LCOV_EXCL_START
    if (n == std::numeric_limits<std::uint32_t>::max()
        || batch_size > std::numeric_limits<std::uint32_t>::max() / (n + 1u)) {
        throw std::overflow_error("Overflow detected while adding an rtscc function");
    }
    // LCOV_EXCL_STOP

    auto &md = s.module();
    auto &builder = s.builder();
    auto &context = s.context();

    // Add the translator and the sign changes counting function.
    auto pt = add_poly_translator_1<T>(s, n, batch_size);
    auto scc = llvm_add_csc<T>(s, n, batch_size);

    // Fetch the current insertion block.
    auto orig_bb = builder.GetInsertBlock();

    // The function arguments:
    // - two poly coefficients output pointers,
    // - the output pointer to the number of sign changes (write-only),
    // - the input pointer to the original poly coefficients (read-only).
    // No overlap is allowed.
    auto fp_ptr_t = llvm::PointerType::getUnqual(to_llvm_type<T>(context));
    std::vector<llvm::Type *> fargs{fp_ptr_t, fp_ptr_t, llvm::PointerType::getUnqual(builder.getInt32Ty()), fp_ptr_t};
    // The function does not return anything.
    auto *ft = llvm::FunctionType::get(builder.getVoidTy(), fargs, false);
    assert(ft != nullptr); // LCOV_EXCL_LINE
    // Now create the function.
    auto *f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "poly_rtscc", &md);
    // LCOV_EXCL_START
    if (f == nullptr) {
        throw std::invalid_argument("Unable to create an rtscc function");
    }
    // LCOV_EXCL_STOP

    // Set the names/attributes of the function arguments.
    // NOTE: out_ptr1/2 are used both in read and write mode,
    // even though this function never actually reads from them
    // (they are just forwarded to other functions reading from them).
    // Because I am not 100% sure about the writeonly attribute
    // in this case, let's err on the side of caution and do not
    // mark them as writeonly.
    auto out_ptr1 = f->args().begin();
    out_ptr1->setName("out_ptr1");
    out_ptr1->addAttr(llvm::Attribute::NoCapture);
    out_ptr1->addAttr(llvm::Attribute::NoAlias);

    auto out_ptr2 = f->args().begin() + 1;
    out_ptr2->setName("out_ptr2");
    out_ptr2->addAttr(llvm::Attribute::NoCapture);
    out_ptr2->addAttr(llvm::Attribute::NoAlias);

    auto n_sc_ptr = f->args().begin() + 2;
    n_sc_ptr->setName("n_sc_ptr");
    n_sc_ptr->addAttr(llvm::Attribute::NoCapture);
    n_sc_ptr->addAttr(llvm::Attribute::NoAlias);
    n_sc_ptr->addAttr(llvm::Attribute::WriteOnly);

    auto cf_ptr = f->args().begin() + 3;
    cf_ptr->setName("cf_ptr");
    cf_ptr->addAttr(llvm::Attribute::NoCapture);
    cf_ptr->addAttr(llvm::Attribute::NoAlias);
    cf_ptr->addAttr(llvm::Attribute::ReadOnly);

    // Create a new basic block to start insertion into.
    auto *bb = llvm::BasicBlock::Create(context, "entry", f);
    assert(bb != nullptr); // LCOV_EXCL_LINE
    builder.SetInsertPoint(bb);

    // Do the reversion into out_ptr1.
    llvm_loop_u32(s, builder.getInt32(0), builder.getInt32(n + 1u), [&](llvm::Value *i) {
        auto load_idx = builder.CreateMul(builder.CreateSub(builder.getInt32(n), i), builder.getInt32(batch_size));
        auto store_idx = builder.CreateMul(i, builder.getInt32(batch_size));

        auto cur_cf = load_vector_from_memory(builder, builder.CreateInBoundsGEP(cf_ptr, {load_idx}), batch_size);
        store_vector_to_memory(builder, builder.CreateInBoundsGEP(out_ptr1, {store_idx}), cur_cf);
    });

    // Translate out_ptr1 into out_ptr2.
    builder.CreateCall(pt, {out_ptr2, out_ptr1});

    // Count the sign changes in out_ptr2.
    builder.CreateCall(scc, {n_sc_ptr, out_ptr2});

    // Return.
    builder.CreateRetVoid();

    // Verify.
    s.verify_function(f);

    // Restore the original insertion block.
    builder.SetInsertPoint(orig_bb);

    // NOTE: the optimisation pass will be run outside.
    return f;
}

// Add a function for the computation of fast event exclusion check via the computation
// of the enclosure of the event equation's Taylor polynomial. The enclosure is computed
// via Horner's scheme using interval arithmetic.
template <typename T>
llvm::Function *llvm_add_fex_check_impl(llvm_state &s, std::uint32_t n, std::uint32_t batch_size)
{
    assert(batch_size > 0u); // LCOV_EXCL_LINE
    assert(n > 0u);          // LCOV_EXCL_LINE

    // Overflow check: we need to be able to index
    // into the array of coefficients.
    // LCOV_EXCL_START
    if (n == std::numeric_limits<std::uint32_t>::max()
        || batch_size > std::numeric_limits<std::uint32_t>::max() / (n + 1u)) {
        throw std::overflow_error("Overflow detected while adding a fex_check function");
    }
    // LCOV_EXCL_STOP

    auto &md = s.module();
    auto &builder = s.builder();
    auto &context = s.context();

    // Fetch the current insertion block.
    auto orig_bb = builder.GetInsertBlock();

    // The function arguments:
    // - pointer to the array of poly coefficients (read-only),
    // - pointer to the timestep value (s) (read-only),
    // - pointer to the int32 flag(s) to signal integration backwards in time (read-only),
    // - output pointer (write-only).
    // No overlap is allowed.
    auto fp_ptr_t = llvm::PointerType::getUnqual(to_llvm_type<T>(context));
    std::vector<llvm::Type *> fargs{fp_ptr_t, fp_ptr_t, llvm::PointerType::getUnqual(builder.getInt32Ty()),
                                    llvm::PointerType::getUnqual(builder.getInt32Ty())};
    // The function does not return anything.
    auto *ft = llvm::FunctionType::get(builder.getVoidTy(), fargs, false);
    assert(ft != nullptr); // LCOV_EXCL_LINE
    // Now create the function.
    auto *f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "fex_check", &md);
    // LCOV_EXCL_START
    if (f == nullptr) {
        throw std::invalid_argument("Unable to create an fex_check function");
    }
    // LCOV_EXCL_STOP

    // Set the names/attributes of the function arguments.
    auto cf_ptr = f->args().begin();
    cf_ptr->setName("cf_ptr");
    cf_ptr->addAttr(llvm::Attribute::NoCapture);
    cf_ptr->addAttr(llvm::Attribute::NoAlias);
    cf_ptr->addAttr(llvm::Attribute::ReadOnly);

    auto h_ptr = f->args().begin() + 1;
    h_ptr->setName("h_ptr");
    h_ptr->addAttr(llvm::Attribute::NoCapture);
    h_ptr->addAttr(llvm::Attribute::NoAlias);
    h_ptr->addAttr(llvm::Attribute::ReadOnly);

    auto back_flag_ptr = f->args().begin() + 2;
    back_flag_ptr->setName("back_flag_ptr");
    back_flag_ptr->addAttr(llvm::Attribute::NoCapture);
    back_flag_ptr->addAttr(llvm::Attribute::NoAlias);
    back_flag_ptr->addAttr(llvm::Attribute::ReadOnly);

    auto out_ptr = f->args().begin() + 3;
    out_ptr->setName("out_ptr");
    out_ptr->addAttr(llvm::Attribute::NoCapture);
    out_ptr->addAttr(llvm::Attribute::NoAlias);
    out_ptr->addAttr(llvm::Attribute::WriteOnly);

    // Create a new basic block to start insertion into.
    auto *bb = llvm::BasicBlock::Create(context, "entry", f);
    assert(bb != nullptr); // LCOV_EXCL_LINE
    builder.SetInsertPoint(bb);

    // Helper to implement the sum of two intervals.
    // NOTE: see https://en.wikipedia.org/wiki/Interval_arithmetic.
    auto ival_sum = [&builder](llvm::Value *a_lo, llvm::Value *a_hi, llvm::Value *b_lo, llvm::Value *b_hi) {
        return std::make_pair(builder.CreateFAdd(a_lo, b_lo), builder.CreateFAdd(a_hi, b_hi));
    };

    // Helper to implement the product of two intervals.
    auto ival_prod = [&s, &builder](llvm::Value *a_lo, llvm::Value *a_hi, llvm::Value *b_lo, llvm::Value *b_hi) {
        auto tmp1 = builder.CreateFMul(a_lo, b_lo);
        auto tmp2 = builder.CreateFMul(a_lo, b_hi);
        auto tmp3 = builder.CreateFMul(a_hi, b_lo);
        auto tmp4 = builder.CreateFMul(a_hi, b_hi);

        auto cmp1 = llvm_min(s, tmp1, tmp2);
        auto cmp2 = llvm_min(s, tmp3, tmp4);
        auto cmp3 = llvm_max(s, tmp1, tmp2);
        auto cmp4 = llvm_max(s, tmp3, tmp4);

        return std::make_pair(llvm_min(s, cmp1, cmp2), llvm_max(s, cmp3, cmp4));
    };

    // Load the timestep.
    auto h = load_vector_from_memory(builder, h_ptr, batch_size);

    // Load the back_flag, convert it to a boolean vector.
    auto back_flag = builder.CreateTrunc(load_vector_from_memory(builder, back_flag_ptr, batch_size),
                                         make_vector_type(builder.getInt1Ty(), batch_size));

    // Compute the components of the interval version of h. If we are integrating
    // forward, the components are (0, h), otherwise they are (h, 0).
    auto h_lo = builder.CreateSelect(back_flag, h, llvm::Constant::getNullValue(h->getType()));
    auto h_hi = builder.CreateSelect(back_flag, llvm::Constant::getNullValue(h->getType()), h);

    // Create the lo/hi components of the accumulator.
    auto fp_vec_t = to_llvm_vector_type<T>(context, batch_size);
    auto acc_lo = builder.CreateAlloca(fp_vec_t);
    auto acc_hi = builder.CreateAlloca(fp_vec_t);

    // Init the accumulator's hi/lo components with the highest-order coefficient.
    auto ho_cf = load_vector_from_memory(
        builder,
        builder.CreateInBoundsGEP(cf_ptr, {builder.CreateMul(builder.getInt32(n), builder.getInt32(batch_size))}),
        batch_size);
    builder.CreateStore(ho_cf, acc_lo);
    builder.CreateStore(ho_cf, acc_hi);

    // Run the Horner scheme (starting from 1 because we already consumed the
    // highest-order coefficient).
    llvm_loop_u32(s, builder.getInt32(1), builder.getInt32(n + 1u), [&](llvm::Value *i) {
        // Load the current coefficient.
        auto ptr = builder.CreateInBoundsGEP(
            cf_ptr, {builder.CreateMul(builder.CreateSub(builder.getInt32(n), i), builder.getInt32(batch_size))});
        auto cur_cf = load_vector_from_memory(builder, ptr, batch_size);

        // Multiply the accumulator by h.
        auto [acc_h_lo, acc_h_hi] = ival_prod(builder.CreateLoad(acc_lo), builder.CreateLoad(acc_hi), h_lo, h_hi);

        // Add the result to the current coefficient.
        auto [new_acc_lo, new_acc_hi] = ival_sum(cur_cf, cur_cf, acc_h_lo, acc_h_hi);

        // Update the accumulator.
        builder.CreateStore(new_acc_lo, acc_lo);
        builder.CreateStore(new_acc_hi, acc_hi);
    });

    // Compute the sign of the components of the accumulator.
    auto s_lo = llvm_sgn(s, builder.CreateLoad(acc_lo));
    auto s_hi = llvm_sgn(s, builder.CreateLoad(acc_hi));

    // Check if the signs are equal and the low sign is nonzero.
    auto cmp1 = builder.CreateICmpEQ(s_lo, s_hi);
    auto cmp2 = builder.CreateICmpNE(s_lo, llvm::Constant::getNullValue(s_lo->getType()));
    // NOTE: this is a way of creating a logical AND between cmp1 and cmp2. LLVM 13 has a specific
    // function for this.
    auto cmp = builder.CreateSelect(cmp1, cmp2, llvm::Constant::getNullValue(cmp1->getType()));
    // Extend cmp to int32_t.
    auto retval = builder.CreateZExt(cmp, builder.getInt32Ty());

    // Store the result in out_ptr.
    store_vector_to_memory(builder, out_ptr, retval);

    // Return.
    builder.CreateRetVoid();

    // Verify.
    s.verify_function(f);

    // Restore the original insertion block.
    builder.SetInsertPoint(orig_bb);

    // NOTE: the optimisation pass will be run outside.
    return f;
}

// Fetch the JITted functions used in the event detection implementation.
template <typename T>
auto get_ed_jit_functions(std::uint32_t order)
{
    // Polynomial translation function type.
    using pt_t = void (*)(T *, const T *);
    // rtscc function type.
    using rtscc_t = void (*)(T *, T *, std::uint32_t *, const T *);
    using fex_check_t = void (*)(const T *, const T *, const std::uint32_t *, std::uint32_t *);

    thread_local std::unordered_map<std::uint32_t, std::pair<llvm_state, std::tuple<pt_t, rtscc_t, fex_check_t>>>
        tf_map;

    auto it = tf_map.find(order);

    if (it == tf_map.end()) {
        // Cache miss, we need to create
        // a new LLVM state and functions.
        llvm_state s;

        // Add the rtscc function. This will also indirectly
        // add the translator function.
        add_poly_rtscc<T>(s, order, 1);

        llvm_add_fex_check_impl<T>(s, order, 1);

        // Run the optimisation pass.
        s.optimise();

        // Compile.
        s.compile();

        // Fetch the functions.
        auto pt = reinterpret_cast<pt_t>(s.jit_lookup("poly_translate_1"));
        auto rtscc = reinterpret_cast<rtscc_t>(s.jit_lookup("poly_rtscc"));
        auto fex_check = reinterpret_cast<fex_check_t>(s.jit_lookup("fex_check"));

        // Insert state and functions into the cache.
        [[maybe_unused]] const auto ret
            = tf_map.try_emplace(order, std::pair{std::move(s), std::tuple{pt, rtscc, fex_check}});
        assert(ret.second); // LCOV_EXCL_LINE

        return std::tuple{pt, rtscc, fex_check};
    } else {
        // Cache hit, return the function.
        return it->second.second;
    }
}

// Minimal interval class supporting a couple
// of elementary operations.
template <typename T>
struct ival {
    T lower;
    T upper;

    ival() : ival(T(0)) {}
    explicit ival(T val) : ival(val, val) {}
    explicit ival(T l, T u) : lower(l), upper(u)
    {
#if !defined(NDEBUG)
        using std::isnan;

        if (!isnan(lower) && !isnan(upper)) {
            assert(upper >= lower); // LCOV_EXCL_LINE
        }
#endif
    }
};

// NOTE: see https://en.wikipedia.org/wiki/Interval_arithmetic.
template <typename T>
ival<T> operator+(ival<T> a, ival<T> b)
{
    return ival<T>(a.lower + b.lower, a.upper + b.upper);
}

template <typename T>
ival<T> operator*(ival<T> a, ival<T> b)
{
    const auto tmp1 = a.lower * b.lower;
    const auto tmp2 = a.lower * b.upper;
    const auto tmp3 = a.upper * b.lower;
    const auto tmp4 = a.upper * b.upper;

    const auto l = std::min(std::min(tmp1, tmp2), std::min(tmp3, tmp4));
    const auto u = std::max(std::max(tmp1, tmp2), std::max(tmp3, tmp4));

    return ival<T>(l, u);
}

// Implementation of event detection.
template <typename T>
void taylor_detect_events_impl(std::vector<std::tuple<std::uint32_t, T, bool, int, T>> &d_tes,
                               std::vector<std::tuple<std::uint32_t, T, int>> &d_ntes,
                               const std::vector<t_event<T>> &tes, const std::vector<nt_event<T>> &ntes,
                               const std::vector<std::optional<std::pair<T, T>>> &cooldowns, T h,
                               const std::vector<T> &ev_jet, std::uint32_t order, std::uint32_t dim, T g_eps)
{
    using std::abs;
    using std::isfinite;

    // Clear the vectors of detected events.
    // NOTE: do it here as this is always necessary,
    // regardless of issues with h/g_eps.
    d_tes.clear();
    d_ntes.clear();

    // LCOV_EXCL_START
    if (!isfinite(h)) {
        get_logger()->warn("event detection skipped due to an invalid timestep value of {}", h);
        return;
    }
    if (!isfinite(g_eps)) {
        get_logger()->warn("event detection skipped due to an invalid value of {} for the maximum error on the Taylor "
                           "series of the event equations",
                           g_eps);
        return;
    }
    // LCOV_EXCL_STOP

    if (h == 0) {
        // If the timestep is zero, skip event detection.
        return;
    }

    assert(order >= 2u); // LCOV_EXCL_LINE

    // NOTE: trigger the creation of the poly cache
    // *before* triggering the creation of the other
    // thread-local variables. This is important because
    // the working list contains pwrap objects, which in turn
    // interact with the poly cache. If we don't do this,
    // the poly cache will be destroyed before the working
    // list, and the destructors in the working list will
    // interact with invalid memory.
    auto &pc = get_poly_cache<T>();

    // Fetch a reference to the list of isolating intervals.
    auto &isol = get_isol<T>();

    // Fetch a reference to the wlist.
    auto &wl = get_wlist<T>();

    // Fetch the JITted functions.
    auto j_funcs = get_ed_jit_functions<T>(order);
    auto pt = std::get<0>(j_funcs);
    auto rtscc = std::get<1>(j_funcs);
    auto fex_check = std::get<2>(j_funcs);

    // Temporary polynomials used in the bisection loop.
    pwrap<T> tmp1(pc, order), tmp2(pc, order), tmp(pc, order);

    // Interval version of h, for use in the fast exclusion check.
    const auto h_int = (h >= 0) ? ival<T>(0, h) : ival<T>(h, 0);
    const std::uint32_t back_int = h < 0;

    // Helper to run event detection on a vector of events
    // (terminal or not). 'out' is the vector of detected
    // events, 'ev_vec' the input vector of events to detect.
    auto run_detection = [&](auto &out, const auto &ev_vec) {
        // Fetch the event type.
        using ev_type = typename uncvref_t<decltype(ev_vec)>::value_type;

        for (std::uint32_t i = 0; i < ev_vec.size(); ++i) {
            // Extract the pointer to the Taylor polynomial for the
            // current event.
            const auto ptr
                = ev_jet.data() + (i + dim + (is_terminal_event_v<ev_type> ? 0u : tes.size())) * (order + 1u);

            // Run the fast exclusion check to detect sign changes via
            // interval arithmetics.
            // NOTE: in case of non-finite values in the Taylor
            // coefficients of the event equation, the worst that
            // can happen here is that we end up skipping event
            // detection altogether without a warning. This is ok,
            // and non-finite Taylor coefficients will be caught in the
            // step() implementations anyway.
            std::uint32_t fex_check_result;
            fex_check(ptr, &h, &back_int, &fex_check_result);

            if (fex_check_result) {
                continue;
            }
#if 0
            {
                // Run Horner's scheme using interval
                // arithmetic.
                ival<T> acc(ptr[order]);

                for (std::uint32_t j = 1; j <= order; ++j) {
                    acc = ival<T>(ptr[order - j]) + acc * h_int;
                }

                // Check if zero is contained within the
                // resulting interval.
                // NOTE: here we are checking the closed interval [0, h],
                // even though throughout the event detection loop we are
                // actually determining roots in the half-open interval
                // [0, h). This is fine, as if there are no zeroes in
                // [0, h], there are also no zeroes in [0, h).
                const auto s_lower = sgn(acc.lower), s_upper = sgn(acc.upper);
                if (s_lower == s_upper && s_lower != 0) {
                    continue;
                }
            }
#endif

            // Clear out the list of isolating intervals.
            isol.clear();

            // Reset the working list.
            wl.clear();

            // Helper to add a detected event to out.
            // NOTE: the root here is expected to be already rescaled
            // to the [0, h) range.
            auto add_d_event = [&](T root) {
                // NOTE: we do one last check on the root in order to
                // avoid non-finite event times. This guarantees that
                // sorting the events by time is safe.
                if (!isfinite(root)) {
                    // LCOV_EXCL_START
                    get_logger()->warn("polynomial root finding produced a non-finite root of {} - skipping the event",
                                       root);
                    return;
                    // LCOV_EXCL_STOP
                }

                // Evaluate the derivative and its absolute value.
                const auto der = poly_eval_1(ptr, root, order);
                const auto abs_der = abs(der);

                // Check it before proceeding.
                if (!isfinite(der)) {
                    // LCOV_EXCL_START
                    get_logger()->warn("polynomial root finding produced the root {} with nonfinite derivative {} - "
                                       "skipping the event",
                                       root, der);
                    return;
                    // LCOV_EXCL_STOP
                }

                // Check if multiple roots are detected in the cooldown
                // period for a terminal event. For non-terminal events,
                // this will be unused.
                [[maybe_unused]] const bool has_multi_roots = [&]() {
                    if constexpr (is_terminal_event_v<ev_type>) {
                        // Establish the cooldown time.
                        // NOTE: this is the same logic that is
                        // employed in taylor.cpp to assign a cooldown
                        // to a detected terminal event. g_eps has been checked
                        // for finiteness early on, abs_der also has been checked for
                        // finiteness above.
                        const auto cd = (ev_vec[i].get_cooldown() >= 0) ? ev_vec[i].get_cooldown()
                                                                        : taylor_deduce_cooldown(g_eps, abs_der);

                        // NOTE: if the cooldown is zero, no sense to
                        // run the check.
                        if (cd == 0) {
                            return false;
                        }

                        // Evaluate the polynomial at the cooldown boundaries.
                        const auto e1 = poly_eval(ptr, root + cd, order);
                        const auto e2 = poly_eval(ptr, root - cd, order);

                        // We detect multiple roots within the cooldown
                        // if the signs of e1 and e2 are equal.
                        return (e1 > 0) == (e2 > 0);
                    } else {
                        return false;
                    }
                }();

                // Compute sign of the derivative.
                const auto d_sgn = sgn(der);

                // Fetch and cache the desired event direction.
                const auto dir = ev_vec[i].get_direction();

                if (dir == event_direction::any) {
                    // If the event direction does not
                    // matter, just add it.
                    if constexpr (is_terminal_event_v<ev_type>) {
                        out.emplace_back(i, root, has_multi_roots, d_sgn, abs_der);
                    } else {
                        out.emplace_back(i, root, d_sgn);
                    }
                } else {
                    // Otherwise, we need to record the event only if its direction
                    // matches the sign of the derivative.
                    if (static_cast<event_direction>(d_sgn) == dir) {
                        if constexpr (is_terminal_event_v<ev_type>) {
                            out.emplace_back(i, root, has_multi_roots, d_sgn, abs_der);
                        } else {
                            out.emplace_back(i, root, d_sgn);
                        }
                    }
                }
            };

            // NOTE: if we are dealing with a terminal event on cooldown,
            // we will need to ignore roots within the cooldown period.
            // lb_offset is the value in the original [0, 1) range corresponding
            // to the end of the cooldown.
            const auto lb_offset = [&]() {
                if constexpr (is_terminal_event_v<ev_type>) {
                    if (cooldowns[i]) {
                        // NOTE: need to distinguish between forward
                        // and backward integration.
                        if (h >= 0) {
                            return (cooldowns[i]->second - cooldowns[i]->first) / abs(h);
                        } else {
                            return (cooldowns[i]->second + cooldowns[i]->first) / abs(h);
                        }
                    }
                }

                // NOTE: we end up here if the event is not terminal
                // or not on cooldown.
                return T(0);
            }();

            if (lb_offset >= 1) {
                // LCOV_EXCL_START
                // NOTE: the whole integration range is in the cooldown range,
                // move to the next event.
                SPDLOG_LOGGER_DEBUG(
                    get_logger(),
                    "the integration timestep falls within the cooldown range for the terminal event {}, skipping", i);
                continue;
                // LCOV_EXCL_STOP
            }

            // Rescale the event polynomial so that the range [0, h)
            // becomes [0, 1), and write the resulting polynomial into tmp.
            // NOTE: at the first iteration (i.e., for the first event),
            // tmp has been constructed correctly outside this function.
            // Below, tmp will first be moved into wl (thus rendering
            // it invalid) but it will immediately be revived at the
            // first iteration of the do/while loop. Thus, when we get
            // here again, tmp will be again in a well-formed state.
            assert(!tmp.v.empty());             // LCOV_EXCL_LINE
            assert(tmp.v.size() - 1u == order); // LCOV_EXCL_LINE
            poly_rescale(tmp.v.data(), ptr, h, order);

            // Place the first element in the working list.
            wl.emplace_back(0, 1, std::move(tmp));

#if !defined(NDEBUG)
            auto max_wl_size = wl.size();
            auto max_isol_size = isol.size();
#endif

            // Flag to signal that the do-while loop below failed.
            bool loop_failed = false;

            do {
                // Fetch the current interval and polynomial from the working list.
                // NOTE: from now on, tmp contains the polynomial referred
                // to as q(x) in the real-root isolation wikipedia page.
                // NOTE: q(x) is the transformed polynomial whose roots in the x range [0, 1) we will
                // be looking for. lb and ub represent what 0 and 1 correspond to in the *original*
                // [0, 1) range.
                auto lb = std::get<0>(wl.back());
                auto ub = std::get<1>(wl.back());
                // NOTE: this will either revive an invalid tmp (first iteration),
                // or it will replace it with one of the bisecting polynomials.
                tmp = std::move(std::get<2>(wl.back()));
                wl.pop_back();

                // Check for an event at the lower bound, which occurs
                // if the constant term of the polynomial is zero. We also
                // check for finiteness of all the other coefficients, otherwise
                // we cannot really claim to have detected an event.
                // When we do proper root finding below, the
                // algorithm should be able to detect non-finite
                // polynomials.
                if (tmp.v[0] == T(0) // LCOV_EXCL_LINE
                    && std::all_of(tmp.v.data() + 1, tmp.v.data() + 1 + order,
                                   [](const auto &x) { return isfinite(x); })) {
                    // NOTE: we will have to skip the event if we are dealing
                    // with a terminal event on cooldown and the lower bound
                    // falls within the cooldown time.
                    bool skip_event = false;
                    if constexpr (is_terminal_event_v<ev_type>) {
                        if (lb < lb_offset) {
                            SPDLOG_LOGGER_DEBUG(get_logger(),
                                                "terminal event {} detected at the beginning of an isolating interval "
                                                "is subject to cooldown, ignoring",
                                                i);
                            skip_event = true;
                        }
                    }

                    if (!skip_event) {
                        // NOTE: the original range had been rescaled wrt to h.
                        // Thus, we need to rescale back when adding the detected
                        // event.
                        add_d_event(lb * h);
                    }
                }

                // Reverse tmp into tmp1, translate tmp1 by 1 with output
                // in tmp2, and count the sign changes in tmp2.
                std::uint32_t n_sc;
                rtscc(tmp1.v.data(), tmp2.v.data(), &n_sc, tmp.v.data());

                if (n_sc == 1u) {
                    // Found isolating interval, add it to isol.
                    isol.emplace_back(lb, ub);
                } else if (n_sc > 1u) {
                    // No isolating interval found, bisect.

                    // First we transform q into 2**n * q(x/2) and store the result
                    // into tmp1.
                    poly_rescale_p2(tmp1.v.data(), tmp.v.data(), order);
                    // Then we take tmp1 and translate it to produce 2**n * q((x+1)/2).
                    pt(tmp2.v.data(), tmp1.v.data());

                    // Finally we add tmp1 and tmp2 to the working list.
                    const auto mid = (lb + ub) / 2;
                    // NOTE: don't add the lower range if it falls
                    // entirely within the cooldown range.
                    if (lb_offset < mid) {
                        wl.emplace_back(lb, mid, std::move(tmp1));

                        // Revive tmp1.
                        tmp1 = pwrap<T>(pc, order);
                    } else {
                        // LCOV_EXCL_START
                        SPDLOG_LOGGER_DEBUG(
                            get_logger(),
                            "ignoring lower interval in a bisection that would fall entirely in the cooldown period");
                        // LCOV_EXCL_STOP
                    }
                    wl.emplace_back(mid, ub, std::move(tmp2));

                    // Revive tmp2.
                    tmp2 = pwrap<T>(pc, order);
                }

#if !defined(NDEBUG)
                max_wl_size = std::max(max_wl_size, wl.size());
                max_isol_size = std::max(max_isol_size, isol.size());
#endif

                // We want to put limits in order to avoid an endless loop when the algorithm fails.
                // The first check is on the working list size and it is based
                // on heuristic observation of the algorithm's behaviour in pathological
                // cases. The second check is that we cannot possibly find more isolating
                // intervals than the degree of the polynomial.
                if (wl.size() > 250u || isol.size() > order) {
                    get_logger()->warn(
                        "the polynomial root isolation algorithm failed during event detection: the working "
                        "list size is {} and the number of isolating intervals is {}",
                        wl.size(), isol.size());

                    loop_failed = true;

                    break;
                }

            } while (!wl.empty());

#if !defined(NDEBUG)
            SPDLOG_LOGGER_DEBUG(get_logger(), "max working list size: {}", max_wl_size);
            SPDLOG_LOGGER_DEBUG(get_logger(), "max isol list size   : {}", max_isol_size);
#endif

            if (isol.empty() || loop_failed) {
                // Don't do root finding for this event if the loop failed,
                // or if the list of isolating intervals is empty. Just
                // move to the next event.
                continue;
            }

            // Reconstruct a version of the original event polynomial
            // in which the range [0, h) is rescaled to [0, 1). We need
            // to do root finding on the rescaled polynomial because the
            // isolating intervals are also rescaled to [0, 1).
            // NOTE: tmp1 was either created with the correct size outside this
            // function, or it was re-created in the bisection above.
            poly_rescale(tmp1.v.data(), ptr, h, order);

            // Run the root finding in the isolating intervals.
            for (auto &[lb, ub] : isol) {
                if constexpr (is_terminal_event_v<ev_type>) {
                    // NOTE: if we are dealing with a terminal event
                    // subject to cooldown, we need to ensure that
                    // we don't look for roots before the cooldown has expired.
                    if (lb < lb_offset) {
                        // Make sure we move lb past the cooldown.
                        lb = lb_offset;

                        // NOTE: this should be ensured by the fact that
                        // we ensure above (lb_offset < mid) that we don't
                        // end up with an invalid interval.
                        assert(lb < ub); // LCOV_EXCL_LINE

                        // Check if the interval still contains a zero.
                        const auto f_lb = poly_eval(tmp1.v.data(), lb, order);
                        const auto f_ub = poly_eval(tmp1.v.data(), ub, order);

                        if (!(f_lb * f_ub < 0)) {
                            SPDLOG_LOGGER_DEBUG(get_logger(), "terminal event {} is subject to cooldown, ignoring", i);
                            continue;
                        }
                    }
                }

                // Run the root finding.
                const auto [root, cflag] = bracketed_root_find(tmp1, order, lb, ub);

                if (cflag == 0) {
                    // Root finding finished successfully, record the event.
                    // The found root needs to be rescaled by h.
                    add_d_event(root * h);
                } else {
                    // Root finding encountered some issue. Ignore the
                    // event and log the issue.
                    if (cflag == -1) {
                        // LCOV_EXCL_START
                        get_logger()->warn(
                            "polynomial root finding during event detection failed due to too many iterations");
                        // LCOV_EXCL_STOP
                    } else {
                        get_logger()->warn(
                            "polynomial root finding during event detection returned a nonzero errno with message '{}'",
                            std::strerror(cflag));
                    }
                }
            }
        }
    };

    run_detection(d_tes, tes);
    run_detection(d_ntes, ntes);
}

} // namespace

template <>
void taylor_detect_events(std::vector<std::tuple<std::uint32_t, double, bool, int, double>> &d_tes,
                          std::vector<std::tuple<std::uint32_t, double, int>> &d_ntes,
                          const std::vector<t_event<double>> &tes, const std::vector<nt_event<double>> &ntes,
                          const std::vector<std::optional<std::pair<double, double>>> &cooldowns, double h,
                          const std::vector<double> &ev_jet, std::uint32_t order, std::uint32_t dim, double g_eps)
{
    taylor_detect_events_impl(d_tes, d_ntes, tes, ntes, cooldowns, h, ev_jet, order, dim, g_eps);
}

template <>
void taylor_detect_events(std::vector<std::tuple<std::uint32_t, long double, bool, int, long double>> &d_tes,
                          std::vector<std::tuple<std::uint32_t, long double, int>> &d_ntes,
                          const std::vector<t_event<long double>> &tes, const std::vector<nt_event<long double>> &ntes,
                          const std::vector<std::optional<std::pair<long double, long double>>> &cooldowns,
                          long double h, const std::vector<long double> &ev_jet, std::uint32_t order, std::uint32_t dim,
                          long double g_eps)
{
    taylor_detect_events_impl(d_tes, d_ntes, tes, ntes, cooldowns, h, ev_jet, order, dim, g_eps);
}

#if defined(HEYOKA_HAVE_REAL128)

template <>
void taylor_detect_events(std::vector<std::tuple<std::uint32_t, mppp::real128, bool, int, mppp::real128>> &d_tes,
                          std::vector<std::tuple<std::uint32_t, mppp::real128, int>> &d_ntes,
                          const std::vector<t_event<mppp::real128>> &tes,
                          const std::vector<nt_event<mppp::real128>> &ntes,
                          const std::vector<std::optional<std::pair<mppp::real128, mppp::real128>>> &cooldowns,
                          mppp::real128 h, const std::vector<mppp::real128> &ev_jet, std::uint32_t order,
                          std::uint32_t dim, mppp::real128 g_eps)
{
    taylor_detect_events_impl(d_tes, d_ntes, tes, ntes, cooldowns, h, ev_jet, order, dim, g_eps);
}

#endif

namespace
{

// Helper to automatically deduce the cooldown
// for a terminal event. g_eps is the maximum
// absolute error on the Taylor series of the event
// equation (which depends on the integrator tolerance
// and the infinity norm of the state vector/event equations),
// abs_der is the absolute value of the time derivative of the
// event equation at the zero.
template <typename T>
T taylor_deduce_cooldown_impl(T g_eps, T abs_der)
{
    using std::isfinite;

    // LCOV_EXCL_START
    assert(isfinite(g_eps));
    assert(isfinite(abs_der));
    assert(g_eps >= 0);
    assert(abs_der >= 0);
    // LCOV_EXCL_STOP

    // NOTE: the * 10 is a safety factor composed of:
    // - 2 is the original factor from theoretical considerations,
    // - 2 factor to deal with very small values of the derivative,
    // - 2 factor to deal with the common case of event equation
    //   flipping around after the event (e.g., for collisions).
    // The rest is additional safety.
    auto ret = g_eps / abs_der * 10;

    if (isfinite(ret)) {
        return ret;
        // LCOV_EXCL_START
    } else {
        get_logger()->warn("deducing a cooldown of zero for a terminal event because the automatic deduction "
                           "heuristic produced a non-finite value of {}",
                           ret);

        return 0;
    }
    // LCOV_EXCL_STOP
}

} // namespace

template <>
double taylor_deduce_cooldown(double g_eps, double abs_der)
{
    return taylor_deduce_cooldown_impl(g_eps, abs_der);
}

template <>
long double taylor_deduce_cooldown(long double g_eps, long double abs_der)
{
    return taylor_deduce_cooldown_impl(g_eps, abs_der);
}

#if defined(HEYOKA_HAVE_REAL128)

template <>
mppp::real128 taylor_deduce_cooldown(mppp::real128 g_eps, mppp::real128 abs_der)
{
    return taylor_deduce_cooldown_impl(g_eps, abs_der);
}

#endif

} // namespace heyoka::detail
