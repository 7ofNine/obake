// Copyright 2019 Francesco Biscani (bluescarni@gmail.com)
//
// This file is part of the piranha library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef PIRANHA_SERIES_HPP
#define PIRANHA_SERIES_HPP

#include <x86intrin.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include <boost/container/small_vector.hpp>
#include <boost/iterator/iterator_categories.hpp>
#include <boost/iterator/iterator_facade.hpp>

#include <piranha/config.hpp>
#include <piranha/hash.hpp>
#include <piranha/math/is_zero.hpp>
#include <piranha/math/pow.hpp>
#include <piranha/symbols.hpp>
#include <piranha/type_traits.hpp>

namespace piranha
{

template <typename T>
using is_key = ::std::conjunction<is_semi_regular<T>, is_hashable<T>>;

template <typename T>
inline constexpr bool is_key_v = is_key<T>::value;

#if defined(PIRANHA_HAVE_CONCEPTS)

template <typename T>
PIRANHA_CONCEPT_DECL Key = is_key_v<T>;

#endif

template <typename T>
using is_cf = ::std::conjunction<is_semi_regular<T>, is_zero_testable<T>>;

template <typename T>
inline constexpr bool is_cf_v = is_cf<T>::value;

#if defined(PIRANHA_HAVE_CONCEPTS)

template <typename T>
PIRANHA_CONCEPT_DECL Cf = is_cf_v<T>;

#endif

namespace detail
{

// A small hashing wrapper for keys. It accomplishes the task
// of forcing the evaluation of a key through const reference,
// so that, in the Key requirements, we can request hashability
// through const lvalue ref.
struct key_hasher {
    template <typename K>
    constexpr ::std::size_t operator()(const K &k) const noexcept(noexcept(::piranha::hash(k)))
    {
        return ::piranha::hash(k);
    }
};

} // namespace detail

#if defined(PIRANHA_HAVE_CONCEPTS)
template <Cf C, Key K, typename Tag>
#else
template <typename C, typename K, typename Tag,
          typename = ::std::enable_if_t<::std::conjunction_v<is_cf<C>, is_key<K>>>>
#endif
class series
{
public:
    using cf_type = C;
    using key_type = K;
    using hash_map_type = ::absl::flat_hash_map<K, C, detail::key_hasher>;
    using container_type = ::boost::container::small_vector<hash_map_type, 1>;

    series() : m_container(1), m_log2_size(0) {}
    series(const series &) = default;
    series(series &&) = default;
    series &operator=(const series &) = default;
    series &operator=(series &&) = default;
    ~series()
    {
        assert(!m_container.empty() && m_container.size() == (c_size_t(1) << m_log2_size));
    }

    // Member function implementation of the swap primitive.
    void swap(series &other) noexcept
    {
        using ::std::swap;
        swap(m_container, other.m_container);
        swap(m_log2_size, other.m_log2_size);
        swap(m_symbol_set, other.m_symbol_set);
    }

    bool empty() const noexcept
    {
        return ::std::all_of(m_container.begin(), m_container.end(), [](const auto &map) { return map.empty(); });
    }

private:
    // Shortcut for the container size type.
    using c_size_t = typename container_type::size_type;

    // A small helper to select the (const) iterator of container_type, depending on whether
    // T is const or not. Used in the iterator implementation below.
    template <typename T>
    using local_it_t = ::std::conditional_t<::std::is_const_v<T>, typename container_type::value_type::const_iterator,
                                            typename container_type::value_type::iterator>;

    // NOTE: this is mostly taken from:
    // https://www.boost.org/doc/libs/1_70_0/libs/iterator/doc/iterator_facade.html
    template <typename T>
    class iterator_impl
        : public ::boost::iterator_facade<iterator_impl<T>, T, ::boost::forward_traversal_tag, T &,
                                          // Fetch the difference type from the underlying local iterator.
                                          typename ::std::iterator_traits<local_it_t<T>>::difference_type>
    {
        // Make friends with other specialisations for interoperability
        // between const/non-const variants.
        template <typename U>
        friend class iterator_impl;
        // Make friends with boost's iterator machinery.
        friend class ::boost::iterator_core_access;
        // Make friends also with the series class, as sometimes
        // we need to poke the internals of the iterator from there.
        friend class series;

        using container_ptr_t = ::std::conditional_t<::std::is_const_v<T>, const container_type *, container_type *>;

    public:
        // Defaul constructor.
        // NOTE: C++14 requires that all value-inited forward iterators
        // compare equal. This is guaranteed by this constructor, since
        // local_it_t is also a forward iterator which is default-inited.
        // https://en.cppreference.com/w/cpp/named_req/ForwardIterator
        iterator_impl() : m_container_ptr(nullptr), m_idx(0), m_local_it{} {}
        // Init with a pointer to the container and an index.
        // Used in the begin()/end() implementations.
        // NOTE: ensure m_local_it is default-inited, so it is in
        // a known state. This is also exploited in the implementation
        // of begin()/end().
        explicit iterator_impl(container_ptr_t container_ptr, c_size_t idx)
            : m_container_ptr(container_ptr), m_idx(idx), m_local_it{}
        {
        }
        // Default the copy/move ctors.
        iterator_impl(const iterator_impl &) = default;
        iterator_impl(iterator_impl &&) = default;
        // Implicit converting ctor from another specialisation. This is
        // used to construct a const iterator from a mutable one.
        template <typename U,
                  ::std::enable_if_t<
                      ::std::conjunction_v<
                          ::std::is_constructible<container_ptr_t, const typename iterator_impl<U>::container_ptr_t &>,
                          ::std::is_constructible<local_it_t<T>, const local_it_t<U> &>>,
                      int> = 0>
        iterator_impl(const iterator_impl<U> &other)
            : m_container_ptr(other.m_container_ptr), m_idx(other.m_idx), m_local_it(other.m_local_it)
        {
        }

        // Specialise the swap primitive.
        friend void swap(iterator_impl &it1, iterator_impl &it2) noexcept
        {
            using ::std::swap;
            swap(it1.m_container_ptr, it2.m_container_ptr);
            swap(it1.m_idx, it2.m_idx);
            // NOTE: all iterators are required to be swappable,
            // so this must always be supported by the local iterator
            // type. If it throws (unlikely), the program will
            // terminate as we are marking this function as noexcept.
            swap(it1.m_local_it, it2.m_local_it);
        }

    private:
        void increment()
        {
            // Must be pointing to something.
            assert(m_container_ptr != nullptr);
            // Cannot be already at the end of the container.
            assert(m_idx < m_container_ptr->size());
            // The current table cannot be empty.
            assert(!m_container_ptr[m_idx].empty());
            // The local iterator cannot be pointing
            // at the end of the current table.
            assert(m_local_it != (*m_container_ptr)[m_idx].end());
            // Move to the next item in the current table.
            const auto &c = *m_container_ptr;
            if (++m_local_it == c[m_idx].end()) {
                // We reached the end of the current table.
                // Keep bumping m_idx until we either
                // arrive in a non-empty table, or we reach
                // the end of the container.
                const auto c_size = c.size();
                while (true) {
                    ++m_idx;
                    if (m_idx == c_size) {
                        // End of the container, reset m_local_it to
                        // value-inited and exit.
                        // NOTE: this is important, because this is now an end()
                        // iterator and end() iterators contain a value-inited
                        // local iterator.
                        m_local_it = local_it_t<T>{};
                        break;
                    } else if (!c[m_idx].empty()) {
                        // The next non-empty table was found.
                        // Set m_local_it to its beginning and exit.
                        m_local_it = c[m_idx].begin();
                        break;
                    }
                }
            }
        }
        // NOTE: templated in order to enable comparisons
        // between the const and mutable variants.
        template <typename U,
                  ::std::enable_if_t<is_equality_comparable_v<const local_it_t<T> &, const local_it_t<U> &>, int> = 0>
        bool equal(const iterator_impl<U> &other) const
        {
            // Can compare only iterators referring to the same container.
            assert(m_container_ptr != nullptr);
            assert(m_container_ptr == other.m_container_ptr);
            // NOTE: this is fine when comparing end() with itself,
            // as m_local_it as a unique representation for end
            // iterators.
            return (m_idx == other.m_idx && m_local_it == other.m_local_it);
        }
        T &dereference() const
        {
            // Must point to something.
            assert(m_container_ptr);
            // Must not be the end iterator.
            assert(m_idx < m_container_ptr->size());
            // The current map must not be empty.
            assert(!(*m_container_ptr)[m_idx].empty());
            // The local iterator must not point to the
            // end of the current map.
            assert(m_local_it != (*m_container_ptr)[m_idx].end());
            return *m_local_it;
        }

    private:
        container_ptr_t m_container_ptr;
        c_size_t m_idx;
        local_it_t<T> m_local_it;
    };

public:
    using iterator = iterator_impl<typename hash_map_type::value_type>;
    using const_iterator = iterator_impl<const typename hash_map_type::value_type>;

private:
    template <typename It, typename Container>
    static It begin_impl(Container &c)
    {
        // NOTE: this could take a while in case of an empty set with lots of buckets.
        It retval(&c, 0);
        // Look for a non-empty map.
        for (auto &map : c) {
            if (!map.empty()) {
                retval.m_local_it = map.begin();
                break;
            }
            ++retval.m_idx;
        }
        // NOTE: if all the maps are empty, or the container
        // is empty, m_idx is now set to the size of m_container
        // and the local iterator stays in its value-inited
        // state. That is, retval becomes the end iterator.
        return retval;
    }

public:
    const_iterator begin() const noexcept
    {
        return series::begin_impl<const_iterator>(m_container);
    }
    const_iterator end() const noexcept
    {
        // NOTE: end iterators contain a value-inited
        // local iterator.
        return const_iterator(&m_container, m_container.size());
    }
    const_iterator cbegin() const noexcept
    {
        return begin();
    }
    const_iterator cend() const noexcept
    {
        return end();
    }
    iterator begin() noexcept
    {
        return series::begin_impl<iterator>(m_container);
    }
    iterator end() noexcept
    {
        return iterator(&m_container, m_container.size());
    }

#if defined(PIRANHA_HAVE_CONCEPTS)
    // TODO flip around T and U.
    // TODO SameCvref -> SameCvr.
    template <SameCvref<key_type> T, SameCvref<cf_type> U>
#else
    template <typename T, typename U,
              ::std::enable_if_t<::std::conjunction_v<is_same_cvref<T, key_type>, is_same_cvref<U, cf_type>>, int> = 0>
#endif
    void add_term(T &&key, U &&cf)
    {
        // Check early if the coefficient is zero. We want
        // to optimise for the case in which we are inserting
        // nonzero elements.
        // TODO: key zero check.
        // TODO: key compat check.
        if (piranha_unlikely(::piranha::is_zero(static_cast<const cf_type &>(::std::forward<U>(cf))))) {
            return;
        }
        if (m_log2_size == 0u) {
            auto &hm = m_container[0];
            const auto res = hm.try_emplace(::std::forward<T>(key), ::std::forward<U>(cf));
            if (!res.second) {
                // Insertion did not take place because a term with
                // the same key exists already. Add the input coefficient
                // to the existing one.
                res.first->second += ::std::forward<U>(cf);
                // Now check that the updated coefficient is not zero.
                // TODO.
            }
        } else {
        }
    }

private:
    container_type m_container;
    c_size_t m_log2_size;
    symbol_set m_symbol_set;
};

// Free function implementation of the swapping primitive.
template <typename C, typename K, typename Tag>
inline void swap(series<C, K, Tag> &s1, series<C, K, Tag> &s2) noexcept
{
    s1.swap(s2);
}

namespace detail
{

template <typename T>
struct is_series_impl : ::std::false_type {
};

template <typename C, typename K, typename Tag>
struct is_series_impl<series<C, K, Tag>> : ::std::true_type {
};

} // namespace detail

template <typename T>
using is_cvr_series = detail::is_series_impl<remove_cvref_t<T>>;

template <typename T>
inline constexpr bool is_cvr_series_v = is_cvr_series<T>::value;

#if defined(PIRANHA_HAVE_CONCEPTS)

template <typename T>
PIRANHA_CONCEPT_DECL CvrSeries = is_cvr_series_v<T>;

#endif

namespace detail
{

template <typename>
struct series_cf_t_impl {
};

template <typename C, typename K, typename Tag>
struct series_cf_t_impl<series<C, K, Tag>> {
    using type = C;
};

} // namespace detail

template <typename T>
using series_cf_t = typename detail::series_cf_t_impl<::std::remove_cv_t<T>>::type;

namespace customisation::internal
{

template <typename T, typename U>
#if defined(PIRANHA_HAVE_CONCEPTS)
requires CvrSeries<T> &&Integral<::std::remove_reference_t<U>> inline constexpr auto pow<T, U>
#else
inline constexpr auto
    pow<T, U, ::std::enable_if_t<::std::conjunction_v<is_cvr_series<T>, is_integral<::std::remove_reference_t<U>>>>>
#endif
    = [](auto &&, auto &&) constexpr
{
    return 0;
};

} // namespace customisation::internal

} // namespace piranha

#endif
