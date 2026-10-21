/*
 MIT License
 *
 * Copyright (c) 2026 Tyge Løvset
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/*
#include <stdio.h>
#include "cspan.h"
use_cspan(Span2f, float, 2);
use_cspan(Intspan, int);

int demo1() {
    float raw[4*5];
    Span2f ms = cspan_md(raw, 4, 5);

    for (int i=0; i<ms.shape[0]; i++)
        for (int j=0; j<ms.shape[1]; j++)
            *cspan_at(&ms, i, j) = i*1000 + j;

    printf("%f\n", *cspan_at(&ms, 3, 4));
}

int demo2() {
    int array[] = {10, 20, 30, 23, 22, 21};
    Intspan span = cspan_from_array(array);

    for (Intspan_iter i = Intspan_begin(&span); i.ref; Intspan_next(&i))
        printf(" %d", *i.ref);
    puts("");
}
*/
#ifndef STC_CSPAN_H_INCLUDED
#define STC_CSPAN_H_INCLUDED

/* This header is self-contained and C-only: it uses C99 compound literals and
   designated initializers. It does not require STC's common.h, linkage.h, or a
   separate object file. C++ support is intentionally not provided. */

#ifdef _MSC_VER
    #pragma warning(disable: 4116 4996) // unnamed type definition in parentheses
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef ISIZE_MAX
    typedef ptrdiff_t   isize_t;
    typedef isize_t     isize; // [deprecated]
    #define ISIZE_MIN   PTRDIFF_MIN
    #define ISIZE_MAX   PTRDIFF_MAX
#endif

#if defined __GNUC__ || defined __clang__
    #define _CSPAN_GNUATTR(...) __attribute__((__VA_ARGS__))
#else
    #define _CSPAN_GNUATTR(...)
#endif
#define _CSPAN_INLINE static inline _CSPAN_GNUATTR(unused)

// Macro overloading feature support
#define _CSPAN_OVERLOAD(name, ...) _CSPAN_JOIN(name##_, _CSPAN_NUMARGS(__VA_ARGS__))(__VA_ARGS__)
#define _CSPAN_JOIN0(a, b)          a##b
#define _CSPAN_JOIN(a, b)           _CSPAN_JOIN0(a, b)
#define _CSPAN_NUMARGS(...)         _CSPAN_APPLY_ARG_N((__VA_ARGS__, _CSPAN_RSEQ_N))
#define _CSPAN_APPLY_ARG_N(args)    _CSPAN_ARG_N args
#define _CSPAN_RSEQ_N               20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1,
#define _CSPAN_ARG_N(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,_17,_18,_19,_20,N,...) N
#define _CSPAN_TUPLE_AT_1(x, y, ...) y
#define _CSPAN_COMMA_N(x)            , x
#define _CSPAN_EXPAND(...)           __VA_ARGS__

#define _cspan_literal(T)              (T)
#define _cspan_literal_count(T, ...)   (sizeof((T[])__VA_ARGS__) / sizeof(T))
#define _cspan_make_array(T, ...)      ((T[])__VA_ARGS__)
#define _cspan_make_array2d(T, N, ...) ((T[][N])__VA_ARGS__)

#define _cspan_static_assert(expr) (void)sizeof(int[(expr) ? 1 : -1])
#if defined STC_NDEBUG || defined NDEBUG
    #define _cspan_assert(expr) (void)sizeof(expr)
#else
    #define _cspan_assert(expr) assert(expr)
#endif

// These are part of the standalone cspan API, and are also handy internally.
#define cspan_sizeof        (isize_t)sizeof
#define cspan_uless(a, b)   ((size_t)(a) < (size_t)(b))
#define cspan_countof(a)    (isize_t)(sizeof(a) / sizeof 0[a])
#define cspan_arraylen(a)   cspan_countof(a)

/* Short standalone aliases. */
#define c_sizeof            cspan_sizeof
#define c_countof(a)        cspan_countof(a)
#define c_arraylen(a)       cspan_arraylen(a)

// Only used on _istride arrays (shape and strides), so no typeof is needed.
#define _cspan_swap(xp, yp) do { \
    _istride _tv = *(xp); *(xp) = *(yp); *(yp) = _tv; \
} while (0)

#ifndef STC_CSPAN_INDEX_TYPE
    #ifdef CSPAN_INDEX_TYPE
        #define STC_CSPAN_INDEX_TYPE CSPAN_INDEX_TYPE
    #else
        #define STC_CSPAN_INDEX_TYPE int32_t
    #endif
#endif
typedef STC_CSPAN_INDEX_TYPE _istride;
typedef _istride _ituple3[3];

#define using_cspan use_cspan                   // [deprecated]
#define using_cspan2 use_cspan2                 // [deprecated]
#define using_cspan3 use_cspan3                 // [deprecated]
#define using_cspan2_with_eq use_cspan2_with_eq // [deprecated]
#define using_cspan3_with_eq use_cspan3_with_eq // [deprecated]

#define use_cspan(...) _CSPAN_OVERLOAD(_cspan_use, __VA_ARGS__)
#define _cspan_use_2(Self, T) \
    _cspan_use_3(Self, T, 1); \
    _CSPAN_INLINE Self Self##_from_n(Self##_value* dataptr, isize_t n) \
        { return (Self)cspan_from_n(dataptr, n); } \
    _CSPAN_INLINE const Self##_value* Self##_at(const Self* self, isize_t idx) \
        { return cspan_at(self, idx); } \
    _CSPAN_INLINE Self##_value* Self##_at_mut(Self* self, isize_t idx) \
        { return cspan_at(self, idx); } \
    struct stc_nostruct

#define use_cspan_with_eq(...) _CSPAN_OVERLOAD(_cspan_use_with_eq, __VA_ARGS__)
#define _cspan_use_with_eq_3(Self, T, i_eq) \
    _cspan_use_with_eq_4(Self, T, i_eq, 1); \
    _CSPAN_INLINE Self Self##_from_n(Self##_value* dataptr, isize_t n) \
        { return (Self)cspan_from_n(dataptr, n); } \
    struct stc_nostruct

#define _cspan_use_3(Self, T, RANK) \
    typedef T Self##_value; \
    typedef T Self##_raw; \
    typedef struct { \
        Self##_value *data; \
        _istride shape[RANK]; \
        cspan_tuple##RANK stride; \
    } Self; \
    \
    typedef struct { \
        Self##_value *ref; \
        const Self *_s; \
        _istride pos[RANK]; \
    } Self##_iter; \
    \
    _CSPAN_INLINE Self Self##_slice_(Self##_value* d, const _istride shape[], const _istride stri[], \
                                     const isize_t args[][3], const int rank) { \
        Self s; int outrank; \
        s.data = d + _cspan_slice(s.shape, s.stride.d, &outrank, shape, stri, args, rank); \
        _cspan_assert(outrank == RANK); \
        return s; \
    } \
    _CSPAN_INLINE Self##_iter Self##_begin(const Self* self) { \
        return _cspan_literal(Self##_iter){ \
            .ref=cspan_size(self) == 0 ? NULL : self->data, ._s=self, .pos={0}}; \
    } \
    _CSPAN_INLINE Self##_iter Self##_end(const Self* self) { \
        (void)self; \
        return _cspan_literal(Self##_iter){.ref=NULL, ._s=NULL, .pos={0}}; \
    } \
    _CSPAN_INLINE void Self##_next(Self##_iter* it) { \
        isize_t off = it->_s->stride.d[RANK - 1]; \
        bool done = _cspan_next##RANK(it->pos, it->_s->shape, it->_s->stride.d, RANK, &off); \
        if (done) it->ref = NULL; else it->ref += off; \
    } \
    _CSPAN_INLINE isize_t Self##_size(const Self* self) \
        { return cspan_size(self); } \
    _CSPAN_INLINE Self Self##_transposed(Self sp) \
        { _cspan_transpose(sp.shape, sp.stride.d, cspan_rank(&sp)); return sp; } \
    _CSPAN_INLINE Self Self##_swapped_axes(Self sp, int ax1, int ax2) \
        { _cspan_swap_axes(sp.shape, sp.stride.d, cspan_rank(&sp), ax1, ax2); return sp; } \
    struct stc_nostruct

#define _cspan_use_with_eq_4(Self, T, i_eq, RANK) \
    _cspan_use_3(Self, T, RANK); \
    _CSPAN_INLINE bool Self##_eq(const Self* x, const Self* y) { \
        if (memcmp(x->shape, y->shape, sizeof x->shape) != 0) \
            return false; \
        for (Self##_iter _i = Self##_begin(x), _j = Self##_begin(y); \
             _i.ref != NULL; Self##_next(&_i), Self##_next(&_j)) \
            { if (!(i_eq(_i.ref, _j.ref))) return false; } \
        return true; \
    } \
    _CSPAN_INLINE bool Self##_equals(Self sp1, Self sp2) \
        { return Self##_eq(&sp1, &sp2); } \
    struct stc_nostruct

#define use_cspan2(Self, T) _cspan_use_2(Self, T); _cspan_use_3(Self##2, T, 2)
#define use_cspan3(Self, T) use_cspan2(Self, T); _cspan_use_3(Self##3, T, 3)
#define use_cspan2_with_eq(Self, T, eq) _cspan_use_with_eq_3(Self, T, eq); \
                                        _cspan_use_with_eq_4(Self##2, T, eq, 2)
#define use_cspan3_with_eq(Self, T, eq) use_cspan2_with_eq(Self, T, eq); \
                                        _cspan_use_with_eq_4(Self##3, T, eq, 3)
#define use_cspan_tuple(N) typedef struct { _istride d[N]; } cspan_tuple##N
use_cspan_tuple(1); use_cspan_tuple(2);
use_cspan_tuple(3); use_cspan_tuple(4);
use_cspan_tuple(5); use_cspan_tuple(6);
use_cspan_tuple(7); use_cspan_tuple(8);

// Construct a cspan from a pointer+size
#define cspan_from_n(dataptr, n) \
    {.data=dataptr, \
     .shape={(_istride)(n)}, \
     .stride=_cspan_literal(cspan_tuple1){.d={1}}}

// Create a 1d-span in the local lexical scope. N must be a compile-time constant.
#define cspan_by_copy(dataptr, N) \
    cspan_from_n(memcpy((char[(N)*sizeof *(dataptr)]){0}, dataptr, (N)*sizeof *(dataptr)), N)

// Create a zeroed out 1d-span in the local lexical scope. N must be a compile-time constant.
#define cspan_zeros(Span, N) \
    ((Span)cspan_from_n((Span##_value[N]){0}, N))

// Create a global scope 1d-span from constant initializer list, otherwise like c_make(Span, ...).
#define cspan_make(Span, ...) \
    ((Span)cspan_from_n(_cspan_make_array(Span##_value, __VA_ARGS__), \
                        _cspan_literal_count(Span##_value, __VA_ARGS__)))

// Make 1d-span from a c-array.
#define cspan_from_array(array) \
    cspan_from_n(array, c_countof(array))

// Make 1d-span from a vec or stack container.
#define cspan_from_vec(container) \
    cspan_from_n((container)->data, (container)->size)

// Make a 1d-sub-span from a 1d-span
#define cspan_subspan(self, offset, count) \
    {.data=cspan_at(self, offset), \
     .shape={(_istride)(count)}, \
     .stride=(self)->stride}

// Accessors
//
#define cspan_size(self) _cspan_size((self)->shape, cspan_rank(self))
#define cspan_rank(self) c_countof((self)->shape) // constexpr
#define cspan_at(self, ...) ((self)->data + cspan_index(self, __VA_ARGS__))
#define cspan_front(self) ((self)->data)
#define cspan_back(self) ((self)->data + cspan_size(self) - 1)

#define cspan_index(...) _cspan_index_fn(__VA_ARGS__, _CSPAN_COMMA_N(_cspan_index_3d), \
                                                     _CSPAN_COMMA_N(_cspan_index_2d), \
                                                     _CSPAN_COMMA_N(_cspan_index_1d),)(__VA_ARGS__)
#define _cspan_index_fn(self, i,j,k,n, ...) _CSPAN_TUPLE_AT_1(n, _cspan_index_nd,)
#define _cspan_index_1d(self, i)     (_cspan_static_assert(cspan_rank(self) == 1), \
                                      _cspan_assert((i) < (self)->shape[0]), \
                                      (i)*(self)->stride.d[0])
#define _cspan_index_2d(self, i,j)   (_cspan_static_assert(cspan_rank(self) == 2), \
                                      _cspan_assert((i) < (self)->shape[0] && (j) < (self)->shape[1]), \
                                      (i)*(self)->stride.d[0] + (j)*(self)->stride.d[1])
#define _cspan_index_3d(self, i,j,k) (_cspan_static_assert(cspan_rank(self) == 3), \
                                      _cspan_assert((i) < (self)->shape[0] && (j) < (self)->shape[1] && \
                                                    (k) < (self)->shape[2]), \
                                      (i)*(self)->stride.d[0] + (j)*(self)->stride.d[1] + \
                                      (k)*(self)->stride.d[2])
#define _cspan_index_nd(self, ...) _cspan_index((self)->shape, (self)->stride.d, \
                                                _cspan_make_array(isize_t, {__VA_ARGS__}), \
                                                (_cspan_static_assert(cspan_rank(self) == \
                                                                      _CSPAN_NUMARGS(__VA_ARGS__)), \
                                                 cspan_rank(self)))


// Multi-dimensional span constructors
//
typedef enum {c_ROWMAJOR, c_COLMAJOR, c_STRIDED} cspan_layout;

#define cspan_is_colmajor(self) \
    _cspan_is_layout(c_COLMAJOR, (self)->shape, (self)->stride.d, cspan_rank(self))
#define cspan_is_rowmajor(self) \
    _cspan_is_layout(c_ROWMAJOR, (self)->shape, (self)->stride.d, cspan_rank(self))
#define cspan_get_layout(self) \
    (cspan_is_rowmajor(self) ? c_ROWMAJOR : cspan_is_colmajor(self) ? c_COLMAJOR : c_STRIDED)

#define cspan_md(dataptr, ...) \
    cspan_md_layout(c_ROWMAJOR, dataptr, __VA_ARGS__)

// Span2 sp1 = cspan_md(data, 30, 50);
// Span2 sp2 = {data, cspan_shape(15, 25), cspan_strides(50*2, 2)}; // every second in each dim
#define cspan_shape(...) {__VA_ARGS__}
#define cspan_strides(...) {.d={__VA_ARGS__}}

#define cspan_md_layout(layout, dataptr, ...) \
    {.data=dataptr, \
     .shape={__VA_ARGS__}, \
     .stride=*(_CSPAN_JOIN(cspan_tuple,_CSPAN_NUMARGS(__VA_ARGS__))*) \
             _cspan_shape2stride(layout, _cspan_make_array(_istride, {__VA_ARGS__}), \
                                 _CSPAN_NUMARGS(__VA_ARGS__))}

// Transpose matrix
#define cspan_transpose(self) \
    _cspan_transpose((self)->shape, (self)->stride.d, cspan_rank(self))

// Swap two matrix axes
#define cspan_swap_axes(self, ax1, ax2) \
    _cspan_swap_axes((self)->shape, (self)->stride.d, cspan_rank(self), ax1, ax2)

// Set all span elements to value.
#define cspan_set_all(Span, self, value) do { \
    Span##_value _v = value; \
    const Span* _self = (self); \
    for (Span##_iter _it = Span##_begin(_self); _it.ref; Span##_next(&_it)) \
        *_it.ref = _v; \
} while (0)

// General slicing function.
//
#define c_END (_istride)(((size_t)1 << (sizeof(_istride)*8 - 1)) - 1)
#define c_ALL 0, c_END

#define cspan_slice(self, Outspan, ...) \
    Outspan##_slice_((self)->data, (self)->shape, (self)->stride.d, \
                     _cspan_make_array2d(const isize_t, 3, {__VA_ARGS__}), \
                     (_cspan_static_assert(cspan_rank(self) == \
                                           _cspan_literal_count(_ituple3, {__VA_ARGS__})), \
                      cspan_rank(self)))

// submd#(): Reduces rank, fully typesafe + range checked by default
//           int ms3[N1][N2][N3];
//           int (*ms2)[N3] = ms3[1]; // traditional, lose range test/info. VLA.
//           Span3 ms3 = cspan_md(data, N1,N2,N3); // Uses cspan_md instead.
//           *cspan_at(&ms3, 1,1,1) = 42;
//           Span2 ms2 = cspan_slice(&ms3, Span2, {1}, {c_ALL}, {c_ALL});
//           Span2 ms2 = cspan_submd3(&ms3, 1); // Same as line above, optimized.
#define cspan_submd2(self, x) \
    {.data=cspan_at(self, x, 0), \
     .shape={(self)->shape[1]}, \
     .stride=_cspan_literal(cspan_tuple1){.d={(self)->stride.d[1]}}}

#define cspan_submd3(...) _CSPAN_OVERLOAD(_cspan_submd3, __VA_ARGS__)
#define _cspan_submd3_2(self, x) \
    {.data=cspan_at(self, x, 0, 0), \
     .shape={(self)->shape[1], (self)->shape[2]}, \
     .stride=_cspan_literal(cspan_tuple2){.d={(self)->stride.d[1], (self)->stride.d[2]}}}
#define _cspan_submd3_3(self, x, y) \
    {.data=cspan_at(self, x, y, 0), \
     .shape={(self)->shape[2]}, \
     .stride=_cspan_literal(cspan_tuple1){.d={(self)->stride.d[2]}}}

#define cspan_submd4(...) _CSPAN_OVERLOAD(_cspan_submd4, __VA_ARGS__)
#define _cspan_submd4_2(self, x) \
    {.data=cspan_at(self, x, 0, 0, 0), \
     .shape={(self)->shape[1], (self)->shape[2], (self)->shape[3]}, \
     .stride=_cspan_literal(cspan_tuple3){.d={(self)->stride.d[1], (self)->stride.d[2], \
                                               (self)->stride.d[3]}}}
#define _cspan_submd4_3(self, x, y) \
    {.data=cspan_at(self, x, y, 0, 0), \
     .shape={(self)->shape[2], (self)->shape[3]}, \
     .stride=_cspan_literal(cspan_tuple2){.d={(self)->stride.d[2], (self)->stride.d[3]}}}
#define _cspan_submd4_4(self, x, y, z) \
    {.data=cspan_at(self, x, y, z, 0), \
     .shape={(self)->shape[3]}, \
     .stride=_cspan_literal(cspan_tuple1){.d={(self)->stride.d[3]}}}

#define cspan_print(...) _CSPAN_OVERLOAD(_cspan_print, __VA_ARGS__)
// Backward-compatible names for the internal helpers.
#define cspan_print_3 _cspan_print_3
#define cspan_print_4 _cspan_print_4
#define cspan_print_5 _cspan_print_5
#define cspan_print_6 _cspan_print_6
#define cspan_index_1d _cspan_index_1d
#define cspan_index_2d _cspan_index_2d
#define cspan_index_3d _cspan_index_3d
#define cspan_index_nd _cspan_index_nd
#define cspan_index_I _cspan_index_fn
#define cspan_CMPLX_FLD _CSPAN_CMPLX_FLD
#define use_cspan_2 _cspan_use_2
#define use_cspan_3 _cspan_use_3
#define use_cspan_with_eq_3 _cspan_use_with_eq_3
#define use_cspan_with_eq_4 _cspan_use_with_eq_4
#define cspan_submd3_2 _cspan_submd3_2
#define cspan_submd3_3 _cspan_submd3_3
#define cspan_submd4_2 _cspan_submd4_2
#define cspan_submd4_3 _cspan_submd4_3
#define cspan_submd4_4 _cspan_submd4_4

#define _cspan_print_3(Span, fmt, span) \
    _cspan_print_4(Span, fmt, span, stdout)
#define _cspan_print_4(Span, fmt, span, fp) \
    _cspan_print_5(Span, fmt, span, fp, "[]")
#define _cspan_print_5(Span, fmt, span, fp, brackets) \
    _cspan_print_6(Span, fmt, span, fp, brackets, _CSPAN_EXPAND)
#define cspan_print_complex(Span, prec, span, fp) \
    _cspan_print_6(Span, "%." #prec "f%+." #prec "fi", span, fp, "[]", _CSPAN_CMPLX_FLD)
#define _CSPAN_CMPLX_FLD(x) creal(x), cimag(x)

#define _cspan_print_6(Span, fmt, span, fp, brackets, field) do { \
    const Span _s = span; \
    const char *_f = fmt, *_b = brackets; \
    FILE* _fp = fp; \
    int _w, _max = 0; \
    char _res[2][20], _fld[64]; \
    for (Span##_iter _it = Span##_begin(&_s); _it.ref; Span##_next(&_it)) { \
        _w = snprintf(NULL, 0ULL, _f, field(_it.ref[0])); \
        if (_w > _max) _max = _w; \
    } \
    for (Span##_iter _it = Span##_begin(&_s); _it.ref; Span##_next(&_it)) { \
        _cspan_print_assist(_it.pos, _s.shape, cspan_rank(&_s), _b, _res); \
        _w = _max + (_it.pos[cspan_rank(&_s) - 1] > 0); \
        snprintf(_fld, sizeof _fld, _f, field(_it.ref[0])); \
        fprintf(_fp, "%s%*s%s", _res[0], _w, _fld, _res[1]); \
    } \
} while (0)

/* ----- PRIVATE ----- */

_CSPAN_INLINE isize_t _cspan_size(const _istride shape[], int rank) {
    isize_t size = shape[0];
    while (--rank) size *= shape[rank];
    return size;
}

_CSPAN_INLINE void _cspan_swap_axes(_istride shape[], _istride stride[],
                                    int rank, int ax1, int ax2) {
    (void)rank;
    _cspan_assert(cspan_uless(ax1, rank) & cspan_uless(ax2, rank));
    _cspan_swap(shape + ax1, shape + ax2);
    _cspan_swap(stride + ax1, stride + ax2);
}

_CSPAN_INLINE void _cspan_transpose(_istride shape[], _istride stride[], int rank) {
    for (int i = 0; i < --rank; ++i) {
        _cspan_swap(shape + i, shape + rank);
        _cspan_swap(stride + i, stride + rank);
    }
}

_CSPAN_INLINE isize_t _cspan_index(const _istride shape[], const _istride stride[],
                                   const isize_t args[], int rank) {
    isize_t off = 0;
    (void)shape;
    while (rank-- != 0) {
        _cspan_assert(args[rank] < shape[rank]);
        off += args[rank]*stride[rank];
    }
    return off;
}

_CSPAN_INLINE void _cspan_print_assist(_istride pos[], const _istride shape[], const int rank,
                                       const char* brackets, char result[2][20]);

_CSPAN_INLINE bool _cspan_nextN(_istride pos[], const _istride shape[], const _istride stride[],
                                int rank, isize_t* off);
#define _cspan_next1(pos, shape, stride, rank, off)            (++pos[0] == shape[0])
#define _cspan_next2(pos, shape, stride, rank, off)            (++pos[1] == shape[1] && \
    (pos[1] = 0, *off += stride[0] - (isize_t)shape[1]*stride[1], ++pos[0] == shape[0]))
#define _cspan_next3(pos, shape, stride, rank, off)            (++pos[2] == shape[2] && \
    (pos[2] = 0, *off += stride[1] - (isize_t)shape[2]*stride[2], ++pos[1] == shape[1]) && \
    (pos[1] = 0, *off += stride[0] - (isize_t)shape[1]*stride[1], ++pos[0] == shape[0]))
#define _cspan_next4 _cspan_nextN
#define _cspan_next5 _cspan_nextN
#define _cspan_next6 _cspan_nextN
#define _cspan_next7 _cspan_nextN
#define _cspan_next8 _cspan_nextN

_CSPAN_INLINE isize_t _cspan_slice(_istride oshape[], _istride ostride[], int* orank,
                                   const _istride shape[], const _istride stride[],
                                   const isize_t args[][3], int rank);
_CSPAN_INLINE _istride* _cspan_shape2stride(cspan_layout layout, _istride shape[], int rank);
_CSPAN_INLINE bool _cspan_is_layout(cspan_layout layout, const _istride shape[],
                                    const _istride strides[], int rank);

/* --------------------- IMPLEMENTATION --------------------- */

_CSPAN_INLINE bool _cspan_is_layout(cspan_layout layout, const _istride shape[],
                                    const _istride strides[], int rank) {
    _istride tmpshape[16]; // 16 = "max" rank
    size_t sz = (size_t)rank*sizeof(_istride);
    memcpy(tmpshape, shape, sz);
    return memcmp(strides, _cspan_shape2stride(layout, tmpshape, rank), sz) == 0;
}

_CSPAN_INLINE void _cspan_print_assist(_istride pos[], const _istride shape[], const int rank,
                                       const char* brackets, char result[2][20]) {
    int n = 0, j = 0, r = rank - 1;
    memset(result, 0, 32);

    // left braces:
    while (n <= r && pos[r - n] == 0)
        ++n;
    if (n) for (; j < rank; ++j)
        result[0][j] = j < rank - n ? ' ' : brackets[0];

    // right braces:
    for (j = 0; r >= 0 && pos[r] + 1 == shape[r]; --r, ++j)
        result[1][j] = brackets[1];

    // comma and newlines:
    n = (j > 0) + ((j > 1) & (j < rank));
    if (brackets[2] && j < rank)
        result[1][j++] = brackets[2]; // comma
    while (n--)
        result[1][j++] = '\n';
}

_CSPAN_INLINE bool _cspan_nextN(_istride pos[], const _istride shape[], const _istride stride[],
                                int rank, isize_t* off) {
    ++pos[--rank];
    for (; rank && pos[rank] == shape[rank]; --rank) {
        pos[rank] = 0; ++pos[rank - 1];
        *off += stride[rank - 1] - (isize_t)shape[rank]*stride[rank];
    }
    return pos[rank] == shape[rank];
}

_CSPAN_INLINE _istride* _cspan_shape2stride(cspan_layout layout, _istride shpstri[], int rank) {
    int i, inc;
    if (layout == c_COLMAJOR) i = 0, inc = 1;
    else i = rank - 1, inc = -1;
    _istride k = 1, s1 = shpstri[i], s2;

    shpstri[i] = 1;
    while (--rank) {
        i += inc;
        s2 = shpstri[i];
        shpstri[i] = (k *= s1);
        s1 = s2;
    }
    return shpstri;
}

_CSPAN_INLINE isize_t _cspan_slice(_istride oshape[], _istride ostride[], int* orank,
                                   const _istride shape[], const _istride stride[],
                                   const isize_t args[][3], int rank) {
    isize_t end, off = 0;
    int i = 0, oi = 0;

    for (; i < rank; ++i) {
        off += args[i][0]*stride[i];
        switch (args[i][1]) {
            case 0: _cspan_assert(cspan_uless(args[i][0], shape[i])); continue;
            case c_END: end = shape[i]; break;
            default: end = args[i][1];
        }
        oshape[oi] = (_istride)(end - args[i][0]);
        ostride[oi] = stride[i];
        _cspan_assert((oshape[oi] > 0) & !cspan_uless(shape[i], end));
        if (args[i][2] > 0) {
            ostride[oi] *= (_istride)args[i][2];
            oshape[oi] = (oshape[oi] - 1)/(_istride)args[i][2] + 1;
        }
        ++oi;
    }
    *orank = oi;
    return off;
}

#endif // STC_CSPAN_H_INCLUDED
