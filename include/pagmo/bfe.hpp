/* Copyright 2017-2018 PaGMO development team

This file is part of the PaGMO library.

The PaGMO library is free software; you can redistribute it and/or modify
it under the terms of either:

  * the GNU Lesser General Public License as published by the Free
    Software Foundation; either version 3 of the License, or (at your
    option) any later version.

or

  * the GNU General Public License as published by the Free Software
    Foundation; either version 3 of the License, or (at your option) any
    later version.

or both in parallel, as here.

The PaGMO library is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received copies of the GNU General Public License and the
GNU Lesser General Public License along with the PaGMO library.  If not,
see https://www.gnu.org/licenses/. */

#ifndef PAGMO_BFE_HPP
#define PAGMO_BFE_HPP

#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>

#include <boost/numeric/conversion/cast.hpp>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <pagmo/detail/bfe_impl.hpp>
#include <pagmo/detail/make_unique.hpp>
#include <pagmo/exceptions.hpp>
#include <pagmo/problem.hpp>
#include <pagmo/serialization.hpp>
#include <pagmo/threading.hpp>
#include <pagmo/type_traits.hpp>
#include <pagmo/types.hpp>

#define PAGMO_REGISTER_BFE(b) CEREAL_REGISTER_TYPE_WITH_NAME(pagmo::detail::bfe_inner<b>, "udbfe " #b)

namespace pagmo
{

// Check if T has a call operator conforming to the UDBFE requirements.
template <typename T>
class has_bfe_call_operator
{
    template <typename U>
    using call_t
        = decltype(std::declval<const U &>()(std::declval<const problem &>(), std::declval<const vector_double &>()));
    static const bool implementation_defined = std::is_same<detected_t<call_t, T>, vector_double>::value;

public:
    static const bool value = implementation_defined;
};

template <typename T>
const bool has_bfe_call_operator<T>::value;

namespace detail
{

// Specialise this to true in order to disable all the UDBFE checks and mark a type
// as a UDBFE regardless of the features provided by it.
// NOTE: this is needed when implementing the machinery for Python batch evaluators.
// NOTE: leave this as an implementation detail for now.
template <typename>
struct disable_udbfe_checks : std::false_type {
};

} // namespace detail

// Check if T is a UDBFE.
template <typename T>
class is_udbfe
{
    static const bool implementation_defined
        = (std::is_same<T, uncvref_t<T>>::value && std::is_default_constructible<T>::value
           && std::is_copy_constructible<T>::value && std::is_move_constructible<T>::value
           && std::is_destructible<T>::value && has_bfe_call_operator<T>::value)
          || detail::disable_udbfe_checks<T>::value;

public:
    static const bool value = implementation_defined;
};

template <typename T>
const bool is_udbfe<T>::value;

namespace detail
{

struct bfe_inner_base {
    virtual ~bfe_inner_base() {}
    virtual std::unique_ptr<bfe_inner_base> clone() const = 0;
    virtual vector_double operator()(const problem &, const vector_double &) const = 0;
    virtual std::string get_name() const = 0;
    virtual std::string get_extra_info() const = 0;
    virtual thread_safety get_thread_safety() const = 0;
    template <typename Archive>
    void serialize(Archive &)
    {
    }
};

template <typename T>
struct bfe_inner final : bfe_inner_base {
    // We just need the def ctor, delete everything else.
    bfe_inner() = default;
    bfe_inner(const bfe_inner &) = delete;
    bfe_inner(bfe_inner &&) = delete;
    bfe_inner &operator=(const bfe_inner &) = delete;
    bfe_inner &operator=(bfe_inner &&) = delete;
    // Constructors from T (copy and move variants).
    explicit bfe_inner(const T &x) : m_value(x) {}
    explicit bfe_inner(T &&x) : m_value(std::move(x)) {}
    // The clone method, used in the copy constructor of bfe.
    virtual std::unique_ptr<bfe_inner_base> clone() const override final
    {
        return detail::make_unique<bfe_inner>(m_value);
    }
    // Mandatory methods.
    virtual vector_double operator()(const problem &p, const vector_double &dvs) const override final
    {
        return m_value(p, dvs);
    }
    // Optional methods.
    virtual std::string get_name() const override final
    {
        return get_name_impl(m_value);
    }
    virtual std::string get_extra_info() const override final
    {
        return get_extra_info_impl(m_value);
    }
    virtual thread_safety get_thread_safety() const override final
    {
        return get_thread_safety_impl(m_value);
    }
    // Implementation of the optional methods.
    template <typename U, enable_if_t<has_name<U>::value, int> = 0>
    static std::string get_name_impl(const U &value)
    {
        return value.get_name();
    }
    template <typename U, enable_if_t<!has_name<U>::value, int> = 0>
    static std::string get_name_impl(const U &)
    {
        return typeid(U).name();
    }
    template <typename U, enable_if_t<has_extra_info<U>::value, int> = 0>
    static std::string get_extra_info_impl(const U &value)
    {
        return value.get_extra_info();
    }
    template <typename U, enable_if_t<!has_extra_info<U>::value, int> = 0>
    static std::string get_extra_info_impl(const U &)
    {
        return "";
    }
    template <typename U, enable_if_t<has_get_thread_safety<U>::value, int> = 0>
    static thread_safety get_thread_safety_impl(const U &value)
    {
        return value.get_thread_safety();
    }
    template <typename U, enable_if_t<!has_get_thread_safety<U>::value, int> = 0>
    static thread_safety get_thread_safety_impl(const U &)
    {
        return thread_safety::basic;
    }
    // Serialization.
    template <typename Archive>
    void serialize(Archive &ar)
    {
        ar(cereal::base_class<bfe_inner_base>(this), m_value);
    }
    T m_value;
};

} // namespace detail

// Multi-threaded bfe.
class thread_bfe
{
public:
    // Call operator.
    vector_double operator()(const problem &p, const vector_double &dvs) const
    {
        // Fetch a few quantities from the problem.
        // Problem dimension.
        const auto n_dim = p.get_nx();
        // Fitness dimension.
        const auto f_dim = p.get_nf();
        // Total number of dvs.
        const auto n_dvs = dvs.size() / n_dim;

        // NOTE: as usual, we assume that thread_bfe is always wrapped
        // by a bfe, where we already check that dvs
        // is compatible with p.
        // NOTE: this is what we always do with user-defined classes:
        // we do the sanity checks in the type-erased container.
        assert(dvs.size() % n_dim == 0u);

        // Prepare the return value.
        // Guard against overflow.
        // LCOV_EXCL_START
        if (n_dvs > std::numeric_limits<vector_double::size_type>::max() / f_dim) {
            pagmo_throw(std::overflow_error,
                        "Overflow detected in the computation of the size of the output of a thread_bfe");
        }
        // LCOV_EXCL_STOP
        vector_double retval(n_dvs * f_dim);

        // Functor to implement the fitness evaluation of a range of input dvs. begin/end are the indices
        // of the individuals in dv (ranging from 0 to n_dvs), the resulting fitnesses will be written directly into
        // retval.
        auto range_evaluator = [&dvs, &retval, n_dim, f_dim, n_dvs](const problem &prob, decltype(dvs.size()) begin,
                                                                    decltype(dvs.size()) end) {
            assert(begin <= end);
            assert(end <= n_dvs);
            (void)n_dvs;

            // Temporary dv that will be used for fitness evaluation.
            vector_double tmp_dv(n_dim);
            for (; begin != end; ++begin) {
                auto in_ptr = dvs.data() + begin * n_dim;
                auto out_ptr = retval.data() + begin * f_dim;
                std::copy(
#if defined(_MSC_VER)
                    stdext::make_checked_array_iterator(in_ptr, n_dim),
                    stdext::make_checked_array_iterator(in_ptr, n_dim, n_dim), tmp_dv.begin()
#else
                    in_ptr, in_ptr + n_dim, tmp_dv.begin()
#endif
                );
                const auto fv = prob.fitness(tmp_dv);
                assert(fv.size() == f_dim);
                std::copy(
#if defined(_MSC_VER)
                    fv.begin(), fv.end(), stdext::make_checked_array_iterator(out_ptr, f_dim)
#else
                    fv.begin(), fv.end(), out_ptr
#endif
                );
            }
        };

        using range_t = tbb::blocked_range<decltype(dvs.size())>;
        if (p.get_thread_safety() >= thread_safety::constant) {
            // We can concurrently call the objfun on the input prob, hence we can
            // capture it by reference and do all the fitness calls on the same object.
            tbb::parallel_for(range_t(0u, n_dvs), [&p, &range_evaluator](const range_t &range) {
                range_evaluator(p, range.begin(), range.end());
            });
        } else if (p.get_thread_safety() == thread_safety::basic) {
            // We cannot concurrently call the objfun on the input prob. We will need
            // to make a copy of p for each parallel iteration.
            tbb::parallel_for(range_t(0u, n_dvs), [p, &range_evaluator](const range_t &range) {
                range_evaluator(p, range.begin(), range.end());
            });
            // Manually increment the fitness eval counter in p. Since we used copies
            // of p for the parallel fitness evaluations, the counter in p did not change.
            p.increment_fevals(boost::numeric_cast<unsigned long long>(n_dvs));
        } else {
            pagmo_throw(std::invalid_argument, "Cannot use a thread_bfe on the problem '" + p.get_name()
                                                   + "', which does not provide the required level of thread safety");
        }

        return retval;
    }
    // Name.
    std::string get_name() const
    {
        return "Multi-threaded batch fitness evaluator";
    }
    // Serialization support.
    template <typename Archive>
    void serialize(Archive &)
    {
    }
};

// Bfe that uses problem's member function.
class member_bfe
{
public:
    // Call operator.
    vector_double operator()(const problem &p, const vector_double &dvs) const
    {
        return detail::prob_invoke_mem_batch_fitness(p, dvs);
    }
    // Name.
    std::string get_name() const
    {
        return "Member function batch fitness evaluator";
    }
    // Serialization support.
    template <typename Archive>
    void serialize(Archive &)
    {
    }
};

namespace detail
{

// Usual trick for global variables in header file.
template <typename = void>
struct default_bfe_impl {
    static std::function<vector_double(const problem &, const vector_double &)> s_func;
};

// C++ implementation of the heuristic for the automatic deduction of the "best"
// bfe strategy.
inline vector_double default_bfe_cpp_impl(const problem &p, const vector_double &dvs)
{
    // The member function batch_fitness() of p, if present, has priority.
    if (p.has_batch_fitness()) {
        return member_bfe{}(p, dvs);
    }
    // Otherwise, we run the generic thread-based bfe, if the problem
    // is thread-safe enough.
    if (p.get_thread_safety() >= thread_safety::basic) {
        return thread_bfe{}(p, dvs);
    }
    pagmo_throw(std::invalid_argument,
                "Cannot execute fitness evaluations in batch mode for a problem of type '" + p.get_name()
                    + "': the problem does not implement the batch_fitness() member function, and its thread safety "
                      "level is not sufficient to run a thread-based batch fitness evaluation implementation");
}

template <typename T>
std::function<vector_double(const problem &, const vector_double &)> default_bfe_impl<T>::s_func
    = &default_bfe_cpp_impl;

} // namespace detail

// Default bfe implementation.
class default_bfe
{
public:
    // Call operator.
    vector_double operator()(const problem &p, const vector_double &dvs) const
    {
        return detail::default_bfe_impl<>::s_func(p, dvs);
    }
    // Name.
    std::string get_name() const
    {
        return "Default batch fitness evaluator";
    }
    // Serialization support.
    template <typename Archive>
    void serialize(Archive &)
    {
    }
};

class bfe
{
    // Enable the generic ctor only if T is not a bfe (after removing
    // const/reference qualifiers), and if T is a udbfe. Additionally,
    // enable the ctor also if T is a function type (in that case, we
    // will convert the function type to a function pointer in
    // the machinery below).
    template <typename T>
    using generic_ctor_enabler
        = enable_if_t<(!std::is_same<bfe, uncvref_t<T>>::value && is_udbfe<uncvref_t<T>>::value)
                          || std::is_same<vector_double(const problem &, const vector_double &), uncvref_t<T>>::value,
                      int>;
    // Dispatching for the generic ctor. We have a special case if T is
    // a function type, in which case we will manually do the conversion to
    // function pointer and delegate to the other overload.
    template <typename T>
    explicit bfe(T &&x, std::true_type)
        : bfe(static_cast<vector_double (*)(const problem &, const vector_double &)>(std::forward<T>(x)),
              std::false_type{})
    {
    }
    template <typename T>
    explicit bfe(T &&x, std::false_type)
        : m_ptr(detail::make_unique<detail::bfe_inner<uncvref_t<T>>>(std::forward<T>(x)))
    {
    }

public:
    // Default ctor.
    bfe() : bfe(default_bfe{}) {}
    // Constructor from a UDBFE.
    template <typename T, generic_ctor_enabler<T> = 0>
    explicit bfe(T &&x) : bfe(std::forward<T>(x), std::is_function<uncvref_t<T>>{})
    {
        // Assign the name.
        m_name = ptr()->get_name();
        // Assign the thread safety level.
        m_thread_safety = ptr()->get_thread_safety();
    }
    // Copy constructor.
    bfe(const bfe &other) : m_ptr(other.ptr()->clone()), m_name(other.m_name), m_thread_safety(other.m_thread_safety) {}
    // Move constructor. The default implementation is fine.
    bfe(bfe &&) noexcept = default;
    // Move assignment operator
    bfe &operator=(bfe &&other) noexcept
    {
        if (this != &other) {
            m_ptr = std::move(other.m_ptr);
            m_name = std::move(other.m_name);
            m_thread_safety = std::move(other.m_thread_safety);
        }
        return *this;
    }
    // Copy assignment operator
    bfe &operator=(const bfe &other)
    {
        // Copy ctor + move assignment.
        return *this = bfe(other);
    }
    // Extraction and related.
    template <typename T>
    const T *extract() const noexcept
    {
        auto p = dynamic_cast<const detail::bfe_inner<T> *>(ptr());
        return p == nullptr ? nullptr : &(p->m_value);
    }
    template <typename T>
    T *extract() noexcept
    {
        auto p = dynamic_cast<detail::bfe_inner<T> *>(ptr());
        return p == nullptr ? nullptr : &(p->m_value);
    }
    template <typename T>
    bool is() const noexcept
    {
        return extract<T>() != nullptr;
    }
    // Call operator.
    vector_double operator()(const problem &p, const vector_double &dvs) const
    {
        // Check the input dvs.
        detail::bfe_check_input_dvs(p, dvs);
        // Invoke the call operator from the UDBFE.
        auto retval((*ptr())(p, dvs));
        // Check the produced vector of fitnesses.
        detail::bfe_check_output_fvs(p, dvs, retval);
        return retval;
    }
    // Name.
    std::string get_name() const
    {
        return m_name;
    }
    // Extra info.
    std::string get_extra_info() const
    {
        return ptr()->get_extra_info();
    }
    // Thread safety level.
    thread_safety get_thread_safety() const
    {
        return m_thread_safety;
    }
    // Serialisation support.
    template <typename Archive>
    void save(Archive &ar) const
    {
        ar(m_ptr, m_name, m_thread_safety);
    }
    template <typename Archive>
    void load(Archive &ar)
    {
        // Deserialize in a separate object and move it in later, for exception safety.
        bfe tmp_bfe;
        ar(tmp_bfe.m_ptr, tmp_bfe.m_name, tmp_bfe.m_thread_safety);
        *this = std::move(tmp_bfe);
    }
    // Stream operator.
    friend std::ostream &operator<<(std::ostream &os, const bfe &b)
    {
        os << "BFE name: " << b.get_name() << '\n';
        os << "\n\tThread safety: " << b.get_thread_safety() << '\n';
        const auto extra_str = b.get_extra_info();
        if (!extra_str.empty()) {
            os << "\nExtra info:\n" << extra_str << '\n';
        }
        return os;
    }

private:
    // Just two small helpers to make sure that whenever we require
    // access to the pointer it actually points to something.
    detail::bfe_inner_base const *ptr() const
    {
        assert(m_ptr.get() != nullptr);
        return m_ptr.get();
    }
    detail::bfe_inner_base *ptr()
    {
        assert(m_ptr.get() != nullptr);
        return m_ptr.get();
    }

private:
    // Pointer to the inner base bfe
    std::unique_ptr<detail::bfe_inner_base> m_ptr;
    // Various properties determined at construction time
    // from the udbfe. These will be constant for the lifetime
    // of bfe, but we cannot mark them as such because we want to be
    // able to assign and deserialise bfes.
    std::string m_name;
    // Thread safety.
    thread_safety m_thread_safety;
};

} // namespace pagmo

PAGMO_REGISTER_BFE(pagmo::thread_bfe)
PAGMO_REGISTER_BFE(pagmo::member_bfe)
PAGMO_REGISTER_BFE(pagmo::default_bfe)

#endif
