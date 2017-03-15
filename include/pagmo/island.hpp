/* Copyright 2017 PaGMO development team

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

#ifndef PAGMO_ISLAND_HPP
#define PAGMO_ISLAND_HPP

#include <boost/any.hpp>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include <pagmo/algorithm.hpp>
#include <pagmo/detail/make_unique.hpp>
#include <pagmo/detail/task_queue.hpp>
#include <pagmo/exceptions.hpp>
#include <pagmo/io.hpp>
#include <pagmo/population.hpp>
#include <pagmo/rng.hpp>
#include <pagmo/serialization.hpp>
#include <pagmo/threading.hpp>
#include <pagmo/type_traits.hpp>

/// Macro for the registration of the serialization functionality for user-defined islands.
/**
 * This macro should always be invoked after the declaration of a user-defined island: it will register
 * the island with pagmo's serialization machinery. The macro should be called in the root namespace
 * and using the fully qualified name of the island to be registered. For example:
 * @code{.unparsed}
 * namespace my_namespace
 * {
 *
 * class my_island
 * {
 *    // ...
 * };
 *
 * }
 *
 * PAGMO_REGISTER_ISLAND(my_namespace::my_island)
 * @endcode
 */
#define PAGMO_REGISTER_ISLAND(isl) CEREAL_REGISTER_TYPE_WITH_NAME(pagmo::detail::isl_inner<isl>, "udi " #isl)

namespace pagmo
{

/// Detect \p run_evolve() method.
/**
 * This type trait will be \p true if \p T provides a method with
 * the following signature:
 * @code{.unparsed}
 * void run_evolve(pagmo::algorithm &, ulock_t &, pagmo::population &, ulock_t &);
 * @endcode
 * where \p ulock_t is <tt>std::unique_lock<std::mutex></tt>.
 * The \p run_evolve() method is part of the interface for the definition of an island
 * (see pagmo::island).
 */
template <typename T>
class has_run_evolve
{
    using ulock_t = std::unique_lock<std::mutex>;
    template <typename U>
    using run_evolve_t
        = decltype(std::declval<U &>().run_evolve(std::declval<algorithm &>(), std::declval<ulock_t &>(),
                                                  std::declval<population &>(), std::declval<ulock_t &>()));
    static const bool implementation_defined = std::is_same<void, detected_t<run_evolve_t, T>>::value;

public:
    /// Value of the type trait.
    static const bool value = implementation_defined;
};

template <typename T>
const bool has_run_evolve<T>::value;

namespace detail
{

// Specialise this to true in order to disable all the UDI checks and mark a type
// as a UDI regardless of the features provided by it.
// NOTE: this is needed when implementing the machinery for Python islands.
// NOTE: leave this as an implementation detail for now.
template <typename>
struct disable_udi_checks : std::false_type {
};
}

/// Detect user-defined islands (UDI).
/**
 * This type trait will be \p true if \p T is not cv/reference qualified, it is destructible, default, copy and move
 * constructible, and if it satisfies the pagmo::has_run_evolve type trait.
 *
 * Types satisfying this type trait can be used as user-defined islands (UDI) in pagmo::island.
 */
template <typename T>
class is_udi
{
    static const bool implementation_defined
        = (std::is_same<T, uncvref_t<T>>::value && std::is_default_constructible<T>::value
           && std::is_copy_constructible<T>::value && std::is_move_constructible<T>::value
           && std::is_destructible<T>::value && has_run_evolve<T>::value)
          || detail::disable_udi_checks<T>::value;

public:
    /// Value of the type trait.
    static const bool value = implementation_defined;
};

template <typename T>
const bool is_udi<T>::value;

namespace detail
{

struct isl_inner_base {
    using ulock_t = std::unique_lock<std::mutex>;
    virtual ~isl_inner_base()
    {
    }
    virtual std::unique_ptr<isl_inner_base> clone() const = 0;
    virtual void run_evolve(algorithm &, ulock_t &, population &, ulock_t &) = 0;
    virtual std::string get_name() const = 0;
    virtual std::string get_extra_info() const = 0;
    template <typename Archive>
    void serialize(Archive &)
    {
    }
};

template <typename T>
struct isl_inner final : isl_inner_base {
    // We just need the def ctor, delete everything else.
    isl_inner() = default;
    isl_inner(const isl_inner &) = delete;
    isl_inner(isl_inner &&) = delete;
    isl_inner &operator=(const isl_inner &) = delete;
    isl_inner &operator=(isl_inner &&) = delete;
    // Constructors from T.
    explicit isl_inner(const T &x) : m_value(x)
    {
    }
    explicit isl_inner(T &&x) : m_value(std::move(x))
    {
    }
    // The clone method, used in the copy constructor of island.
    virtual std::unique_ptr<isl_inner_base> clone() const override final
    {
        return make_unique<isl_inner>(m_value);
    }
    // The enqueue_evolution() method.
    virtual void run_evolve(algorithm &algo, ulock_t &algo_lock, population &pop, ulock_t &pop_lock) override final
    {
        m_value.run_evolve(algo, algo_lock, pop, pop_lock);
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
    // Serialization
    template <typename Archive>
    void serialize(Archive &ar)
    {
        ar(cereal::base_class<isl_inner_base>(this), m_value);
    }
    T m_value;
};

// NOTE: this construct is used to create a RAII-style object at the beginning
// of island::wait(). Normally this object's constructor and destructor will not
// do anything, but in Python we need to override this getter to that it returns
// a RAII object that unlocks the GIL, otherwise we could run into deadlocks in Python
// if isl::wait() holds the GIL while waiting.
template <typename = void>
struct wait_raii {
    static std::function<boost::any()> getter;
};

// NOTE: the default implementation just returns a defcted boost::any, whose ctor and dtor
// will have no effect.
template <typename T>
std::function<boost::any()> wait_raii<T>::getter = []() { return boost::any{}; };
}

/// Thread island.
/**
 * This class is a user-defined island (UDI) that will run evolutions directly inside
 * the separate thread of execution within pagmo::island.
 */
class thread_island
{
    template <typename T>
    static void check_thread_safety(const T &x)
    {
        if (static_cast<int>(x.get_thread_safety()) < static_cast<int>(thread_safety::basic)) {
            pagmo_throw(
                std::invalid_argument,
                "thread islands require objects which provide at least the basic thread safety level, but the object '"
                    + x.get_name() + "' provides only the '"
                    + std::string(x.get_thread_safety() == thread_safety::copyonly ? "copyonly" : "none")
                    + "' thread safety guarantee");
        }
    }

public:
    /// Island's name.
    /**
     * @return <tt>"Thread island"</tt>.
     */
    std::string get_name() const
    {
        return "Thread island";
    }
    /// Run evolve.
    /**
     * This method will invoke the <tt>evolve()</tt> method on a copy of \p algo, using a copy
     * of \p pop as argument, and it will then assign the result of the evolution back to \p pop.
     * During the evolution the input locks are released.
     *
     * @param algo the algorithm that will be used for the evolution.
     * @param algo_lock a locked lock that guarantees exclusive access to \p algo.
     * @param pop the population that will be evolved by \p algo.
     * @param pop_lock a locked lock that guarantees exclusive access to \p pop.
     *
     * @throws std::invalid_argument if either \p algo or <tt>pop</tt>'s problem do not provide
     * at least the pagmo::thread_safety::basic thread safety guarantee.
     * @throws unspecified any exception thrown by:
     * - threading primitives,
     * - the copy constructors of \p pop and \p algo,
     * - the <tt>evolve()</tt> method of \p algo.
     */
    void run_evolve(algorithm &algo, std::unique_lock<std::mutex> &algo_lock, population &pop,
                    std::unique_lock<std::mutex> &pop_lock)
    {
        // NOTE: these are checks run on pagmo::algo/prob, both of which have thread-safe implementations
        // of the get_thread_safety() method.
        check_thread_safety(algo);
        check_thread_safety(pop.get_problem());

        // Create copies of algo/pop and unlock the locks.
        // NOTE: copies cannot alter the thread safety property of algo/prob, as
        // it is a class member.
        auto algo_copy(algo);
        algo_lock.unlock();
        auto pop_copy(pop);
        pop_lock.unlock();

        // Run the actual evolution.
        auto new_pop(algo_copy.evolve(pop_copy));

        // Lock and assign back.
        pop_lock.lock();
        // NOTE: this does not need any thread safety, as we are just moving in a problem pointer.
        pop = std::move(new_pop);
    }
    /// Serialization support.
    /**
     * This class is stateless, no data will be saved to or loaded from the archive.
     */
    template <typename Archive>
    void serialize(Archive &)
    {
    }
};

class archipelago;

namespace detail
{

// NOTE: this structure holds an std::function that implements the logic for the selection of the UDI
// type in the constructor of island_data. The logic is decoupled so that we can override the default logic with
// alternative implementations (e.g., use a process-based island rather than the default thread island if prob, algo,
// etc. do not provide thread safety).
template <typename = void>
struct island_factory {
    using func_t
        = std::function<void(const algorithm &, const population &, std::unique_ptr<detail::isl_inner_base> &)>;
    static func_t s_func;
};

// This is the default UDI type selector. It will always select thread_island as UDI.
inline void default_island_factory(const algorithm &, const population &, std::unique_ptr<detail::isl_inner_base> &ptr)
{
    ptr = make_unique<isl_inner<thread_island>>();
}

// Static init.
template <typename T>
typename island_factory<T>::func_t island_factory<T>::s_func = default_island_factory;

// NOTE: the idea with this class is that we use it to store the data members of pagmo::island, and,
// within pagmo::island, we store a pointer to an instance of this struct. The reason for this approach
// is that, like this, we can provide sensible move semantics: just move the internal pointer of pagmo::island,
// the operation will be noexcept and there will be no need to synchronise anything - the evolution can continue
// while the move operation is taking place.
struct island_data {
    // NOTE: thread_island is ok as default choice, as the null_prob/null_algo
    // are both thread safe.
    island_data() : isl_ptr(make_unique<isl_inner<thread_island>>())
    {
    }

    // This is the main ctor, from an algo and a population. The UDI type will be selected
    // by the island_factory functor.
    template <typename Algo, typename Pop>
    explicit island_data(Algo &&a, Pop &&p) : algo(std::forward<Algo>(a)), pop(std::forward<Pop>(p))
    {
        island_factory<>::s_func(algo, pop, isl_ptr);
    }

    // As above, but the UDI is explicitly passed by the user.
    template <typename Isl, typename Algo, typename Pop>
    explicit island_data(Isl &&isl, Algo &&a, Pop &&p)
        : isl_ptr(make_unique<isl_inner<uncvref_t<Isl>>>(std::forward<Isl>(isl))), algo(std::forward<Algo>(a)),
          pop(std::forward<Pop>(p))
    {
    }

    // This is used only in the copy ctor of island. It's equivalent to the ctor from Algo + pop,
    // the island will come from the clone() method of an isl_inner.
    template <typename Algo, typename Pop>
    explicit island_data(std::unique_ptr<isl_inner_base> &&ptr, Algo &&a, Pop &&p)
        : isl_ptr(std::move(ptr)), algo(std::forward<Algo>(a)), pop(std::forward<Pop>(p))
    {
    }

    // Delete all the rest, make sure we don't implicitly rely on any of this.
    island_data(const island_data &) = delete;
    island_data(island_data &&) = delete;
    island_data &operator=(const island_data &) = delete;
    island_data &operator=(island_data &&) = delete;

    // The data members.
    // NOTE: isl_ptr has no associated mutex, as it's supposed to be fully
    // threads-safe on its own.
    std::unique_ptr<isl_inner_base> isl_ptr;
    // Algo, pop and futures all need a mutex to regulate access.
    std::mutex algo_mutex;
    algorithm algo;
    std::mutex pop_mutex;
    population pop;
    std::mutex futures_mutex;
    std::vector<std::future<void>> futures;
    archipelago *archi_ptr = nullptr;
    // task_queue is thread-safe on its own.
    task_queue queue;
};
}

/// Island class.
/**
 * \image html island.jpg
 *
 * In the pagmo jargon, an island is a class that encapsulates three entities:
 * - a user-defined island (UDI),
 * - a pagmo::algorithm,
 * - a pagmo::population.
 *
 * Through the UDI, the island class manages the asynchronous evolution (or optimisation)
 * of the pagmo::population via the algorithm's algorithm::evolve() method. Depending
 * on the UDI, the evolution might take place in a separate thread (e.g., if the UDI is a
 * pagmo::thread_island), in a separate process or even in a separate machine. The evolution
 * is always asynchronous (i.e., running in the "background") and it is initiated by a call
 * to the island::evolve() method. At any time the user can query the state of the island
 * and fetch its internal data members. The user can explicitly wait for pending evolutions
 * to conclude by calling the island::wait() method.
 *
 * Typically, pagmo users will employ an already-available UDI (such as pagmo::thread_island) in
 * conjunction with this class, but advanced users can implement their own UDI types. A user-defined
 * island must implement the following method:
 * @code{.unparsed}
 * void run_evolve(pagmo::algorithm &, ulock_t &, pagmo::population &, ulock_t &);
 * @endcode
 * where \p ulock_t is <tt>std::unique_lock<std::mutex></tt>.
 *
 * The <tt>run_evolve()</tt> method of
 * the UDI will use the input algorithm's algorithm::evolve() method to evolve the input population
 * and, once the evolution is finished, will replace the input population with the evolved population.
 * The two extra arguments are locked locks that guarantee exclusive access to the input algorithm and population
 * respectively. Typically, a UDI's <tt>run_evolve()</tt> method will first copy the input algorithm and population,
 * release the locks, evolve the population, re-acquire the population's lock and finally assign the evolved population.
 * In addition to providing the above method, a UDI must also be default, copy and move constructible. Also, since
 * internally the pagmo::island class uses a separate thread of execution to provide asynchronous behaviour, a UDI needs
 * to guarantee strong thread-safety: it must be safe to interact with UDI instances simultaneously from multiple
 * threads, or the behaviour will be undefined.
 *
 * In addition to the mandatory <tt>run_evolve()</tt> method, a UDI might implement the following optional methods:
 * @code{.unparsed}
 * std::string get_name() const;
 * std::string get_extra_info() const;
 * @endcode
 *
 * See the documentation of the corresponding methods in this class for details on how the optional
 * methods in the UDI are used by pagmo::island.
 *
 * **NOTE**: a moved-from pagmo::island is destructible and assignable. Any other operation will result
 * in undefined behaviour.
 */
class island
{
    using idata_t = detail::island_data;

public:
    /// Default constructor.
    /**
     * The default constructor will initialise an island containing a UDI of type pagmo::thread_island,
     * and default-constructed pagmo::algorithm and pagmo::population.
     *
     * @throws unspecified any exception thrown by any invoked constructor or by memory allocation failures.
     */
    island() : m_ptr(detail::make_unique<idata_t>())
    {
    }
    /// Copy constructor.
    /**
     * The copy constructor will initialise an island containing a copy of <tt>other</tt>'s UDI, population
     * and algorithm. It is safe to call this constructor while \p other is evolving.
     *
     * @param other the island tht will be copied.
     *
     * @throws unspecified any exception thrown by:
     * - threading primitives,
     * - memory allocation errors,
     * - the copy constructors of pagmo::algorithm and pagmo::population.
     */
    island(const island &other)
        : m_ptr(detail::make_unique<idata_t>(other.m_ptr->isl_ptr->clone(), other.get_algorithm(),
                                             other.get_population()))
    {
        // NOTE: the idata_t ctor will set the archi ptr to null. The archi ptr is never copied.
    }
    /// Move constructor.
    /**
     * The move constructor will transfer the state of \p other (including any ongoing evolution)
     * into \p this.
     *
     * @param other the island tht will be moved.
     */
    island(island &&other) noexcept : m_ptr(std::move(other.m_ptr))
    {
    }

private:
    template <typename Algo, typename Pop>
    using algo_pop_enabler = enable_if_t<std::is_constructible<algorithm, Algo &&>::value
                                             && std::is_same<population, uncvref_t<Pop>>::value,
                                         int>;

public:
    /// Constructor from algorithm and population.
    /**
     * **NOTE**: this constructor is enabled only if \p a can be used to construct a
     * pagmo::algorithm and \p p is an instance of pagmo::population.
     *
     * This constructor will use \p a to construct the internal algorithm, and \p p to construct
     * the internal population. A default-constructed pagmo::thread_island will be the internal UDI.
     *
     * @param a the input algorithm.
     * @param p the input population.
     *
     * @throws unspecified any exception thrown by:
     * - memory allocation errors,
     * - the constructors of pagmo::algorithm and pagmo::population.
     */
    template <typename Algo, typename Pop, algo_pop_enabler<Algo, Pop> = 0>
    explicit island(Algo &&a, Pop &&p)
        : m_ptr(detail::make_unique<idata_t>(std::forward<Algo>(a), std::forward<Pop>(p)))
    {
    }

private:
    template <typename Isl, typename Algo, typename Pop>
    using isl_algo_pop_enabler
        = enable_if_t<is_udi<uncvref_t<Isl>>::value && std::is_constructible<algorithm, Algo &&>::value
                          && std::is_same<population, uncvref_t<Pop>>::value,
                      int>;

public:
    /// Constructor from UDI, algorithm and population.
    /**
     * **NOTE**: this constructor is enabled only if:
     * - \p Isl satisfies pagmo::is_udi,
     * - \p a can be used to construct a pagmo::algorithm,
     * - \p p is an instance of pagmo::population.
     *
     * This constructor will use \p isl to construct the internal UDI, \p a to construct the internal algorithm,
     * and \p p to construct the internal population.
     *
     * @param isl the input UDI.
     * @param a the input algorithm.
     * @param p the input population.
     *
     * @throws unspecified any exception thrown by:
     * - memory allocation errors,
     * - the constructors of \p Isl, pagmo::algorithm and pagmo::population.
     */
    template <typename Isl, typename Algo, typename Pop, isl_algo_pop_enabler<Isl, Algo, Pop> = 0>
    explicit island(Isl &&isl, Algo &&a, Pop &&p)
        : m_ptr(detail::make_unique<idata_t>(std::forward<Isl>(isl), std::forward<Algo>(a), std::forward<Pop>(p)))
    {
    }

private:
    template <typename Algo, typename Prob>
    using algo_prob_enabler
        = enable_if_t<std::is_constructible<algorithm, Algo &&>::value
                          && std::is_constructible<population, Prob &&, population::size_type, unsigned>::value,
                      int>;

public:
    /// Constructor from algorithm, problem, size and seed.
    /**
     * **NOTE**: this constructor is enabled only if \p a can be used to construct a
     * pagmo::algorithm, and \p p, \p size and \p seed can be used to construct a pagmo::population.
     *
     * This constructor will construct a pagmo::population \p pop from \p p, \p size and \p seed, and it will
     * then invoke island(Algo &&, Pop &&) with \p a and \p pop as arguments.
     *
     * @param a the input algorithm.
     * @param p the input problem.
     * @param size the population size.
     * @param seed the population seed.
     *
     * @throws unspecified any exception thrown by the invoked pagmo::population constructor or by
     * island(Algo &&, Pop &&).
     */
    template <typename Algo, typename Prob, algo_prob_enabler<Algo, Prob> = 0>
    explicit island(Algo &&a, Prob &&p, population::size_type size, unsigned seed = pagmo::random_device::next())
        : island(std::forward<Algo>(a), population(std::forward<Prob>(p), size, seed))
    {
    }

private:
    template <typename Isl, typename Algo, typename Prob>
    using isl_algo_prob_enabler
        = enable_if_t<is_udi<uncvref_t<Isl>>::value && std::is_constructible<algorithm, Algo &&>::value
                          && std::is_constructible<population, Prob &&, population::size_type, unsigned>::value,
                      int>;

public:
    /// Constructor from UDI, algorithm, problem, size and seed.
    /**
     * **NOTE**: this constructor is enabled only if \p Isl satisfies pagmo::is_udi, \p a can be used to construct a
     * pagmo::algorithm, and \p p, \p size and \p seed can be used to construct a pagmo::population.
     *
     * This constructor will construct a pagmo::population \p pop from \p p, \p size and \p seed, and it will
     * then invoke island(Isl &&, Algo &&, Pop &&) with \p isl, \p a and \p pop as arguments.
     *
     * @param isl the input UDI.
     * @param a the input algorithm.
     * @param p the input problem.
     * @param size the population size.
     * @param seed the population seed.
     *
     * @throws unspecified any exception thrown by:
     * - the invoked pagmo::population constructor,
     * - island(Isl &&, Algo &&, Pop &&),
     * - the invoked constructor of \p Isl.
     */
    template <typename Isl, typename Algo, typename Prob, isl_algo_prob_enabler<Isl, Algo, Prob> = 0>
    explicit island(Isl &&isl, Algo &&a, Prob &&p, population::size_type size,
                    unsigned seed = pagmo::random_device::next())
        : island(std::forward<Isl>(isl), std::forward<Algo>(a), population(std::forward<Prob>(p), size, seed))
    {
    }
    /// Destructor.
    /**
     * If the island has not been moved-from, the destructor will call island::wait(), ignoring
     * any exception that might be thrown apart from \p std::system_error. If an \p std::system_error
     * exception is thrown, the program will terminate.
     */
    ~island()
    {
        // If the island has been moved from, don't do anything.
        if (!m_ptr) {
            return;
        }
        try {
            wait();
        } catch (const std::system_error &) {
            // NOTE: the rationale here is that we interpret system_error as
            // a failure in the locking primitives inside wait(), and if that
            // fails we will have loose threads hanging around. Best just to
            // rethrow, which will cause program termination as this dtor is noexcept.
            throw;
        } catch (...) {
        }
    }
    /// Move assignment.
    /**
     * Move assignment will transfer the state of \p other (including any ongoing evolution)
     * into \p this.
     *
     * @param other the island tht will be moved.
     *
     * @return a reference to \p this.
     */
    island &operator=(island &&other) noexcept
    {
        if (this != &other) {
            m_ptr = std::move(other.m_ptr);
        }
        return *this;
    }
    /// Copy assignment.
    /**
     * Copy assignment is implemented as copy construction followed by move assignment.
     *
     * @param other the island tht will be copied.
     *
     * @return a reference to \p this.
     *
     * @throws unspecified any exception thrown by the copy constructor.
     */
    island &operator=(const island &other)
    {
        if (this != &other) {
            *this = island(other);
        }
        return *this;
    }
    /// Launch evolution.
    /**
     * This method will evolve the island's pagmo::population using the
     * island's pagmo::algorithm. The evolution happens asynchronously:
     * a call to island::evolve() will create an evolution task that will be pushed
     * to a queue, and then return immediately.
     * The tasks in the queue are consumed
     * by a separate thread of execution managed by the pagmo::island object,
     * which will invoke the <tt>run_evolve()</tt>
     * method of the UDI to perform the actual evolution. The island's population will be updated
     * at the end of each evolution task. Exceptions raised insided the tasks are stored within
     * the island object, and can be re-raised by calling wait().
     *
     * It is possible to call this method multiple times to enqueue multiple evolution tasks, which
     * will be consumed in a FIFO (first-in first-out) fashion. The user may call island::wait() to block until
     * all tasks have been completed, and to fetch exceptions raised during the execution of the tasks.
     *
     * @throws unspecified any exception thrown by:
     * - threading primitives,
     * - memory allocation errors,
     * - the public interface of \p std::future.
     */
    void evolve()
    {
        std::lock_guard<std::mutex> lock(m_ptr->futures_mutex);
        // First add an empty future, so that if an exception is thrown
        // we will not have modified m_futures, nor we will have a future
        // in flight which we cannot wait upon.
        m_ptr->futures.emplace_back();
        try {
            // Get a pointer to the members tuple. Capturing ptr in the lambda
            // rather than this ensures tasks can still be executed after a move
            // operation on this.
            auto ptr = m_ptr.get();
            // Move assign a new future provided by the enqueue() method.
            // NOTE: enqueue either returns a valid future, or throws without
            // having enqueued any task.
            m_ptr->futures.back() = m_ptr->queue.enqueue([ptr]() {
                {
                    // Lock down access to algo and pop.
                    std::unique_lock<std::mutex> algo_lock(ptr->algo_mutex);
                    std::unique_lock<std::mutex> pop_lock(ptr->pop_mutex);
                    ptr->isl_ptr->run_evolve(ptr->algo, algo_lock, ptr->pop, pop_lock);
                }
                auto archi = ptr->archi_ptr;
                (void)archi;
            });
        } catch (...) {
            // We end up here only if enqueue threw. In such a case, we need to cleanup
            // the empty future we added above before re-throwing and exiting.
            m_ptr->futures.pop_back();
            throw;
        }
    }
    /// Block until evolution ends.
    /**
     * This method will block until all the evolution tasks enqueued via island::evolve() have been completed.
     * The method will also raise the first exception raised by any task enqueued since the last time wait() was called.
     *
     * @throws unspecified any exception thrown by:
     * - evolution tasks,
     * - threading primitives.
     */
    void wait() const
    {
        auto iwr = detail::wait_raii<>::getter();
        (void)iwr;
        std::lock_guard<std::mutex> lock(m_ptr->futures_mutex);
        for (decltype(m_ptr->futures.size()) i = 0; i < m_ptr->futures.size(); ++i) {
            // NOTE: this has to be valid, as the only way to get the value of the futures is via
            // this method, and we clear the futures vector after we are done.
            assert(m_ptr->futures[i].valid());
            try {
                m_ptr->futures[i].get();
            } catch (...) {
                // If any of the futures stores an exception, we will re-raise it.
                // But first, we need to get all the other futures and erase the futures
                // vector.
                for (i = i + 1u; i < m_ptr->futures.size(); ++i) {
                    try {
                        m_ptr->futures[i].get();
                    } catch (...) {
                    }
                }
                m_ptr->futures.clear();
                throw;
            }
        }
        m_ptr->futures.clear();
    }
    /// Check island status.
    /**
     * @return \p true if the island is evolving, \p false otherwise.
     *
     * @throws unspecified any exception thrown by threading primitives.
     */
    bool busy() const
    {
        std::lock_guard<std::mutex> lock(m_ptr->futures_mutex);
        for (const auto &f : m_ptr->futures) {
            assert(f.valid());
            if (f.wait_for(std::chrono::duration<int>::zero()) != std::future_status::ready) {
                return true;
            }
        }
        return false;
    }
    /// Get the algorithm.
    /**
     * It is safe to call this method while the island is evolving.
     *
     * @return a copy of the island's algorithm.
     *
     * @throws unspecified any exception thrown by threading primitives.
     */
    algorithm get_algorithm() const
    {
        std::lock_guard<std::mutex> lock(m_ptr->algo_mutex);
        return m_ptr->algo;
    }
    /// Get the population.
    /**
     * It is safe to call this method while the island is evolving.
     *
     * @return a copy of the island's population.
     *
     * @throws unspecified any exception thrown by threading primitives.
     */
    population get_population() const
    {
        std::lock_guard<std::mutex> lock(m_ptr->pop_mutex);
        return m_ptr->pop;
    }
    /// Island's name.
    /**
     * If the UDI satisfies pagmo::has_name, then this method will return the output of its <tt>%get_name()</tt> method.
     * Otherwise, an implementation-defined name based on the type of the UDI will be returned.
     *
     * @return the name of the UDI.
     *
     * @throws unspecified any exception thrown by the <tt>%get_name()</tt> method of the UDI.
     */
    std::string get_name() const
    {
        return m_ptr->isl_ptr->get_name();
    }
    /// Island's extra info.
    /**
     * If the UDI satisfies pagmo::has_extra_info, then this method will return the output of its
     * <tt>%get_extra_info()</tt> method. Otherwise, an empty string will be returned.
     *
     * @return extra info about the UDI.
     *
     * @throws unspecified any exception thrown by the <tt>%get_extra_info()</tt> method of the UDI.
     */
    std::string get_extra_info() const
    {
        return m_ptr->isl_ptr->get_extra_info();
    }
    /// Stream operator for pagmo::island.
    /**
     * This operator will stream to \p os a human-readable representation of \p isl.
     * It is safe to call this operator while the island is evolving.
     *
     * @param os the target stream.
     * @param isl the island.
     *
     * @return a reference to \p os.
     *
     * @throws unspecified any exception thrown by the stream operators of fundamental types,
     * pagmo::algorithm and pagmo::population, or by pagmo::island::get_extra_info().
     */
    friend std::ostream &operator<<(std::ostream &os, const island &isl)
    {
        stream(os, "Island name: ", isl.get_name(), "\n\n");
        stream(os, isl.get_algorithm(), "\n\n");
        stream(os, isl.get_population(), "\n\n");
        const auto extra_str = isl.get_extra_info();
        if (!extra_str.empty()) {
            stream(os, "\nExtra info:\n", extra_str);
        }
        return os;
    }
    /// Save to archive.
    /**
     * This method will save \p this to the archive \p ar.
     * It is safe to call this method while the island is evolving.
     *
     * @param ar the target archive.
     *
     * @throws unspecified any exception thrown by:
     * - the serialization of pagmo::algorithm, pagmo::population and of the UDI type,
     * - get_algorithm() and get_population().
     */
    template <typename Archive>
    void save(Archive &ar) const
    {
        ar(m_ptr->isl_ptr, get_algorithm(), get_population());
    }
    /// Load from archive.
    /**
     * This method will load into \p this the content of \p ar.
     * It is safe to call this method while the island is evolving,
     * but this method will wait until all evolution tasks are completed
     * before returning.
     *
     * @param ar the source archive.
     *
     * @throws unspecified any exception thrown by the deserialization of pagmo::algorithm, pagmo::population and of the
     * UDI type.
     */
    template <typename Archive>
    void load(Archive &ar)
    {
        // Deserialize into tmp island, and then move assign it.
        island tmp_island;
        // NOTE: no need to lock access to these, as there is no evolution going on in tmp_island.
        ar(tmp_island.m_ptr->isl_ptr);
        ar(tmp_island.m_ptr->algo);
        ar(tmp_island.m_ptr->pop);
        *this = std::move(tmp_island);
    }

private:
    mutable std::unique_ptr<idata_t> m_ptr;
};
}

PAGMO_REGISTER_ISLAND(pagmo::thread_island)

#endif
