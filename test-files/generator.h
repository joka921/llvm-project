///////////////////////////////////////////////////////////////////////////////
// Copyright (c) Lewis Baker, Johannes Kalmbach (functionality to add details).
// Licenced under MIT license. See LICENSE.txt for details.
///////////////////////////////////////////////////////////////////////////////
#ifndef CPPCORO_GENERATOR_HPP_INCLUDED
#define CPPCORO_GENERATOR_HPP_INCLUDED

#include <coroutine>
#include <exception>
#include <functional>
#include <iterator>
#include <type_traits>
#include <utility>
#include <limits>

namespace cppcoro {
    struct SuspendAlways {
        constexpr bool await_ready() const noexcept { return false; }

        template<typename Handle>
        constexpr void await_suspend([[maybe_unused]] Handle handle) const noexcept {
        }

        constexpr void await_resume() const noexcept {
        }
    };

    // This struct can be `co_await`ed inside a `generator` to obtain a reference to
    // the details object (the value of which is a template parameter to the
    // generator). For an example see `GeneratorTest.cpp`.
    struct GetDetails {
    };

    static constexpr GetDetails getDetails;

    // This struct is used as the default of the details object for the case that
    // there are no details
    struct NoDetails {
    };

    template<typename T, typename Details = NoDetails,
        template <typename...> typename GeneratorHandle =
        std::coroutine_handle>
    class generator;

    namespace detail {
        template<typename T, typename Details = NoDetails,
            template <typename...> typename GeneratorHandle =
            std::coroutine_handle>
        class generator_promise {
        public:
            // Even if the generator only yields `const` values, the `value_type`
            // shouldn't be `const` because otherwise several static checks when
            // interacting with the STL fail.
            using value_type = std::remove_cvref_t<T>;
            using reference_type = std::conditional_t<std::is_reference_v<T>, T, T &>;
            using pointer_type = std::remove_reference_t<T> *;

            generator_promise() = default;

            generator<T, Details, GeneratorHandle> get_return_object() noexcept;

            constexpr cppcoro::SuspendAlways initial_suspend() const noexcept {
                return {};
            }

            constexpr cppcoro::SuspendAlways final_suspend() const noexcept { return {}; }

            template<typename U = T,
                std::enable_if_t<!std::is_rvalue_reference<U>::value, int> = 0>
            cppcoro::SuspendAlways yield_value(
                std::remove_reference_t<T> &value) noexcept {
                m_value = std::addressof(value);
                return {};
            }

            cppcoro::SuspendAlways yield_value(
                std::remove_reference_t<T> &&value) noexcept {
                m_value = std::addressof(value);
                return {};
            }

            void unhandled_exception() { m_exception = std::current_exception(); }

            void return_void() {
            }

            reference_type value() const noexcept {
                return static_cast<reference_type>(*m_value);
            }

            // Don't allow any use of 'co_await' inside the generator coroutine.
            template<typename U>
            void await_transform(U &&value) = delete;

            void rethrow_if_exception() const {
                if (m_exception) {
                    std::rethrow_exception(m_exception);
                }
            }

        private:
            pointer_type m_value;
            std::exception_ptr m_exception;
        };

        struct generator_sentinel {
        };

        template<typename T, typename Details,
            template <typename...> typename GeneratorHandle,
            bool ConstDummy = false>
        class generator_iterator {
            using promise_type = generator_promise<T, Details, GeneratorHandle>;
            using coroutine_handle = GeneratorHandle<promise_type>;

        public:
            using iterator_category = std::input_iterator_tag;
            // What type should we use for counting elements of a potentially infinite
            // sequence?
            using difference_type = std::ptrdiff_t;
            using value_type = typename promise_type::value_type;
            using reference = typename promise_type::reference_type;
            using pointer = typename promise_type::pointer_type;

            // Iterator needs to be default-constructible to satisfy the Range concept.
            generator_iterator() noexcept : m_coroutine(nullptr) {
            }

            explicit generator_iterator(coroutine_handle coroutine) noexcept
                : m_coroutine(coroutine) {
            }

            friend bool operator==(const generator_iterator &it,
                                   generator_sentinel) noexcept {
                return !it.m_coroutine || it.m_coroutine.done();
            }

            friend bool operator!=(const generator_iterator &it,
                                   generator_sentinel s) noexcept {
                return !(it == s);
            }

            friend bool operator==(generator_sentinel s,
                                   const generator_iterator &it) noexcept {
                return (it == s);
            }

            friend bool operator!=(generator_sentinel s,
                                   const generator_iterator &it) noexcept {
                return it != s;
            }

            generator_iterator &operator++() {
                m_coroutine.resume();
                if (m_coroutine.done()) {
                    m_coroutine.promise().rethrow_if_exception();
                }

                return *this;
            }

            // Need to provide post-increment operator to implement the 'Range' concept.
            void operator++(int) { (void) operator++(); }

            reference operator*() const noexcept { return m_coroutine.promise().value(); }

            pointer operator->() const noexcept { return std::addressof(operator*()); }

        private:
            coroutine_handle m_coroutine;
        };
    } // namespace detail

    template<typename T, typename Details,
        template <typename...> typename GeneratorHandle>
    class [[nodiscard]] generator {
    public:
        using promise_type = detail::generator_promise<T, Details, GeneratorHandle>;
        using iterator =
        detail::generator_iterator<T, Details, GeneratorHandle, false>;
        // TODO<joka921> Check if this fixes anything wrt ::ranges
        // using const_iterator = detail::generator_iterator<T, Details, true>;
        using value_type = typename iterator::value_type;

        generator() noexcept : m_coroutine(nullptr) {
        }

        generator(generator &&other) noexcept : m_coroutine(other.m_coroutine) {
            other.m_coroutine = nullptr;
        }

        generator(const generator &other) = delete;

        ~generator() {
            if (m_coroutine) {
                m_coroutine.destroy();
            }
        }

        generator &operator=(generator other) noexcept {
            swap(other);
            return *this;
        }

        iterator begin() {
            if (m_coroutine) {
                m_coroutine.resume();
                if (m_coroutine.done()) {
                    m_coroutine.promise().rethrow_if_exception();
                }
            }

            return iterator{m_coroutine};
        }

        /*
        iterator begin() const;
        detail::generator_sentinel end() const;
        */

        detail::generator_sentinel end() noexcept {
            return detail::generator_sentinel{};
        }

        /*
        // Not defined and not useful, but required for range-v3
        const_iterator begin() const;
        const_iterator end() const;
        */

        void swap(generator &other) noexcept {
            std::swap(m_coroutine, other.m_coroutine);
        }

        const Details &details() const {
            return m_coroutine
                       ? m_coroutine.promise().details()
                       : m_details_if_default_constructed;
        }

        Details &details() {
            return m_coroutine
                       ? m_coroutine.promise().details()
                       : m_details_if_default_constructed;
        }

        void setDetailsPointer(Details *pointer) {
            m_coroutine.promise().setDetailsPointer(pointer);
        }

    private:
        friend class detail::generator_promise<T, Details, GeneratorHandle>;

        // In the case of an empty, default-constructed `generator` object we still
        // want the call to `details` to return a valid object that in this case is
        // owned directly by the generator itself.
        [[no_unique_address]] Details m_details_if_default_constructed;

        explicit generator(GeneratorHandle<promise_type> coroutine) noexcept
            : m_coroutine(coroutine) {
        }

        GeneratorHandle<promise_type> m_coroutine;
    };

    template<typename T, typename D, template <typename...> typename H>
    void swap(generator<T, D, H> &a, generator<T, D, H> &b) {
        a.swap(b);
    }

    namespace detail {
        template<typename T, typename Details, template <typename...> typename H>
        generator<T, Details, H>
        generator_promise<T, Details, H>::get_return_object() noexcept {
            using coroutine_handle = H<generator_promise<T, Details, H> >;
            return generator<T, Details, H>{coroutine_handle::from_promise(*this)};
        }
    } // namespace detail
} // namespace cppcoro


namespace coro_detail {
    // A class that is implicitly convertible to anything, useful for type traits.
    struct ConvertibleToAnything {
        template<typename T>
        operator T();
    };

    // `has_await_transform<Promise>::value` is true iff. `Promise` has a member function `await_transform` that can be called
    // with a single argument.
    // TODO Check whether actually an await_transform with 0 or > 1 arguments also should be detected.
    template<typename Promise, typename = void>
    struct has_await_transform : std::false_type {
    };

    template<typename Promise>
    struct has_await_transform<Promise,
                std::void_t<decltype(std::declval<Promise &>()
                    .await_transform(std::declval<ConvertibleToAnything>()))> >
            : std::true_type {
    };

    // Given a `promise` and a `co_await`ed expression, get the actual `awaitable`, possibly through the `await_transform` mechanism.
    template<typename Promise, typename Expr>
    decltype(auto) get_awaitable(Promise &promise, Expr &&expr) {
        if constexpr (has_await_transform<Promise>::value) {
            return promise.await_transform(std::forward<Expr>(expr));
        } else {
            return std::forward<Expr>(expr);
        }
    }

    // Type trait to check whether a given `Promise` has a `return_void` member function.
    template<typename Promise, typename = void>
    struct has_return_void : std::false_type {
    };

    template<typename Promise>
    struct has_return_void<Promise,
                std::void_t<decltype(std::declval<Promise &>()
                    .return_void())> >
            : std::true_type {
    };


    // Type trait that checks whether a given promise type has an overloaded `operator new`.
    // Note: This only supports the simple form that only takes a `size_t`.
    // TODO What about alignment.
    // TODO Implement the leading allocator convention thing.
    template<typename P, typename = void>
    struct has_promise_new : std::false_type {
    };

    template<typename P>
    struct has_promise_new<P,
                std::void_t<decltype(P::operator new(size_t{}))> >
            : std::true_type {
    };

    // Type trait that checks whether a given promise type has an overloaded `operator new`
    // that accepts the coroutine arguments in addition to the size.
    // Per the C++ spec, `promise_type::operator new(size, args...)` is tried first.
    template<typename P, typename ArgsTuple, typename = void>
    struct has_promise_new_with_args_impl : std::false_type {};

    template<typename P, typename... Args>
    struct has_promise_new_with_args_impl<P, std::tuple<Args...>,
                std::void_t<decltype(P::operator new(size_t{}, std::declval<Args>()...))>>
            : std::true_type {};

    template<typename P, typename... Args>
    using has_promise_new_with_args = has_promise_new_with_args_impl<P, std::tuple<Args...>>;

    template<typename P, typename = void>
    struct has_promise_delete : std::false_type {
    };

    template<typename P>
    struct has_promise_delete<P,
                std::void_t<decltype(P::operator delete(
                    std::declval<void *>(), size_t{}))> >
            : std::true_type {
    };

    // Type trait for unsized promise delete: `P::operator delete(void*)`.
    template<typename P, typename = void>
    struct has_promise_delete_unsized : std::false_type {};

    template<typename P>
    struct has_promise_delete_unsized<P,
                std::void_t<decltype(P::operator delete(std::declval<void *>()))>>
            : std::true_type {};

    // Allocate the coroutine frame using the promise type's operator new if available.
    // Per the C++ spec, the fallback chain is:
    //   1. P::operator new(size, args...) — with coroutine arguments
    //   2. P::operator new(size)          — without coroutine arguments
    //   3. ::operator new(size)           — global
    template<typename P, typename... Args>
    void *promise_allocate(size_t size, Args&&... args) {
        if constexpr (sizeof...(Args) > 0 && has_promise_new_with_args<P, Args...>::value) {
            return P::operator new(size, std::forward<Args>(args)...);
        } else if constexpr (has_promise_new<P>::value) {
            return P::operator new(size);
        } else {
            return ::operator new(size);
        }
    }
} // namespace coro_detail

// A type-erased "base" class for a coroutine frame, consists of three function pointers to implement
// the member functions of the `CoroutineHandle` replacement below.
struct HandleFrame {
    using F = void(void *);
    using B = bool(void *);
    void *target;
    F *resumeFunc;
    F *destroyFunc;
    B *doneFunc;
};

// A replacement for `std::coroutine_handle`. Consists of a single pointer to the `HandleFrame` from above.
template<typename Promise = void>
struct Handle {
    HandleFrame *ptr;

    // resume, done, destroy, and operator bool just use the indirection to the `HandleFrame`.
    void resume() { ptr->resumeFunc(ptr->target); }
    operator bool() const { return static_cast<bool>(ptr); }
    bool done() const { return ptr->doneFunc(ptr->target); }
    void destroy() { ptr->destroyFunc(ptr->target); }

    //  Only for non-void `Promise` types: convert from promise to handle and back.
    //  Assumes that in our final coroutine frame the promise object will be declared directly after the
    // `HandleFrame` above.
    static constexpr size_t promise_offset =
            (sizeof(HandleFrame) + alignof(Promise) - 1) & ~(alignof(Promise) - 1);

    static Handle from_promise(Promise &p) {
        auto *ptr = reinterpret_cast<HandleFrame *>(reinterpret_cast<char *>(&p) -
                                                    promise_offset);
        return Handle{ptr};
    }

    Promise &promise() {
        return *reinterpret_cast<Promise *>(reinterpret_cast<char *>(ptr) +
                                            promise_offset);
    }

    const Promise &promise() const {
        return *reinterpret_cast<const Promise *>(reinterpret_cast<const char *>(ptr) +
                                                  promise_offset);
    }

};

template<typename Ref, bool isOwningStorage>
struct _coro_storage {
    static constexpr bool isOwning = isOwningStorage;
    using Storage = std::conditional_t<isOwning, std::decay_t<Ref>, std::add_pointer_t<std::decay_t<Ref> > >;
    alignas(Storage) char buffer[sizeof(Storage)];

    struct Val {
        Ref ref_;
    };

    template<typename... Args>
    void construct(Args &&... args) {
        if constexpr (!isOwning) {
            new(buffer) Storage(&args...);
        } else {
            new(buffer) Storage(std::forward<Args>(args)...);
        }
    }

    void destroy() {
        reinterpret_cast<Storage *>(buffer)->~Storage();
    }

    Val get() {
        Storage &storage = *reinterpret_cast<Storage *>(buffer);
        if constexpr (isOwning) {
            return Val{static_cast<Ref>(storage)};
        } else {
            return Val{static_cast<Ref>(*storage)};
        }
    }

    ~_coro_storage() {
    }
};

#define CO_AWAIT_IMPL_IMPL(awaiterMem, handle, ...) \
{ \
auto& awaiter = CO_GET(awaiterMem); \
if (!awaiter.await_ready()) { \
using type = decltype(awaiter.await_suspend(handle)); \
if constexpr (std::is_void_v<type>) { \
awaiter.await_suspend(handle); \
return __VA_ARGS__; \
} else if constexpr (std::is_same_v<type, bool>) { \
  if (! awaiter.await_suspend(handle)) { return __VA_ARGS__; } \
} else { \
/* TODO encourage tail call optimization for empty `__VA_ARGS__`*/ \
awaiter.await_suspend(handle).resume(); \
return __VA_ARGS__; \
} \
} \
} void()
#define CO_AWAIT_IMPL(awaiterMem) CO_AWAIT_IMPL_IMPL(awaiterMem, Hdl::from_promise(pt))

#define CO_RESUME(index, awaiterMem) \
  label_##index:                                                       \
  CO_GET(awaiterMem).await_resume();                                   \
  this->awaiterMem.destroy();

#define CO_YIELD(index, awaiterMem, value)                            \
    this->awaiterMem.construct(promise().yield_value(value));           \
    this->curState = index;                                            \
    CO_AWAIT_IMPL(awaiterMem); \
    CO_RESUME(index, awaiterMem);

#define CO_AWAIT_VOID(index, awaiterMem, expr)                                  \
{                                                                              \
this->awaiterMem.construct(coro_detail::get_awaitable(promise(), expr));      \
this->curState = index;                                                      \
    CO_AWAIT_IMPL(awaiterMem);                                                \
    CO_RESUME(index, awaiterMem);

#define CO_RETURN_IMPL(index, finalAwaiterMem)                                  \
  this->destroySuspendedCoro(index);                                  \
  this->done_ = true;                                                          \
  this->atFinalSuspend_ = true;                                                \
  this->finalAwaiterMem.construct(promise().final_suspend());                  \
  CO_AWAIT_IMPL(finalAwaiterMem);                                              \
  CO_GET(finalAwaiterMem).await_resume();                                      \
  this->finalAwaiterMem.destroy();                                             \
  this->atFinalSuspend_ = false;                                               \
  Hdl::from_promise(pt).destroy();                                             \
  return;                                                                      \
  void()

#define CO_RETURN_VOID(index, finalAwaiterMem)                                  \
    if constexpr (coro_detail::has_return_void<std::decay_t<decltype(promise())>>::value) { \
    promise().return_void();                                                     \
    }                                                                             \
    CO_RETURN_IMPL(index, finalAwaiterMem);                                     \
    void()

#define CO_RETURN_VALUE(index, finalAwaiterMem, value)                           \
    promise().return_value(value);                                               \
    CO_RETURN_IMPL(index, finalAwaiterMem);                                       \
    void()

#define TRY_BEGIN(try_index) this->currentTryBlock_ = (try_index); void()
#define TRY_END(parent_index, label) this->currentTryBlock_ = (parent_index); label_##label: void()

inline constexpr size_t CO_NO_TRY_BLOCK = static_cast<size_t>(-1);
template<typename Derived, typename PromiseType>
struct CoroImpl {
    HandleFrame frm;
    PromiseType pt;

    static void CHECK() {
        static_assert(offsetof(CoroImpl, pt) -
                      offsetof(CoroImpl, frm) ==
                      Handle<PromiseType>::promise_offset);
    }

    size_t curState = 0;
    bool done_ = false;
    bool atFinalSuspend_ = false;
    using Hdl = Handle<PromiseType>;

    PromiseType &promise() { return pt; }

    static auto cast(void *blubb) {
        return static_cast<Derived *>(reinterpret_cast<CoroImpl *>(
            reinterpret_cast<char *>(blubb) -
            offsetof(CoroImpl, frm)));
    }

    static void resume(void *blubb) { cast(blubb)->doStep(); }

    static void destroy(void *blubb) {
        auto *self = cast(blubb);
        if (!self->done_) {
            self->destroySuspendedCoro(self->curState);
        } else if (self->atFinalSuspend_) {
            self->destroyFinalSuspend();
        }
        if constexpr (coro_detail::has_promise_delete<PromiseType>::value) {
            self->~Derived();
            PromiseType::operator delete(
                static_cast<void *>(self), sizeof(Derived));
        } else if constexpr (coro_detail::has_promise_delete_unsized<PromiseType>::value) {
            self->~Derived();
            PromiseType::operator delete(static_cast<void *>(self));
        } else {
            delete self;
        }
    }

    static bool done(void *blubb) {
        return cast(blubb)->done_;
    }

    CoroImpl() {
        CHECK();
        frm.target = this;
        frm.resumeFunc = &CoroImpl::resume;
        frm.destroyFunc = &CoroImpl::destroy;
        frm.doneFunc = &CoroImpl::done;
    }

    template<typename... T>
    static void destroySafely(T &... mems) {
        (..., mems.destroy());
    }

    template<typename... CoroArgs>
    static auto ramp(CoroArgs &&... coroArgs) {
        // TODO<joka921> alignment.
        void *__coro_mem = coro_detail::promise_allocate<PromiseType>(sizeof(Derived), std::forward<CoroArgs>(coroArgs)...);
        auto *frame = new(__coro_mem) Derived{std::forward<CoroArgs>(coroArgs)...};
        auto ret = frame->pt.get_return_object();
        frame->__initial_awaiter.construct(frame->pt.initial_suspend());
        if (co_await_impl(frame->__initial_awaiter.get().ref_, Handle<PromiseType>::from_promise(frame->pt))) {
            frame->doStep();
        }
        return ret;
    }
};

#define FOR_LOOP_HEADER(N)


#define CO_GET(arg) this->arg.get().ref_
#define CO_GET_STATE(arg) arg.get().ref_

// TODO<joka921> Update the other OWNING also to the new lambda syntax.
#define CO_INIT_REF(mem, ...)  \
    new(this->mem.buffer) typename decltype(this->mem)::Storage{ &__VA_ARGS__} ;\
    this->__constructed.mem=true

#define CO_BRACED_INIT(mem, ...) CO_INIT_REF(mem, __VA_ARGS__)
#define CO_PAREN_INIT(mem, ...) CO_INIT_REF(mem, __VA_ARGS__)
#define CO_PAREN_INIT_OWNING(mem, ...) \
  [&]() -> decltype(auto) { \
    new(this->mem.buffer) typename decltype(this->mem)::Storage( __VA_ARGS__); \
    this->__constructed.mem=true; \
    return std::move(CO_GET(mem)); \
  }()

#define CO_BRACED_INIT_OWNING(mem, ...) \
[&]() -> decltype(auto) { \
new(this->mem.buffer) typename decltype(this->mem)::Storage{ __VA_ARGS__}; \
this->__constructed.mem=true; \
return std::move(CO_GET(mem)); \
}()


template<typename Ref, bool isOwning>
struct coro_for_loop_storage {
    _coro_storage<Ref, isOwning> range_;
    // TODO<joka921> This doesn't work for nonmember begin and end, but that should work for most cases now.
    using Begin = decltype(std::declval<Ref>().begin());
    using End = decltype(std::declval<Ref>().end());
    _coro_storage<Begin &, true> begin_;
    _coro_storage<End &, true> end_;

    struct {
        bool range_ = false;
        bool begin_ = false;
        bool end_ = false;
    } __constructed;
};


#endif
