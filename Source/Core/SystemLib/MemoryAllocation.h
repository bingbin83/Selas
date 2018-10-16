#pragma once

//=================================================================================================================================
// Joe Schutte
//=================================================================================================================================

#include "SystemLib/BasicTypes.h"

#include <new>

namespace Selas
{
    #define PlacementNew_(Type_, Var_)                     new(Var_) Type_()
    #define PlacementDelete_(Type_, Var_)                  (Var_)->~Type_()

    #define New_(Type_)                                    SelasNew<Type_>(__FUNCTION__, __FILE__, __LINE__)
    #define Delete_(Var_)                                  SelasDelete(Var_)
    #define SafeDelete_(Var_)                              if (Var_) { SelasDelete(Var_); Var_ = nullptr; }

    #define Alloc_(AllocSize_)                             Selas::SelasMalloc(AllocSize_, __FUNCTION__, __FILE__, __LINE__)
    #define AllocArray_(Type_, Count_)                     static_cast<Type_*>(Selas::SelasMalloc(Count_ * sizeof(Type_), __FUNCTION__, __FILE__, __LINE__))
    #define Realloc_(Addr_, Size_)                         Selas::SelasRealloc(Addr_, Size_, __FUNCTION__, __FILE__, __LINE__);
    #define Free_(Var_)                                    Selas::SelasFree(Var_)
    #define SafeFree_(Var_)                                if(Var_) { Selas::SelasFree(Var_); Var_ = nullptr; }

    #define AllocArrayAligned_(Type_, Count_, Alignment_)  static_cast<Type_*>(Selas::SelasAlignedMalloc(Count_ * sizeof(Type_), Alignment_, __FUNCTION__, __FILE__, __LINE__))
    #define AllocAligned_(AllocSize_, Alignment_)          Selas::SelasAlignedMalloc(AllocSize_, Alignment_, __FUNCTION__, __FILE__, __LINE__)
    #define AllocAlignedNamed_(AllocSize_, Alignment_, Name_) Selas::SelasAlignedMalloc(AllocSize_, Alignment_, Name_, __FILE__, __LINE__)
    #define FreeAligned_(Var_)                             Selas::SelasAlignedFree(Var_)
    #define SafeFreeAligned_(Var_)                         if(Var_) { Selas::SelasAlignedFree(Var_); Var_ = nullptr; }

    extern void* SelasAlignedMalloc(uint size, uint alignment, const char* name, const char* file, int line);
    extern void* SelasMalloc(uint size, const char* name, const char* file, int line);
    extern void* SelasRealloc(void* address, uint size, const char* name, const char* file, int line);
    extern void  SelasAlignedFree(void* address);
    extern void  SelasFree(void* address);

    extern void MemoryReport();

    //=============================================================================================================================
    template <typename Type_>
    inline Type_* SelasNew(const char* function, const char* file, int line)
    {
        Type_* mem = static_cast<Type_*>(SelasMalloc(sizeof(Type_), function, file, line));
        PlacementNew_(Type_, mem);

        return mem;
    }

    //=============================================================================================================================
    template <typename Type_>
    inline void SelasDelete(Type_* memory)
    {
        PlacementDelete_(Type_, memory);
        SelasFree(static_cast<void*>(memory));
    }
}