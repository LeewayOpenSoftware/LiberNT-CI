
#pragma once

#include <wil/resource.h>
#include <kmacros.h>

template<typename T>
using KDeletePtr = wistd::default_delete<T>;

template<typename T>
struct KFreePool
{
    PAGED KFreePool() {}

    template<class T2>
    PAGED KFreePool(const KFreePool<T2>&) { }

    PAGED void operator()(T *p) const
    {
        if (p != nullptr)
        {
            if constexpr (!wistd::is_void<T>::value) p->~T();
            ExFreePool(p);
        }
    }
};

template<typename T>
struct KFreePoolNP
{
    NONPAGED KFreePoolNP() {}

    template<class T2>
    NONPAGED KFreePoolNP(const KFreePoolNP<T2>&) { }

    NONPAGED void operator()(T *p) const
    {
        if (p != nullptr)
        {
            if constexpr (!wistd::is_void<T>::value) p->~T();
            ExFreePool(p);
        }
    }
};

template<typename T>
struct KInplaceDelete
{
    PAGED KInplaceDelete() { }

    template<class T2>
    PAGED KInplaceDelete(const KInplaceDelete<T2>&) { }

    PAGED void operator()(T *p) const { p->~T(); }
};
