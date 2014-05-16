/* ===-- gcc_personality_v0.c - Implement __gcc_personality_v0 -------------===
 *
 *                 The LLVM Compiler Infrastructure
 *
 * This file is dual licensed under the MIT and the University of Illinois Open
 * Source Licenses. See LICENSE.TXT for details.
 *
 * ===----------------------------------------------------------------------===
 *
 */

#include "int_lib.h"

/*
 * _Unwind_* stuff based on C++ ABI public documentation
 * http://refspecs.freestandards.org/abi-eh-1.21.html
 */

typedef enum {
    _URC_NO_REASON = 0,
    _URC_FOREIGN_EXCEPTION_CAUGHT = 1,
    _URC_FATAL_PHASE2_ERROR = 2,
    _URC_FATAL_PHASE1_ERROR = 3,
    _URC_NORMAL_STOP = 4,
    _URC_END_OF_STACK = 5,
    _URC_HANDLER_FOUND = 6,
    _URC_INSTALL_CONTEXT = 7,
    _URC_CONTINUE_UNWIND = 8
} _Unwind_Reason_Code;

typedef enum {
    _UA_SEARCH_PHASE = 1,
    _UA_CLEANUP_PHASE = 2,
    _UA_HANDLER_FRAME = 4,
    _UA_FORCE_UNWIND = 8,
    _UA_END_OF_STACK = 16
} _Unwind_Action;

typedef struct _Unwind_Context* _Unwind_Context_t;

struct _Unwind_Exception {
    uint64_t                exception_class;
    void                    (*exception_cleanup)(_Unwind_Reason_Code reason, 
                                                 struct _Unwind_Exception* exc);
    uintptr_t                private_1;    
    uintptr_t                private_2;    
};

COMPILER_RT_ABI  const uint8_t*    _Unwind_GetLanguageSpecificData(_Unwind_Context_t c);
COMPILER_RT_ABI  void              _Unwind_SetGR(_Unwind_Context_t c, int i, uintptr_t n);
COMPILER_RT_ABI  void              _Unwind_SetIP(_Unwind_Context_t, uintptr_t new_value);
COMPILER_RT_ABI  uintptr_t         _Unwind_GetIP(_Unwind_Context_t context);
COMPILER_RT_ABI  uintptr_t         _Unwind_GetRegionStart(_Unwind_Context_t context);


/*
 * Pointer encodings documented at:
 *   http://refspecs.freestandards.org/LSB_1.3.0/gLSB/gLSB/ehframehdr.html
 */

#define DW_EH_PE_omit      0xff  /* no data follows */

#define DW_EH_PE_absptr    0x00
#define DW_EH_PE_uleb128   0x01
#define DW_EH_PE_udata2    0x02
#define DW_EH_PE_udata4    0x03
#define DW_EH_PE_udata8    0x04
#define DW_EH_PE_sleb128   0x09
#define DW_EH_PE_sdata2    0x0A
#define DW_EH_PE_sdata4    0x0B
#define DW_EH_PE_sdata8    0x0C

#define DW_EH_PE_pcrel     0x10
#define DW_EH_PE_textrel   0x20
#define DW_EH_PE_datarel   0x30
#define DW_EH_PE_funcrel   0x40
#define DW_EH_PE_aligned   0x50  
#define DW_EH_PE_indirect  0x80 /* gcc extension */



/* read a uleb128 encoded value and advance pointer */
static uintptr_t readULEB128(const uint8_t** data)
{
    uintptr_t result = 0;
    uintptr_t shift = 0;
    unsigned char byte;
    const uint8_t* p = *data;
    do {
        byte = *p++;
        result |= (byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    *data = p;
    return result;
}

/* read a pointer encoded value and advance pointer */
static uintptr_t readEncodedPointer(const uint8_t** data, uint8_t encoding)
{
    const uint8_t* p = *data;
    uintptr_t result = 0;

    if ( encoding == DW_EH_PE_omit ) 
        return 0;

    /* first get value */
    switch (encoding & 0x0F) {
        case DW_EH_PE_absptr:
            result = *((const uintptr_t*)p);
            p += sizeof(uintptr_t);
            break;
        case DW_EH_PE_uleb128:
            result = readULEB128(&p);
            break;
        case DW_EH_PE_udata2:
            result = *((const uint16_t*)p);
            p += sizeof(uint16_t);
            break;
        case DW_EH_PE_udata4:
            result = *((const uint32_t*)p);
            p += sizeof(uint32_t);
            break;
        case DW_EH_PE_udata8:
            result = *((const uint64_t*)p);
            p += sizeof(uint64_t);
            break;
        case DW_EH_PE_sdata2:
            result = *((const int16_t*)p);
            p += sizeof(int16_t);
            break;
        case DW_EH_PE_sdata4:
            result = *((const int32_t*)p);
            p += sizeof(int32_t);
            break;
        case DW_EH_PE_sdata8:
            result = *((const int64_t*)p);
            p += sizeof(int64_t);
            break;
        case DW_EH_PE_sleb128:
        default:
            /* not supported */
            compilerrt_abort();
            break;
    }

    /* then add relative offset */
    switch ( encoding & 0x70 ) {
        case DW_EH_PE_absptr:
            /* do nothing */
            break;
        case DW_EH_PE_pcrel:
            result += (uintptr_t)(*data);
            break;
        case DW_EH_PE_textrel:
        case DW_EH_PE_datarel:
        case DW_EH_PE_funcrel:
        case DW_EH_PE_aligned:
        default:
            /* not supported */
            compilerrt_abort();
            break;
    }

    /* then apply indirection */
    if (encoding & DW_EH_PE_indirect) {
        result = *((const uintptr_t*)result);
    }

    *data = p;
    return result;
}


/*
 * The C compiler makes references to __gcc_personality_v0 in
 * the dwarf unwind information for translation units that use
 * __attribute__((cleanup(xx))) on local variables.
 * This personality routine is called by the system unwinder
 * on each frame as the stack is unwound during a C++ exception
 * throw through a C function compiled with -fexceptions.
 */
#if __USING_SJLJ_EXCEPTIONS__
// the setjump-longjump based exceptions personality routine has a different name
COMPILER_RT_ABI _Unwind_Reason_Code __gcc_personality_sj0(int version, _Unwind_Action actions,
         uint64_t exceptionClass, struct _Unwind_Exception* exceptionObject,
         _Unwind_Context_t context)
#else
COMPILER_RT_ABI _Unwind_Reason_Code __gcc_personality_v0(int version, _Unwind_Action actions,
         uint64_t exceptionClass, struct _Unwind_Exception* exceptionObject,
         _Unwind_Context_t context)
#endif
{
    /* Since C does not have catch clauses, there is nothing to do during */
    /* phase 1 (the search phase). */
    if ( actions & _UA_SEARCH_PHASE ) 
        return _URC_CONTINUE_UNWIND;
        
    /* There is nothing to do if there is no LSDA for this frame. */
    const uint8_t* lsda = _Unwind_GetLanguageSpecificData(context);
    if ( lsda == (uint8_t*) 0 )
        return _URC_CONTINUE_UNWIND;

    // Get the current instruction pointer and offset it before next
    // instruction in the current frame which threw the exception.
    uintptr_t ip = _Unwind_GetIP(context) - 1;
    // Get beginning current frame's code (as defined by the 
    // emitted dwarf code)
    uintptr_t funcStart = _Unwind_GetRegionStart(context);

    uintptr_t resumeIp = 0;
#if __USING_SJLJ_EXCEPTIONS__
    if (ip == (uintptr_t)-1)
    {
        // no action
        return _URC_CONTINUE_UNWIND;
    }
    else if (ip == 0)
        return _URC_FATAL_PHASE2_ERROR;
    // ip is 1-based index into call site table
#else  // !__USING_SJLJ_EXCEPTIONS__
    uintptr_t ipOffset = ip - funcStart;
#endif  // !defined(_USING_SLJL_EXCEPTIONS__)
    // Note: See JITDwarfEmitter::EmitExceptionTable(...) for corresponding
    //       dwarf emission
    // Parse LSDA header.
    const uint8_t* lpStart = 0;

    uint8_t lpStartEncoding = *lsda++;
    if (lpStartEncoding != DW_EH_PE_omit)
        lpStart = (const uint8_t*)readEncodedPointer(&lsda, lpStartEncoding);

    if (lpStart == 0)
        lpStart = (const uint8_t*)funcStart;

    uint8_t ttypeEncoding = *lsda++;
    if (ttypeEncoding != DW_EH_PE_omit)
    {
        // Calculate type info locations in emitted dwarf code which
        // were flagged by type info arguments to llvm.eh.selector
        // intrinsic
        readULEB128(&lsda);
    }
    // Walk call-site table looking for range that 
    // includes current PC. 
    uint8_t callSiteEncoding = *lsda++;
#if __USING_SJLJ_EXCEPTIONS__
    (void)callSiteEncoding;  // When using SjLj exceptions, callSiteEncoding is never used
#endif
    uint32_t callSiteTableLength = readULEB128(&lsda);
    const uint8_t* callSiteTableStart = lsda;
    const uint8_t* callSiteTableEnd = callSiteTableStart + callSiteTableLength;
    const uint8_t* callSitePtr = callSiteTableStart;
    while (callSitePtr < callSiteTableEnd)
    {
        // There is one entry per call site.
#if !__USING_SJLJ_EXCEPTIONS__
        // The call sites are non-overlapping in [start, start+length)
        // The call sites are ordered in increasing value of start
        uintptr_t start = readEncodedPointer(&callSitePtr, callSiteEncoding);
        uintptr_t length = readEncodedPointer(&callSitePtr, callSiteEncoding);
        uintptr_t landingPad = readEncodedPointer(&callSitePtr, callSiteEncoding);
        uintptr_t actionEntry = readULEB128(&callSitePtr);
        (void)actionEntry; // action is ignored for C code
        if (landingPad == 0)
            continue;

        if ((start <= ipOffset) && (ipOffset < (start + length)))
#else  // __USING_SJLJ_EXCEPTIONS__
        // ip is 1-based index into this table
        uintptr_t landingPad = readULEB128(&callSitePtr);
        uintptr_t actionEntry = readULEB128(&callSitePtr);
        (void)actionEntry; // action is ignored for C code
        if (--ip == 0)
#endif  // __USING_SJLJ_EXCEPTIONS__
        {
            // Found the call site containing ip.
#if !__USING_SJLJ_EXCEPTIONS__
            /* Found landing pad for the PC.
             * Set Instruction Pointer to so we re-enter function
             * at landing pad. The landing pad is created by the compiler
             * to take two parameters in registers.
            */
            resumeIp = (uintptr_t)lpStart + landingPad;
            break;
#else  // __USING_SJLJ_EXCEPTIONS__
            resumeIp = ++landingPad;
#endif  // __USING_SJLJ_EXCEPTIONS__
        }
    } 

    if (resumeIp == 0)
        return _URC_CONTINUE_UNWIND;
    else
    {
        _Unwind_SetGR(context, __builtin_eh_return_data_regno (0), (uintptr_t)exceptionObject);
        _Unwind_SetGR(context, __builtin_eh_return_data_regno (1), 0);
        _Unwind_SetIP(context, resumeIp);
        return _URC_INSTALL_CONTEXT;
    }
}

