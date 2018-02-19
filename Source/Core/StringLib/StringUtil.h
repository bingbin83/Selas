#pragma once

//==============================================================================
// StringUtil
//==============================================================================

#include <SystemLib/BasicTypes.h>
#include <vadefs.h>

namespace Shooty
{

    namespace StringUtil
    {
        char Lowercase(char c);
        int32 Length(char const* text);

        char* FindChar(char* text, char searchChar);
        char* FindLastChar(char* text, char searchChar);

        char* FindSubString(char* text, char const* searchString);

        int32 FindIndexOf(char const* text, char searchChar);
        int32 FindIndexOf(char const* text, char const* searchText);
        int32 FindIndexOf(char const* text, char const* searchText, int32 offset);

        int32 Compare(char const* lhs, char const* rhs);
        int32 CompareN(char const* lhs, char const* rhs, int32 compareLength);
        int32 CompareNIgnoreCase(char const* lhs, char const* rhs, int32 compareLength);

        bool Equals(char const* lhs, char const* rhs);
        bool EqualsN(char const* lhs, char const* rhs, int32 compareLength);
        bool EqualsIgnoreCase(char const* lhs, char const* rhs);

        bool EndsWithIgnoreCase(const char* lhs, const char* rhs);

        void Copy(char* destString, int32 destMaxLength, char const* srcString);
        void CopyN(char* destString, int32 destMaxLength, char const* srcString, int32 srcStringLength);

        int32 ToInt32(char const* text);
        float ToFloat(char const* text);

        int32 Sprintf(char* destString, int destMaxLength, char const* format, va_list arglist);

        void RemoveExtension(char* str);
    }

}