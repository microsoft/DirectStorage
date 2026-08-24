/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 *
 * Advanced Technology Group (ATG)
 * Author(s):   Pavel Martishevsky (pamartis@microsoft.com)
 *
 * zstdgpu_srt_tool.c
 *
 * Code generator for the declarative SRT system. Includes zstdgpu_srt_tool.h
 *
 * Build (MSVC):
 *      cl.exe /nologo /Zc:preprocessor /W4 /WX zstdgpu_srt_tool.c /Fe:zstdgpu_srt_tool.exe
 *
 * Run:
 *      zstdgpu_srt_tool.exe <outputDirectory>
 *
 * /Zc:preprocessor is required: the conformant preprocessor is needed for stringification of macro
 * parameters that share a name with a struct member.
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include <assert.h>
#include <sal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

#define MAX_GROUPS      32
#define MAX_SRTS        64
#define MAX_PASSES      256
#define MAX_ENTRIES     64
#define MAX_CONSTS      32
#define MAX_RESOURCES   256
#define MAX_ROOT_DWORDS 64
#define STAGE_COUNT     3

enum
{
    kAccessRO = 0,
    kAccessRW = 1,
    kAccessRNW = 2
};
enum
{
    kKindStruct = 0,
    kKindTyped = 1,
    kKindByte = 2
};
enum
{
    kConstCpu = 0,
    kConstIndirect = 1,
    kConstInline = 2
};
enum
{
    Direct = 0,
    Indirect = 1
};
enum
{
    Stage0 = 1 << 0,
    Stage1 = 1 << 1,
    Stage2 = 1 << 2
};

/**
 *  Error and warning reporting
 */
static int  g_errorCount = 0;

static void fail(_Printf_format_string_ const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "[zstdgpu_srt_tool] [FAIL] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    g_errorCount += 1;
}

static void warn(_Printf_format_string_ const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "[zstdgpu_srt_tool] [WARN] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

/**
 *  String interning via stb_ds.h::shmap
 */
typedef uint16_t    NameId;

static const NameId kNameIdFull = 0xffff;
static const NameId kNameIdError = 0;
static const NameId kNameIdEmpty = 1;

typedef struct CStrToNameId
{
    char  *key; /**< owned by the map's string arena */
    size_t len;
    NameId idx;
} CStrToNameId;

static CStrToNameId *gCStrToNameId = NULL;

#ifdef _MSC_VER
__pragma(warning(push))
__pragma(warning(disable : 4090))
#endif

static NameId cstrIntern(const char *cstr)
{
    CStrToNameId *cstr2name = NULL;
    CStrToNameId  cstr2nameS;

    if (NULL == gCStrToNameId)
    {
        sh_new_arena(gCStrToNameId);

        cstr2nameS.key = "(error)";
        cstr2nameS.len = strlen(cstr2nameS.key);
        cstr2nameS.idx = kNameIdError;
        shputs(gCStrToNameId, cstr2nameS);

        cstr2nameS.key = "";
        cstr2nameS.len = strlen(cstr2nameS.key);
        cstr2nameS.idx = kNameIdEmpty;
        shputs(gCStrToNameId, cstr2nameS);
    }

    cstr2name = shgetp_null(gCStrToNameId, cstr);
    if (NULL == cstr2name)
    {
        NameId name = (NameId)shlenu(gCStrToNameId);
        if (name == kNameIdFull)
        {
            fail("`gCStrToNameId` is full. Increase `NameId` bit width");
            return kNameIdError;
        }
        cstr2nameS.key = cstr;
        cstr2nameS.len = strlen(cstr2nameS.key);
        cstr2nameS.idx = name;
        shputs(gCStrToNameId, cstr2nameS);
        return name;
    }
    else
    {
        assert(cstr2name->key == gCStrToNameId[cstr2name->idx].key && "Pointers are supposed to be identical");
        return cstr2name->idx;
    }
}
#ifdef _MSC_VER
__pragma(warning(pop))
#endif

static const char *nameToCStr(NameId id)
{
    assert(id == gCStrToNameId[id].idx && "Validation is supposed to hold");
    return gCStrToNameId[id].key;
}

static size_t nameToCStrLen(NameId id)
{
    assert(id == gCStrToNameId[id].idx && "Validation is supposed to hold");
    return gCStrToNameId[id].len;
}

typedef struct Entry
{
    uint16_t access : 2;
    uint16_t kind   : 2;
    uint16_t glc    : 1;
    uint16_t mod    : 1;
    uint16_t reg    : 10;

    NameId   hlslType;
    NameId   dataType;

    /** The name of the resource that must be bound to this bind entry, also bind entry name */
    NameId name;

    /** The alias suffix used to distinguish bind entries populated from the same resource. If this is other than "" (empty string)
     *  it means this bind entry is going to be named as `name_asfx` in the shader but the resource that is going to be bound is still `name
     */
    NameId asfx;

    NameId macroText;
    NameId memberText;
    NameId globalText;
} Entry;

typedef struct Group
{
    NameId   name;
    uint16_t space;
    Entry    entries[MAX_ENTRIES];
    uint16_t entryCount;
    uint16_t srvCount;
    uint16_t uavCount;

    /** Bit N set when this descriptor table needs a copy in stage N.
     *  It's currently for bind group matching D3D12 descriptor table versioning:
     *      - a new stage re-creates D3D12 descriptors from D3D12 resources.
     *      - so whenever resource change (e.g. re-created or initialised for the first time) -- a new D3D12 descriptor table is created
     */
    uint32_t stageMask;
} Group;

typedef struct Const
{
    int    kind;
    NameId type;
    NameId name;
} Const;

typedef struct Srt
{
    NameId name;

    int    groupIdx[MAX_ENTRIES];
    int    groupCount;

    Entry  rootBufs[MAX_ENTRIES];
    int    rootBufCount;

    Const  consts[MAX_CONSTS]; /**< every constant */
    int    constCount;

    Const  boundConsts[MAX_CONSTS]; /**< the non-CONST_INLINE subset, declaration order.*/
    int    boundConstCount;

    Const  inlineConsts[MAX_CONSTS]; /**< the CONST_INLINE subset, declaration order */
    int    inlineConstCount;

    int    stageMask; /**< union of the stage masks of this SRT's bind groups; 0 when it uses none, which means one unsuffixed binder */

    int    cpuConstCount; /**< only read to enforce INDIRECT-before-CPU declaration order */
    int    indirect;      /**< dispatch kind declared on ZSTDGPU_SRT_BEGIN */
} Srt;

typedef struct Pass
{
    int    srtIdx;
    int    indirect;
    NameId name; /**< kNameIdError for the implicit default pass -- it has no name */
    NameId resources[MAX_ENTRIES];
} Pass;

static Group  g_groups[MAX_GROUPS];
static int    g_groupCount = 0;

static Srt    g_srts[MAX_SRTS];
static int    g_srtCount = 0;

static Pass   g_passes[MAX_PASSES];
static int    g_passCount = 0;

static NameId g_resources[MAX_RESOURCES];
static int    g_resourceCount = 0;

static int    g_currentGroup = -1;
static int    g_currentSrt = -1;
static int    g_currentPass = -1;

/**
 * A wrapper on top of stb_ds.h array to not misinterpret `char *` as raw C-string.
 * The invariant is that data is ither NULL or points to '\0' terminated C-string.
 */
typedef struct StrBuilder
{
    char *data;
} StrBuilder;

/** Content length, excluding the terminator. */
static size_t sb_Len(const StrBuilder *sb)
{
    const size_t used = arrlenu(sb->data);
    return (used > 0) ? used - 1 : 0;
}

static void sb_Reset(StrBuilder *sb)
{
    sb->data[0] = '\0';
    arrsetlen(sb->data, 1);
}

/** Appends an end-of-line to a string literal at compile time. */
#define StrLitEoL(text) text "\n"

static StrBuilder *sb_AppendCStr(StrBuilder *sb, const char *text, size_t len)
{
    const size_t used = sb_Len(sb);
    arrsetlen(sb->data, used + len + 1);
    memcpy(&sb->data[used], text, len);
    sb->data[used + len] = '\0';
    return sb;
}

/** Appends a string literal. The sizeof() rejects anything that is not a literal. */
#define sb_StrLit(sb, lit)    sb_AppendCStr(sb, lit, sizeof(lit) - 1)
#define sb_StrLitEoL(sb, lit) sb_AppendCStr(sb, lit "\n", sizeof(lit))
#define sb_ExtraLine(sb)      sb_AppendCStr(sb, "\n", sizeof("\n") - 1)

static StrBuilder *sb_Str(StrBuilder *sb, const char *text)
{
    return sb_AppendCStr(sb, text, strlen(text));
}

static StrBuilder *sb_Fmt(StrBuilder *sb, _Printf_format_string_ const char *fmt, ...)
{
    va_list args0, args1;
    int     len;

    va_start(args0, fmt);
    va_copy(args1, args0);
    len = vsnprintf(NULL, 0, fmt, args1);
    va_end(args1);

    if (len < 0)
    {
        fail("sb_Fmt: cannot format '%s'", fmt);
    }
    else
    {
        size_t used = sb_Len(sb);
        size_t need = (size_t)len + 1;
        arrsetlen(sb->data, used + need);
        vsnprintf(&sb->data[used], need, fmt, args0);
    }
    va_end(args0);
    return sb;
}

static const char *sb_CStr(StrBuilder *sb)
{
    return (NULL != sb->data) ? sb->data : nameToCStr(kNameIdEmpty);
}

static void sb_BeginFile(StrBuilder *sb, const char *guard)
{
    sb_StrLitEoL(sb,
        "/**\n"
        " * Copyright (c) Microsoft. All rights reserved.\n"
        " * This code is licensed under the MIT License (MIT).\n"
        " * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF\n"
        " * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY\n"
        " * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR\n"
        " * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.\n"
        " *\n"
        " * Advanced Technology Group (ATG)\n"
        " *\n"
        " * AUTO-GENERATED by zstdgpu_srt_tool.exe. DO NOT EDIT.\n"
        " * Source of truth: zstdgpu_srt_decl.h\n"
        " */\n");
    sb_Fmt(sb,
        "#ifndef %s\n"
        "#define %s\n"
        "\n",
        guard, guard);
}

static void sb_EndFile(StrBuilder *sb, const char *guard, const char *path)
{
    size_t len;
    FILE  *f;

    sb_Fmt(sb, "#endif /* %s */\n", guard);

    len = sb_Len(sb);

    f = fopen(path, "wb");
    if (NULL == f)
    {
        fail("cannot open '%s' for writing", path);
        return;
    }
    if (len != fwrite(sb->data, 1, len, f))
    {
        fail("failed to write '%s'", path);
    }
    fclose(f);

    arrfree(sb->data);
    sb->data = NULL;
}

static void registerResource(NameId name)
{
    int i;
    for (i = 0; i < g_resourceCount; ++i)
    {
        if (g_resources[i] == name)
        {
            return;
        }
    }
    if (g_resourceCount >= MAX_RESOURCES)
    {
        fail("too many resources at '%s'", nameToCStr(name));
        return;
    }
    g_resources[g_resourceCount] = name;
    g_resourceCount += 1;
}

static int findGroup(NameId name)
{
    int i;
    for (i = 0; i < g_groupCount; ++i)
    {
        if (g_groups[i].name == name)
        {
            return i;
        }
    }
    return -1;
}

static void groupBegin(const char *name, int stageMask)
{
    if (g_groupCount >= MAX_GROUPS)
    {
        fail("too many bind groups at '%s'", name);
        return;
    }
    if (0 == stageMask || 0 != (stageMask & ~(Stage0 | Stage1 | Stage2)))
    {
        fail("bind group '%s': stages must be Stage0, Stage1 or Stage2 combined with '|'", name);
        stageMask = Stage0; /* keep going, so the rest of the declaration still parses cleanly */
    }
    g_currentGroup = g_groupCount++;
    memset(&g_groups[g_currentGroup], 0, sizeof(Group));
    g_groups[g_currentGroup].name = cstrIntern(name);
    g_groups[g_currentGroup].space = (uint16_t)g_currentGroup + 1; /* space0 is reserved for root parameters */
    g_groups[g_currentGroup].stageMask = stageMask;
}

static void groupEnd(void)
{
    g_currentGroup = -1;
}

static int checkDispatch(const char *what, const char *name, int dispatch)
{
    if (Direct != dispatch && Indirect != dispatch)
    {
        fail("%s '%s': dispatch must be Direct or Indirect", what, name);
        return Direct;
    }
    return dispatch;
}

static void srtBegin(const char *name, int dispatch)
{
    if (g_srtCount >= MAX_SRTS)
    {
        fail("too many SRTs at '%s'", name);
        return;
    }
    g_currentSrt = g_srtCount++;
    memset(&g_srts[g_currentSrt], 0, sizeof(Srt));
    g_srts[g_currentSrt].name = cstrIntern(name);
    g_srts[g_currentSrt].indirect = checkDispatch("SRT", name, dispatch);
}

static void srtEnd(void)
{
    g_currentSrt = -1;
}

static void srtUseGroup(const char *name)
{
    Srt *srt;
    int  idx = findGroup(cstrIntern(name));

    if (g_currentSrt < 0)
    {
        fail("ZSTDGPU_SRT_USE_BIND_GROUP(%s) outside of an SRT", name);
        return;
    }
    if (idx < 0)
    {
        fail("SRT '%s' references undefined bind group '%s'", nameToCStr(g_srts[g_currentSrt].name), name);
        return;
    }
    srt = &g_srts[g_currentSrt];
    if (srt->groupCount >= MAX_ENTRIES)
    {
        fail("SRT '%s': too many bind groups at '%s'", nameToCStr(srt->name), name);
        return;
    }
    srt->groupIdx[srt->groupCount++] = idx;
    srt->stageMask |= g_groups[idx].stageMask;
}

static const char *const kKindWord[] = { "BUFFER", "TYPED_BUFFER", "RAW_BUFFER" };

static StrBuilder        gScratch = { NULL };

/**
 * Expands `Entry` into the following macro:
 *      ZSTDGPU_{RO|RW}_{|TYPED_|RAW_}BUFFER[_GLC](hlslType[, dataType])
 */
static NameId emitSrtBindEntryType(const Entry *e)
{
    NameId name;
    sb_Fmt(&gScratch, "ZSTDGPU_%s_%s", (kAccessRO == e->access) ? "RO" : "RW", kKindWord[e->kind]);
    if (0 != e->glc)
    {
        sb_StrLit(&gScratch, "_GLC");
    }

    sb_Fmt(&gScratch, "(%s", nameToCStr(e->hlslType));
    if (kKindTyped == e->kind)
    {
        sb_Fmt(&gScratch, ", %s", nameToCStr(e->dataType));
    }
    sb_StrLit(&gScratch, ")");

    name = cstrIntern(gScratch.data);
    sb_Reset(&gScratch);
    return name;
}

static NameId emitSrtBindEntryName(const Entry *e, const char *roNamePrefix, const char *rwNamePrefix)
{
    NameId name;
    sb_Str(&gScratch, (kAccessRO == e->access) ? roNamePrefix : rwNamePrefix);
    sb_Str(&gScratch, nameToCStr(e->name));
    if (kNameIdEmpty != e->asfx)
    {
        sb_StrLit(&gScratch, "_");
        sb_Str(&gScratch, nameToCStr(e->asfx));
    }
    name = cstrIntern(gScratch.data);
    sb_Reset(&gScratch);
    return name;
}

static void addBuf(uint16_t access, uint16_t kind, uint16_t glc, const char *hlslType, const char *dataType, const char *name, const char *aliasPostfix)
{
    Entry       *e = NULL;
    const NameId nameId = cstrIntern(name);

    if (g_currentGroup >= 0)
    {
        Group *g = &g_groups[g_currentGroup];
        if (kAccessRO == access && g->uavCount > 0)
        {
            fail("bind group '%s': read-only entry '%s' declared after a read-write entry (a descriptor range requires all SRVs before all UAVs)", nameToCStr(g->name), name);
        }
        if (g->entryCount >= MAX_ENTRIES)
        {
            fail("bind group '%s': too many entries at '%s'", nameToCStr(g->name), name);
            return;
        }
        e = &g->entries[g->entryCount++];
        memset(e, 0, sizeof(Entry));
        e->reg = (kAccessRO == access) ? g->srvCount++ : g->uavCount++;

        /* a group entry names the resource directly */
        registerResource(nameId);
    }
    else if (g_currentSrt >= 0)
    {
        /* a root descriptor is a slot -- the resource it points at is named by each pass */
        Srt *srt = &g_srts[g_currentSrt];
        int  i;
        int  count = 0;

        if (kKindTyped == kind)
        {
            fail("SRT '%s': typed buffer '%s' cannot be a root descriptor -- D3D12 allows only raw/byte buffers or structured buffers there, so put it in a bind group", nameToCStr(srt->name), name);
        }
        for (i = 0; i < srt->rootBufCount; ++i)
        {
            if ((kAccessRO == access) == (kAccessRO == srt->rootBufs[i].access))
            {
                count += 1;
            }
        }

        if (srt->rootBufCount >= MAX_ENTRIES)
        {
            fail("SRT '%s': too many root descriptors at '%s'", nameToCStr(srt->name), name);
            return;
        }
        e = &srt->rootBufs[srt->rootBufCount++];
        memset(e, 0, sizeof(Entry));
        e->reg = (uint16_t)count;
    }
    else
    {
        fail("buffer entry '%s' declared outside of a bind group or SRT", name);
        return;
    }

    e->access = access;
    e->kind = kind;
    e->glc = glc;
    e->name = nameId;
    e->asfx = cstrIntern(aliasPostfix);
    e->hlslType = cstrIntern(hlslType);
    e->dataType = cstrIntern(dataType);

    e->macroText = emitSrtBindEntryType(e);
    e->memberText = emitSrtBindEntryName(e, "in", "inout");
    e->globalText = emitSrtBindEntryName(e, "ZstdIn", "ZstdInOut");
}

static void addConst(int kind, const char *type, const char *name)
{
    Srt *srt;

    if (g_currentSrt < 0)
    {
        fail("constant '%s' declared outside of an SRT", name);
        return;
    }
    srt = &g_srts[g_currentSrt];
    if (srt->constCount >= MAX_CONSTS)
    {
        fail("SRT '%s': too many constants at '%s'", nameToCStr(srt->name), name);
        return;
    }
    if (kConstIndirect == kind && srt->cpuConstCount > 0)
    {
        fail("SRT '%s': ZSTDGPU_SRT_CONST_INDIRECT('%s') must be declared before any ZSTDGPU_SRT_CONST - the command signature injects the leading constants", nameToCStr(srt->name), name);
    }
    srt->consts[srt->constCount].kind = kind;
    srt->consts[srt->constCount].type = cstrIntern(type);
    srt->consts[srt->constCount].name = cstrIntern(name);

    /** split constants */
    if (kConstInline == kind)
    {
        srt->inlineConsts[srt->inlineConstCount++] = srt->consts[srt->constCount];
    }
    else
    {
        srt->boundConsts[srt->boundConstCount++] = srt->consts[srt->constCount];
        srt->cpuConstCount += (kConstCpu == kind);
    }
    srt->constCount += 1;
}

static void passBegin(const char *kernel, const char *name, int dispatch)
{
    const NameId kernelId = cstrIntern(kernel);
    int          i;
    int          srtIdx = -1;

    for (i = 0; i < g_srtCount; ++i)
    {
        if (g_srts[i].name == kernelId)
        {
            srtIdx = i;
            break;
        }
    }
    if (srtIdx < 0)
    {
        fail("pass '%s' references undefined SRT '%s'", name, kernel);
        return;
    }
    if (g_passCount >= MAX_PASSES)
    {
        fail("too many passes at '%s'", name);
        return;
    }
    g_currentPass = g_passCount++;
    memset(&g_passes[g_currentPass], 0, sizeof(Pass));
    g_passes[g_currentPass].name = cstrIntern(name);
    g_passes[g_currentPass].srtIdx = srtIdx;
    g_passes[g_currentPass].indirect = checkDispatch("pass", name, dispatch);
}

static void passEnd(void)
{
    if (g_currentPass >= 0)
    {
        Pass *pass = &g_passes[g_currentPass];
        Srt  *srt = &g_srts[pass->srtIdx];
        int   i;

        for (i = 0; i < srt->rootBufCount; ++i)
        {
            if (kNameIdError == pass->resources[i])
            {
                fail("pass '%s' does not bind '%s' of '%s'", nameToCStr(pass->name), nameToCStr(srt->rootBufs[i].name), nameToCStr(srt->name));
            }
        }
    }
    g_currentPass = -1;
}

static void passBind(const char *slot, const char *resource)
{
    Pass  *pass;
    Srt   *srt;
    NameId slotId;
    NameId resourceId;
    int    i;

    if (g_currentPass < 0)
    {
        fail("ZSTDGPU_SRT_BIND(%s) outside of a pass", slot);
        return;
    }
    pass = &g_passes[g_currentPass];
    srt = &g_srts[pass->srtIdx];
    slotId = cstrIntern(slot);
    resourceId = cstrIntern(resource);

    for (i = 0; i < srt->rootBufCount; ++i)
    {
        if (srt->rootBufs[i].name == slotId)
        {
            srt->rootBufs[i].mod = 1;
            break;
        }
    }
    if (i == srt->rootBufCount)
    {
        fail("pass '%s' binds '%s' which is not a root descriptor of its SRT (bind group entries are bound by the descriptor table, not per pass)", nameToCStr(pass->name), slot);
        return;
    }
    if (kNameIdError != pass->resources[i])
    {
        fail("pass '%s' binds '%s' more than once", nameToCStr(pass->name), slot);
        return;
    }
    pass->resources[i] = resourceId;

    registerResource(resourceId);
}

/**
 *  Declaration expansion macros
 */
#define ZSTDGPU_SRT_BIND_GROUP_BEGIN(name, stages)            groupBegin(#name, (stages));
#define ZSTDGPU_SRT_BIND_GROUP_END()                          groupEnd();

#define ZSTDGPU_SRT_BEGIN(name, disp)                         srtBegin(#name, (disp));
#define ZSTDGPU_SRT_END()                                     srtEnd();
#define ZSTDGPU_SRT_USE_BIND_GROUP(name)                      srtUseGroup(#name);

#define ZSTDGPU_SRT_PASS_BEGIN(srt, pass, disp)               passBegin(#srt, #pass, (disp));
#define ZSTDGPU_SRT_PASS_END()                                passEnd();

#define ZSTDGPU_SRT_BUF_RO_STRUCT(type, name)                 addBuf(kAccessRO, kKindStruct, 0, #type, #type, #name, "");
#define ZSTDGPU_SRT_BUF_RNW_STRUCT(type, name)                addBuf(kAccessRNW, kKindStruct, 0, #type, #type, #name, "");
#define ZSTDGPU_SRT_BUF_RW_STRUCT(type, name)                 addBuf(kAccessRW, kKindStruct, 0, #type, #type, #name, "");
#define ZSTDGPU_SRT_BUF_RWGLC_STRUCT(type, name)              addBuf(kAccessRW, kKindStruct, 1, #type, #type, #name, "");

#define ZSTDGPU_SRT_BUF_RO_TYPED(hlslType, dataType, name)    addBuf(kAccessRO, kKindTyped, 0, #hlslType, #dataType, #name, "");
#define ZSTDGPU_SRT_BUF_RNW_TYPED(hlslType, dataType, name)   addBuf(kAccessRNW, kKindTyped, 0, #hlslType, #dataType, #name, "");
#define ZSTDGPU_SRT_BUF_RW_TYPED(hlslType, dataType, name)    addBuf(kAccessRW, kKindTyped, 0, #hlslType, #dataType, #name, "");
#define ZSTDGPU_SRT_BUF_RWGLC_TYPED(hlslType, dataType, name) addBuf(kAccessRW, kKindTyped, 1, #hlslType, #dataType, #name, "");

#define ZSTDGPU_SRT_BUF_RO_BYTE(name)                         addBuf(kAccessRO, kKindByte, 0, "uint32_t", "uint32_t", #name, "");
#define ZSTDGPU_SRT_BUF_RNW_BYTE(name)                        addBuf(kAccessRNW, kKindByte, 0, "uint32_t", "uint32_t", #name, "");
#define ZSTDGPU_SRT_BUF_RW_BYTE(name)                         addBuf(kAccessRW, kKindByte, 0, "uint32_t", "uint32_t", #name, "");
#define ZSTDGPU_SRT_BUF_RWGLC_BYTE(name)                      addBuf(kAccessRW, kKindByte, 1, "uint32_t", "uint32_t", #name, "");

#define ZSTDGPU_SRT_BUF_RO_STRUCT_ALIAS(type, name, postfix)  addBuf(kAccessRO, kKindStruct, 0, #type, #type, #name, #postfix);
#define ZSTDGPU_SRT_BUF_RW_STRUCT_ALIAS(type, name, postfix)  addBuf(kAccessRW, kKindStruct, 0, #type, #type, #name, #postfix);

#define ZSTDGPU_SRT_CONST(type, name)                         addConst(kConstCpu, #type, #name);
#define ZSTDGPU_SRT_CONST_INDIRECT(type, name)                addConst(kConstIndirect, #type, #name);
#define ZSTDGPU_SRT_CONST_INLINE(type, name)                  addConst(kConstInline, #type, #name);

#define ZSTDGPU_SRT_BIND(slot, resource)                      passBind(#slot, #resource);

static void collect(void)
{
#include "zstdgpu_srt_decl.h"
}

static int passMatchesDefault(const Pass *pass, const Srt *srt)
{
    int i;

    if (pass->indirect != srt->indirect)
        return 0;
    for (i = 0; i < srt->rootBufCount; ++i)
    {
        if (pass->resources[i] != srt->rootBufs[i].name)
            return 0;
    }
    return 1;
}

static void addDefaultPasses(void)
{
    int s;
    int i;

    for (s = 0; s < g_srtCount; ++s)
    {
        Srt *srt = &g_srts[s];
        int  explicitCount = 0;

        for (i = 0; i < g_passCount; ++i)
        {
            if (g_passes[i].srtIdx != s)
                continue;

            explicitCount += 1;
            if (passMatchesDefault(&g_passes[i], srt))
            {
                warn("SRT '%s': pass '%s' is identical to the implicit default pass. Remove it.", nameToCStr(srt->name), nameToCStr(g_passes[i].name));
            }
        }

        if (explicitCount > 0)
            continue;

        if (g_passCount >= MAX_PASSES)
        {
            fail("too many passes at default for '%s'", nameToCStr(srt->name));
            return;
        }

        {
            Pass *pass = &g_passes[g_passCount++];
            memset(pass, 0, sizeof(Pass));
            pass->name = kNameIdError; /* unnamed -- the binder carries no pass suffix */
            pass->srtIdx = s;
            pass->indirect = srt->indirect;
            for (i = 0; i < srt->rootBufCount; ++i)
            {
                pass->resources[i] = srt->rootBufs[i].name;
                registerResource(srt->rootBufs[i].name);
            }
        }
    }
}

static int srtConstsRootSlot(const Srt *srt)
{
    return srt->groupCount + srt->rootBufCount;
}

static void checkRootBudget(void)
{
    int s;
    for (s = 0; s < g_srtCount; ++s)
    {
        const Srt *srt = &g_srts[s];
        const int  rootDwords = srt->groupCount * 1 + srt->rootBufCount * 2 + srt->boundConstCount;

        if (rootDwords > MAX_ROOT_DWORDS)
        {
            fail("SRT '%s': root signature costs %d (limit %d) DWORDs", nameToCStr(srt->name), rootDwords, MAX_ROOT_DWORDS);
        }
    }
}

/**
 *  Code generation
 */
#define calculateEntriesMaxLen(sz, name, e, c) for (uint32_t j = 0, nlen = 0; j<(uint32_t)c; nlen = (uint32_t)nameToCStrLen(e[j].name), sz = sz> nlen ? sz : nlen, ++j)

static void emitHLSLBindPointNamesWithRegisters(StrBuilder *b, const Entry *e, int c, int space)
{
    uint32_t macroTextLen = 0, globalTextLen = 0;

    calculateEntriesMaxLen(macroTextLen, macroText, e, c);
    calculateEntriesMaxLen(globalTextLen, globalText, e, c);

    macroTextLen = (macroTextLen + 3 + 1) & ~3;
    globalTextLen = (globalTextLen + 3 + 1) & ~3;

    for (int i = 0; i < c; ++i)
    {
        sb_Fmt(b, "%-*s%-*s: register(%s%u", macroTextLen, nameToCStr(e[i].macroText), globalTextLen, nameToCStr(e[i].globalText), (kAccessRO == e[i].access) ? "t" : "u", e[i].reg);
        if (space > 0)
            sb_Fmt(b, ", space%d", space);
        sb_StrLitEoL(b, ");");
    }

    if (c > 0)
        sb_ExtraLine(b);
}

static void emitHLSLResourceAssignment(StrBuilder *b, const Entry *e, int c, const char *structName)
{
    uint32_t memberTextLen = 0;
    calculateEntriesMaxLen(memberTextLen, memberText, e, c);
    memberTextLen = (memberTextLen + 3 + 1) & ~3;

    if (NULL == structName)
    {
        for (int i = 0; i < c; ++i)
            sb_Fmt(b, "    srt.%-*s= %s;\n", memberTextLen, nameToCStr(e[i].memberText), nameToCStr(e[i].globalText));
    }
    else
    {
        for (int i = 0; i < c; ++i)
            sb_Fmt(b, "    srt.%-*s= %s.%s;\n", memberTextLen, nameToCStr(e[i].memberText), structName, nameToCStr(e[i].name));
    }
}

static void emitBindGroupHeader(const char *dir, const Group *g)
{
    StrBuilder  sb = { NULL };
    StrBuilder *b = &sb;
    StrBuilder  guard = { NULL };
    StrBuilder  path = { NULL };

    sb_Fmt(&guard, "ZSTDGPU_SRT_GENERATED_RS_BIND_GROUP_%s_H", nameToCStr(g->name));
    sb_Fmt(&path, "%s/ZstdGpuSrt_BindGroup_%s.h", dir, nameToCStr(g->name));

    sb_BeginFile(b, sb_CStr(&guard));

    /** the start of emitting HLSL-specific declaration */
    sb_StrLitEoL(b, "#ifdef __hlsl_dx_compiler");
    sb_ExtraLine(b);

    sb_Fmt(b, "#define ZSTDGPU_SRT_RS_BIND_GROUP_%s ", nameToCStr(g->name));
    sb_StrLit(b, "\"DescriptorTable(");
    if (g->srvCount > 0)
    {
        sb_Fmt(b, "SRV(t0, space=%d, numDescriptors=%d)", (int)g->space, (int)g->srvCount);
    }
    if (g->uavCount > 0)
    {
        if (g->srvCount > 0)
            sb_StrLit(b, ", ");

        sb_Fmt(b, "UAV(u0, space=%d, numDescriptors=%d)", (int)g->space, (int)g->uavCount);
    }
    sb_StrLit(b, ")\"");
    sb_StrLitEoL(b, "\n");

    emitHLSLBindPointNamesWithRegisters(b, g->entries, g->entryCount, g->space);

    /** emit a function (for HLSL use) that assigns bind group entries to SRT-derived structure from global bind points */
    sb_Fmt(b, "template<typename T>\nstatic void zstdgpu_Srt_FillBindGroup_%s(ZSTDGPU_PARAM_INOUT(T) srt)\n{\n", nameToCStr(g->name));
    emitHLSLResourceAssignment(b, g->entries, g->entryCount, NULL);
    sb_StrLitEoL(b, "}");
    sb_ExtraLine(b);
    sb_StrLitEoL(b, "#else");
    sb_ExtraLine(b);

    /** emit a function (for C++ use) that assigns bind group entries to SRT-derived structure from externally supplied structure */
    sb_Fmt(b, "template<typename T>\nstatic void zstdgpu_Srt_FillBindGroup_%s(T &srt, const zstdgpu_ResourceDataCpu &cpuRes)\n{\n", nameToCStr(g->name));
    emitHLSLResourceAssignment(b, g->entries, g->entryCount, "cpuRes");
    sb_StrLitEoL(b, "}");
    sb_ExtraLine(b);
    sb_StrLitEoL(b, "#endif /* #ifdef __hlsl_dx_compiler */");

    sb_EndFile(b, sb_CStr(&guard), sb_CStr(&path));

    arrfree(guard.data);
    arrfree(path.data);
}

/** 1 when the SRT declares explicit passes, so it is only ever used through one of them. */
static int srtIsMultiPass(int srtIdx)
{
    int i;

    for (i = 0; i < g_passCount; ++i)
    {
        if (g_passes[i].srtIdx == srtIdx && kNameIdError != g_passes[i].name)
        {
            return 1;
        }
    }
    return 0;
}

static void emitSrtHeader(const char *dir, int srtIdx)
{
    const Srt   *srt = &g_srts[srtIdx];
    StrBuilder   sb = { NULL };
    StrBuilder  *b = &sb;
    StrBuilder   guard = { NULL };
    StrBuilder   path = { NULL };
    int          i;
    int          resourcesUsed;
    const int    multiPass = srtIsMultiPass(srtIdx);
    uint32_t     maxTypeLen = 0, maxNameLen = 0;
    const Entry *points = srt->rootBufs;
    const Const *boundConsts = srt->boundConsts;
    NameId       funcName = kNameIdEmpty;

    /** count maximal spacing */
    for (i = 0; i < srt->rootBufCount; ++i)
    {
        uint32_t curLen = (uint32_t)nameToCStrLen(points[i].macroText);
        if (points[i].mod)
            maxTypeLen = maxTypeLen > curLen ? maxTypeLen : curLen;

        curLen = (uint32_t)nameToCStrLen(points[i].memberText);
        maxNameLen = maxNameLen > curLen ? maxNameLen : curLen;
    }
    for (i = 0; i < srt->boundConstCount; ++i)
    {
        uint32_t curLen = (uint32_t)nameToCStrLen(boundConsts[i].type);
        maxTypeLen = maxTypeLen > curLen ? maxTypeLen : curLen;

        curLen = (uint32_t)nameToCStrLen(boundConsts[i].name);
        maxNameLen = maxNameLen > curLen ? maxNameLen : curLen;
    }
    maxTypeLen = (maxTypeLen + 3 + 1) & ~3u;
    maxNameLen = (maxNameLen + 3 + 1) & ~3u;

    sb_Fmt(&guard, "ZSTDGPU_SRT_GENERATED_%s_H", nameToCStr(srt->name));
    sb_Fmt(&path, "%s/ZstdGpuSrt_%s.h", dir, nameToCStr(srt->name));

    sb_BeginFile(b, sb_CStr(&guard));

    for (i = 0; i < srt->groupCount; ++i)
    {
        sb_Fmt(b, "#include \"ZstdGpuSrt_BindGroup_%s.h\"\n", nameToCStr(g_groups[srt->groupIdx[i]].name));
    }
    if (srt->groupCount > 0)
    {
        sb_ExtraLine(b);
    }

    sb_StrLitEoL(b, "#ifdef __hlsl_dx_compiler\n");
    emitHLSLBindPointNamesWithRegisters(b, srt->rootBufs, srt->rootBufCount, -1);

    /** emit HLSL constants struct + buffer if there're non-inline constants */
    if (srt->boundConstCount > 0)
    {
        uint32_t maxLen = 0;
        sb_Fmt(b, "typedef struct zstdgpu_%s_Consts\n{\n", nameToCStr(srt->name));

        for (i = 0; i < srt->boundConstCount; ++i)
        {
            uint32_t len = (uint32_t)nameToCStrLen(boundConsts[i].type);
            maxLen = maxLen > len ? maxLen : len;
        }

        maxLen = (maxLen + 3 + 1) & ~3u;

        for (i = 0; i < srt->boundConstCount; ++i)
            sb_Fmt(b, "    %-*s%s;\n", maxLen, nameToCStr(boundConsts[i].type), nameToCStr(boundConsts[i].name));

        sb_Fmt(b, "} zstdgpu_%s_Consts;\n\n", nameToCStr(srt->name));
        sb_Fmt(b, "ConstantBuffer<zstdgpu_%s_Consts> ZstdConstants_%s : register(b0);\n\n", nameToCStr(srt->name), nameToCStr(srt->name));
    }

    /* the root signature is composed from the bind group fragments, so a group's text exists once */
    sb_Fmt(b, "#define ZSTDGPU_SRT_RS_%s", nameToCStr(srt->name));

    /** the root signature starts with a macro fragments expanding to resource tables, one per bind group*/
    for (i = 0; i < srt->groupCount; ++i)
    {
        if (i > 0)
            sb_StrLit(b, " \", \"");

        sb_Str(b, " ZSTDGPU_SRT_RS_BIND_GROUP_");
        sb_Str(b, nameToCStr(g_groups[srt->groupIdx[i]].name));
    }
    /** then string fragments for root descriptors, one fragment per root bind point */
    for (i = 0; i < srt->rootBufCount; ++i)
    {
        const Entry *e = &srt->rootBufs[i];

        sb_StrLit(b, " \"");
        if (srt->groupCount + i > 0)
            sb_StrLit(b, ", ");

        sb_Fmt(b, (kAccessRO == e->access) ? "SRV(t%d)\"" : "UAV(u%d)\"", e->reg);
    }
    /** and finally root constants blocks corresponding to bindConsts*/
    if (srt->boundConstCount > 0)
    {
        sb_StrLit(b, " \"");
        if (srt->groupCount + srt->rootBufCount > 0)
            sb_StrLit(b, ", ");
        sb_Fmt(b, "RootConstants(b0, num32BitConstants=%d)\"", srt->boundConstCount);
    }
    sb_StrLitEoL(b, "\n");

    /** emit HLSL function */
    sb_Fmt(b, "static void zstdgpu_Srt_Fill(ZSTDGPU_PARAM_INOUT(zstdgpu_%s_SRT) srt)\n{\n", nameToCStr(srt->name));
    for (i = 0; i < srt->groupCount; ++i)
    {
        sb_Fmt(b, "    zstdgpu_Srt_FillBindGroup_%s(srt);\n", nameToCStr(g_groups[srt->groupIdx[i]].name));
    }
    if (srt->groupCount > 0 && (srt->rootBufCount > 0 || srt->constCount > 0))
    {
        sb_ExtraLine(b);
    }

    /** emit HLSL side assignment of SRT-derived structure members corresponding to bind points */
    for (i = 0; i < srt->rootBufCount; ++i)
        sb_Fmt(b, "    srt.%-*s= %s;\n", maxNameLen, nameToCStr(points[i].memberText), nameToCStr(points[i].globalText));

    /** emit HLSL side assignment of SRT-derived structure members corresponding to constats */
    for (i = 0; i < srt->boundConstCount; ++i)
        sb_Fmt(b, "    srt.%-*s= ZstdConstants_%s.%s;\n", maxNameLen, nameToCStr(boundConsts[i].name), nameToCStr(srt->name), nameToCStr(boundConsts[i].name));

    sb_StrLitEoL(b, "}\n\n#else\n");

    resourcesUsed = (srt->groupCount > 0);
    for (i = 0; i < srt->rootBufCount; ++i)
    {
        /** if at least one bind point is never modified by passes (or SRT doesn't have passes derived from it), resource structure is needed */
        if (0 == srt->rootBufs[i].mod)
        {
            resourcesUsed = 1;
            break;
        }
    }

    /** emit C++ function function */
    funcName = cstrIntern("static void zstdgpu_Srt_Fill(");
    sb_Fmt(b, "%szstdgpu_%s_SRT &srt, const zstdgpu_ResourceDataCpu %s", nameToCStr(funcName), nameToCStr(srt->name), (0 != resourcesUsed) ? "&cpuRes" : "&");

    /** emit function parameters: bind points modified by passes + constants */
    if (0 != multiPass)
    {
        for (i = 0; i < srt->rootBufCount; ++i)
        {
            if (points[i].mod)
                sb_Fmt(b, ",\n%*s%-*s %s", (uint32_t)nameToCStrLen(funcName), "", maxTypeLen, nameToCStr(points[i].macroText), nameToCStr(points[i].memberText));
        }
    }

    for (i = 0; i < srt->boundConstCount; ++i)
        sb_Fmt(b, ",\n%*s%-*s %s", (uint32_t)nameToCStrLen(funcName), "", maxTypeLen, nameToCStr(boundConsts[i].type), nameToCStr(boundConsts[i].name));

    sb_StrLitEoL(b, ")\n{");

    /** emit per bind group calls*/
    for (i = 0; i < srt->groupCount; ++i)
        sb_Fmt(b, "    zstdgpu_Srt_FillBindGroup_%s(srt, cpuRes);\n", nameToCStr(g_groups[srt->groupIdx[i]].name));

    if (srt->groupCount > 0 && srt->rootBufCount > 0)
        sb_ExtraLine(b);

    /** emit per bind point assignments  */
    for (i = 0; i < srt->rootBufCount; ++i)
    {
        sb_Fmt(b, "    srt.%-*s= ", maxNameLen, nameToCStr(points[i].memberText));
        if (0 != points[i].mod)
        {
            sb_Str(b, nameToCStr(points[i].memberText));
        }
        else
        {
            sb_Fmt(b, "cpuRes.%s", nameToCStr(points[i].name));
        }
        sb_StrLitEoL(b, ";");
    }
    for (i = 0; i < srt->boundConstCount; ++i)
        sb_Fmt(b, "    srt.%-*s= %s;\n", maxNameLen, nameToCStr(boundConsts[i].name), nameToCStr(boundConsts[i].name));

    sb_StrLitEoL(b, "}\n");

    if (0 != multiPass)
    {
        for (int passIdx = 0; passIdx < g_passCount; ++passIdx)
        {
            const Pass *pass = &g_passes[passIdx];

            if (pass->srtIdx == srtIdx)
            {
                sb_Fmt(b, "static void zstdgpu_Srt_Fill_%s(zstdgpu_%s_SRT &srt, const zstdgpu_ResourceDataCpu &cpuRes", nameToCStr(pass->name), nameToCStr(srt->name));
                for (i = 0; i < srt->boundConstCount; ++i)
                    sb_Fmt(b, ", %s %s", nameToCStr(boundConsts[i].type), nameToCStr(boundConsts[i].name));

                sb_StrLitEoL(b, ")\n{");
                sb_StrLit(b, "    zstdgpu_Srt_Fill(srt, cpuRes");
                for (i = 0; i < srt->rootBufCount; ++i)
                {
                    if (0 != points[i].mod)
                        sb_Fmt(b, ", cpuRes.%s", nameToCStr(pass->resources[i]));
                }
                for (i = 0; i < srt->boundConstCount; ++i)
                    sb_Fmt(b, ", %s", nameToCStr(boundConsts[i].name));

                sb_StrLitEoL(b, ");\n}\n");
            }
        }
    }

    sb_StrLitEoL(b, "#endif\n");

    if (srt->inlineConstCount > 0)
    {
        const Const *inlineConsts = srt->inlineConsts;

        sb_Fmt(b, "static void zstdgpu_Srt_FillInline(ZSTDGPU_PARAM_INOUT(zstdgpu_%s_SRT) srt", nameToCStr(srt->name));
        for (i = 0; i < srt->inlineConstCount; ++i)
            sb_Fmt(b, ", %s %s", nameToCStr(inlineConsts[i].type), nameToCStr(inlineConsts[i].name));

        sb_StrLitEoL(b, ")\n{");
        for (i = 0; i < srt->inlineConstCount; ++i)
            sb_Fmt(b, "    srt.%-*s= %s;\n", 48, nameToCStr(inlineConsts[i].name), nameToCStr(inlineConsts[i].name));

        sb_StrLitEoL(b, "}\n");
    }

    sb_EndFile(b, sb_CStr(&guard), sb_CStr(&path));

    arrfree(guard.data);
    arrfree(path.data);
}

static int collectStageGroups(int stage, int *outGroupIdx)
{
    int count = 0;
    int i;

    for (i = 0; i < g_groupCount; ++i)
    {
        if (0 != (g_groups[i].stageMask & (1 << stage)))
        {
            outGroupIdx[count++] = i;
        }
    }
    return count;
}

static void emitStructs(const char *dir)
{
    StrBuilder  sb = { NULL };
    StrBuilder *b = &sb;
    StrBuilder  path = { NULL };
    const char *guard = "ZSTDGPU_SRT_GENERATED_STRUCTS_H";
    int         s;
    int         i;
    int         j;

    sb_Fmt(&path, "%s/zstdgpu_srt_structs.h", dir);

    sb_BeginFile(b, guard);

    for (s = 0; s < g_srtCount; ++s)
    {
        const Srt *srt = &g_srts[s];

        sb_Fmt(b, "typedef struct zstdgpu_%s_SRT\n{\n", nameToCStr(srt->name));

        for (i = 0; i < srt->groupCount; ++i)
        {
            const Group *g = &g_groups[srt->groupIdx[i]];
            for (j = 0; j < g->entryCount; ++j)
            {
                const Entry *e = &g->entries[j];

                sb_Fmt(b, "    %-*s%s;\n", 56, nameToCStr(e->macroText), nameToCStr(e->memberText));
            }
        }
        for (i = 0; i < srt->rootBufCount; ++i)
        {
            const Entry *e = &srt->rootBufs[i];

            sb_Fmt(b, "    %-*s%s;\n", 56, nameToCStr(e->macroText), nameToCStr(e->memberText));
        }
        for (i = 0; i < srt->constCount; ++i)
        {
            sb_Fmt(b, "    %-*s%s;\n", 56, nameToCStr(srt->consts[i].type), nameToCStr(srt->consts[i].name));
        }
        sb_Fmt(b, "} zstdgpu_%s_SRT;\n\n", nameToCStr(srt->name));
    }

    sb_EndFile(b, guard, sb_CStr(&path));

    arrfree(path.data);
}

static void emitConstsRootSlots(StrBuilder *b)
{
    int emitted = 0;
    int i;

    for (i = 0; i < g_srtCount; ++i)
    {
        const Srt *srt = &g_srts[i];

        if (0 == srt->boundConstCount)
            continue;

        if (0 == emitted)
        {
            sb_StrLitEoL(b, "/**\n"
                            " * Root parameter index of an SRT's constants block bound that must be set by Indirect arguments\n"
                            " * Pass this to ZSTDGPU_DISPATCH32_CMD_SIG or zstdgpu_Dispatch32Bit instead of a hand-written number.\n"
                            " */");
            emitted = 1;
        }

        const char *srtName = nameToCStr(srt->name);
        const int   gap = (25 > (int)strlen(srtName)) ? 25 - (int)strlen(srtName) : 1;

        sb_Fmt(b, "static const uint32_t kzstdgpu_SrtConstsRootSlot_%s%*s= %d;\n",
            srtName, gap, "", srtConstsRootSlot(srt));
    }

    if (0 != emitted)
    {
        sb_ExtraLine(b);
    }
}

static const char *dxgiFormatForCpuType(NameId dataType)
{
    static const char *const kMap[][2] = {
        {  "uint8_t",   "DXGI_FORMAT_R8_UINT" },
        { "uint16_t",  "DXGI_FORMAT_R16_UINT" },
        { "uint32_t",  "DXGI_FORMAT_R32_UINT" },
        {  "int16_t",  "DXGI_FORMAT_R16_SINT" },
        {  "int32_t",  "DXGI_FORMAT_R32_SINT" },
        {    "float", "DXGI_FORMAT_R32_FLOAT" }
    };
    const char *name = nameToCStr(dataType);
    int         i;

    for (i = 0; i < (int)(sizeof(kMap) / sizeof(kMap[0])); ++i)
    {
        if (0 == strcmp(name, kMap[i][0]))
        {
            return kMap[i][1];
        }
    }
    fail("no DXGI format mapping for typed buffer element type '%s' -- add it to dxgiFormatForCpuType", name);
    return "DXGI_FORMAT_UNKNOWN";
}

static void emitBindGroupEntryPush(StrBuilder *b, const Entry *e)
{
    const char *name = nameToCStr(e->name);
    const char *view = (kAccessRO == e->access) ? "Srv" : "Uav";

    if (kKindByte == e->kind)
    {
        sb_Fmt(b, "    zstdgpu_Srt_PushRawBufferSrv(cpuDest, descSize, device, b.%s, resInfo.%s_ByteSize);\n", name, name);
    }
    else if (kKindTyped == e->kind)
    {
        sb_Fmt(b, "    zstdgpu_Srt_PushTypedBuffer%s(cpuDest, descSize, device, b.%s, resInfo.%s_ByteSize, %s, sizeof(%s));\n", view, name, name, dxgiFormatForCpuType(e->dataType), nameToCStr(e->dataType));
    }
    else if (kKindStruct == e->kind)
    {
        sb_Fmt(b, "    zstdgpu_Srt_PushStructBuffer%s(cpuDest, descSize, device, b.%s, resInfo.%s_ByteSize, sizeof(%s));\n", view, name, name, nameToCStr(e->dataType));
    }
    else
    {
        fail("Unknown buffer kind (%u) while processing '%s' bind pointt", e->kind, name);
    }
}

static void emitBindGroups(StrBuilder *b)
{
    int stageGroups[MAX_GROUPS];
    int stage;
    int i, j;

    emitConstsRootSlots(b);

    sb_StrLitEoL(b, "/**\n"
                    " * Descriptors each stage's bind groups occupy in the shader-visible heap.\n"
                    " * Known at generation time; heap sizing needs it before any descriptor exists.\n"
                    " */");
    sb_StrLit(b, "static const uint32_t zstdgpu_kSrtStageDescCount[] = {");
    for (stage = 0; stage < STAGE_COUNT; ++stage)
    {
        const int count = collectStageGroups(stage, stageGroups);
        int       total = 0;

        for (j = 0; j < count; ++j)
        {
            total += g_groups[stageGroups[j]].entryCount;
        }
        sb_Fmt(b, "%s %d", (0 == stage) ? "" : ",", total);
    }
    sb_StrLitEoL(b, " };\n");

    for (stage = 0; stage < STAGE_COUNT; ++stage)
    {
        const int count = collectStageGroups(stage, stageGroups);

        sb_Fmt(b, "/** GPU descriptor table handles of the bind groups that live in stage %d. */\n", stage);
        sb_Fmt(b, "struct zstdgpu_Srt_BindGroups_Stage%d\n{\n", stage);
        for (j = 0; j < count; ++j)
        {
            sb_StrLit(b, "    D3D12_GPU_DESCRIPTOR_HANDLE ");
            sb_Str(b, nameToCStr(g_groups[stageGroups[j]].name));
            sb_StrLitEoL(b, ";");
        }
        sb_StrLitEoL(b, "};\n");
    }

    sb_StrLitEoL(b, "/**\n"
                    " * Everything a generated binder needs: the shader-visible descriptor heap, the compute\n"
                    " * PSO/root-signature pair of every SRT, and the per-stage bind group handles.\n"
                    " */\n"
                    "struct zstdgpu_Srts\n"
                    "{");
    sb_StrLitEoL(b, "    ID3D12DescriptorHeap         *heap;");
    sb_StrLitEoL(b, "    uint32_t                      heapOffset;\n");
    for (i = 0; i < g_srtCount; ++i)
    {
        sb_StrLit(b, "    d3d12aid_ComputeRsPs          ");
        sb_Str(b, nameToCStr(g_srts[i].name));
        sb_StrLitEoL(b, ";");
    }
    sb_ExtraLine(b);
    for (stage = 0; stage < STAGE_COUNT; ++stage)
    {
        sb_Fmt(b, "    zstdgpu_Srt_BindGroups_Stage%d stage%d;\n", stage, stage);
    }
    sb_StrLitEoL(b, "};\n");

    sb_StrLitEoL(b,
        "static D3D12_SHADER_RESOURCE_VIEW_DESC *zstdgpu_SRV_InitAsInvalidBuffer(D3D12_SHADER_RESOURCE_VIEW_DESC *outDesc, uint64_t sizeInBytes, uint32_t strideSizeInBytes)\n"
        "{\n"
        "    ZSTDGPU_ASSERT(0 == sizeInBytes % strideSizeInBytes);\n"
        "    UINT elemCount = (UINT)(sizeInBytes / strideSizeInBytes);\n"
        "    outDesc->Format                     = DXGI_FORMAT_UNKNOWN;\n"
        "    outDesc->ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;\n"
        "    outDesc->Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;\n"
        "    outDesc->Buffer.FirstElement        = 0/*elemStart*/;\n"
        "    outDesc->Buffer.NumElements         = elemCount;\n"
        "    outDesc->Buffer.StructureByteStride = 0;\n"
        "    outDesc->Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;\n"
        "    return outDesc;\n"
        "}\n"
        "static D3D12_UNORDERED_ACCESS_VIEW_DESC *zstdgpu_UAV_InitAsInvalidBuffer(D3D12_UNORDERED_ACCESS_VIEW_DESC *outDesc, uint64_t sizeInBytes, uint32_t strideSizeInBytes)\n"
        "{\n"
        "    ZSTDGPU_ASSERT(0 == sizeInBytes % strideSizeInBytes);\n"
        "    UINT elemCount = (UINT)(sizeInBytes / strideSizeInBytes);\n"
        "    outDesc->Format                     = DXGI_FORMAT_UNKNOWN;\n"
        "    outDesc->ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;\n"
        "    outDesc->Buffer.FirstElement        = 0/*elemStart*/;\n"
        "    outDesc->Buffer.NumElements         = elemCount;\n"
        "    outDesc->Buffer.StructureByteStride = 0;\n"
        "    outDesc->Buffer.CounterOffsetInBytes= 0;\n"
        "    outDesc->Buffer.Flags               = D3D12_BUFFER_UAV_FLAG_NONE;\n"
        "    return outDesc;\n"
        "}\n"
        "static D3D12_SHADER_RESOURCE_VIEW_DESC *zstdgpu_SRV_InitAsRawBuffer(D3D12_SHADER_RESOURCE_VIEW_DESC *outDesc, uint64_t sizeInBytes)\n"
        "{\n"
        "    zstdgpu_SRV_InitAsInvalidBuffer(outDesc, sizeInBytes, sizeof(uint32_t));\n"
        "    outDesc->Format = DXGI_FORMAT_R32_TYPELESS;\n"
        "    outDesc->Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;\n"
        "    return outDesc;\n"
        "}\n"
        "static D3D12_UNORDERED_ACCESS_VIEW_DESC *zstdgpu_UAV_InitAsRawBuffer(D3D12_UNORDERED_ACCESS_VIEW_DESC *outDesc, uint64_t sizeInBytes)\n"
        "{\n"
        "    zstdgpu_UAV_InitAsInvalidBuffer(outDesc, sizeInBytes, sizeof(uint32_t));\n"
        "    outDesc->Format = DXGI_FORMAT_R32_TYPELESS;\n"
        "    outDesc->Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;\n"
        "    return outDesc;\n"
        "}\n"
        "static D3D12_SHADER_RESOURCE_VIEW_DESC *zstdgpu_SRV_InitAsStructBuffer(D3D12_SHADER_RESOURCE_VIEW_DESC *outDesc, uint64_t sizeInBytes, uint32_t structSizeInBytes)\n"
        "{\n"
        "    zstdgpu_SRV_InitAsInvalidBuffer(outDesc, sizeInBytes, structSizeInBytes);\n"
        "    outDesc->Buffer.StructureByteStride = structSizeInBytes;\n"
        "    return outDesc;\n"
        "}\n"
        "static D3D12_UNORDERED_ACCESS_VIEW_DESC *zstdgpu_UAV_InitAsStructBuffer(D3D12_UNORDERED_ACCESS_VIEW_DESC *outDesc, uint64_t sizeInBytes, uint32_t structSizeInBytes)\n"
        "{\n"
        "    zstdgpu_UAV_InitAsInvalidBuffer(outDesc, sizeInBytes, structSizeInBytes);\n"
        "    outDesc->Buffer.StructureByteStride = structSizeInBytes;\n"
        "    return outDesc;\n"
        "}\n"
        "static D3D12_SHADER_RESOURCE_VIEW_DESC *zstdgpu_SRV_InitAsTypedBuffer(D3D12_SHADER_RESOURCE_VIEW_DESC *outDesc, uint64_t sizeInBytes, DXGI_FORMAT format, uint32_t elemSizeInBytes)\n"
        "{\n"
        "    zstdgpu_SRV_InitAsInvalidBuffer(outDesc, sizeInBytes, elemSizeInBytes);\n"
        "    outDesc->Format = format;\n"
        "    return outDesc;\n"
        "}\n"
        "static D3D12_UNORDERED_ACCESS_VIEW_DESC *zstdgpu_UAV_InitAsTypedBuffer(D3D12_UNORDERED_ACCESS_VIEW_DESC *outDesc, uint64_t sizeInBytes, DXGI_FORMAT format, uint32_t elemSizeInBytes)\n"
        "{\n"
        "    zstdgpu_UAV_InitAsInvalidBuffer(outDesc, sizeInBytes, elemSizeInBytes);\n"
        "    outDesc->Format = format;\n"
        "    return outDesc;\n"
        "}\n"
        "/** Descriptor writers. Each advances `cpuDest` by exactly one descriptor. */\n"
        "static void zstdgpu_Srt_PushRawBufferSrv(D3D12_CPU_DESCRIPTOR_HANDLE &cpuDest, uint32_t descSize, ID3D12Device *device, ID3D12Resource *resource, uint32_t byteSize)\n"
        "{\n"
        "    D3D12_SHADER_RESOURCE_VIEW_DESC SRV;\n"
        "    device->CreateShaderResourceView(resource, zstdgpu_SRV_InitAsRawBuffer(&SRV, byteSize), cpuDest);\n"
        "    cpuDest.ptr += descSize;\n"
        "}\n"
        "\n"
        "static void zstdgpu_Srt_PushRawBufferUav(D3D12_CPU_DESCRIPTOR_HANDLE &cpuDest, uint32_t descSize, ID3D12Device *device, ID3D12Resource *resource, uint32_t byteSize)\n"
        "{\n"
        "    D3D12_UNORDERED_ACCESS_VIEW_DESC UAV;\n"
        "    device->CreateUnorderedAccessView(resource, NULL, zstdgpu_UAV_InitAsRawBuffer(&UAV, byteSize), cpuDest);\n"
        "    cpuDest.ptr += descSize;\n"
        "}\n"
        "\n"
        "static void zstdgpu_Srt_PushStructBufferSrv(D3D12_CPU_DESCRIPTOR_HANDLE &cpuDest, uint32_t descSize, ID3D12Device *device, ID3D12Resource *resource, uint32_t byteSize, uint32_t stride)\n"
        "{\n"
        "    D3D12_SHADER_RESOURCE_VIEW_DESC SRV;\n"
        "    device->CreateShaderResourceView(resource, zstdgpu_SRV_InitAsStructBuffer(&SRV, byteSize, stride), cpuDest);\n"
        "    cpuDest.ptr += descSize;\n"
        "}\n"
        "\n"
        "static void zstdgpu_Srt_PushStructBufferUav(D3D12_CPU_DESCRIPTOR_HANDLE &cpuDest, uint32_t descSize, ID3D12Device *device, ID3D12Resource *resource, uint32_t byteSize, uint32_t stride)\n"
        "{\n"
        "    D3D12_UNORDERED_ACCESS_VIEW_DESC UAV;\n"
        "    device->CreateUnorderedAccessView(resource, NULL, zstdgpu_UAV_InitAsStructBuffer(&UAV, byteSize, stride), cpuDest);\n"
        "    cpuDest.ptr += descSize;\n"
        "}\n"
        "\n"
        "static void zstdgpu_Srt_PushTypedBufferSrv(D3D12_CPU_DESCRIPTOR_HANDLE &cpuDest, uint32_t descSize, ID3D12Device *device, ID3D12Resource *resource, uint32_t byteSize, DXGI_FORMAT format, uint32_t stride)\n"
        "{\n"
        "    D3D12_SHADER_RESOURCE_VIEW_DESC SRV;\n"
        "    device->CreateShaderResourceView(resource, zstdgpu_SRV_InitAsTypedBuffer(&SRV, byteSize, format, stride), cpuDest);\n"
        "    cpuDest.ptr += descSize;\n"
        "}\n"
        "\n"
        "static void zstdgpu_Srt_PushTypedBufferUav(D3D12_CPU_DESCRIPTOR_HANDLE &cpuDest, uint32_t descSize, ID3D12Device *device, ID3D12Resource *resource, uint32_t byteSize, DXGI_FORMAT format, uint32_t stride)\n"
        "{\n"
        "    D3D12_UNORDERED_ACCESS_VIEW_DESC UAV;\n"
        "    device->CreateUnorderedAccessView(resource, NULL, zstdgpu_UAV_InitAsTypedBuffer(&UAV, byteSize, format, stride), cpuDest);\n"
        "    cpuDest.ptr += descSize;\n"
        "}\n");

    /** emit C++ bind group initialiser */
    for (i = 0; i < g_groupCount; ++i)
    {
        const Group *g = &g_groups[i];

        sb_Fmt(b, "static D3D12_GPU_DESCRIPTOR_HANDLE zstdgpu_Srt_InitBindGroup_%s(zstdgpu_Srts &srts, ID3D12Device *device, const zstdgpu_ResourceInfo &resInfo, const zstdgpu_GpuOnlyBuffers &b)\n{\n", nameToCStr(g->name));
        sb_StrLitEoL(b, "    const uint32_t descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);");
        sb_StrLitEoL(b, "    const D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = d3d12aid_DescriptorHeap_GetCpuStart(srts.heap);");
        sb_StrLitEoL(b, "    const D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = d3d12aid_DescriptorHeap_GetGpuStart(srts.heap);");
        sb_StrLitEoL(b, "    const D3D12_GPU_DESCRIPTOR_HANDLE gpuDest  = { gpuStart.ptr + (UINT64)srts.heapOffset * descSize };");
        sb_StrLitEoL(b, "    D3D12_CPU_DESCRIPTOR_HANDLE       cpuDest  = { cpuStart.ptr + (SIZE_T)srts.heapOffset * descSize };\n");
        for (j = 0; j < g->entryCount; ++j)
        {
            emitBindGroupEntryPush(b, &g->entries[j]);
        }
        sb_Fmt(b, "\n    srts.heapOffset += %d;\n    return gpuDest;\n}\n\n", (int)g->entryCount);
    }

    for (stage = 0; stage < STAGE_COUNT; ++stage)
    {
        const int count = collectStageGroups(stage, stageGroups);

        sb_Fmt(b, "static void zstdgpu_Srt_InitBindGroups_Stage%d(zstdgpu_Srts &srts, ID3D12Device *device, const zstdgpu_ResourceInfo &resInfo, const zstdgpu_GpuOnlyBuffers &b)\n{\n", stage);
        if (0 == count)
        {
            sb_StrLitEoL(b, "    /* no descriptor tables in this stage */");
        }
        for (j = 0; j < count; ++j)
        {
            const char *name = nameToCStr(g_groups[stageGroups[j]].name);
            sb_Fmt(b, "    srts.stage%d.%s = zstdgpu_Srt_InitBindGroup_%s(srts, device, resInfo, b);\n", stage, name, name);
        }
        sb_StrLitEoL(b, "}\n");
    }

    /* two call sites choose the stage at run time */
    sb_StrLitEoL(b, "static void zstdgpu_Srt_InitBindGroups_Stage(zstdgpu_Srts &srts, ID3D12Device *device, const zstdgpu_ResourceInfo &resInfo, const zstdgpu_GpuOnlyBuffers &b, uint32_t stageIndex)\n"
                    "{\n"
                    "    switch (stageIndex)\n"
                    "    {");
    for (stage = 0; stage < STAGE_COUNT; ++stage)
    {
        sb_Fmt(b, "    case %d: zstdgpu_Srt_InitBindGroups_Stage%d(srts, device, resInfo, b); break;\n", stage, stage);
    }
    sb_StrLitEoL(b, "    default: ZSTDGPU_ASSERT(0); break;\n"
                    "    }\n"
                    "}\n");
}

static void emitBind(const char *dir)
{
    StrBuilder  sb = { NULL };
    StrBuilder *b = &sb;
    StrBuilder  path = { NULL };
    const char *guard = "ZSTDGPU_SRT_GENERATED_BIND_H";
    int         i;
    int         j;

    sb_Fmt(&path, "%s/zstdgpu_srt_bind.h", dir);

    sb_BeginFile(b, guard);
    sb_StrLitEoL(b, "/** Stable resource ids -- the barrier tracker indexes its state array with these. */\n"
                    "enum\n"
                    "{");

    for (i = 0; i < g_resourceCount; ++i)
    {
        sb_Fmt(b, "    kzstdgpu_SrtRes_%-*s= %d,\n", 36, nameToCStr(g_resources[i]), i);
    }
    sb_Fmt(b, "    kzstdgpu_SrtRes_Count%*s= %d\n", 31, "", g_resourceCount);
    sb_StrLitEoL(b, "};\n");

    emitBindGroups(b);

    for (i = 0; i < g_passCount; ++i)
    {
        const Pass *pass = &g_passes[i];
        const Srt  *srt = &g_srts[pass->srtIdx];
        int         stage;

        for (stage = (0 == srt->groupCount) ? -1 : 0; stage < STAGE_COUNT; ++stage)
        {
            if (stage >= 0 && 0 == (srt->stageMask & (1 << stage)))
                continue;

            sb_Fmt(b, "static void zstdgpu_Bind_%s", nameToCStr(srt->name));
            if (kNameIdError != pass->name)
            {
                sb_Fmt(b, "_%s", nameToCStr(pass->name));
            }
            if (stage >= 0)
            {
                sb_Fmt(b, "_Stage%d", stage);
            }
            sb_StrLit(b, "(ID3D12GraphicsCommandList *cmdList, const zstdgpu_Srts &srts, const zstdgpu_GpuOnlyBuffers &");
            if (srt->rootBufCount > 0)
                sb_StrLit(b, "b");

            for (j = 0; j < srt->boundConstCount; ++j)
            {
                const Const *c = &srt->boundConsts[j];

                if (pass->indirect == Direct || kConstIndirect != c->kind)
                    sb_Fmt(b, ", %s %s", nameToCStr(c->type), nameToCStr(c->name));
            }
            sb_StrLitEoL(b, ")\n{");
            sb_Fmt(b, "    d3d12aid_ComputeRsPs_Set(&srts.%s, cmdList);\n", nameToCStr(srt->name));

            if (srt->groupCount > 0)
            {
                sb_StrLitEoL(b, "    cmdList->SetDescriptorHeaps(1, &srts.heap);");
                for (j = 0; j < srt->groupCount; ++j)
                {
                    const Group *g = &g_groups[srt->groupIdx[j]];

                    sb_Fmt(b, "    cmdList->SetComputeRootDescriptorTable(%d /* %s */, srts.stage%d.%s);\n", j, nameToCStr(g->name), stage, nameToCStr(g->name));
                }
            }

            for (j = 0; j < srt->rootBufCount; ++j)
            {
                const Entry *e = &srt->rootBufs[j];

                sb_Fmt(b, "    cmdList->SetComputeRoot%sView(%d /* %s */,", (kAccessRO == e->access) ? "ShaderResource" : "UnorderedAccess", srt->groupCount + j, nameToCStr(e->name));
                sb_Fmt(b, " b.%s->GetGPUVirtualAddress());\n", nameToCStr(pass->resources[j]));
            }

            for (j = 0; j < srt->boundConstCount; ++j)
            {
                const Const *c = &srt->boundConsts[j];

                if (pass->indirect == Direct || kConstIndirect != c->kind)
                    sb_Fmt(b, "    cmdList->SetComputeRoot32BitConstant(%d /* Consts */, %s, %d /* %s */);\n", srtConstsRootSlot(srt), nameToCStr(c->name), j, nameToCStr(c->name));
            }

            sb_StrLitEoL(b, "}\n");
        }
    }

    sb_EndFile(b, guard, sb_CStr(&path));

    arrfree(path.data);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".generated";
    int         i;

    collect();
    addDefaultPasses();
    checkRootBudget();

    if (g_errorCount > 0)
    {
        fprintf(stderr, "[zstdgpu_srt_tool] [FAIL] %d error(s), no output written\n", g_errorCount);
        return 1;
    }

    for (i = 0; i < g_groupCount; ++i)
    {
        emitBindGroupHeader(dir, &g_groups[i]);
    }
    for (i = 0; i < g_srtCount; ++i)
    {
        emitSrtHeader(dir, i);
    }
    emitStructs(dir);
    emitBind(dir);

    if (g_errorCount > 0)
    {
        fprintf(stderr, "[zstdgpu_srt_tool] [FAIL] %d error(s) while writing output\n", g_errorCount);
        return 1;
    }

    printf("[zstdgpu_srt_tool] [INFO] %d bind group(s), %d SRT(s), %d pass(es), %d resource(s) -> %s\n", g_groupCount, g_srtCount, g_passCount, g_resourceCount, dir);
    return 0;
}
