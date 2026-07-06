/**
 * Copyright (c) 2026 Pavel Martishevsky
 *
 * This header is distributed under the MIT License. See notice at the end of this file.
 *
 * tta_assert.h - A multi-mode assert macro for C/C++ supporting unit test mode
 */

#ifndef TTA_ASSERT_H
#define TTA_ASSERT_H

/* clang-format off */
#ifdef __cplusplus
#   define TTA_ASSERT_API extern "C"
#   define TTA_ASSERT_MAY_THROW_API extern
#else
#   define TTA_ASSERT_API
#   define TTA_ASSERT_MAY_THROW_API
#endif

/**
 *  @brief  Compile-time modes controlling assertion macros behaviour
 *              - passthrough: executes condition only in `TTA_ASSERT_IF{_MSG}` or `TTA_ASSERT_RET{_MSG}`,
 *                             `TTA_ASSERT{_MSG}` assertions are compiled out and don't carry any overhead
 *
 *              - instrumented: executes the condition, reports details of the assertion to the calling site via
 *                              callback set in `tta_AssertSetReportCback`, optionally calls a debugreak or throw/longjmp
 */
#define TTA_ASSERT_MODE_PASSTHROUGH 0
#define TTA_ASSERT_MODE_INSTRUMENTED 1

/**
 *  @brief  The enum specifying runtime assertion level of reporting when instrumentation is enabled
 *          via `TTA_ASSERT_MODE` being set to `TTA_ASSERT_MODE_INSTRUMENTED`
 */
enum tta_AssertReportLevel
{
    ktta_AssertReportLevel_Print         = 0,   /**< every assertion triggers a callback setup through `tta_AssertSetReportCback` */
    ktta_AssertReportLevel_PrintAndBreak = 1,   /**< same _Print + assertion triggers a debugbreak when debugger is attached */
    ktta_AssertReportLevel_PrintAndThrow = 2,   /**< same _Print + assertion calls `throw`/`longjmp` to `catch`/`setjmp` later
                                                     mainly to support reusing existing code with asserts in tests */
    ktta_AssertReportLevel_ForceInt      = 0x3fffffff
};

#ifndef __cplusplus
typedef enum tta_AssertReportLevel tta_AssertReportLevel;
#endif

#if defined(_MSC_VER)
#   define TTA_BREAK() __debugbreak()
#elif defined(__clang__) && __has_builtin(__builtin_debugtrap)
#   define TTA_BREAK() __builtin_debugtrap()
#else
#   error Unknown compiler
#endif

#if defined(_MSC_VER)
#   ifdef __cplusplus
#       define TTA_FUNC __FUNCSIG__
#   else
#       define TTA_FUNC __FUNCTION__
#   endif
#elif defined(__clang__)
#   define TTA_FUNC __PRETTY_FUNCTION__
#else
#   error Unknown compiler
#endif
/* clang-format on */

/** @brief  Function type used as a callback implementing how assertion information is used / printed */
TTA_ASSERT_API typedef void tta_AssertReportCback(const char *expr, const char *file, int line, const char *func, const char *msg, void *args);

TTA_ASSERT_API int tta_AssertAlways_0(void);
TTA_ASSERT_API int tta_AssertAlways_1(void);

TTA_ASSERT_API tta_AssertReportLevel  tta_AssertGetReportLevel(void);
TTA_ASSERT_API tta_AssertReportCback *tta_AssertGetReportCback(void);

/** @brief  Function to setup assertion level of reporting */
TTA_ASSERT_API void tta_AssertSetReportLevel(tta_AssertReportLevel level);

/** @brief  Function to setup a callback that receive assertion information */
TTA_ASSERT_API void tta_AssertSetReportCback(tta_AssertReportCback *cback);

/** @brief  Function that is called by assert macros in instrumented mode to pass information to the `tta_AssertReportCback` */
TTA_ASSERT_MAY_THROW_API int tta_AssertReport(const char *cond, const char *, int, const char *, const char *, ...);

/**
 *  @brief  The function that could be used to implement testing by re-using existing code containing TTA_ASSERT:
 *              1. Just pass the `callback` that executes a code with TTA_ASSERT, and `userdata` for it
 *              2. If TTA_ASSERT is triggered by any code executed by `callback` (not necessarily triggered within `callback`
 *                 which assumes arbitrarily nesting), this function returns `1`, otherwise it returns `0`,
 *                 so it's really easy to count failed tests `failed += tta_AssertCallAndCatch(&my_test, my_test_userdata);`
 *  @note   For now, it's not reentrant: any code executed within callback is not allowed to call `tta_AssertCallAndCatch`
 *  @warning  When `TTA_ASSERT_NOEXCEPT` is enabled, this function uses `setjmp`/`longjmp` internally.
 *            `longjmp` over C++ objects with non-trivial destructors is undefined behavior (C++ $21.10.4, CERT ERR52-CPP).
 *            Ensure no RAII objects (`std::string`, `std::vector`, smart pointers, lock guards, etc.) are alive between
 *            the `TTA_ASSERT` call site and the `tta_AssertCallAndCatch` frame when using `TTA_ASSERT_NOEXCEPT`.
 */
TTA_ASSERT_MAY_THROW_API int tta_AssertCallAndCatch(int (*callback)(void *), void *userdata);

#endif /* TTA_ASSERT_H */

/* clang-format off */
/**
 *  Assert macros.
 *
 *  To switch compile-time mode:
 *      #undef  TTA_ASSERT_MODE
 *      #define TTA_ASSERT_MODE TTA_ASSERT_MODE_INSTRUMENTED
 *      #include "tta_assert.h"
 *
 *  Because the header file can be included multiple times, the macro are undefined first
 */
#ifdef TTA_ASSERT
#   undef TTA_ASSERT
#endif

#ifdef TTA_ASSERT_MSG
#   undef TTA_ASSERT_MSG
#endif

#ifdef TTA_ASSERT_RET
#   undef TTA_ASSERT_RET
#endif

#ifdef TTA_ASSERT_RET_MSG
#   undef TTA_ASSERT_RET_MSG
#endif

#ifdef TTA_ASSERT_IF
#   undef TTA_ASSERT_IF
#endif

#ifdef TTA_ASSERT_IF_MSG
#   undef TTA_ASSERT_IF_MSG
#endif

/**
 *  `TTA_ASSERT_MODE` is set to passthrough by default
 */
#ifndef TTA_ASSERT_MODE
#   define TTA_ASSERT_MODE TTA_ASSERT_MODE_PASSTHROUGH
#endif

#if TTA_ASSERT_MODE == TTA_ASSERT_MODE_PASSTHROUGH
#   define TTA_ASSERT_MSG(cond, msg, ...)           ((void)sizeof((cond) != 0))
#   define TTA_ASSERT_IF_MSG(cond, msg, ...)        if (!!(cond))
#   define TTA_ASSERT_RET_MSG(cond, retv, msg, ...) do { if (!(cond)) return (retv); } while (0)
#elif TTA_ASSERT_MODE == TTA_ASSERT_MODE_INSTRUMENTED
#   define TTA_ASSERT_MSG(cond, msg, ...)                                           \
        (!!(cond) ||                                                                \
            (                                                                       \
                (                                                                   \
                    tta_AssertReport(#cond, __FILE__, __LINE__, TTA_FUNC, msg, ##__VA_ARGS__) &&   \
                    (TTA_BREAK(), tta_AssertAlways_1())                             \
                ),                                                                  \
                tta_AssertAlways_0()                                                \
            )                                                                       \
        )

#   define TTA_ASSERT_IF_MSG(cond, msg, ...)            \
        if (TTA_ASSERT_MSG(cond, msg, ##__VA_ARGS__))

#   define TTA_ASSERT_RET_MSG(cond, retv, msg, ...)                                \
        do                                                                         \
        {                                                                          \
            if (!TTA_ASSERT_MSG(cond, msg, ##__VA_ARGS__))                         \
                return (retv);                                                     \
        } while (0)
#else
#   error Unknown TTA_ASSERT_MODE.
#endif /* TTA_ASSERT_MODE == TTA_ASSERT_MODE_PASSTHROUGH */

#define TTA_ASSERT(cond)            TTA_ASSERT_MSG(cond, 0)
#define TTA_ASSERT_IF(cond)         TTA_ASSERT_IF_MSG(cond, 0)
#define TTA_ASSERT_RET(cond, retv)  TTA_ASSERT_RET_MSG(cond, retv, 0)
/* clang-format on */

/**
 *  Define TTA_ASSERT_IMPL in exactly one translation unit to provide storage.
 */
#ifdef TTA_ASSERT_IMPL

#ifndef TTA_ASSERT_IMPL_PRESENT
#define TTA_ASSERT_IMPL_PRESENT 1

static void tta_AssertReportCback_Default(const char *expr, const char *file, int line, const char *func, const char *msg, void *args)
{
    (void)expr;
    (void)file;
    (void)line;
    (void)func;
    (void)msg;
    (void)args;
}

static volatile int tta_zero = 0;
static volatile int tta_one  = 1;

int tta_AssertAlways_0(void)
{
    return tta_zero;
}

int tta_AssertAlways_1(void)
{
    return tta_one;
}

static tta_AssertReportCback *tta_Cback = &tta_AssertReportCback_Default;
static tta_AssertReportLevel  tta_Level = ktta_AssertReportLevel_Print;

tta_AssertReportLevel tta_AssertGetReportLevel(void)
{
    return tta_Level;
}

tta_AssertReportCback *tta_AssertGetReportCback(void)
{
    return tta_Cback;
}

void tta_AssertSetReportLevel(tta_AssertReportLevel level)
{
    tta_Level = level;
}

void tta_AssertSetReportCback(tta_AssertReportCback *cback)
{
    tta_Cback = cback ? cback : &tta_AssertReportCback_Default;
}

/* clang-format off */
#if defined(__cplusplus) && !(defined(TTA_ASSERT_NOEXCEPT) && TTA_ASSERT_NOEXCEPT)
    struct tta_AssertExcept
    {
        const char *msg;
        tta_AssertExcept(const char *m) : msg(m)
        {
        }
    };
#else
    /*
     * TTA_ASSERT_NOEXCEPT / C mode: uses setjmp/longjmp instead of C++ exceptions.
     * WARNING: longjmp over C++ objects with non-trivial destructors is undefined behavior.
     * See C++ $21.10.4 and CERT ERR52-CPP.
     */
#   ifdef _MSC_VER
#       pragma warning(push)
#       pragma warning(disable : 4820) /* struct padding in system headers */
#   endif
#   include <setjmp.h>
#   ifdef _MSC_VER
#       pragma warning(pop)
#   endif
    static jmp_buf     tta_jmpbuf;
    static const char *tta_error;
#endif
/* clang-format on */

#include <stdarg.h>

/* Debugger presence detection, current support only WIN32 */
#if defined(_WIN32)
    TTA_ASSERT_API __declspec(dllimport) int __stdcall IsDebuggerPresent(void);
    static int tta_IsDebuggerPresent(void) { return IsDebuggerPresent(); }
#else
    static int tta_IsDebuggerPresent(void) { return 1; }
#endif

int tta_AssertReport(const char *cond, const char *file, int line, const char *func, const char *msg, ...)
{
    va_list args;
    va_start(args, msg);
    tta_Cback(cond, file, line, func, msg, &args);
    va_end(args);
    if (tta_Level == ktta_AssertReportLevel_PrintAndThrow)
    {
#if defined(__cplusplus) && !(defined(TTA_ASSERT_NOEXCEPT) && TTA_ASSERT_NOEXCEPT)
        throw tta_AssertExcept("tta_Except");
#else
        tta_error = "tta_Except";
        longjmp(tta_jmpbuf, 1);
#endif
    }
    return tta_Level == ktta_AssertReportLevel_PrintAndBreak && tta_IsDebuggerPresent();
}

int tta_AssertCallAndCatch(int (*callback)(void *), void *userdata)
{
    tta_AssertReportCback *prev_cback = tta_Cback;
    tta_AssertReportLevel  prev_level = tta_Level;
    volatile int           failed     = 0;
    tta_Level                         = ktta_AssertReportLevel_PrintAndThrow;
#if defined(__cplusplus) && !(defined(TTA_ASSERT_NOEXCEPT) && TTA_ASSERT_NOEXCEPT)
    try
#else
    if (setjmp(tta_jmpbuf) == 0)
#endif
    {
        callback(userdata);
    }
#if defined(__cplusplus) && !(defined(TTA_ASSERT_NOEXCEPT) && TTA_ASSERT_NOEXCEPT)
    catch (tta_AssertExcept &)
    {
        failed = 1;
    }
#   ifdef _MSC_VER
#       pragma warning(push)
#       pragma warning(disable : 4571) /* catch(...) semantics changed since VC++ 7.1 */
#   endif
    catch (...)
#   ifdef _MSC_VER
#       pragma warning(pop)
#   endif
#else
    else
#endif
    {
        failed = 1;
    }
    tta_Cback = prev_cback;
    tta_Level = prev_level;
    return (int)failed;
}

#endif /* #ifndef TTA_ASSERT_IMPL_PRESENT */

#endif /* #ifdef TTA_ASSERT_IMPL */

/**
 * Copyright (c) 2026 Pavel Martishevsky
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
