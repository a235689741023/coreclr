// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// 

/*****************************************************************************\
*                                                                             *
* CorInfo.h -    EE / Code generator interface                                *
*                                                                             *
*******************************************************************************
*
* This file exposes CLR runtime functionality. It can be used by compilers,
* both Just-in-time and ahead-of-time, to generate native code which
* executes in the runtime environment.
*******************************************************************************

//////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE
//
// The JIT/EE interface is versioned. By "interface", we mean mean any and all communication between the
// JIT and the EE. Any time a change is made to the interface, the JIT/EE interface version identifier
// must be updated. See code:JITEEVersionIdentifier for more information.
// 
// NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE
//
//////////////////////////////////////////////////////////////////////////////////////////////////////////

#EEJitContractDetails

The semantic contract between the EE and the JIT should be documented here It is incomplete, but as time goes
on, that hopefully will change

See file:../../doc/BookOfTheRuntime/JIT/JIT%20Design.doc for details on the JIT compiler. See
code:EEStartup#TableOfContents for information on the runtime as a whole.

-------------------------------------------------------------------------------
#Tokens

The tokens in IL stream needs to be resolved to EE handles (CORINFO_CLASS/METHOD/FIELD_HANDLE) that 
the runtime operates with. ICorStaticInfo::resolveToken is the method that resolves the found in IL stream 
to set of EE handles (CORINFO_RESOLVED_TOKEN). All other APIs take resolved token as input. This design 
avoids redundant token resolutions.

The token validation is done as part of token resolution. The JIT is not required to do explicit upfront
token validation.

-------------------------------------------------------------------------------
#ClassConstruction

First of all class contruction comes in two flavors precise and 'beforeFieldInit'. In C# you get the former
if you declare an explicit class constructor method and the later if you declaratively initialize static
fields. Precise class construction guarentees that the .cctor is run precisely before the first access to any
method or field of the class. 'beforeFieldInit' semantics guarentees only that the .cctor will be run some
time before the first static field access (note that calling methods (static or insance) or accessing
instance fields does not cause .cctors to be run).

Next you need to know that there are two kinds of code generation that can happen in the JIT: appdomain
neutral and appdomain specialized. The difference between these two kinds of code is how statics are handled.
For appdomain specific code, the address of a particular static variable is embeded in the code. This makes
it usable only for one appdomain (since every appdomain gets a own copy of its statics). Appdomain neutral
code calls a helper that looks up static variables off of a thread local variable. Thus the same code can be
used by mulitple appdomains in the same process.  

Generics also introduce a similar issue. Code for generic classes might be specialised for a particular set
of type arguments, or it could use helpers to access data that depends on type parameters and thus be shared
across several instantiations of the generic type.

Thus there four cases

    * BeforeFieldInitCCtor - Unshared code. Cctors are only called when static fields are fetched. At the
        time the method that touches the static field is JITed (or fixed up in the case of NGENed code), the
        .cctor is called.
    * BeforeFieldInitCCtor - Shared code. Since the same code is used for multiple classes, the act of JITing
        the code can not be used as a hook. However, it is also the case that since the code is shared, it
        can not wire in a particular address for the static and thus needs to use a helper that looks up the
        correct address based on the thread ID. This helper does the .cctor check, and thus no additional
        cctor logic is needed.
    * PreciseCCtor - Unshared code. Any time a method is JITTed (or fixed up in the case of NGEN), a cctor
        check for the class of the method being JITTed is done. In addition the JIT inserts explicit checks
        before any static field accesses. Instance methods and fields do NOT have hooks because a .ctor
        method must be called before the instance can be created.
    * PreciseCctor - Shared code .cctor checks are placed in the prolog of every .ctor and static method. All
        methods that access static fields have an explicit .cctor check before use. Again instance methods
        don't have hooks because a .ctor would have to be called first.

Technically speaking, however the optimization of avoiding checks on instance methods is flawed. It requires
that a .ctor always preceed a call to an instance methods. This break down when

    * A NULL is passed to an instance method.
    * A .ctor does not call its superclasses .ctor. This allows an instance to be created without necessarily
        calling all the .cctors of all the superclasses. A virtual call can then be made to a instance of a
        superclass without necessarily calling the superclass's .cctor.
    * The class is a value class (which exists without a .ctor being called)

Nevertheless, the cost of plugging these holes is considered to high and the benefit is low.

----------------------------------------------------------------------

#ClassConstructionFlags 

Thus the JIT's cctor responsibilities require it to check with the EE on every static field access using
initClass and before jitting any method to see if a .cctor check must be placed in the prolog.

    * CORINFO_FLG_BEFOREFIELDINIT indicate the class has beforeFieldInit semantics. The jit does not strictly
        need this information however, it is valuable in optimizing static field fetch helper calls. Helper
        call for classes with BeforeFieldInit semantics can be hoisted before other side effects where
        classes with precise .cctor semantics do not allow this optimization.

Inlining also complicates things. Because the class could have precise semantics it is also required that the
inlining of any constructor or static method must also do the initClass check. The inliner has the option of 
inserting any required runtime check or simply not inlining the function.

-------------------------------------------------------------------------------

#StaticFields

The first 4 options are mutially exclusive 

    * CORINFO_FLG_HELPER If the field has this set, then the JIT must call getFieldHelper and call the
        returned helper with the object ref (for an instance field) and a fieldDesc. Note that this should be
        able to handle ANY field so to get a JIT up quickly, it has the option of using helper calls for all
        field access (and skip the complexity below). Note that for statics it is assumed that you will
        alwasy ask for the ADDRESSS helper and to the fetch in the JIT.

    * CORINFO_FLG_SHARED_HELPER This is currently only used for static fields. If this bit is set it means
        that the field is feched by a helper call that takes a module identifier (see getModuleDomainID) and
        a class identifier (see getClassDomainID) as arguments. The exact helper to call is determined by
        getSharedStaticBaseHelper. The return value is of this function is the base of all statics in the
        module. The offset from getFieldOffset must be added to this value to get the address of the field
        itself. (see also CORINFO_FLG_STATIC_IN_HEAP).


    * CORINFO_FLG_GENERICS_STATIC This is currently only used for static fields (of generic type). This
        function is intended to be called with a Generic handle as a argument (from embedGenericHandle). The
        exact helper to call is determined by getSharedStaticBaseHelper. The returned value is the base of
        all statics in the class. The offset from getFieldOffset must be added to this value to get the
        address of the (see also CORINFO_FLG_STATIC_IN_HEAP).

    * CORINFO_FLG_TLS This indicate that the static field is a Windows style Thread Local Static. (We also
        have managed thread local statics, which work through the HELPER. Support for this is considered
        legacy, and going forward, the EE should

    * <NONE> This is a normal static field. Its address in in memory is determined by getFieldAddress. (see
        also CORINFO_FLG_STATIC_IN_HEAP).


This last field can modify any of the cases above except CORINFO_FLG_HELPER

CORINFO_FLG_STATIC_IN_HEAP This is currently only used for static fields of value classes. If the field has
this set then after computing what would normally be the field, what you actually get is a object pointer
(that must be reported to the GC) to a boxed version of the value. Thus the actual field address is computed
by addr = (*addr+sizeof(OBJECTREF))

Instance fields

    * CORINFO_FLG_HELPER This is used if the class is MarshalByRef, which means that the object might be a
        proxyt to the real object in some other appdomain or process. If the field has this set, then the JIT
        must call getFieldHelper and call the returned helper with the object ref. If the helper returned is
        helpers that are for structures the args are as follows

    * CORINFO_HELP_GETFIELDSTRUCT - args are: retBuff, object, fieldDesc 
    * CORINFO_HELP_SETFIELDSTRUCT - args are object fieldDesc value

The other GET helpers take an object fieldDesc and return the value The other SET helpers take an object
fieldDesc and value

    Note that unlike static fields there is no helper to take the address of a field because in general there
    is no address for proxies (LDFLDA is illegal on proxies).

    CORINFO_FLG_EnC This is to support adding new field for edit and continue. This field also indicates that
    a helper is needed to access this field. However this helper is always CORINFO_HELP_GETFIELDADDR, and
    this helper always takes the object and field handle and returns the address of the field. It is the
                            JIT's responcibility to do the fetch or set. 

-------------------------------------------------------------------------------

TODO: Talk about initializing strutures before use 


*******************************************************************************
*/

#ifndef _COR_INFO_H_
#define _COR_INFO_H_

#include <corhdr.h>
#include <specstrings.h>

//////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE
//
// #JITEEVersionIdentifier
//
// This GUID represents the version of the JIT/EE interface. Any time the interface between the JIT and
// the EE changes (by adding or removing methods to any interface shared between them), this GUID should
// be changed. This is the identifier verified by ICorJitCompiler::getVersionIdentifier().
//
// You can use "uuidgen.exe -s" to generate this value.
//
// **** NOTE TO INTEGRATORS:
//
// If there is a merge conflict here, because the version changed in two different places, you must
// create a **NEW** GUID, not simply choose one or the other!
//
// NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE
//
//////////////////////////////////////////////////////////////////////////////////////////////////////////

#if !defined(SELECTANY)
#if defined(__GNUC__)
    #define SELECTANY extern __attribute__((weak))
#else
    #define SELECTANY extern __declspec(selectany)
#endif
#endif

SELECTANY const GUID JITEEVersionIdentifier = { /* 1ce51eeb-dfd0-4450-ba2c-ea0d2d863df5 */
    0x1ce51eeb,
    0xdfd0,
    0x4450,
    {0xba, 0x2c, 0xea, 0x0d, 0x2d, 0x86, 0x3d, 0xf5}
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// END JITEEVersionIdentifier
//
//////////////////////////////////////////////////////////////////////////////////////////////////////////

// For System V on the CLR type system number of registers to pass in and return a struct is the same.
// The CLR type system allows only up to 2 eightbytes to be passed in registers. There is no SSEUP classification types.
#define CLR_SYSTEMV_MAX_EIGHTBYTES_COUNT_TO_PASS_IN_REGISTERS   2 
#define CLR_SYSTEMV_MAX_EIGHTBYTES_COUNT_TO_RETURN_IN_REGISTERS 2
#define CLR_SYSTEMV_MAX_STRUCT_BYTES_TO_PASS_IN_REGISTERS       16

// System V struct passing
// The Classification types are described in the ABI spec at http://www.x86-64.org/documentation/abi.pdf
enum SystemVClassificationType : unsigned __int8
{
    SystemVClassificationTypeUnknown            = 0,
    SystemVClassificationTypeStruct             = 1,
    SystemVClassificationTypeNoClass            = 2,
    SystemVClassificationTypeMemory             = 3,
    SystemVClassificationTypeInteger            = 4,
    SystemVClassificationTypeIntegerReference   = 5,
    SystemVClassificationTypeIntegerByRef       = 6,
    SystemVClassificationTypeSSE                = 7,
    // SystemVClassificationTypeSSEUp           = Unused, // Not supported by the CLR.
    // SystemVClassificationTypeX87             = Unused, // Not supported by the CLR.
    // SystemVClassificationTypeX87Up           = Unused, // Not supported by the CLR.
    // SystemVClassificationTypeComplexX87      = Unused, // Not supported by the CLR.

    // Internal flags - never returned outside of the classification implementation.

    // This value represents a very special type with two eightbytes. 
    // First ByRef, second Integer (platform int).
    // The VM has a special Elem type for this type - ELEMENT_TYPE_TYPEDBYREF.
    // This is the classification counterpart for that element type. It is used to detect 
    // the special TypedReference type and specialize its classification.
    // This type is represented as a struct with two fields. The classification needs to do
    // special handling of it since the source/methadata type of the fieds is IntPtr. 
    // The VM changes the first to ByRef. The second is left as IntPtr (TYP_I_IMPL really). The classification needs to match this and
    // special handling is warranted (similar thing is done in the getGCLayout function for this type).
    SystemVClassificationTypeTypedReference     = 8,
    SystemVClassificationTypeMAX                = 9,
};

// Represents classification information for a struct.
struct SYSTEMV_AMD64_CORINFO_STRUCT_REG_PASSING_DESCRIPTOR
{
    SYSTEMV_AMD64_CORINFO_STRUCT_REG_PASSING_DESCRIPTOR()
    {
        Initialize();
    }

    bool                        passedInRegisters; // Whether the struct is passable/passed (this includes struct returning) in registers.
    unsigned __int8             eightByteCount;    // Number of eightbytes for this struct.
    SystemVClassificationType   eightByteClassifications[CLR_SYSTEMV_MAX_EIGHTBYTES_COUNT_TO_PASS_IN_REGISTERS]; // The eightbytes type classification.
    unsigned __int8             eightByteSizes[CLR_SYSTEMV_MAX_EIGHTBYTES_COUNT_TO_PASS_IN_REGISTERS];           // The size of the eightbytes (an eightbyte could include padding. This represents the no padding size of the eightbyte).
    unsigned __int8             eightByteOffsets[CLR_SYSTEMV_MAX_EIGHTBYTES_COUNT_TO_PASS_IN_REGISTERS];         // The start offset of the eightbytes (in bytes).

    // Members

    //------------------------------------------------------------------------
    // CopyFrom: Copies a struct classification into this one.
    //
    // Arguments:
    //    'copyFrom' the struct classification to copy from.
    //
    void CopyFrom(const SYSTEMV_AMD64_CORINFO_STRUCT_REG_PASSING_DESCRIPTOR& copyFrom)
    {
        passedInRegisters = copyFrom.passedInRegisters;
        eightByteCount = copyFrom.eightByteCount;

        for (int i = 0; i < CLR_SYSTEMV_MAX_EIGHTBYTES_COUNT_TO_PASS_IN_REGISTERS; i++)
        {
            eightByteClassifications[i] = copyFrom.eightByteClassifications[i];
            eightByteSizes[i] = copyFrom.eightByteSizes[i];
            eightByteOffsets[i] = copyFrom.eightByteOffsets[i];
        }
    }

    //------------------------------------------------------------------------
    // IsIntegralSlot: Returns whether the eightbyte at slotIndex is of integral type.
    //
    // Arguments:
    //    'slotIndex' the slot number we are determining if it is of integral type.
    //
    // Return value:
    //     returns true if we the eightbyte at index slotIndex is of integral type.
    // 

    bool IsIntegralSlot(unsigned slotIndex) const
    {
        return ((eightByteClassifications[slotIndex] == SystemVClassificationTypeInteger) ||
                (eightByteClassifications[slotIndex] == SystemVClassificationTypeIntegerReference) ||
                (eightByteClassifications[slotIndex] == SystemVClassificationTypeIntegerByRef));
    }

    //------------------------------------------------------------------------
    // IsSseSlot: Returns whether the eightbyte at slotIndex is SSE type.
    //
    // Arguments:
    //    'slotIndex' the slot number we are determining if it is of SSE type.
    //
    // Return value:
    //     returns true if we the eightbyte at index slotIndex is of SSE type.
    // 
    // Follows the rules of the AMD64 System V ABI specification at www.x86-64.org/documentation/abi.pdf.
    // Please refer to it for definitions/examples.
    //
    bool IsSseSlot(unsigned slotIndex) const
    {
        return (eightByteClassifications[slotIndex] == SystemVClassificationTypeSSE);
    }

private:
    void Initialize()
    {
        passedInRegisters = false;
        eightByteCount = 0;

        for (int i = 0; i < CLR_SYSTEMV_MAX_EIGHTBYTES_COUNT_TO_PASS_IN_REGISTERS; i++)
        {
            eightByteClassifications[i] = SystemVClassificationTypeUnknown;
            eightByteSizes[i] = 0;
            eightByteOffsets[i] = 0;
        }
    }
};

// CorInfoHelpFunc defines the set of helpers (accessed via the ICorDynamicInfo::getHelperFtn())
// These helpers can be called by native code which executes in the runtime.
// Compilers can emit calls to these helpers.
//
// The signatures of the helpers are below (see RuntimeHelperArgumentCheck)

enum CorInfoHelpFunc
{
    CORINFO_HELP_UNDEF,         // invalid value. This should never be used

    /* Arithmetic helpers */

    CORINFO_HELP_DIV,           // For the ARM 32-bit integer divide uses a helper call :-(
    CORINFO_HELP_MOD,
    CORINFO_HELP_UDIV,
    CORINFO_HELP_UMOD,

    CORINFO_HELP_LLSH,
    CORINFO_HELP_LRSH,
    CORINFO_HELP_LRSZ,
    CORINFO_HELP_LMUL,
    CORINFO_HELP_LMUL_OVF,
    CORINFO_HELP_ULMUL_OVF,
    CORINFO_HELP_LDIV,
    CORINFO_HELP_LMOD,
    CORINFO_HELP_ULDIV,
    CORINFO_HELP_ULMOD,
    CORINFO_HELP_LNG2DBL,               // Convert a signed int64 to a double
    CORINFO_HELP_ULNG2DBL,              // Convert a unsigned int64 to a double
    CORINFO_HELP_DBL2INT,
    CORINFO_HELP_DBL2INT_OVF,
    CORINFO_HELP_DBL2LNG,
    CORINFO_HELP_DBL2LNG_OVF,
    CORINFO_HELP_DBL2UINT,
    CORINFO_HELP_DBL2UINT_OVF,
    CORINFO_HELP_DBL2ULNG,
    CORINFO_HELP_DBL2ULNG_OVF,
    CORINFO_HELP_FLTREM,
    CORINFO_HELP_DBLREM,
    CORINFO_HELP_FLTROUND,
    CORINFO_HELP_DBLROUND,

    /* Allocating a new object. Always use ICorClassInfo::getNewHelper() to decide 
       which is the right helper to use to allocate an object of a given type. */

    CORINFO_HELP_NEW_CROSSCONTEXT,  // cross context new object
    CORINFO_HELP_NEWFAST,
    CORINFO_HELP_NEWSFAST,          // allocator for small, non-finalizer, non-array object
    CORINFO_HELP_NEWSFAST_FINALIZE, // allocator for small, finalizable, non-array object
    CORINFO_HELP_NEWSFAST_ALIGN8,   // allocator for small, non-finalizer, non-array object, 8 byte aligned
    CORINFO_HELP_NEWSFAST_ALIGN8_VC,// allocator for small, value class, 8 byte aligned
    CORINFO_HELP_NEWSFAST_ALIGN8_FINALIZE, // allocator for small, finalizable, non-array object, 8 byte aligned
    CORINFO_HELP_NEW_MDARR,         // multi-dim array helper (with or without lower bounds - dimensions passed in as vararg)
    CORINFO_HELP_NEW_MDARR_NONVARARG,// multi-dim array helper (with or without lower bounds - dimensions passed in as unmanaged array)
    CORINFO_HELP_NEWARR_1_DIRECT,   // helper for any one dimensional array creation
    CORINFO_HELP_NEWARR_1_R2R_DIRECT, // wrapper for R2R direct call, which extracts method table from ArrayTypeDesc
    CORINFO_HELP_NEWARR_1_OBJ,      // optimized 1-D object arrays
    CORINFO_HELP_NEWARR_1_VC,       // optimized 1-D value class arrays
    CORINFO_HELP_NEWARR_1_ALIGN8,   // like VC, but aligns the array start

    CORINFO_HELP_STRCNS,            // create a new string literal
    CORINFO_HELP_STRCNS_CURRENT_MODULE, // create a new string literal from the current module (used by NGen code)

    /* Object model */

    CORINFO_HELP_INITCLASS,         // Initialize class if not already initialized
    CORINFO_HELP_INITINSTCLASS,     // Initialize class for instantiated type

    // Use ICorClassInfo::getCastingHelper to determine
    // the right helper to use

    CORINFO_HELP_ISINSTANCEOFINTERFACE, // Optimized helper for interfaces
    CORINFO_HELP_ISINSTANCEOFARRAY,  // Optimized helper for arrays
    CORINFO_HELP_ISINSTANCEOFCLASS, // Optimized helper for classes
    CORINFO_HELP_ISINSTANCEOFANY,   // Slow helper for any type

    CORINFO_HELP_CHKCASTINTERFACE,
    CORINFO_HELP_CHKCASTARRAY,
    CORINFO_HELP_CHKCASTCLASS,
    CORINFO_HELP_CHKCASTANY,
    CORINFO_HELP_CHKCASTCLASS_SPECIAL, // Optimized helper for classes. Assumes that the trivial cases 
                                    // has been taken care of by the inlined check

    CORINFO_HELP_BOX,
    CORINFO_HELP_BOX_NULLABLE,      // special form of boxing for Nullable<T>
    CORINFO_HELP_UNBOX,
    CORINFO_HELP_UNBOX_NULLABLE,    // special form of unboxing for Nullable<T>
    CORINFO_HELP_GETREFANY,         // Extract the byref from a TypedReference, checking that it is the expected type

    CORINFO_HELP_ARRADDR_ST,        // assign to element of object array with type-checking
    CORINFO_HELP_LDELEMA_REF,       // does a precise type comparision and returns address

    /* Exceptions */

    CORINFO_HELP_THROW,             // Throw an exception object
    CORINFO_HELP_RETHROW,           // Rethrow the currently active exception
    CORINFO_HELP_USER_BREAKPOINT,   // For a user program to break to the debugger
    CORINFO_HELP_RNGCHKFAIL,        // array bounds check failed
    CORINFO_HELP_OVERFLOW,          // throw an overflow exception
    CORINFO_HELP_THROWDIVZERO,      // throw a divide by zero exception
    CORINFO_HELP_THROWNULLREF,      // throw a null reference exception

    CORINFO_HELP_INTERNALTHROW,     // Support for really fast jit
    CORINFO_HELP_VERIFICATION,      // Throw a VerificationException
    CORINFO_HELP_SEC_UNMGDCODE_EXCPT, // throw a security unmanaged code exception
    CORINFO_HELP_FAIL_FAST,         // Kill the process avoiding any exceptions or stack and data dependencies (use for GuardStack unsafe buffer checks)

    CORINFO_HELP_METHOD_ACCESS_EXCEPTION,//Throw an access exception due to a failed member/class access check.
    CORINFO_HELP_FIELD_ACCESS_EXCEPTION,
    CORINFO_HELP_CLASS_ACCESS_EXCEPTION,

    CORINFO_HELP_ENDCATCH,          // call back into the EE at the end of a catch block

    /* Synchronization */

    CORINFO_HELP_MON_ENTER,
    CORINFO_HELP_MON_EXIT,
    CORINFO_HELP_MON_ENTER_STATIC,
    CORINFO_HELP_MON_EXIT_STATIC,

    CORINFO_HELP_GETCLASSFROMMETHODPARAM, // Given a generics method handle, returns a class handle
    CORINFO_HELP_GETSYNCFROMCLASSHANDLE,  // Given a generics class handle, returns the sync monitor 
                                          // in its ManagedClassObject

    /* Security callout support */
    
    CORINFO_HELP_SECURITY_PROLOG,   // Required if CORINFO_FLG_SECURITYCHECK is set, or CORINFO_FLG_NOSECURITYWRAP is not set
    CORINFO_HELP_SECURITY_PROLOG_FRAMED, // Slow version of CORINFO_HELP_SECURITY_PROLOG. Used for instrumentation.

    CORINFO_HELP_METHOD_ACCESS_CHECK, // Callouts to runtime security access checks
    CORINFO_HELP_FIELD_ACCESS_CHECK,
    CORINFO_HELP_CLASS_ACCESS_CHECK,

    CORINFO_HELP_DELEGATE_SECURITY_CHECK, // Callout to delegate security transparency check

     /* Verification runtime callout support */

    CORINFO_HELP_VERIFICATION_RUNTIME_CHECK, // Do a Demand for UnmanagedCode permission at runtime

    /* GC support */

    CORINFO_HELP_STOP_FOR_GC,       // Call GC (force a GC)
    CORINFO_HELP_POLL_GC,           // Ask GC if it wants to collect

    CORINFO_HELP_STRESS_GC,         // Force a GC, but then update the JITTED code to be a noop call
    CORINFO_HELP_CHECK_OBJ,         // confirm that ECX is a valid object pointer (debugging only)

    /* GC Write barrier support */

    CORINFO_HELP_ASSIGN_REF,        // universal helpers with F_CALL_CONV calling convention
    CORINFO_HELP_CHECKED_ASSIGN_REF,
    CORINFO_HELP_ASSIGN_REF_ENSURE_NONHEAP,  // Do the store, and ensure that the target was not in the heap.

    CORINFO_HELP_ASSIGN_BYREF,
    CORINFO_HELP_ASSIGN_STRUCT,


    /* Accessing fields */

    // For COM object support (using COM get/set routines to update object)
    // and EnC and cross-context support
    CORINFO_HELP_GETFIELD8,
    CORINFO_HELP_SETFIELD8,
    CORINFO_HELP_GETFIELD16,
    CORINFO_HELP_SETFIELD16,
    CORINFO_HELP_GETFIELD32,
    CORINFO_HELP_SETFIELD32,
    CORINFO_HELP_GETFIELD64,
    CORINFO_HELP_SETFIELD64,
    CORINFO_HELP_GETFIELDOBJ,
    CORINFO_HELP_SETFIELDOBJ,
    CORINFO_HELP_GETFIELDSTRUCT,
    CORINFO_HELP_SETFIELDSTRUCT,
    CORINFO_HELP_GETFIELDFLOAT,
    CORINFO_HELP_SETFIELDFLOAT,
    CORINFO_HELP_GETFIELDDOUBLE,
    CORINFO_HELP_SETFIELDDOUBLE,

    CORINFO_HELP_GETFIELDADDR,

    CORINFO_HELP_GETSTATICFIELDADDR_CONTEXT,    // Helper for context-static fields
    CORINFO_HELP_GETSTATICFIELDADDR_TLS,        // Helper for PE TLS fields

    // There are a variety of specialized helpers for accessing static fields. The JIT should use 
    // ICorClassInfo::getSharedStaticsOrCCtorHelper to determine which helper to use

    // Helpers for regular statics
    CORINFO_HELP_GETGENERICS_GCSTATIC_BASE,
    CORINFO_HELP_GETGENERICS_NONGCSTATIC_BASE,
    CORINFO_HELP_GETSHARED_GCSTATIC_BASE,
    CORINFO_HELP_GETSHARED_NONGCSTATIC_BASE,
    CORINFO_HELP_GETSHARED_GCSTATIC_BASE_NOCTOR,
    CORINFO_HELP_GETSHARED_NONGCSTATIC_BASE_NOCTOR,
    CORINFO_HELP_GETSHARED_GCSTATIC_BASE_DYNAMICCLASS,
    CORINFO_HELP_GETSHARED_NONGCSTATIC_BASE_DYNAMICCLASS,
    // Helper to class initialize shared generic with dynamicclass, but not get static field address
    CORINFO_HELP_CLASSINIT_SHARED_DYNAMICCLASS,

    // Helpers for thread statics
    CORINFO_HELP_GETGENERICS_GCTHREADSTATIC_BASE,
    CORINFO_HELP_GETGENERICS_NONGCTHREADSTATIC_BASE,
    CORINFO_HELP_GETSHARED_GCTHREADSTATIC_BASE,
    CORINFO_HELP_GETSHARED_NONGCTHREADSTATIC_BASE,
    CORINFO_HELP_GETSHARED_GCTHREADSTATIC_BASE_NOCTOR,
    CORINFO_HELP_GETSHARED_NONGCTHREADSTATIC_BASE_NOCTOR,
    CORINFO_HELP_GETSHARED_GCTHREADSTATIC_BASE_DYNAMICCLASS,
    CORINFO_HELP_GETSHARED_NONGCTHREADSTATIC_BASE_DYNAMICCLASS,

    /* Debugger */

    CORINFO_HELP_DBG_IS_JUST_MY_CODE,    // Check if this is "JustMyCode" and needs to be stepped through.

    /* Profiling enter/leave probe addresses */
    CORINFO_HELP_PROF_FCN_ENTER,        // record the entry to a method (caller)
    CORINFO_HELP_PROF_FCN_LEAVE,        // record the completion of current method (caller)
    CORINFO_HELP_PROF_FCN_TAILCALL,     // record the completionof current method through tailcall (caller)

    /* Miscellaneous */

    CORINFO_HELP_BBT_FCN_ENTER,         // record the entry to a method for collecting Tuning data

    CORINFO_HELP_PINVOKE_CALLI,         // Indirect pinvoke call
    CORINFO_HELP_TAILCALL,              // Perform a tail call
    
    CORINFO_HELP_GETCURRENTMANAGEDTHREADID,

    CORINFO_HELP_INIT_PINVOKE_FRAME,   // initialize an inlined PInvoke Frame for the JIT-compiler

    CORINFO_HELP_MEMSET,                // Init block of memory
    CORINFO_HELP_MEMCPY,                // Copy block of memory

    CORINFO_HELP_RUNTIMEHANDLE_METHOD,          // determine a type/field/method handle at run-time
    CORINFO_HELP_RUNTIMEHANDLE_METHOD_LOG,      // determine a type/field/method handle at run-time, with IBC logging
    CORINFO_HELP_RUNTIMEHANDLE_CLASS,           // determine a type/field/method handle at run-time
    CORINFO_HELP_RUNTIMEHANDLE_CLASS_LOG,       // determine a type/field/method handle at run-time, with IBC logging

    CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE, // Convert from a TypeHandle (native structure pointer) to RuntimeType at run-time
    CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE_MAYBENULL, // Convert from a TypeHandle (native structure pointer) to RuntimeType at run-time, the type may be null
    CORINFO_HELP_METHODDESC_TO_STUBRUNTIMEMETHOD, // Convert from a MethodDesc (native structure pointer) to RuntimeMethodHandle at run-time
    CORINFO_HELP_FIELDDESC_TO_STUBRUNTIMEFIELD, // Convert from a FieldDesc (native structure pointer) to RuntimeFieldHandle at run-time
    CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPEHANDLE, // Convert from a TypeHandle (native structure pointer) to RuntimeTypeHandle at run-time
    CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPEHANDLE_MAYBENULL, // Convert from a TypeHandle (native structure pointer) to RuntimeTypeHandle at run-time, handle might point to a null type

    CORINFO_HELP_ARE_TYPES_EQUIVALENT, // Check whether two TypeHandles (native structure pointers) are equivalent

    CORINFO_HELP_VIRTUAL_FUNC_PTR,      // look up a virtual method at run-time
    //CORINFO_HELP_VIRTUAL_FUNC_PTR_LOG,  // look up a virtual method at run-time, with IBC logging

    // Not a real helpers. Instead of taking handle arguments, these helpers point to a small stub that loads the handle argument and calls the static helper.
    CORINFO_HELP_READYTORUN_NEW,
    CORINFO_HELP_READYTORUN_NEWARR_1,
    CORINFO_HELP_READYTORUN_ISINSTANCEOF,
    CORINFO_HELP_READYTORUN_CHKCAST,
    CORINFO_HELP_READYTORUN_STATIC_BASE,
    CORINFO_HELP_READYTORUN_VIRTUAL_FUNC_PTR,
    CORINFO_HELP_READYTORUN_GENERIC_HANDLE,
    CORINFO_HELP_READYTORUN_DELEGATE_CTOR,
    CORINFO_HELP_READYTORUN_GENERIC_STATIC_BASE,

    CORINFO_HELP_EE_PRESTUB,            // Not real JIT helper. Used in native images.

    CORINFO_HELP_EE_PRECODE_FIXUP,      // Not real JIT helper. Used for Precode fixup in native images.
    CORINFO_HELP_EE_PINVOKE_FIXUP,      // Not real JIT helper. Used for PInvoke target fixup in native images.
    CORINFO_HELP_EE_VSD_FIXUP,          // Not real JIT helper. Used for VSD cell fixup in native images.
    CORINFO_HELP_EE_EXTERNAL_FIXUP,     // Not real JIT helper. Used for to fixup external method thunks in native images.
    CORINFO_HELP_EE_VTABLE_FIXUP,       // Not real JIT helper. Used for inherited vtable slot fixup in native images.

    CORINFO_HELP_EE_REMOTING_THUNK,     // Not real JIT helper. Used for remoting precode in native images.

    CORINFO_HELP_EE_PERSONALITY_ROUTINE,// Not real JIT helper. Used in native images.
    CORINFO_HELP_EE_PERSONALITY_ROUTINE_FILTER_FUNCLET,// Not real JIT helper. Used in native images to detect filter funclets.

    // ASSIGN_REF_EAX - CHECKED_ASSIGN_REF_EBP: NOGC_WRITE_BARRIERS JIT helper calls
    //
    // For unchecked versions EDX is required to point into GC heap.
    //
    // NOTE: these helpers are only used for x86.
    CORINFO_HELP_ASSIGN_REF_EAX,    // EAX holds GC ptr, do a 'mov [EDX], EAX' and inform GC
    CORINFO_HELP_ASSIGN_REF_EBX,    // EBX holds GC ptr, do a 'mov [EDX], EBX' and inform GC
    CORINFO_HELP_ASSIGN_REF_ECX,    // ECX holds GC ptr, do a 'mov [EDX], ECX' and inform GC
    CORINFO_HELP_ASSIGN_REF_ESI,    // ESI holds GC ptr, do a 'mov [EDX], ESI' and inform GC
    CORINFO_HELP_ASSIGN_REF_EDI,    // EDI holds GC ptr, do a 'mov [EDX], EDI' and inform GC
    CORINFO_HELP_ASSIGN_REF_EBP,    // EBP holds GC ptr, do a 'mov [EDX], EBP' and inform GC

    CORINFO_HELP_CHECKED_ASSIGN_REF_EAX,  // These are the same as ASSIGN_REF above ...
    CORINFO_HELP_CHECKED_ASSIGN_REF_EBX,  // ... but also check if EDX points into heap.
    CORINFO_HELP_CHECKED_ASSIGN_REF_ECX,
    CORINFO_HELP_CHECKED_ASSIGN_REF_ESI,
    CORINFO_HELP_CHECKED_ASSIGN_REF_EDI,
    CORINFO_HELP_CHECKED_ASSIGN_REF_EBP,

    CORINFO_HELP_LOOP_CLONE_CHOICE_ADDR, // Return the reference to a counter to decide to take cloned path in debug stress.
    CORINFO_HELP_DEBUG_LOG_LOOP_CLONING, // Print a message that a loop cloning optimization has occurred in debug mode.

    CORINFO_HELP_THROW_ARGUMENTEXCEPTION,           // throw ArgumentException
    CORINFO_HELP_THROW_ARGUMENTOUTOFRANGEEXCEPTION, // throw ArgumentOutOfRangeException
    CORINFO_HELP_THROW_NOT_IMPLEMENTED,             // throw NotImplementedException
    CORINFO_HELP_THROW_PLATFORM_NOT_SUPPORTED,      // throw PlatformNotSupportedException
    CORINFO_HELP_THROW_TYPE_NOT_SUPPORTED,          // throw TypeNotSupportedException

    CORINFO_HELP_JIT_PINVOKE_BEGIN, // Transition to preemptive mode before a P/Invoke, frame is the first argument
    CORINFO_HELP_JIT_PINVOKE_END,   // Transition to cooperative mode after a P/Invoke, frame is the first argument

    CORINFO_HELP_JIT_REVERSE_PINVOKE_ENTER, // Transition to cooperative mode in reverse P/Invoke prolog, frame is the first argument
    CORINFO_HELP_JIT_REVERSE_PINVOKE_EXIT,  // Transition to preemptive mode in reverse P/Invoke epilog, frame is the first argument

    CORINFO_HELP_GVMLOOKUP_FOR_SLOT,        // Resolve a generic virtual method target from this pointer and runtime method handle 

    CORINFO_HELP_STACK_PROBE,               // Probes each page of the allocated stack frame

    CORINFO_HELP_COUNT,
};

#define CORINFO_HELP_READYTORUN_ATYPICAL_CALLSITE 0x40000000

//This describes the signature for a helper method.
enum CorInfoHelpSig
{
    CORINFO_HELP_SIG_UNDEF,
    CORINFO_HELP_SIG_NO_ALIGN_STUB,
    CORINFO_HELP_SIG_NO_UNWIND_STUB,
    CORINFO_HELP_SIG_REG_ONLY,
    CORINFO_HELP_SIG_4_STACK,
    CORINFO_HELP_SIG_8_STACK,
    CORINFO_HELP_SIG_12_STACK,
    CORINFO_HELP_SIG_16_STACK,
    CORINFO_HELP_SIG_8_VA, //2 arguments plus varargs

    CORINFO_HELP_SIG_EBPCALL, //special calling convention that uses EDX and
                              //EBP as arguments

    CORINFO_HELP_SIG_CANNOT_USE_ALIGN_STUB,

    CORINFO_HELP_SIG_COUNT
};

// The enumeration is returned in 'getSig','getType', getArgType methods
enum CorInfoType
{
    CORINFO_TYPE_UNDEF           = 0x0,
    CORINFO_TYPE_VOID            = 0x1,
    CORINFO_TYPE_BOOL            = 0x2,
    CORINFO_TYPE_CHAR            = 0x3,
    CORINFO_TYPE_BYTE            = 0x4,
    CORINFO_TYPE_UBYTE           = 0x5,
    CORINFO_TYPE_SHORT           = 0x6,
    CORINFO_TYPE_USHORT          = 0x7,
    CORINFO_TYPE_INT             = 0x8,
    CORINFO_TYPE_UINT            = 0x9,
    CORINFO_TYPE_LONG            = 0xa,
    CORINFO_TYPE_ULONG           = 0xb,
    CORINFO_TYPE_NATIVEINT       = 0xc,
    CORINFO_TYPE_NATIVEUINT      = 0xd,
    CORINFO_TYPE_FLOAT           = 0xe,
    CORINFO_TYPE_DOUBLE          = 0xf,
    CORINFO_TYPE_STRING          = 0x10,         // Not used, should remove
    CORINFO_TYPE_PTR             = 0x11,
    CORINFO_TYPE_BYREF           = 0x12,
    CORINFO_TYPE_VALUECLASS      = 0x13,
    CORINFO_TYPE_CLASS           = 0x14,
    CORINFO_TYPE_REFANY          = 0x15,

    // CORINFO_TYPE_VAR is for a generic type variable.
    // Generic type variables only appear when the JIT is doing
    // verification (not NOT compilation) of generic code
    // for the EE, in which case we're running
    // the JIT in "import only" mode.

    CORINFO_TYPE_VAR             = 0x16,
    CORINFO_TYPE_COUNT,                         // number of jit types
};

enum CorInfoTypeWithMod
{
    CORINFO_TYPE_MASK            = 0x3F,        // lower 6 bits are type mask
    CORINFO_TYPE_MOD_PINNED      = 0x40,        // can be applied to CLASS, or BYREF to indiate pinned
};

inline CorInfoType strip(CorInfoTypeWithMod val) {
    return CorInfoType(val & CORINFO_TYPE_MASK);
}

// The enumeration is returned in 'getSig'

enum CorInfoCallConv
{
    // These correspond to CorCallingConvention

    CORINFO_CALLCONV_DEFAULT    = 0x0,
    CORINFO_CALLCONV_C          = 0x1,
    CORINFO_CALLCONV_STDCALL    = 0x2,
    CORINFO_CALLCONV_THISCALL   = 0x3,
    CORINFO_CALLCONV_FASTCALL   = 0x4,
    CORINFO_CALLCONV_VARARG     = 0x5,
    CORINFO_CALLCONV_FIELD      = 0x6,
    CORINFO_CALLCONV_LOCAL_SIG  = 0x7,
    CORINFO_CALLCONV_PROPERTY   = 0x8,
    CORINFO_CALLCONV_NATIVEVARARG = 0xb,    // used ONLY for IL stub PInvoke vararg calls

    CORINFO_CALLCONV_MASK       = 0x0f,     // Calling convention is bottom 4 bits
    CORINFO_CALLCONV_GENERIC    = 0x10,
    CORINFO_CALLCONV_HASTHIS    = 0x20,
    CORINFO_CALLCONV_EXPLICITTHIS=0x40,
    CORINFO_CALLCONV_PARAMTYPE  = 0x80,     // Passed last. Same as CORINFO_GENERICS_CTXT_FROM_PARAMTYPEARG
};

#ifdef UNIX_X86_ABI
inline bool IsCallerPop(CorInfoCallConv callConv)
{
    unsigned int umask = CORINFO_CALLCONV_STDCALL
                       | CORINFO_CALLCONV_THISCALL
                       | CORINFO_CALLCONV_FASTCALL;

    return !(callConv & umask);
}
#endif // UNIX_X86_ABI

enum CorInfoUnmanagedCallConv
{
    // These correspond to CorUnmanagedCallingConvention

    CORINFO_UNMANAGED_CALLCONV_UNKNOWN,
    CORINFO_UNMANAGED_CALLCONV_C,
    CORINFO_UNMANAGED_CALLCONV_STDCALL,
    CORINFO_UNMANAGED_CALLCONV_THISCALL,
    CORINFO_UNMANAGED_CALLCONV_FASTCALL
};

// These are returned from getMethodOptions
enum CorInfoOptions
{
    CORINFO_OPT_INIT_LOCALS                 = 0x00000010, // zero initialize all variables

    CORINFO_GENERICS_CTXT_FROM_THIS         = 0x00000020, // is this shared generic code that access the generic context from the this pointer?  If so, then if the method has SEH then the 'this' pointer must always be reported and kept alive.
    CORINFO_GENERICS_CTXT_FROM_METHODDESC   = 0x00000040, // is this shared generic code that access the generic context from the ParamTypeArg(that is a MethodDesc)?  If so, then if the method has SEH then the 'ParamTypeArg' must always be reported and kept alive. Same as CORINFO_CALLCONV_PARAMTYPE
    CORINFO_GENERICS_CTXT_FROM_METHODTABLE  = 0x00000080, // is this shared generic code that access the generic context from the ParamTypeArg(that is a MethodTable)?  If so, then if the method has SEH then the 'ParamTypeArg' must always be reported and kept alive. Same as CORINFO_CALLCONV_PARAMTYPE
    CORINFO_GENERICS_CTXT_MASK              = (CORINFO_GENERICS_CTXT_FROM_THIS |
                                               CORINFO_GENERICS_CTXT_FROM_METHODDESC |
                                               CORINFO_GENERICS_CTXT_FROM_METHODTABLE),
    CORINFO_GENERICS_CTXT_KEEP_ALIVE        = 0x00000100, // Keep the generics context alive throughout the method even if there is no explicit use, and report its location to the CLR

};

//
// what type of code region we are in
//
enum CorInfoRegionKind
{
    CORINFO_REGION_NONE,
    CORINFO_REGION_HOT,
    CORINFO_REGION_COLD,
    CORINFO_REGION_JIT,
};


// these are the attribute flags for fields and methods (getMethodAttribs)
enum CorInfoFlag
{
//  CORINFO_FLG_UNUSED                = 0x00000001,
//  CORINFO_FLG_UNUSED                = 0x00000002,
    CORINFO_FLG_PROTECTED             = 0x00000004,
    CORINFO_FLG_STATIC                = 0x00000008,
    CORINFO_FLG_FINAL                 = 0x00000010,
    CORINFO_FLG_SYNCH                 = 0x00000020,
    CORINFO_FLG_VIRTUAL               = 0x00000040,
//  CORINFO_FLG_UNUSED                = 0x00000080,
    CORINFO_FLG_NATIVE                = 0x00000100,
    CORINFO_FLG_INTRINSIC_TYPE        = 0x00000200, // This type is marked by [Intrinsic]
    CORINFO_FLG_ABSTRACT              = 0x00000400,

    CORINFO_FLG_EnC                   = 0x00000800, // member was added by Edit'n'Continue

    // These are internal flags that can only be on methods
    CORINFO_FLG_FORCEINLINE           = 0x00010000, // The method should be inlined if possible.
    CORINFO_FLG_SHAREDINST            = 0x00020000, // the code for this method is shared between different generic instantiations (also set on classes/types)
    CORINFO_FLG_DELEGATE_INVOKE       = 0x00040000, // "Delegate
    CORINFO_FLG_PINVOKE               = 0x00080000, // Is a P/Invoke call
    CORINFO_FLG_SECURITYCHECK         = 0x00100000, // Is one of the security routines that does a stackwalk (e.g. Assert, Demand)
    CORINFO_FLG_NOGCCHECK             = 0x00200000, // This method is FCALL that has no GC check.  Don't put alone in loops
    CORINFO_FLG_INTRINSIC             = 0x00400000, // This method MAY have an intrinsic ID
    CORINFO_FLG_CONSTRUCTOR           = 0x00800000, // This method is an instance or type initializer
    CORINFO_FLG_AGGRESSIVE_OPT        = 0x01000000, // The method may contain hot code and should be aggressively optimized if possible
    CORINFO_FLG_DISABLE_TIER0_FOR_LOOPS = 0x02000000, // Indicates that tier 0 JIT should not be used for a method that contains a loop
    CORINFO_FLG_NOSECURITYWRAP        = 0x04000000, // The method requires no security checks
    CORINFO_FLG_DONT_INLINE           = 0x10000000, // The method should not be inlined
    CORINFO_FLG_DONT_INLINE_CALLER    = 0x20000000, // The method should not be inlined, nor should its callers. It cannot be tail called.
    CORINFO_FLG_JIT_INTRINSIC         = 0x40000000, // Method is a potential jit intrinsic; verify identity by name check

    // These are internal flags that can only be on Classes
    CORINFO_FLG_VALUECLASS            = 0x00010000, // is the class a value class
//  This flag is define din the Methods section, but is also valid on classes.
//  CORINFO_FLG_SHAREDINST            = 0x00020000, // This class is satisfies TypeHandle::IsCanonicalSubtype
    CORINFO_FLG_VAROBJSIZE            = 0x00040000, // the object size varies depending of constructor args
    CORINFO_FLG_ARRAY                 = 0x00080000, // class is an array class (initialized differently)
    CORINFO_FLG_OVERLAPPING_FIELDS    = 0x00100000, // struct or class has fields that overlap (aka union)
    CORINFO_FLG_INTERFACE             = 0x00200000, // it is an interface
    CORINFO_FLG_CONTEXTFUL            = 0x00400000, // is this a contextful class?
    CORINFO_FLG_CUSTOMLAYOUT          = 0x00800000, // does this struct have custom layout?
    CORINFO_FLG_CONTAINS_GC_PTR       = 0x01000000, // does the class contain a gc ptr ?
    CORINFO_FLG_DELEGATE              = 0x02000000, // is this a subclass of delegate or multicast delegate ?
    CORINFO_FLG_MARSHAL_BYREF         = 0x04000000, // is this a subclass of MarshalByRef ?
    CORINFO_FLG_CONTAINS_STACK_PTR    = 0x08000000, // This class has a stack pointer inside it
    CORINFO_FLG_VARIANCE              = 0x10000000, // MethodTable::HasVariance (sealed does *not* mean uncast-able)
    CORINFO_FLG_BEFOREFIELDINIT       = 0x20000000, // Additional flexibility for when to run .cctor (see code:#ClassConstructionFlags)
    CORINFO_FLG_GENERIC_TYPE_VARIABLE = 0x40000000, // This is really a handle for a variable type
    CORINFO_FLG_UNSAFE_VALUECLASS     = 0x80000000, // Unsafe (C++'s /GS) value type
};

// Flags computed by a runtime compiler
enum CorInfoMethodRuntimeFlags
{
    CORINFO_FLG_BAD_INLINEE         = 0x00000001, // The method is not suitable for inlining
    CORINFO_FLG_VERIFIABLE          = 0x00000002, // The method has verifiable code
    CORINFO_FLG_UNVERIFIABLE        = 0x00000004, // The method has unverifiable code
    CORINFO_FLG_SWITCHED_TO_MIN_OPT = 0x00000008, // The JIT decided to switch to MinOpt for this method, when it was not requested
    CORINFO_FLG_SWITCHED_TO_OPTIMIZED = 0x00000010, // The JIT decided to switch to tier 1 for this method, when a different tier was requested
};


enum CORINFO_ACCESS_FLAGS
{
    CORINFO_ACCESS_ANY        = 0x0000, // Normal access
    CORINFO_ACCESS_THIS       = 0x0001, // Accessed via the this reference
    CORINFO_ACCESS_UNWRAP     = 0x0002, // Accessed via an unwrap reference

    CORINFO_ACCESS_NONNULL    = 0x0004, // Instance is guaranteed non-null

    CORINFO_ACCESS_LDFTN      = 0x0010, // Accessed via ldftn

    // Field access flags
    CORINFO_ACCESS_GET        = 0x0100, // Field get (ldfld)
    CORINFO_ACCESS_SET        = 0x0200, // Field set (stfld)
    CORINFO_ACCESS_ADDRESS    = 0x0400, // Field address (ldflda)
    CORINFO_ACCESS_INIT_ARRAY = 0x0800, // Field use for InitializeArray
    CORINFO_ACCESS_ATYPICAL_CALLSITE = 0x4000, // Atypical callsite that cannot be disassembled by delay loading helper
    CORINFO_ACCESS_INLINECHECK= 0x8000, // Return fieldFlags and fieldAccessor only. Used by JIT64 during inlining.
};

// These are the flags set on an CORINFO_EH_CLAUSE
enum CORINFO_EH_CLAUSE_FLAGS
{
    CORINFO_EH_CLAUSE_NONE      = 0,
    CORINFO_EH_CLAUSE_FILTER    = 0x0001, // If this bit is on, then this EH entry is for a filter
    CORINFO_EH_CLAUSE_FINALLY   = 0x0002, // This clause is a finally clause
    CORINFO_EH_CLAUSE_FAULT     = 0x0004, // This clause is a fault clause
    CORINFO_EH_CLAUSE_DUPLICATE = 0x0008, // Duplicated clause. This clause was duplicated to a funclet which was pulled out of line
    CORINFO_EH_CLAUSE_SAMETRY   = 0x0010, // This clause covers same try block as the previous one. (Used by CoreRT ABI.)
};

// This enumeration is passed to InternalThrow
enum CorInfoException
{
    CORINFO_NullReferenceException,
    CORINFO_DivideByZeroException,
    CORINFO_InvalidCastException,
    CORINFO_IndexOutOfRangeException,
    CORINFO_OverflowException,
    CORINFO_SynchronizationLockException,
    CORINFO_ArrayTypeMismatchException,
    CORINFO_RankException,
    CORINFO_ArgumentNullException,
    CORINFO_ArgumentException,
    CORINFO_Exception_Count,
};


// This enumeration is returned by getIntrinsicID. Methods corresponding to
// these values will have "well-known" specified behavior. Calls to these
// methods could be replaced with inlined code corresponding to the
// specified behavior (without having to examine the IL beforehand).

enum CorInfoIntrinsics
{
    CORINFO_INTRINSIC_Sin,
    CORINFO_INTRINSIC_Cos,
    CORINFO_INTRINSIC_Cbrt,
    CORINFO_INTRINSIC_Sqrt,
    CORINFO_INTRINSIC_Abs,
    CORINFO_INTRINSIC_Round,
    CORINFO_INTRINSIC_Cosh,
    CORINFO_INTRINSIC_Sinh,
    CORINFO_INTRINSIC_Tan,
    CORINFO_INTRINSIC_Tanh,
    CORINFO_INTRINSIC_Asin,
    CORINFO_INTRINSIC_Asinh,
    CORINFO_INTRINSIC_Acos,
    CORINFO_INTRINSIC_Acosh,
    CORINFO_INTRINSIC_Atan,
    CORINFO_INTRINSIC_Atan2,
    CORINFO_INTRINSIC_Atanh,
    CORINFO_INTRINSIC_Log10,
    CORINFO_INTRINSIC_Pow,
    CORINFO_INTRINSIC_Exp,
    CORINFO_INTRINSIC_Ceiling,
    CORINFO_INTRINSIC_Floor,
    CORINFO_INTRINSIC_GetChar,              // fetch character out of string
    CORINFO_INTRINSIC_Array_GetDimLength,   // Get number of elements in a given dimension of an array
    CORINFO_INTRINSIC_Array_Get,            // Get the value of an element in an array
    CORINFO_INTRINSIC_Array_Address,        // Get the address of an element in an array
    CORINFO_INTRINSIC_Array_Set,            // Set the value of an element in an array
    CORINFO_INTRINSIC_StringGetChar,        // fetch character out of string
    CORINFO_INTRINSIC_StringLength,         // get the length
    CORINFO_INTRINSIC_InitializeArray,      // initialize an array from static data
    CORINFO_INTRINSIC_GetTypeFromHandle,
    CORINFO_INTRINSIC_RTH_GetValueInternal,
    CORINFO_INTRINSIC_TypeEQ,
    CORINFO_INTRINSIC_TypeNEQ,
    CORINFO_INTRINSIC_Object_GetType,
    CORINFO_INTRINSIC_StubHelpers_GetStubContext,
    CORINFO_INTRINSIC_StubHelpers_GetStubContextAddr,
    CORINFO_INTRINSIC_StubHelpers_GetNDirectTarget,
    CORINFO_INTRINSIC_InterlockedAdd32,
    CORINFO_INTRINSIC_InterlockedAdd64,
    CORINFO_INTRINSIC_InterlockedXAdd32,
    CORINFO_INTRINSIC_InterlockedXAdd64,
    CORINFO_INTRINSIC_InterlockedXchg32,
    CORINFO_INTRINSIC_InterlockedXchg64,
    CORINFO_INTRINSIC_InterlockedCmpXchg32,
    CORINFO_INTRINSIC_InterlockedCmpXchg64,
    CORINFO_INTRINSIC_MemoryBarrier,
    CORINFO_INTRINSIC_GetCurrentManagedThread,
    CORINFO_INTRINSIC_GetManagedThreadId,
    CORINFO_INTRINSIC_ByReference_Ctor,
    CORINFO_INTRINSIC_ByReference_Value,
    CORINFO_INTRINSIC_Span_GetItem,
    CORINFO_INTRINSIC_ReadOnlySpan_GetItem,
    CORINFO_INTRINSIC_GetRawHandle,

    CORINFO_INTRINSIC_Count,
    CORINFO_INTRINSIC_Illegal = -1,         // Not a true intrinsic,
};

// Can a value be accessed directly from JITed code.
enum InfoAccessType
{
    IAT_VALUE,      // The info value is directly available
    IAT_PVALUE,     // The value needs to be accessed via an         indirection
    IAT_PPVALUE,    // The value needs to be accessed via a double   indirection
    IAT_RELPVALUE   // The value needs to be accessed via a relative indirection
};

enum CorInfoGCType
{
    TYPE_GC_NONE,   // no embedded objectrefs
    TYPE_GC_REF,    // Is an object ref
    TYPE_GC_BYREF,  // Is an interior pointer - promote it but don't scan it
    TYPE_GC_OTHER   // requires type-specific treatment
};

enum CorInfoClassId
{
    CLASSID_SYSTEM_OBJECT,
    CLASSID_TYPED_BYREF,
    CLASSID_TYPE_HANDLE,
    CLASSID_FIELD_HANDLE,
    CLASSID_METHOD_HANDLE,
    CLASSID_STRING,
    CLASSID_ARGUMENT_HANDLE,
    CLASSID_RUNTIME_TYPE,
};

enum CorInfoInline
{
    INLINE_PASS                 = 0,    // Inlining OK

    // failures are negative
    INLINE_FAIL                 = -1,   // Inlining not OK for this case only
    INLINE_NEVER                = -2,   // This method should never be inlined, regardless of context
};

enum CorInfoInlineRestrictions
{
    INLINE_RESPECT_BOUNDARY = 0x00000001, // You can inline if there are no calls from the method being inlined
    INLINE_NO_CALLEE_LDSTR  = 0x00000002, // You can inline only if you guarantee that if inlinee does an ldstr
                                          // inlinee's module will never see that string (by any means).
                                          // This is due to how we implement the NoStringInterningAttribute
                                          // (by reusing the fixup table).
    INLINE_SAME_THIS        = 0x00000004, // You can inline only if the callee is on the same this reference as caller
};

enum CorInfoInlineTypeCheck
{
    CORINFO_INLINE_TYPECHECK_NONE       = 0x00000000, // It's not okay to compare type's vtable with a native type handle
    CORINFO_INLINE_TYPECHECK_PASS       = 0x00000001, // It's okay to compare type's vtable with a native type handle
    CORINFO_INLINE_TYPECHECK_USE_HELPER = 0x00000002, // Use a specialized helper to compare type's vtable with native type handle
};

enum CorInfoInlineTypeCheckSource
{
    CORINFO_INLINE_TYPECHECK_SOURCE_VTABLE = 0x00000000, // Type handle comes from the vtable
    CORINFO_INLINE_TYPECHECK_SOURCE_TOKEN  = 0x00000001, // Type handle comes from an ldtoken
};

// If you add more values here, keep it in sync with TailCallTypeMap in ..\vm\ClrEtwAll.man
// and the string enum in CEEInfo::reportTailCallDecision in ..\vm\JITInterface.cpp
enum CorInfoTailCall
{
    TAILCALL_OPTIMIZED      = 0,    // Optimized tail call (epilog + jmp)
    TAILCALL_RECURSIVE      = 1,    // Optimized into a loop (only when a method tail calls itself)
    TAILCALL_HELPER         = 2,    // Helper assisted tail call (call to JIT_TailCall)

    // failures are negative
    TAILCALL_FAIL           = -1,   // Couldn't do a tail call
};

enum CorInfoCanSkipVerificationResult
{
    CORINFO_VERIFICATION_CANNOT_SKIP    = 0,    // Cannot skip verification during jit time.
    CORINFO_VERIFICATION_CAN_SKIP       = 1,    // Can skip verification during jit time.
    CORINFO_VERIFICATION_RUNTIME_CHECK  = 2,    // Cannot skip verification during jit time,
                                                //     but need to insert a callout to the VM to ask during runtime 
                                                //     whether to raise a verification or not (if the method is unverifiable).
    CORINFO_VERIFICATION_DONT_JIT       = 3,    // Cannot skip verification during jit time,
                                                //     but do not jit the method if is is unverifiable.
};

enum CorInfoInitClassResult
{
    CORINFO_INITCLASS_NOT_REQUIRED  = 0x00, // No class initialization required, but the class is not actually initialized yet 
                                            // (e.g. we are guaranteed to run the static constructor in method prolog)
    CORINFO_INITCLASS_INITIALIZED   = 0x01, // Class initialized
    CORINFO_INITCLASS_SPECULATIVE   = 0x02, // Class may be initialized speculatively
    CORINFO_INITCLASS_USE_HELPER    = 0x04, // The JIT must insert class initialization helper call.
    CORINFO_INITCLASS_DONT_INLINE   = 0x08, // The JIT should not inline the method requesting the class initialization. The class 
                                            // initialization requires helper class now, but will not require initialization 
                                            // if the method is compiled standalone. Or the method cannot be inlined due to some
                                            // requirement around class initialization such as shared generics.
};

// Reason codes for making indirect calls
#define INDIRECT_CALL_REASONS() \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_UNKNOWN) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_EXOTIC) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_PINVOKE) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_GENERIC) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_NO_CODE) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_FIXUPS) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_STUB) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_REMOTING) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_CER) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_RESTORE_METHOD) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_RESTORE_FIRST_CALL) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_RESTORE_VALUE_TYPE) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_RESTORE) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_CANT_PATCH) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_PROFILING) \
    INDIRECT_CALL_REASON_FUNC(CORINFO_INDIRECT_CALL_OTHER_LOADER_MODULE) \

enum CorInfoIndirectCallReason
{
    #undef INDIRECT_CALL_REASON_FUNC
    #define INDIRECT_CALL_REASON_FUNC(x) x,
    INDIRECT_CALL_REASONS()

    #undef INDIRECT_CALL_REASON_FUNC

    CORINFO_INDIRECT_CALL_COUNT
};

// This is for use when the JIT is compiling an instantiation
// of generic code.  The JIT needs to know if the generic code itself
// (which can be verified once and for all independently of the
// instantiations) passed verification.
enum CorInfoInstantiationVerification
{
    // The method is NOT a concrete instantiation (eg. List<int>.Add()) of a method 
    // in a generic class or a generic method. It is either the typical instantiation 
    // (eg. List<T>.Add()) or entirely non-generic.
    INSTVER_NOT_INSTANTIATION           = 0,

    // The method is an instantiation of a method in a generic class or a generic method, 
    // and the generic class was successfully verified
    INSTVER_GENERIC_PASSED_VERIFICATION = 1,

    // The method is an instantiation of a method in a generic class or a generic method, 
    // and the generic class failed verification
    INSTVER_GENERIC_FAILED_VERIFICATION = 2,
};

// When using CORINFO_HELPER_TAILCALL, the JIT needs to pass certain special
// calling convention/argument passing/handling details to the helper
enum CorInfoHelperTailCallSpecialHandling
{
    CORINFO_TAILCALL_NORMAL =               0x00000000,
    CORINFO_TAILCALL_STUB_DISPATCH_ARG =    0x00000001,
};


inline bool dontInline(CorInfoInline val) {
    return(val < 0);
}

// Cookie types consumed by the code generator (these are opaque values
// not inspected by the code generator):

typedef struct CORINFO_ASSEMBLY_STRUCT_*    CORINFO_ASSEMBLY_HANDLE;
typedef struct CORINFO_MODULE_STRUCT_*      CORINFO_MODULE_HANDLE;
typedef struct CORINFO_DEPENDENCY_STRUCT_*  CORINFO_DEPENDENCY_HANDLE;
typedef struct CORINFO_CLASS_STRUCT_*       CORINFO_CLASS_HANDLE;
typedef struct CORINFO_METHOD_STRUCT_*      CORINFO_METHOD_HANDLE;
typedef struct CORINFO_FIELD_STRUCT_*       CORINFO_FIELD_HANDLE;
typedef struct CORINFO_ARG_LIST_STRUCT_*    CORINFO_ARG_LIST_HANDLE;    // represents a list of argument types
typedef struct CORINFO_JUST_MY_CODE_HANDLE_*CORINFO_JUST_MY_CODE_HANDLE;
typedef struct CORINFO_PROFILING_STRUCT_*   CORINFO_PROFILING_HANDLE;   // a handle guaranteed to be unique per process
typedef struct CORINFO_GENERIC_STRUCT_*     CORINFO_GENERIC_HANDLE;     // a generic handle (could be any of the above)

// what is actually passed on the varargs call
typedef struct CORINFO_VarArgInfo *         CORINFO_VARARGS_HANDLE;

// Generic tokens are resolved with respect to a context, which is usually the method
// being compiled. The CORINFO_CONTEXT_HANDLE indicates which exact instantiation
// (or the open instantiation) is being referred to.
// CORINFO_CONTEXT_HANDLE is more tightly scoped than CORINFO_MODULE_HANDLE. For cases 
// where the exact instantiation does not matter, CORINFO_MODULE_HANDLE is used.
typedef CORINFO_METHOD_HANDLE               CORINFO_CONTEXT_HANDLE;

typedef struct CORINFO_DEPENDENCY_STRUCT_
{
    CORINFO_MODULE_HANDLE moduleFrom;
    CORINFO_MODULE_HANDLE moduleTo; 
} CORINFO_DEPENDENCY;

// Bit-twiddling of contexts assumes word-alignment of method handles and type handles
// If this ever changes, some other encoding will be needed
enum CorInfoContextFlags
{
    CORINFO_CONTEXTFLAGS_METHOD = 0x00, // CORINFO_CONTEXT_HANDLE is really a CORINFO_METHOD_HANDLE
    CORINFO_CONTEXTFLAGS_CLASS  = 0x01, // CORINFO_CONTEXT_HANDLE is really a CORINFO_CLASS_HANDLE
    CORINFO_CONTEXTFLAGS_MASK   = 0x01
};

#define MAKE_CLASSCONTEXT(c)  (CORINFO_CONTEXT_HANDLE((size_t) (c) | CORINFO_CONTEXTFLAGS_CLASS))
#define MAKE_METHODCONTEXT(m) (CORINFO_CONTEXT_HANDLE((size_t) (m) | CORINFO_CONTEXTFLAGS_METHOD))

enum CorInfoSigInfoFlags
{
    CORINFO_SIGFLAG_IS_LOCAL_SIG = 0x01,
    CORINFO_SIGFLAG_IL_STUB      = 0x02,
};

struct CORINFO_SIG_INST
{
    unsigned                classInstCount;
    CORINFO_CLASS_HANDLE *  classInst; // (representative, not exact) instantiation for class type variables in signature
    unsigned                methInstCount;
    CORINFO_CLASS_HANDLE *  methInst; // (representative, not exact) instantiation for method type variables in signature
};

struct CORINFO_SIG_INFO
{
    CorInfoCallConv         callConv;
    CORINFO_CLASS_HANDLE    retTypeClass;   // if the return type is a value class, this is its handle (enums are normalized)
    CORINFO_CLASS_HANDLE    retTypeSigClass;// returns the value class as it is in the sig (enums are not converted to primitives)
    CorInfoType             retType : 8;
    unsigned                flags   : 8;    // used by IL stubs code
    unsigned                numArgs : 16;
    struct CORINFO_SIG_INST sigInst;  // information about how type variables are being instantiated in generic code
    CORINFO_ARG_LIST_HANDLE args;
    PCCOR_SIGNATURE         pSig;
    unsigned                cbSig;
    CORINFO_MODULE_HANDLE   scope;          // passed to getArgClass
    mdToken                 token;

    CorInfoCallConv     getCallConv()       { return CorInfoCallConv((callConv & CORINFO_CALLCONV_MASK)); }
    bool                hasThis()           { return ((callConv & CORINFO_CALLCONV_HASTHIS) != 0); }
    bool                hasExplicitThis()   { return ((callConv & CORINFO_CALLCONV_EXPLICITTHIS) != 0); }
    unsigned            totalILArgs()       { return (numArgs + hasThis()); }
    bool                isVarArg()          { return ((getCallConv() == CORINFO_CALLCONV_VARARG) || (getCallConv() == CORINFO_CALLCONV_NATIVEVARARG)); }
    bool                hasTypeArg()        { return ((callConv & CORINFO_CALLCONV_PARAMTYPE) != 0); }
};

struct CORINFO_METHOD_INFO
{
    CORINFO_METHOD_HANDLE       ftn;
    CORINFO_MODULE_HANDLE       scope;
    BYTE *                      ILCode;
    unsigned                    ILCodeSize;
    unsigned                    maxStack;
    unsigned                    EHcount;
    CorInfoOptions              options;
    CorInfoRegionKind           regionKind;
    CORINFO_SIG_INFO            args;
    CORINFO_SIG_INFO            locals;
};

//----------------------------------------------------------------------------
// Looking up handles and addresses.
//
// When the JIT requests a handle, the EE may direct the JIT that it must
// access the handle in a variety of ways.  These are packed as
//    CORINFO_CONST_LOOKUP
// or CORINFO_LOOKUP (contains either a CORINFO_CONST_LOOKUP or a CORINFO_RUNTIME_LOOKUP)
//
// Constant Lookups v. Runtime Lookups (i.e. when will Runtime Lookups be generated?)
// -----------------------------------------------------------------------------------
//
// CORINFO_LOOKUP_KIND is part of the result type of embedGenericHandle,
// getVirtualCallInfo and any other functions that may require a
// runtime lookup when compiling shared generic code.
//
// CORINFO_LOOKUP_KIND indicates whether a particular token in the instruction stream can be:
// (a) Mapped to a handle (type, field or method) at compile-time (!needsRuntimeLookup)
// (b) Must be looked up at run-time, and if so which runtime lookup technique should be used (see below)
//
// If the JIT or EE does not support code sharing for generic code, then
// all CORINFO_LOOKUP results will be "constant lookups", i.e.
// the needsRuntimeLookup of CORINFO_LOOKUP.lookupKind.needsRuntimeLookup
// will be false.
//
// Constant Lookups
// ----------------
//
// Constant Lookups are either:
//     IAT_VALUE: immediate (relocatable) values,
//     IAT_PVALUE: immediate values access via an indirection through an immediate (relocatable) address
//     IAT_RELPVALUE: immediate values access via a relative indirection through an immediate offset
//     IAT_PPVALUE: immediate values access via a double indirection through an immediate (relocatable) address
//
// Runtime Lookups
// ---------------
//
// CORINFO_LOOKUP_KIND is part of the result type of embedGenericHandle,
// getVirtualCallInfo and any other functions that may require a
// runtime lookup when compiling shared generic code.
//
// CORINFO_LOOKUP_KIND indicates whether a particular token in the instruction stream can be:
// (a) Mapped to a handle (type, field or method) at compile-time (!needsRuntimeLookup)
// (b) Must be looked up at run-time using the class dictionary
//     stored in the vtable of the this pointer (needsRuntimeLookup && THISOBJ)
// (c) Must be looked up at run-time using the method dictionary
//     stored in the method descriptor parameter passed to a generic
//     method (needsRuntimeLookup && METHODPARAM)
// (d) Must be looked up at run-time using the class dictionary stored
//     in the vtable parameter passed to a method in a generic
//     struct (needsRuntimeLookup && CLASSPARAM)

struct CORINFO_CONST_LOOKUP
{
    // If the handle is obtained at compile-time, then this handle is the "exact" handle (class, method, or field)
    // Otherwise, it's a representative... 
    // If accessType is
    //     IAT_VALUE     --> "handle" stores the real handle or "addr " stores the computed address
    //     IAT_PVALUE    --> "addr" stores a pointer to a location which will hold the real handle
    //     IAT_RELPVALUE --> "addr" stores a relative pointer to a location which will hold the real handle
    //     IAT_PPVALUE   --> "addr" stores a double indirection to a location which will hold the real handle

    InfoAccessType              accessType;
    union
    {
        CORINFO_GENERIC_HANDLE  handle;
        void *                  addr;
    };
};

enum CORINFO_RUNTIME_LOOKUP_KIND
{
    CORINFO_LOOKUP_THISOBJ,
    CORINFO_LOOKUP_METHODPARAM,
    CORINFO_LOOKUP_CLASSPARAM,
};

struct CORINFO_LOOKUP_KIND
{
    bool                        needsRuntimeLookup;
    CORINFO_RUNTIME_LOOKUP_KIND runtimeLookupKind;

    // The 'runtimeLookupFlags' and 'runtimeLookupArgs' fields
    // are just for internal VM / ZAP communication, not to be used by the JIT.
    WORD                        runtimeLookupFlags;
    void *                      runtimeLookupArgs;
} ;


// CORINFO_RUNTIME_LOOKUP indicates the details of the runtime lookup
// operation to be performed.
//
// CORINFO_MAXINDIRECTIONS is the maximum number of
// indirections used by runtime lookups.
// This accounts for up to 2 indirections to get at a dictionary followed by a possible spill slot
//
#define CORINFO_MAXINDIRECTIONS 4
#define CORINFO_USEHELPER ((WORD) 0xffff)

struct CORINFO_RUNTIME_LOOKUP
{
    // This is signature you must pass back to the runtime lookup helper
    LPVOID                  signature;

    // Here is the helper you must call. It is one of CORINFO_HELP_RUNTIMEHANDLE_* helpers.
    CorInfoHelpFunc         helper;

    // Number of indirections to get there
    // CORINFO_USEHELPER = don't know how to get it, so use helper function at run-time instead
    // 0 = use the this pointer itself (e.g. token is C<!0> inside code in sealed class C)
    //     or method desc itself (e.g. token is method void M::mymeth<!!0>() inside code in M::mymeth)
    // Otherwise, follow each byte-offset stored in the "offsets[]" array (may be negative)
    WORD                    indirections;

    // If set, test for null and branch to helper if null
    bool                    testForNull;

    // If set, test the lowest bit and dereference if set (see code:FixupPointer)
    bool                    testForFixup;

    SIZE_T                  offsets[CORINFO_MAXINDIRECTIONS];

    // If set, first offset is indirect.
    // 0 means that value stored at first offset (offsets[0]) from pointer is next pointer, to which the next offset
    // (offsets[1]) is added and so on.
    // 1 means that value stored at first offset (offsets[0]) from pointer is offset1, and the next pointer is
    // stored at pointer+offsets[0]+offset1.
    bool                indirectFirstOffset;

    // If set, second offset is indirect.
    // 0 means that value stored at second offset (offsets[1]) from pointer is next pointer, to which the next offset
    // (offsets[2]) is added and so on.
    // 1 means that value stored at second offset (offsets[1]) from pointer is offset2, and the next pointer is
    // stored at pointer+offsets[1]+offset2.
    bool                indirectSecondOffset;
} ;

// Result of calling embedGenericHandle
struct CORINFO_LOOKUP
{
    CORINFO_LOOKUP_KIND     lookupKind;

    union
    {
        // If kind.needsRuntimeLookup then this indicates how to do the lookup
        CORINFO_RUNTIME_LOOKUP  runtimeLookup;

        // If the handle is obtained at compile-time, then this handle is the "exact" handle (class, method, or field)
        // Otherwise, it's a representative...  If accessType is
        //     IAT_VALUE --> "handle" stores the real handle or "addr " stores the computed address
        //     IAT_PVALUE --> "addr" stores a pointer to a location which will hold the real handle
        //     IAT_RELPVALUE --> "addr" stores a relative pointer to a location which will hold the real handle
        //     IAT_PPVALUE --> "addr" stores a double indirection to a location which will hold the real handle
        CORINFO_CONST_LOOKUP    constLookup;
    };
};

enum CorInfoGenericHandleType
{
    CORINFO_HANDLETYPE_UNKNOWN,
    CORINFO_HANDLETYPE_CLASS,
    CORINFO_HANDLETYPE_METHOD,
    CORINFO_HANDLETYPE_FIELD
};

//----------------------------------------------------------------------------
// Embedding type, method and field handles (for "ldtoken" or to pass back to helpers)

// Result of calling embedGenericHandle
struct CORINFO_GENERICHANDLE_RESULT
{
    CORINFO_LOOKUP          lookup;

    // compileTimeHandle is guaranteed to be either NULL or a handle that is usable during compile time.
    // It must not be embedded in the code because it might not be valid at run-time.
    CORINFO_GENERIC_HANDLE  compileTimeHandle;

    // Type of the result
    CorInfoGenericHandleType handleType;
};

#define CORINFO_ACCESS_ALLOWED_MAX_ARGS 4

enum CorInfoAccessAllowedHelperArgType
{
    CORINFO_HELPER_ARG_TYPE_Invalid = 0,
    CORINFO_HELPER_ARG_TYPE_Field   = 1,
    CORINFO_HELPER_ARG_TYPE_Method  = 2,
    CORINFO_HELPER_ARG_TYPE_Class   = 3,
    CORINFO_HELPER_ARG_TYPE_Module  = 4,
    CORINFO_HELPER_ARG_TYPE_Const   = 5,
};
struct CORINFO_HELPER_ARG
{
    union
    {
        CORINFO_FIELD_HANDLE fieldHandle;
        CORINFO_METHOD_HANDLE methodHandle;
        CORINFO_CLASS_HANDLE classHandle;
        CORINFO_MODULE_HANDLE moduleHandle;
        size_t constant;
    };
    CorInfoAccessAllowedHelperArgType argType;

    void Set(CORINFO_METHOD_HANDLE handle)
    {
        argType = CORINFO_HELPER_ARG_TYPE_Method;
        methodHandle = handle;
    }

    void Set(CORINFO_FIELD_HANDLE handle)
    {
        argType = CORINFO_HELPER_ARG_TYPE_Field;
        fieldHandle = handle;
    }

    void Set(CORINFO_CLASS_HANDLE handle)
    {
        argType = CORINFO_HELPER_ARG_TYPE_Class;
        classHandle = handle;
    }

    void Set(size_t value)
    {
        argType = CORINFO_HELPER_ARG_TYPE_Const;
        constant = value;
    }
};

struct CORINFO_HELPER_DESC
{
    CorInfoHelpFunc helperNum;
    unsigned numArgs;
    CORINFO_HELPER_ARG args[CORINFO_ACCESS_ALLOWED_MAX_ARGS];
};

//----------------------------------------------------------------------------
// getCallInfo and CORINFO_CALL_INFO: The EE instructs the JIT about how to make a call
//
// callKind
// --------
//
// CORINFO_CALL :
//   Indicates that the JIT can use getFunctionEntryPoint to make a call,
//   i.e. there is nothing abnormal about the call.  The JITs know what to do if they get this.
//   Except in the case of constraint calls (see below), [targetMethodHandle] will hold
//   the CORINFO_METHOD_HANDLE that a call to findMethod would
//   have returned.
//   This flag may be combined with nullInstanceCheck=TRUE for uses of callvirt on methods that can
//   be resolved at compile-time (non-virtual, final or sealed).
//
// CORINFO_CALL_CODE_POINTER (shared generic code only) :
//   Indicates that the JIT should do an indirect call to the entrypoint given by address, which may be specified
//   as a runtime lookup by CORINFO_CALL_INFO::codePointerLookup.
//   [targetMethodHandle] will not hold a valid value.
//   This flag may be combined with nullInstanceCheck=TRUE for uses of callvirt on methods whose target method can
//   be resolved at compile-time but whose instantiation can be resolved only through runtime lookup.
//
// CORINFO_VIRTUALCALL_STUB (interface calls) :
//   Indicates that the EE supports "stub dispatch" and request the JIT to make a
//   "stub dispatch" call (an indirect call through CORINFO_CALL_INFO::stubLookup,
//   similar to CORINFO_CALL_CODE_POINTER).
//   "Stub dispatch" is a specialized calling sequence (that may require use of NOPs)
//   which allow the runtime to determine the call-site after the call has been dispatched.
//   If the call is too complex for the JIT (e.g. because
//   fetching the dispatch stub requires a runtime lookup, i.e. lookupKind.needsRuntimeLookup
//   is set) then the JIT is allowed to implement the call as if it were CORINFO_VIRTUALCALL_LDVIRTFTN
//   [targetMethodHandle] will hold the CORINFO_METHOD_HANDLE that a call to findMethod would
//   have returned.
//   This flag is always accompanied by nullInstanceCheck=TRUE.
//
// CORINFO_VIRTUALCALL_LDVIRTFTN (virtual generic methods) :
//   Indicates that the EE provides no way to implement the call directly and
//   that the JIT should use a LDVIRTFTN sequence (as implemented by CORINFO_HELP_VIRTUAL_FUNC_PTR)
//   followed by an indirect call.
//   [targetMethodHandle] will hold the CORINFO_METHOD_HANDLE that a call to findMethod would
//   have returned.
//   This flag is always accompanied by nullInstanceCheck=TRUE though typically the null check will
//   be implicit in the access through the instance pointer.
//
//  CORINFO_VIRTUALCALL_VTABLE (regular virtual methods) :
//   Indicates that the EE supports vtable dispatch and that the JIT should use getVTableOffset etc.
//   to implement the call.
//   [targetMethodHandle] will hold the CORINFO_METHOD_HANDLE that a call to findMethod would
//   have returned.
//   This flag is always accompanied by nullInstanceCheck=TRUE though typically the null check will
//   be implicit in the access through the instance pointer.
//
// thisTransform and constraint calls
// ----------------------------------
//
// For evertyhing besides "constrained." calls "thisTransform" is set to
// CORINFO_NO_THIS_TRANSFORM.
//
// For "constrained." calls the EE attempts to resolve the call at compile
// time to a more specific method, or (shared generic code only) to a runtime lookup
// for a code pointer for the more specific method.
//
// In order to permit this, the "this" pointer supplied for a "constrained." call
// is a byref to an arbitrary type (see the IL spec). The "thisTransform" field
// will indicate how the JIT must transform the "this" pointer in order
// to be able to call the resolved method:
//
//  CORINFO_NO_THIS_TRANSFORM --> Leave it as a byref to an unboxed value type
//  CORINFO_BOX_THIS          --> Box it to produce an object
//  CORINFO_DEREF_THIS        --> Deref the byref to get an object reference
//
// In addition, the "kind" field will be set as follows for constraint calls:

//    CORINFO_CALL              --> the call was resolved at compile time, and
//                                  can be compiled like a normal call.
//    CORINFO_CALL_CODE_POINTER --> the call was resolved, but the target address will be
//                                  computed at runtime.  Only returned for shared generic code.
//    CORINFO_VIRTUALCALL_STUB,
//    CORINFO_VIRTUALCALL_LDVIRTFTN,
//    CORINFO_VIRTUALCALL_VTABLE   --> usual values indicating that a virtual call must be made

enum CORINFO_CALL_KIND
{
    CORINFO_CALL,
    CORINFO_CALL_CODE_POINTER,
    CORINFO_VIRTUALCALL_STUB,
    CORINFO_VIRTUALCALL_LDVIRTFTN,
    CORINFO_VIRTUALCALL_VTABLE
};

// Indicates that the CORINFO_VIRTUALCALL_VTABLE lookup needn't do a chunk indirection
#define CORINFO_VIRTUALCALL_NO_CHUNK 0xFFFFFFFF

enum CORINFO_THIS_TRANSFORM
{
    CORINFO_NO_THIS_TRANSFORM,
    CORINFO_BOX_THIS,
    CORINFO_DEREF_THIS
};

enum CORINFO_CALLINFO_FLAGS
{
    CORINFO_CALLINFO_NONE           = 0x0000,
    CORINFO_CALLINFO_ALLOWINSTPARAM = 0x0001,   // Can the compiler generate code to pass an instantiation parameters? Simple compilers should not use this flag
    CORINFO_CALLINFO_CALLVIRT       = 0x0002,   // Is it a virtual call?
    CORINFO_CALLINFO_KINDONLY       = 0x0004,   // This is set to only query the kind of call to perform, without getting any other information
    CORINFO_CALLINFO_VERIFICATION   = 0x0008,   // Gets extra verification information.
    CORINFO_CALLINFO_SECURITYCHECKS = 0x0010,   // Perform security checks.
    CORINFO_CALLINFO_LDFTN          = 0x0020,   // Resolving target of LDFTN
    CORINFO_CALLINFO_ATYPICAL_CALLSITE = 0x0040, // Atypical callsite that cannot be disassembled by delay loading helper
};

enum CorInfoIsAccessAllowedResult
{
    CORINFO_ACCESS_ALLOWED = 0,           // Call allowed
    CORINFO_ACCESS_ILLEGAL = 1,           // Call not allowed
    CORINFO_ACCESS_RUNTIME_CHECK = 2,     // Ask at runtime whether to allow the call or not
};


// This enum is used for JIT to tell EE where this token comes from.
// E.g. Depending on different opcodes, we might allow/disallow certain types of tokens or 
// return different types of handles (e.g. boxed vs. regular entrypoints)
enum CorInfoTokenKind
{
    CORINFO_TOKENKIND_Class     = 0x01,
    CORINFO_TOKENKIND_Method    = 0x02,
    CORINFO_TOKENKIND_Field     = 0x04,
    CORINFO_TOKENKIND_Mask      = 0x07,

    // token comes from CEE_LDTOKEN
    CORINFO_TOKENKIND_Ldtoken   = 0x10 | CORINFO_TOKENKIND_Class | CORINFO_TOKENKIND_Method | CORINFO_TOKENKIND_Field,

    // token comes from CEE_CASTCLASS or CEE_ISINST
    CORINFO_TOKENKIND_Casting   = 0x20 | CORINFO_TOKENKIND_Class,

    // token comes from CEE_NEWARR
    CORINFO_TOKENKIND_Newarr    = 0x40 | CORINFO_TOKENKIND_Class,

    // token comes from CEE_BOX
    CORINFO_TOKENKIND_Box       = 0x80 | CORINFO_TOKENKIND_Class,

    // token comes from CEE_CONSTRAINED
    CORINFO_TOKENKIND_Constrained = 0x100 | CORINFO_TOKENKIND_Class,

    // token comes from CEE_NEWOBJ
    CORINFO_TOKENKIND_NewObj    = 0x200 | CORINFO_TOKENKIND_Method,

    // token comes from CEE_LDVIRTFTN
    CORINFO_TOKENKIND_Ldvirtftn = 0x400 | CORINFO_TOKENKIND_Method,
};

struct CORINFO_RESOLVED_TOKEN
{
    //
    // [In] arguments of resolveToken
    //
    CORINFO_CONTEXT_HANDLE  tokenContext;       //Context for resolution of generic arguments
    CORINFO_MODULE_HANDLE   tokenScope;
    mdToken                 token;              //The source token
    CorInfoTokenKind        tokenType;

    //
    // [Out] arguments of resolveToken. 
    // - Type handle is always non-NULL.
    // - At most one of method and field handles is non-NULL (according to the token type).
    // - Method handle is an instantiating stub only for generic methods. Type handle 
    //   is required to provide the full context for methods in generic types.
    //
    CORINFO_CLASS_HANDLE    hClass;
    CORINFO_METHOD_HANDLE   hMethod;
    CORINFO_FIELD_HANDLE    hField;

    //
    // [Out] TypeSpec and MethodSpec signatures for generics. NULL otherwise.
    //
    PCCOR_SIGNATURE         pTypeSpec;
    ULONG                   cbTypeSpec;
    PCCOR_SIGNATURE         pMethodSpec;
    ULONG                   cbMethodSpec;
};

struct CORINFO_CALL_INFO
{
    CORINFO_METHOD_HANDLE   hMethod;            //target method handle
    unsigned                methodFlags;        //flags for the target method

    unsigned                classFlags;         //flags for CORINFO_RESOLVED_TOKEN::hClass

    CORINFO_SIG_INFO       sig;

    //Verification information
    unsigned                verMethodFlags;     // flags for CORINFO_RESOLVED_TOKEN::hMethod
    CORINFO_SIG_INFO        verSig;
    //All of the regular method data is the same... hMethod might not be the same as CORINFO_RESOLVED_TOKEN::hMethod


    //If set to:
    //  - CORINFO_ACCESS_ALLOWED - The access is allowed.
    //  - CORINFO_ACCESS_ILLEGAL - This access cannot be allowed (i.e. it is public calling private).  The
    //      JIT may either insert the callsiteCalloutHelper into the code (as per a verification error) or
    //      call throwExceptionFromHelper on the callsiteCalloutHelper.  In this case callsiteCalloutHelper
    //      is guaranteed not to return.
    //  - CORINFO_ACCESS_RUNTIME_CHECK - The jit must insert the callsiteCalloutHelper at the call site.
    //      the helper may return
    CorInfoIsAccessAllowedResult accessAllowed;
    CORINFO_HELPER_DESC     callsiteCalloutHelper;

    // See above section on constraintCalls to understand when these are set to unusual values.
    CORINFO_THIS_TRANSFORM  thisTransform;

    CORINFO_CALL_KIND       kind;
    BOOL                    nullInstanceCheck;

    // Context for inlining and hidden arg
    CORINFO_CONTEXT_HANDLE  contextHandle;
    BOOL                    exactContextNeedsRuntimeLookup; // Set if contextHandle is approx handle. Runtime lookup is required to get the exact handle.

    // If kind.CORINFO_VIRTUALCALL_STUB then stubLookup will be set.
    // If kind.CORINFO_CALL_CODE_POINTER then entryPointLookup will be set.
    union
    {
        CORINFO_LOOKUP      stubLookup;

        CORINFO_LOOKUP      codePointerLookup;
    };

    CORINFO_CONST_LOOKUP    instParamLookup;    // Used by Ready-to-Run

    BOOL                    secureDelegateInvoke;
};

//----------------------------------------------------------------------------
// getFieldInfo and CORINFO_FIELD_INFO: The EE instructs the JIT about how to access a field

enum CORINFO_FIELD_ACCESSOR
{
    CORINFO_FIELD_INSTANCE,                 // regular instance field at given offset from this-ptr
    CORINFO_FIELD_INSTANCE_WITH_BASE,       // instance field with base offset (used by Ready-to-Run)
    CORINFO_FIELD_INSTANCE_HELPER,          // instance field accessed using helper (arguments are this, FieldDesc * and the value)
    CORINFO_FIELD_INSTANCE_ADDR_HELPER,     // instance field accessed using address-of helper (arguments are this and FieldDesc *)

    CORINFO_FIELD_STATIC_ADDRESS,           // field at given address
    CORINFO_FIELD_STATIC_RVA_ADDRESS,       // RVA field at given address
    CORINFO_FIELD_STATIC_SHARED_STATIC_HELPER, // static field accessed using the "shared static" helper (arguments are ModuleID + ClassID)
    CORINFO_FIELD_STATIC_GENERICS_STATIC_HELPER, // static field access using the "generic static" helper (argument is MethodTable *)
    CORINFO_FIELD_STATIC_ADDR_HELPER,       // static field accessed using address-of helper (argument is FieldDesc *)
    CORINFO_FIELD_STATIC_TLS,               // unmanaged TLS access
    CORINFO_FIELD_STATIC_READYTORUN_HELPER, // static field access using a runtime lookup helper

    CORINFO_FIELD_INTRINSIC_ZERO,           // intrinsic zero (IntPtr.Zero, UIntPtr.Zero)
    CORINFO_FIELD_INTRINSIC_EMPTY_STRING,   // intrinsic emptry string (String.Empty)
    CORINFO_FIELD_INTRINSIC_ISLITTLEENDIAN, // intrinsic BitConverter.IsLittleEndian
};

// Set of flags returned in CORINFO_FIELD_INFO::fieldFlags
enum CORINFO_FIELD_FLAGS
{
    CORINFO_FLG_FIELD_STATIC                    = 0x00000001,
    CORINFO_FLG_FIELD_UNMANAGED                 = 0x00000002, // RVA field
    CORINFO_FLG_FIELD_FINAL                     = 0x00000004,
    CORINFO_FLG_FIELD_STATIC_IN_HEAP            = 0x00000008, // See code:#StaticFields. This static field is in the GC heap as a boxed object
    CORINFO_FLG_FIELD_SAFESTATIC_BYREF_RETURN   = 0x00000010, // Field can be returned safely (has GC heap lifetime)
    CORINFO_FLG_FIELD_INITCLASS                 = 0x00000020, // initClass has to be called before accessing the field
    CORINFO_FLG_FIELD_PROTECTED                 = 0x00000040,
};

struct CORINFO_FIELD_INFO
{
    CORINFO_FIELD_ACCESSOR  fieldAccessor;
    unsigned                fieldFlags;

    // Helper to use if the field access requires it
    CorInfoHelpFunc         helper;

    // Field offset if there is one
    DWORD                   offset;

    CorInfoType             fieldType;
    CORINFO_CLASS_HANDLE    structType; //possibly null

    //See CORINFO_CALL_INFO.accessAllowed
    CorInfoIsAccessAllowedResult accessAllowed;
    CORINFO_HELPER_DESC     accessCalloutHelper;

    CORINFO_CONST_LOOKUP    fieldLookup;        // Used by Ready-to-Run
};

//----------------------------------------------------------------------------
// Exception handling

struct CORINFO_EH_CLAUSE
{
    CORINFO_EH_CLAUSE_FLAGS     Flags;
    DWORD                       TryOffset;
    DWORD                       TryLength;
    DWORD                       HandlerOffset;
    DWORD                       HandlerLength;
    union
    {
        DWORD                   ClassToken;       // use for type-based exception handlers
        DWORD                   FilterOffset;     // use for filter-based exception handlers (COR_ILEXCEPTION_FILTER is set)
    };
};

enum CORINFO_OS
{
    CORINFO_WINNT,
    CORINFO_PAL,
};

struct CORINFO_CPU
{
    DWORD           dwCPUType;
    DWORD           dwFeatures;
    DWORD           dwExtendedFeatures;
};

enum CORINFO_RUNTIME_ABI
{
    CORINFO_DESKTOP_ABI = 0x100,
    CORINFO_CORECLR_ABI = 0x200,
    CORINFO_CORERT_ABI = 0x300,
};

// For some highly optimized paths, the JIT must generate code that directly
// manipulates internal EE data structures. The getEEInfo() helper returns
// this structure containing the needed offsets and values.
struct CORINFO_EE_INFO
{
    // Information about the InlinedCallFrame structure layout
    struct InlinedCallFrameInfo
    {
        // Size of the Frame structure
        unsigned    size;

        unsigned    offsetOfGSCookie;
        unsigned    offsetOfFrameVptr;
        unsigned    offsetOfFrameLink;
        unsigned    offsetOfCallSiteSP;
        unsigned    offsetOfCalleeSavedFP;
        unsigned    offsetOfCallTarget;
        unsigned    offsetOfReturnAddress;
        // This offset is used only for ARM
        unsigned    offsetOfSPAfterProlog;
    }
    inlinedCallFrameInfo;

    // Offsets into the Thread structure
    unsigned    offsetOfThreadFrame;            // offset of the current Frame
    unsigned    offsetOfGCState;                // offset of the preemptive/cooperative state of the Thread

    // Delegate offsets
    unsigned    offsetOfDelegateInstance;
    unsigned    offsetOfDelegateFirstTarget;

    // Secure delegate offsets
    unsigned    offsetOfSecureDelegateIndirectCell;

    // Remoting offsets
    unsigned    offsetOfTransparentProxyRP;
    unsigned    offsetOfRealProxyServer;

    // Array offsets
    unsigned    offsetOfObjArrayData;

    // Reverse PInvoke offsets
    unsigned    sizeOfReversePInvokeFrame;

    // OS Page size
    size_t      osPageSize;

    // Null object offset
    size_t      maxUncheckedOffsetForNullObject;

    // Target ABI. Combined with target architecture and OS to determine
    // GC, EH, and unwind styles.
    CORINFO_RUNTIME_ABI targetAbi;

    CORINFO_OS  osType;
    unsigned    osMajor;
    unsigned    osMinor;
    unsigned    osBuild;
};

// This is used to indicate that a finally has been called 
// "locally" by the try block
enum { LCL_FINALLY_MARK = 0xFC }; // FC = "Finally Call"

/**********************************************************************************
 * The following is the internal structure of an object that the compiler knows about
 * when it generates code
 **********************************************************************************/

#include <pshpack4.h>

typedef void* CORINFO_MethodPtr;            // a generic method pointer

struct CORINFO_Object
{
    CORINFO_MethodPtr      *methTable;      // the vtable for the object
};

struct CORINFO_String : public CORINFO_Object
{
    unsigned                stringLen;
    WCHAR                   chars[1];       // actually of variable size
};

struct CORINFO_Array : public CORINFO_Object
{
    unsigned                length;
#ifdef BIT64
    unsigned                alignpad;
#endif // BIT64

#if 0
    /* Multi-dimensional arrays have the lengths and bounds here */
    unsigned                dimLength[length];
    unsigned                dimBound[length];
#endif

    union
    {
        __int8              i1Elems[1];    // actually of variable size
        unsigned __int8     u1Elems[1];
        __int16             i2Elems[1];
        unsigned __int16    u2Elems[1];
        __int32             i4Elems[1];
        unsigned __int32    u4Elems[1];
        float               r4Elems[1];
    };
};

#include <pshpack4.h>
struct CORINFO_Array8 : public CORINFO_Object
{
    unsigned                length;
#ifdef BIT64
    unsigned                alignpad;
#endif // BIT64

    union
    {
        double              r8Elems[1];
        __int64             i8Elems[1];
        unsigned __int64    u8Elems[1];
    };
};

#include <poppack.h>

struct CORINFO_RefArray : public CORINFO_Object
{
    unsigned                length;
#ifdef BIT64
    unsigned                alignpad;
#endif // BIT64

#if 0
    /* Multi-dimensional arrays have the lengths and bounds here */
    unsigned                dimLength[length];
    unsigned                dimBound[length];
#endif

    CORINFO_Object*         refElems[1];    // actually of variable size;
};

struct CORINFO_RefAny
{
    void                      * dataPtr;
    CORINFO_CLASS_HANDLE        type;
};

// The jit assumes the CORINFO_VARARGS_HANDLE is a pointer to a subclass of this
struct CORINFO_VarArgInfo
{
    unsigned                argBytes;       // number of bytes the arguments take up.
                                            // (The CORINFO_VARARGS_HANDLE counts as an arg)
};

#include <poppack.h>

#define SIZEOF__CORINFO_Object                            TARGET_POINTER_SIZE /* methTable */

#define OFFSETOF__CORINFO_Array__length                   SIZEOF__CORINFO_Object
#ifdef _TARGET_64BIT_
#define OFFSETOF__CORINFO_Array__data                     (OFFSETOF__CORINFO_Array__length + sizeof(unsigned __int32) /* length */ + sizeof(unsigned __int32) /* alignpad */)
#else
#define OFFSETOF__CORINFO_Array__data                     (OFFSETOF__CORINFO_Array__length + sizeof(unsigned __int32) /* length */)
#endif

#define OFFSETOF__CORINFO_TypedReference__dataPtr         0
#define OFFSETOF__CORINFO_TypedReference__type            (OFFSETOF__CORINFO_TypedReference__dataPtr + TARGET_POINTER_SIZE /* dataPtr */)

#define OFFSETOF__CORINFO_String__stringLen               SIZEOF__CORINFO_Object
#define OFFSETOF__CORINFO_String__chars                   (OFFSETOF__CORINFO_String__stringLen + sizeof(unsigned __int32) /* stringLen */)

enum CorInfoSecurityRuntimeChecks
{
    CORINFO_ACCESS_SECURITY_NONE                          = 0,
    CORINFO_ACCESS_SECURITY_TRANSPARENCY                  = 0x0001  // check that transparency rules are enforced between the caller and callee
};


/* data to optimize delegate construction */
struct DelegateCtorArgs
{
    void * pMethod;
    void * pArg3;
    void * pArg4;
    void * pArg5;
};

// use offsetof to get the offset of the fields above
#include <stddef.h> // offsetof

// Guard-stack cookie for preventing against stack buffer overruns
typedef SIZE_T GSCookie;

#include "cordebuginfo.h"

/**********************************************************************************/
// Some compilers cannot arbitrarily allow the handler nesting level to grow
// arbitrarily during Edit'n'Continue.
// This is the maximum nesting level that a compiler needs to support for EnC

const int MAX_EnC_HANDLER_NESTING_LEVEL = 6;

// Results from type comparison queries
enum class TypeCompareState
{
    MustNot = -1, // types are not equal
    May = 0,      // types may be equal (must test at runtime)
    Must = 1,     // type are equal
};

//
// This interface is logically split into sections for each class of information 
// (ICorMethodInfo, ICorModuleInfo, etc.). This split used to exist physically as well
// using virtual inheritance, but was eliminated to improve efficiency of the JIT-EE 
// interface calls.
//
class ICorStaticInfo
{
public:
    /**********************************************************************************/
    //
    // ICorMethodInfo
    //
    /**********************************************************************************/

    // return flags (defined above, CORINFO_FLG_PUBLIC ...)
    virtual DWORD getMethodAttribs (
            CORINFO_METHOD_HANDLE       ftn         /* IN */
            ) = 0;

    // sets private JIT flags, which can be, retrieved using getAttrib.
    virtual void setMethodAttribs (
            CORINFO_METHOD_HANDLE       ftn,        /* IN */
            CorInfoMethodRuntimeFlags   attribs     /* IN */
            ) = 0;

    // Given a method descriptor ftnHnd, extract signature information into sigInfo
    //
    // 'memberParent' is typically only set when verifying.  It should be the
    // result of calling getMemberParent.
    virtual void getMethodSig (
             CORINFO_METHOD_HANDLE      ftn,        /* IN  */
             CORINFO_SIG_INFO          *sig,        /* OUT */
             CORINFO_CLASS_HANDLE      memberParent = NULL /* IN */
             ) = 0;

    /*********************************************************************
     * Note the following methods can only be used on functions known
     * to be IL.  This includes the method being compiled and any method
     * that 'getMethodInfo' returns true for
     *********************************************************************/

    // return information about a method private to the implementation
    //      returns false if method is not IL, or is otherwise unavailable.
    //      This method is used to fetch data needed to inline functions
    virtual bool getMethodInfo (
            CORINFO_METHOD_HANDLE   ftn,            /* IN  */
            CORINFO_METHOD_INFO*    info            /* OUT */
            ) = 0;

    // Decides if you have any limitations for inlining. If everything's OK, it will return
    // INLINE_PASS and will fill out pRestrictions with a mask of restrictions the caller of this
    // function must respect. If caller passes pRestrictions = NULL, if there are any restrictions
    // INLINE_FAIL will be returned
    //
    // The callerHnd must be the immediate caller (i.e. when we have a chain of inlined calls)
    //
    // The inlined method need not be verified

    virtual CorInfoInline canInline (
            CORINFO_METHOD_HANDLE       callerHnd,                  /* IN  */
            CORINFO_METHOD_HANDLE       calleeHnd,                  /* IN  */
            DWORD*                      pRestrictions               /* OUT */
            ) = 0;

    // Reports whether or not a method can be inlined, and why.  canInline is responsible for reporting all
    // inlining results when it returns INLINE_FAIL and INLINE_NEVER.  All other results are reported by the
    // JIT.
    virtual void reportInliningDecision (CORINFO_METHOD_HANDLE inlinerHnd,
                                                   CORINFO_METHOD_HANDLE inlineeHnd,
                                                   CorInfoInline inlineResult,
                                                   const char * reason) = 0;


    // Returns false if the call is across security boundaries thus we cannot tailcall
    //
    // The callerHnd must be the immediate caller (i.e. when we have a chain of inlined calls)
    virtual bool canTailCall (
            CORINFO_METHOD_HANDLE   callerHnd,          /* IN */
            CORINFO_METHOD_HANDLE   declaredCalleeHnd,  /* IN */
            CORINFO_METHOD_HANDLE   exactCalleeHnd,     /* IN */
            bool fIsTailPrefix                          /* IN */
            ) = 0;

    // Reports whether or not a method can be tail called, and why.
    // canTailCall is responsible for reporting all results when it returns
    // false.  All other results are reported by the JIT.
    virtual void reportTailCallDecision (CORINFO_METHOD_HANDLE callerHnd,
                                                   CORINFO_METHOD_HANDLE calleeHnd,
                                                   bool fIsTailPrefix,
                                                   CorInfoTailCall tailCallResult,
                                                   const char * reason) = 0;

    // get individual exception handler
    virtual void getEHinfo(
            CORINFO_METHOD_HANDLE ftn,              /* IN  */
            unsigned          EHnumber,             /* IN */
            CORINFO_EH_CLAUSE* clause               /* OUT */
            ) = 0;

    // return class it belongs to
    virtual CORINFO_CLASS_HANDLE getMethodClass (
            CORINFO_METHOD_HANDLE       method
            ) = 0;

    // return module it belongs to
    virtual CORINFO_MODULE_HANDLE getMethodModule (
            CORINFO_METHOD_HANDLE       method
            ) = 0;

    // This function returns the offset of the specified method in the
    // vtable of it's owning class or interface.
    virtual void getMethodVTableOffset (
            CORINFO_METHOD_HANDLE       method,                 /* IN */
            unsigned*                   offsetOfIndirection,    /* OUT */
            unsigned*                   offsetAfterIndirection, /* OUT */
            bool*                       isRelative              /* OUT */
            ) = 0;

    // Find the virtual method in implementingClass that overrides virtualMethod,
    // or the method in implementingClass that implements the interface method
    // represented by virtualMethod.
    //
    // Return null if devirtualization is not possible. Owner type is optional
    // and provides additional context for shared interface devirtualization.
    virtual CORINFO_METHOD_HANDLE resolveVirtualMethod(
            CORINFO_METHOD_HANDLE       virtualMethod,          /* IN */
            CORINFO_CLASS_HANDLE        implementingClass,      /* IN */
            CORINFO_CONTEXT_HANDLE      ownerType = NULL        /* IN */
            ) = 0;

    // Get the unboxed entry point for a method, if possible.
    virtual CORINFO_METHOD_HANDLE getUnboxedEntry(
        CORINFO_METHOD_HANDLE ftn,
        bool* requiresInstMethodTableArg = NULL /* OUT */
        ) = 0;

    // Given T, return the type of the default EqualityComparer<T>.
    // Returns null if the type can't be determined exactly.
    virtual CORINFO_CLASS_HANDLE getDefaultEqualityComparerClass(
            CORINFO_CLASS_HANDLE elemType
            ) = 0;

    // Given resolved token that corresponds to an intrinsic classified as
    // a CORINFO_INTRINSIC_GetRawHandle intrinsic, fetch the handle associated
    // with the token. If this is not possible at compile-time (because the current method's 
    // code is shared and the token contains generic parameters) then indicate 
    // how the handle should be looked up at runtime.
    virtual void expandRawHandleIntrinsic(
        CORINFO_RESOLVED_TOKEN *        pResolvedToken,
        CORINFO_GENERICHANDLE_RESULT *  pResult) = 0;

    // If a method's attributes have (getMethodAttribs) CORINFO_FLG_INTRINSIC set,
    // getIntrinsicID() returns the intrinsic ID.
    // *pMustExpand tells whether or not JIT must expand the intrinsic.
    virtual CorInfoIntrinsics getIntrinsicID(
            CORINFO_METHOD_HANDLE       method,
            bool*                       pMustExpand = NULL      /* OUT */
            ) = 0;

    // Is the given module the System.Numerics.Vectors module?
    // This defaults to false.
    virtual bool isInSIMDModule(
            CORINFO_CLASS_HANDLE        classHnd
            ) { return false; }

    // return the unmanaged calling convention for a PInvoke
    virtual CorInfoUnmanagedCallConv getUnmanagedCallConv(
            CORINFO_METHOD_HANDLE       method
            ) = 0;

    // return if any marshaling is required for PInvoke methods.  Note that
    // method == 0 => calli.  The call site sig is only needed for the varargs or calli case
    virtual BOOL pInvokeMarshalingRequired(
            CORINFO_METHOD_HANDLE       method,
            CORINFO_SIG_INFO*           callSiteSig
            ) = 0;

    // Check constraints on method type arguments (only).
    // The parent class should be checked separately using satisfiesClassConstraints(parent).
    virtual BOOL satisfiesMethodConstraints(
            CORINFO_CLASS_HANDLE        parent, // the exact parent of the method
            CORINFO_METHOD_HANDLE       method
            ) = 0;

    // Given a delegate target class, a target method parent class,  a  target method,
    // a delegate class, check if the method signature is compatible with the Invoke method of the delegate
    // (under the typical instantiation of any free type variables in the memberref signatures).
    virtual BOOL isCompatibleDelegate(
            CORINFO_CLASS_HANDLE        objCls,           /* type of the delegate target, if any */
            CORINFO_CLASS_HANDLE        methodParentCls,  /* exact parent of the target method, if any */
            CORINFO_METHOD_HANDLE       method,           /* (representative) target method, if any */
            CORINFO_CLASS_HANDLE        delegateCls,      /* exact type of the delegate */
            BOOL                        *pfIsOpenDelegate /* is the delegate open */
            ) = 0;

    // Indicates if the method is an instance of the generic
    // method that passes (or has passed) verification
    virtual CorInfoInstantiationVerification isInstantiationOfVerifiedGeneric (
            CORINFO_METHOD_HANDLE   method /* IN  */
            ) = 0;

    // Loads the constraints on a typical method definition, detecting cycles;
    // for use in verification.
    virtual void initConstraintsForVerification(
            CORINFO_METHOD_HANDLE   method, /* IN */
            BOOL *pfHasCircularClassConstraints, /* OUT */
            BOOL *pfHasCircularMethodConstraint /* OUT */
            ) = 0;

    // Returns enum whether the method does not require verification
    // Also see ICorModuleInfo::canSkipVerification
    virtual CorInfoCanSkipVerificationResult canSkipMethodVerification (
            CORINFO_METHOD_HANDLE       ftnHandle
            ) = 0;

    // load and restore the method
    virtual void methodMustBeLoadedBeforeCodeIsRun(
            CORINFO_METHOD_HANDLE       method
            ) = 0;

    virtual CORINFO_METHOD_HANDLE mapMethodDeclToMethodImpl(
            CORINFO_METHOD_HANDLE       method
            ) = 0;

    // Returns the global cookie for the /GS unsafe buffer checks
    // The cookie might be a constant value (JIT), or a handle to memory location (Ngen)
    virtual void getGSCookie(
            GSCookie * pCookieVal,                     // OUT
            GSCookie ** ppCookieVal                    // OUT
            ) = 0;

    /**********************************************************************************/
    //
    // ICorModuleInfo
    //
    /**********************************************************************************/

    // Resolve metadata token into runtime method handles. This function may not
    // return normally (e.g. it may throw) if it encounters invalid metadata or other
    // failures during token resolution.
    virtual void resolveToken(/* IN, OUT */ CORINFO_RESOLVED_TOKEN * pResolvedToken) = 0;

    // Attempt to resolve a metadata token into a runtime method handle. Returns true
    // if resolution succeeded and false otherwise (e.g. if it encounters invalid metadata
    // during token reoslution). This method should be used instead of `resolveToken` in
    // situations that need to be resilient to invalid metadata.
    virtual bool tryResolveToken(/* IN, OUT */ CORINFO_RESOLVED_TOKEN * pResolvedToken) = 0;

    // Signature information about the call sig
    virtual void findSig (
            CORINFO_MODULE_HANDLE       module,     /* IN */
            unsigned                    sigTOK,     /* IN */
            CORINFO_CONTEXT_HANDLE      context,    /* IN */
            CORINFO_SIG_INFO           *sig         /* OUT */
            ) = 0;

    // for Varargs, the signature at the call site may differ from
    // the signature at the definition.  Thus we need a way of
    // fetching the call site information
    virtual void findCallSiteSig (
            CORINFO_MODULE_HANDLE       module,     /* IN */
            unsigned                    methTOK,    /* IN */
            CORINFO_CONTEXT_HANDLE      context,    /* IN */
            CORINFO_SIG_INFO           *sig         /* OUT */
            ) = 0;

    virtual CORINFO_CLASS_HANDLE getTokenTypeAsHandle (
            CORINFO_RESOLVED_TOKEN *    pResolvedToken /* IN  */) = 0;

    // Returns true if the module does not require verification
    //
    // If fQuickCheckOnlyWithoutCommit=TRUE, the function only checks that the
    // module does not currently require verification in the current AppDomain.
    // This decision could change in the future, and so should not be cached.
    // If it is cached, it should only be used as a hint.
    // This is only used by ngen for calculating certain hints.
    //
   
    // Returns enum whether the module does not require verification
    // Also see ICorMethodInfo::canSkipMethodVerification();
    virtual CorInfoCanSkipVerificationResult canSkipVerification (
            CORINFO_MODULE_HANDLE       module     /* IN  */
            ) = 0;

    // Checks if the given metadata token is valid
    virtual BOOL isValidToken (
            CORINFO_MODULE_HANDLE       module,     /* IN  */
            unsigned                    metaTOK     /* IN  */
            ) = 0;

    // Checks if the given metadata token is valid StringRef
    virtual BOOL isValidStringRef (
            CORINFO_MODULE_HANDLE       module,     /* IN  */
            unsigned                    metaTOK     /* IN  */
            ) = 0;

    virtual BOOL shouldEnforceCallvirtRestriction(
            CORINFO_MODULE_HANDLE   scope
            ) = 0;

    /**********************************************************************************/
    //
    // ICorClassInfo
    //
    /**********************************************************************************/

    // If the value class 'cls' is isomorphic to a primitive type it will
    // return that type, otherwise it will return CORINFO_TYPE_VALUECLASS
    virtual CorInfoType asCorInfoType (
            CORINFO_CLASS_HANDLE    cls
            ) = 0;

    // for completeness
    virtual const char* getClassName (
            CORINFO_CLASS_HANDLE    cls
            ) = 0;

    // Return class name as in metadata, or nullptr if there is none.
    // Suitable for non-debugging use.
    virtual const char* getClassNameFromMetadata (
            CORINFO_CLASS_HANDLE    cls,
            const char            **namespaceName   /* OUT */
            ) = 0;

    // Return the type argument of the instantiated generic class,
    // which is specified by the index
    virtual CORINFO_CLASS_HANDLE getTypeInstantiationArgument(
            CORINFO_CLASS_HANDLE cls, 
            unsigned             index
            ) = 0;
    

    // Append a (possibly truncated) representation of the type cls to the preallocated buffer ppBuf of length pnBufLen
    // If fNamespace=TRUE, include the namespace/enclosing classes
    // If fFullInst=TRUE (regardless of fNamespace and fAssembly), include namespace and assembly for any type parameters
    // If fAssembly=TRUE, suffix with a comma and the full assembly qualification
    // return size of representation
    virtual int appendClassName(
            __deref_inout_ecount(*pnBufLen) WCHAR** ppBuf, 
            int* pnBufLen,
            CORINFO_CLASS_HANDLE    cls,
            BOOL fNamespace,
            BOOL fFullInst,
            BOOL fAssembly
            ) = 0;

    // Quick check whether the type is a value class. Returns the same value as getClassAttribs(cls) & CORINFO_FLG_VALUECLASS, except faster.
    virtual BOOL isValueClass(CORINFO_CLASS_HANDLE cls) = 0;

    // Decides how the JIT should do the optimization to inline the check for
    //     GetTypeFromHandle(handle) == obj.GetType() (for CORINFO_INLINE_TYPECHECK_SOURCE_VTABLE)
    //     GetTypeFromHandle(X) == GetTypeFromHandle(Y) (for CORINFO_INLINE_TYPECHECK_SOURCE_TOKEN)
    virtual CorInfoInlineTypeCheck canInlineTypeCheck(CORINFO_CLASS_HANDLE cls, CorInfoInlineTypeCheckSource source) = 0;

    // If this method returns true, JIT will do optimization to inline the check for
    //     GetTypeFromHandle(handle) == obj.GetType()
    virtual BOOL canInlineTypeCheckWithObjectVTable(CORINFO_CLASS_HANDLE cls) = 0;

    // return flags (defined above, CORINFO_FLG_PUBLIC ...)
    virtual DWORD getClassAttribs (
            CORINFO_CLASS_HANDLE    cls
            ) = 0;

    // Returns "TRUE" iff "cls" is a struct type such that return buffers used for returning a value
    // of this type must be stack-allocated.  This will generally be true only if the struct 
    // contains GC pointers, and does not exceed some size limit.  Maintaining this as an invariant allows
    // an optimization: the JIT may assume that return buffer pointers for return types for which this predicate
    // returns TRUE are always stack allocated, and thus, that stores to the GC-pointer fields of such return
    // buffers do not require GC write barriers.
    virtual BOOL isStructRequiringStackAllocRetBuf(CORINFO_CLASS_HANDLE cls) = 0;

    virtual CORINFO_MODULE_HANDLE getClassModule (
            CORINFO_CLASS_HANDLE    cls
            ) = 0;

    // Returns the assembly that contains the module "mod".
    virtual CORINFO_ASSEMBLY_HANDLE getModuleAssembly (
            CORINFO_MODULE_HANDLE   mod
            ) = 0;

    // Returns the name of the assembly "assem".
    virtual const char* getAssemblyName (
            CORINFO_ASSEMBLY_HANDLE assem
            ) = 0;

    // Allocate and delete process-lifetime objects.  Should only be
    // referred to from static fields, lest a leak occur.
    // Note that "LongLifetimeFree" does not execute destructors, if "obj"
    // is an array of a struct type with a destructor.
    virtual void* LongLifetimeMalloc(size_t sz) = 0;
    virtual void LongLifetimeFree(void* obj) = 0;

    virtual size_t getClassModuleIdForStatics (
            CORINFO_CLASS_HANDLE    cls, 
            CORINFO_MODULE_HANDLE *pModule, 
            void **ppIndirection
            ) = 0;

    // return the number of bytes needed by an instance of the class
    virtual unsigned getClassSize (
            CORINFO_CLASS_HANDLE        cls
            ) = 0;

    // return the number of bytes needed by an instance of the class allocated on the heap
    virtual unsigned getHeapClassSize(
        CORINFO_CLASS_HANDLE        cls
    ) = 0;

    virtual BOOL canAllocateOnStack(
        CORINFO_CLASS_HANDLE cls
    ) = 0;

    virtual unsigned getClassAlignmentRequirement (
            CORINFO_CLASS_HANDLE        cls,
            BOOL                        fDoubleAlignHint = FALSE
            ) = 0;

    // This is only called for Value classes.  It returns a boolean array
    // in representing of 'cls' from a GC perspective.  The class is
    // assumed to be an array of machine words
    // (of length // getClassSize(cls) / TARGET_POINTER_SIZE),
    // 'gcPtrs' is a pointer to an array of BYTEs of this length.
    // getClassGClayout fills in this array so that gcPtrs[i] is set
    // to one of the CorInfoGCType values which is the GC type of
    // the i-th machine word of an object of type 'cls'
    // returns the number of GC pointers in the array
    virtual unsigned getClassGClayout (
            CORINFO_CLASS_HANDLE        cls,        /* IN */
            BYTE                       *gcPtrs      /* OUT */
            ) = 0;

    // returns the number of instance fields in a class
    virtual unsigned getClassNumInstanceFields (
            CORINFO_CLASS_HANDLE        cls        /* IN */
            ) = 0;

    virtual CORINFO_FIELD_HANDLE getFieldInClass(
            CORINFO_CLASS_HANDLE clsHnd,
            INT num
            ) = 0;

    virtual BOOL checkMethodModifier(
            CORINFO_METHOD_HANDLE hMethod,
            LPCSTR modifier,
            BOOL fOptional
            ) = 0;

    // returns the "NEW" helper optimized for "newCls."
    virtual CorInfoHelpFunc getNewHelper(
            CORINFO_RESOLVED_TOKEN * pResolvedToken,
            CORINFO_METHOD_HANDLE    callerHandle,
            bool *                   pHasSideEffects = NULL /* OUT */
            ) = 0;

    // returns the newArr (1-Dim array) helper optimized for "arrayCls."
    virtual CorInfoHelpFunc getNewArrHelper(
            CORINFO_CLASS_HANDLE        arrayCls
            ) = 0;

    // returns the optimized "IsInstanceOf" or "ChkCast" helper
    virtual CorInfoHelpFunc getCastingHelper(
            CORINFO_RESOLVED_TOKEN * pResolvedToken,
            bool fThrowing
            ) = 0;

    // returns helper to trigger static constructor
    virtual CorInfoHelpFunc getSharedCCtorHelper(
            CORINFO_CLASS_HANDLE clsHnd
            ) = 0;

    virtual CorInfoHelpFunc getSecurityPrologHelper(
            CORINFO_METHOD_HANDLE   ftn
            ) = 0;

    // This is not pretty.  Boxing nullable<T> actually returns
    // a boxed<T> not a boxed Nullable<T>.  This call allows the verifier
    // to call back to the EE on the 'box' instruction and get the transformed
    // type to use for verification.
    virtual CORINFO_CLASS_HANDLE  getTypeForBox(
            CORINFO_CLASS_HANDLE        cls
            ) = 0;

    // returns the correct box helper for a particular class.  Note
    // that if this returns CORINFO_HELP_BOX, the JIT can assume 
    // 'standard' boxing (allocate object and copy), and optimize
    virtual CorInfoHelpFunc getBoxHelper(
            CORINFO_CLASS_HANDLE        cls
            ) = 0;

    // returns the unbox helper.  If 'helperCopies' points to a true 
    // value it means the JIT is requesting a helper that unboxes the
    // value into a particular location and thus has the signature
    //     void unboxHelper(void* dest, CORINFO_CLASS_HANDLE cls, Object* obj)
    // Otherwise (it is null or points at a FALSE value) it is requesting 
    // a helper that returns a pointer to the unboxed data 
    //     void* unboxHelper(CORINFO_CLASS_HANDLE cls, Object* obj)
    // The EE has the option of NOT returning the copy style helper
    // (But must be able to always honor the non-copy style helper)
    // The EE set 'helperCopies' on return to indicate what kind of
    // helper has been created.  

    virtual CorInfoHelpFunc getUnBoxHelper(
            CORINFO_CLASS_HANDLE        cls
            ) = 0;

    virtual bool getReadyToRunHelper(
            CORINFO_RESOLVED_TOKEN *        pResolvedToken,
            CORINFO_LOOKUP_KIND *           pGenericLookupKind,
            CorInfoHelpFunc                 id,
            CORINFO_CONST_LOOKUP *          pLookup
            ) = 0;

    virtual void getReadyToRunDelegateCtorHelper(
            CORINFO_RESOLVED_TOKEN * pTargetMethod,
            CORINFO_CLASS_HANDLE     delegateType,
            CORINFO_LOOKUP *   pLookup
            ) = 0;

    virtual const char* getHelperName(
            CorInfoHelpFunc
            ) = 0;

    // This function tries to initialize the class (run the class constructor).
    // this function returns whether the JIT must insert helper calls before 
    // accessing static field or method.
    //
    // See code:ICorClassInfo#ClassConstruction.
    virtual CorInfoInitClassResult initClass(
            CORINFO_FIELD_HANDLE    field,          // Non-NULL - inquire about cctor trigger before static field access
                                                    // NULL - inquire about cctor trigger in method prolog
            CORINFO_METHOD_HANDLE   method,         // Method referencing the field or prolog
            CORINFO_CONTEXT_HANDLE  context,        // Exact context of method
            BOOL                    speculative = FALSE     // TRUE means don't actually run it
            ) = 0;

    // This used to be called "loadClass".  This records the fact
    // that the class must be loaded (including restored if necessary) before we execute the
    // code that we are currently generating.  When jitting code
    // the function loads the class immediately.  When zapping code
    // the zapper will if necessary use the call to record the fact that we have
    // to do a fixup/restore before running the method currently being generated.
    //
    // This is typically used to ensure value types are loaded before zapped
    // code that manipulates them is executed, so that the GC can access information
    // about those value types.
    virtual void classMustBeLoadedBeforeCodeIsRun(
            CORINFO_CLASS_HANDLE        cls
            ) = 0;

    // returns the class handle for the special builtin classes
    virtual CORINFO_CLASS_HANDLE getBuiltinClass (
            CorInfoClassId              classId
            ) = 0;

    // "System.Int32" ==> CORINFO_TYPE_INT..
    virtual CorInfoType getTypeForPrimitiveValueClass(
            CORINFO_CLASS_HANDLE        cls
            ) = 0;

    // "System.Int32" ==> CORINFO_TYPE_INT..
    // "System.UInt32" ==> CORINFO_TYPE_UINT..
    virtual CorInfoType getTypeForPrimitiveNumericClass(
            CORINFO_CLASS_HANDLE        cls
            ) = 0;

    // TRUE if child is a subtype of parent
    // if parent is an interface, then does child implement / extend parent
    virtual BOOL canCast(
            CORINFO_CLASS_HANDLE        child,  // subtype (extends parent)
            CORINFO_CLASS_HANDLE        parent  // base type
            ) = 0;

    // TRUE if cls1 and cls2 are considered equivalent types.
    virtual BOOL areTypesEquivalent(
            CORINFO_CLASS_HANDLE        cls1,
            CORINFO_CLASS_HANDLE        cls2
            ) = 0;

    // See if a cast from fromClass to toClass will succeed, fail, or needs
    // to be resolved at runtime.
    virtual TypeCompareState compareTypesForCast(
            CORINFO_CLASS_HANDLE        fromClass,
            CORINFO_CLASS_HANDLE        toClass
            ) = 0;

    // See if types represented by cls1 and cls2 compare equal, not
    // equal, or the comparison needs to be resolved at runtime.
    virtual TypeCompareState compareTypesForEquality(
            CORINFO_CLASS_HANDLE        cls1,
            CORINFO_CLASS_HANDLE        cls2
            ) = 0;

    // Returns the intersection of cls1 and cls2.
    virtual CORINFO_CLASS_HANDLE mergeClasses(
            CORINFO_CLASS_HANDLE        cls1,
            CORINFO_CLASS_HANDLE        cls2
            ) = 0;

    // Returns true if cls2 is known to be a more specific type
    // than cls1 (a subtype or more restrictive shared type)
    // for purposes of jit type tracking. This is a hint to the
    // jit for optimization; it does not have correctness
    // implications.
    virtual BOOL isMoreSpecificType(
            CORINFO_CLASS_HANDLE        cls1,
            CORINFO_CLASS_HANDLE        cls2
            ) = 0;

    // Given a class handle, returns the Parent type.
    // For COMObjectType, it returns Class Handle of System.Object.
    // Returns 0 if System.Object is passed in.
    virtual CORINFO_CLASS_HANDLE getParentType (
            CORINFO_CLASS_HANDLE        cls
            ) = 0;

    // Returns the CorInfoType of the "child type". If the child type is
    // not a primitive type, *clsRet will be set.
    // Given an Array of Type Foo, returns Foo.
    // Given BYREF Foo, returns Foo
    virtual CorInfoType getChildType (
            CORINFO_CLASS_HANDLE       clsHnd,
            CORINFO_CLASS_HANDLE       *clsRet
            ) = 0;

    // Check constraints on type arguments of this class and parent classes
    virtual BOOL satisfiesClassConstraints(
            CORINFO_CLASS_HANDLE cls
            ) = 0;

    // Check if this is a single dimensional array type
    virtual BOOL isSDArray(
            CORINFO_CLASS_HANDLE        cls
            ) = 0;

    // Get the numbmer of dimensions in an array 
    virtual unsigned getArrayRank(
            CORINFO_CLASS_HANDLE        cls
            ) = 0;

    // Get static field data for an array
    virtual void * getArrayInitializationData(
            CORINFO_FIELD_HANDLE        field,
            DWORD                       size
            ) = 0;

    // Check Visibility rules.
    virtual CorInfoIsAccessAllowedResult canAccessClass(
                        CORINFO_RESOLVED_TOKEN * pResolvedToken,
                        CORINFO_METHOD_HANDLE   callerHandle,
                        CORINFO_HELPER_DESC    *pAccessHelper /* If canAccessMethod returns something other
                                                                 than ALLOWED, then this is filled in. */
                        ) = 0;

    /**********************************************************************************/
    //
    // ICorFieldInfo
    //
    /**********************************************************************************/

    // this function is for debugging only.  It returns the field name
    // and if 'moduleName' is non-null, it sets it to something that will
    // says which method (a class name, or a module name)
    virtual const char* getFieldName (
                        CORINFO_FIELD_HANDLE        ftn,        /* IN */
                        const char                **moduleName  /* OUT */
                        ) = 0;

    // return class it belongs to
    virtual CORINFO_CLASS_HANDLE getFieldClass (
                        CORINFO_FIELD_HANDLE    field
                        ) = 0;

    // Return the field's type, if it is CORINFO_TYPE_VALUECLASS 'structType' is set
    // the field's value class (if 'structType' == 0, then don't bother
    // the structure info).
    //
    // 'memberParent' is typically only set when verifying.  It should be the
    // result of calling getMemberParent.
    virtual CorInfoType getFieldType(
                        CORINFO_FIELD_HANDLE    field,
                        CORINFO_CLASS_HANDLE   *structType = NULL,
                        CORINFO_CLASS_HANDLE    memberParent = NULL /* IN */
                        ) = 0;

    // return the data member's instance offset
    virtual unsigned getFieldOffset(
                        CORINFO_FIELD_HANDLE    field
                        ) = 0;

    // TODO: jit64 should be switched to the same plan as the i386 jits - use
    // getClassGClayout to figure out the need for writebarrier helper, and inline the copying.
    // The interpretted value class copy is slow. Once this happens, USE_WRITE_BARRIER_HELPERS
    virtual bool isWriteBarrierHelperRequired(
                        CORINFO_FIELD_HANDLE    field) = 0;

    virtual void getFieldInfo (CORINFO_RESOLVED_TOKEN * pResolvedToken,
                               CORINFO_METHOD_HANDLE  callerHandle,
                               CORINFO_ACCESS_FLAGS   flags,
                               CORINFO_FIELD_INFO    *pResult
                              ) = 0;

    // Returns true iff "fldHnd" represents a static field.
    virtual bool isFieldStatic(CORINFO_FIELD_HANDLE fldHnd) = 0;

    /*********************************************************************************/
    //
    // ICorDebugInfo
    //
    /*********************************************************************************/

    // Query the EE to find out where interesting break points
    // in the code are.  The native compiler will ensure that these places
    // have a corresponding break point in native code.
    //
    // Note that unless CORJIT_FLAG_DEBUG_CODE is specified, this function will
    // be used only as a hint and the native compiler should not change its
    // code generation.
    virtual void getBoundaries(
                CORINFO_METHOD_HANDLE   ftn,                // [IN] method of interest
                unsigned int           *cILOffsets,         // [OUT] size of pILOffsets
                DWORD                 **pILOffsets,         // [OUT] IL offsets of interest
                                                            //       jit MUST free with freeArray!
                ICorDebugInfo::BoundaryTypes *implictBoundaries // [OUT] tell jit, all boundries of this type
                ) = 0;

    // Report back the mapping from IL to native code,
    // this map should include all boundaries that 'getBoundaries'
    // reported as interesting to the debugger.

    // Note that debugger (and profiler) is assuming that all of the
    // offsets form a contiguous block of memory, and that the
    // OffsetMapping is sorted in order of increasing native offset.
    virtual void setBoundaries(
                CORINFO_METHOD_HANDLE   ftn,            // [IN] method of interest
                ULONG32                 cMap,           // [IN] size of pMap
                ICorDebugInfo::OffsetMapping *pMap      // [IN] map including all points of interest.
                                                        //      jit allocated with allocateArray, EE frees
                ) = 0;

    // Query the EE to find out the scope of local varables.
    // normally the JIT would trash variables after last use, but
    // under debugging, the JIT needs to keep them live over their
    // entire scope so that they can be inspected.
    //
    // Note that unless CORJIT_FLAG_DEBUG_CODE is specified, this function will
    // be used only as a hint and the native compiler should not change its
    // code generation.
    virtual void getVars(
            CORINFO_METHOD_HANDLE           ftn,            // [IN]  method of interest
            ULONG32                        *cVars,          // [OUT] size of 'vars'
            ICorDebugInfo::ILVarInfo       **vars,          // [OUT] scopes of variables of interest
                                                            //       jit MUST free with freeArray!
            bool                           *extendOthers    // [OUT] it TRUE, then assume the scope
                                                            //       of unmentioned vars is entire method
            ) = 0;

    // Report back to the EE the location of every variable.
    // note that the JIT might split lifetimes into different
    // locations etc.

    virtual void setVars(
            CORINFO_METHOD_HANDLE           ftn,            // [IN] method of interest
            ULONG32                         cVars,          // [IN] size of 'vars'
            ICorDebugInfo::NativeVarInfo   *vars            // [IN] map telling where local vars are stored at what points
                                                            //      jit allocated with allocateArray, EE frees
            ) = 0;

    /*-------------------------- Misc ---------------------------------------*/

    // Used to allocate memory that needs to handed to the EE.
    // For eg, use this to allocated memory for reporting debug info,
    // which will be handed to the EE by setVars() and setBoundaries()
    virtual void * allocateArray(
                        ULONG              cBytes
                        ) = 0;

    // JitCompiler will free arrays passed by the EE using this
    // For eg, The EE returns memory in getVars() and getBoundaries()
    // to the JitCompiler, which the JitCompiler should release using
    // freeArray()
    virtual void freeArray(
            void               *array
            ) = 0;

    /*********************************************************************************/
    //
    // ICorArgInfo
    //
    /*********************************************************************************/

    // advance the pointer to the argument list.
    // a ptr of 0, is special and always means the first argument
    virtual CORINFO_ARG_LIST_HANDLE getArgNext (
            CORINFO_ARG_LIST_HANDLE     args            /* IN */
            ) = 0;

    // Get the type of a particular argument
    // CORINFO_TYPE_UNDEF is returned when there are no more arguments
    // If the type returned is a primitive type (or an enum) *vcTypeRet set to NULL
    // otherwise it is set to the TypeHandle associted with the type
    // Enumerations will always look their underlying type (probably should fix this)
    // Otherwise vcTypeRet is the type as would be seen by the IL,
    // The return value is the type that is used for calling convention purposes
    // (Thus if the EE wants a value class to be passed like an int, then it will
    // return CORINFO_TYPE_INT
    virtual CorInfoTypeWithMod getArgType (
            CORINFO_SIG_INFO*           sig,            /* IN */
            CORINFO_ARG_LIST_HANDLE     args,           /* IN */
            CORINFO_CLASS_HANDLE       *vcTypeRet       /* OUT */
            ) = 0;

    // If the Arg is a CORINFO_TYPE_CLASS fetch the class handle associated with it
    virtual CORINFO_CLASS_HANDLE getArgClass (
            CORINFO_SIG_INFO*           sig,            /* IN */
            CORINFO_ARG_LIST_HANDLE     args            /* IN */
            ) = 0;

    // Returns type of HFA for valuetype
    virtual CorInfoType getHFAType (
            CORINFO_CLASS_HANDLE hClass
            ) = 0;

 /*****************************************************************************
 * ICorErrorInfo contains methods to deal with SEH exceptions being thrown
 * from the corinfo interface.  These methods may be called when an exception
 * with code EXCEPTION_COMPLUS is caught.
 *****************************************************************************/

    // Returns the HRESULT of the current exception
    virtual HRESULT GetErrorHRESULT(
            struct _EXCEPTION_POINTERS *pExceptionPointers
            ) = 0;

    // Fetches the message of the current exception
    // Returns the size of the message (including terminating null). This can be
    // greater than bufferLength if the buffer is insufficient.
    virtual ULONG GetErrorMessage(
            __inout_ecount(bufferLength) LPWSTR buffer,
            ULONG bufferLength
            ) = 0;

    // returns EXCEPTION_EXECUTE_HANDLER if it is OK for the compile to handle the
    //                        exception, abort some work (like the inlining) and continue compilation
    // returns EXCEPTION_CONTINUE_SEARCH if exception must always be handled by the EE
    //                    things like ThreadStoppedException ...
    // returns EXCEPTION_CONTINUE_EXECUTION if exception is fixed up by the EE

    virtual int FilterException(
            struct _EXCEPTION_POINTERS *pExceptionPointers
            ) = 0;

    // Cleans up internal EE tracking when an exception is caught.
    virtual void HandleException(
            struct _EXCEPTION_POINTERS *pExceptionPointers
            ) = 0;

    virtual void ThrowExceptionForJitResult(
            HRESULT result) = 0;

    //Throws an exception defined by the given throw helper.
    virtual void ThrowExceptionForHelper(
            const CORINFO_HELPER_DESC * throwHelper) = 0;

    // Runs the given function under an error trap. This allows the JIT to make calls
    // to interface functions that may throw exceptions without needing to be aware of
    // the EH ABI, exception types, etc. Returns true if the given function completed
    // successfully and false otherwise.
    virtual bool runWithErrorTrap(
        void (*function)(void*), // The function to run
        void* parameter          // The context parameter that will be passed to the function and the handler
        ) = 0;

/*****************************************************************************
 * ICorStaticInfo contains EE interface methods which return values that are
 * constant from invocation to invocation.  Thus they may be embedded in
 * persisted information like statically generated code. (This is of course
 * assuming that all code versions are identical each time.)
 *****************************************************************************/

    // Return details about EE internal data structures
    virtual void getEEInfo(
                CORINFO_EE_INFO            *pEEInfoOut
                ) = 0;

    // Returns name of the JIT timer log
    virtual LPCWSTR getJitTimeLogFilename() = 0;

    /*********************************************************************************/
    //
    // Diagnostic methods
    //
    /*********************************************************************************/

    // this function is for debugging only. Returns method token.
    // Returns mdMethodDefNil for dynamic methods.
    virtual mdMethodDef getMethodDefFromMethod(
            CORINFO_METHOD_HANDLE hMethod
            ) = 0;

    // this function is for debugging only.  It returns the method name
    // and if 'moduleName' is non-null, it sets it to something that will
    // says which method (a class name, or a module name)
    virtual const char* getMethodName (
            CORINFO_METHOD_HANDLE       ftn,        /* IN */
            const char                **moduleName  /* OUT */
            ) = 0;

    // Return method name as in metadata, or nullptr if there is none,
    // and optionally return the class, enclosing class, and namespace names 
    // as in metadata.
    // Suitable for non-debugging use.
    virtual const char* getMethodNameFromMetadata(
            CORINFO_METHOD_HANDLE       ftn,                  /* IN */
            const char                **className,            /* OUT */
            const char                **namespaceName,        /* OUT */
            const char                **enclosingClassName   /* OUT */
            ) = 0;

    // this function is for debugging only.  It returns a value that
    // is will always be the same for a given method.  It is used
    // to implement the 'jitRange' functionality
    virtual unsigned getMethodHash (
            CORINFO_METHOD_HANDLE       ftn         /* IN */
            ) = 0;

    // this function is for debugging only.
    virtual size_t findNameOfToken (
            CORINFO_MODULE_HANDLE       module,     /* IN  */
            mdToken                     metaTOK,     /* IN  */
            __out_ecount (FQNameCapacity) char * szFQName, /* OUT */
            size_t FQNameCapacity  /* IN */
            ) = 0;

    // returns whether the struct is enregisterable. Only valid on a System V VM. Returns true on success, false on failure.
    virtual bool getSystemVAmd64PassStructInRegisterDescriptor(
        /* IN */    CORINFO_CLASS_HANDLE        structHnd,
        /* OUT */   SYSTEMV_AMD64_CORINFO_STRUCT_REG_PASSING_DESCRIPTOR* structPassInRegDescPtr
        ) = 0;

};

/*****************************************************************************
 * ICorDynamicInfo contains EE interface methods which return values that may
 * change from invocation to invocation.  They cannot be embedded in persisted
 * data; they must be requeried each time the EE is run.
 *****************************************************************************/

class ICorDynamicInfo : public ICorStaticInfo
{
public:

    //
    // These methods return values to the JIT which are not constant
    // from session to session.
    //
    // These methods take an extra parameter : void **ppIndirection.
    // If a JIT supports generation of prejit code (install-o-jit), it
    // must pass a non-null value for this parameter, and check the
    // resulting value.  If *ppIndirection is NULL, code should be
    // generated normally.  If non-null, then the value of
    // *ppIndirection is an address in the cookie table, and the code
    // generator needs to generate an indirection through the table to
    // get the resulting value.  In this case, the return result of the
    // function must NOT be directly embedded in the generated code.
    //
    // Note that if a JIT does not support prejit code generation, it
    // may ignore the extra parameter & pass the default of NULL - the
    // prejit ICorDynamicInfo implementation will see this & generate
    // an error if the jitter is used in a prejit scenario.
    //

    // Return details about EE internal data structures

    virtual DWORD getThreadTLSIndex(
                    void                  **ppIndirection = NULL
                    ) = 0;

    virtual const void * getInlinedCallFrameVptr(
                    void                  **ppIndirection = NULL
                    ) = 0;

    virtual LONG * getAddrOfCaptureThreadGlobal(
                    void                  **ppIndirection = NULL
                    ) = 0;

    // return the native entry point to an EE helper (see CorInfoHelpFunc)
    virtual void* getHelperFtn (
                    CorInfoHelpFunc         ftnNum,
                    void                  **ppIndirection = NULL
                    ) = 0;

    // return a callable address of the function (native code). This function
    // may return a different value (depending on whether the method has
    // been JITed or not.
    virtual void getFunctionEntryPoint(
                              CORINFO_METHOD_HANDLE   ftn,                 /* IN  */
                              CORINFO_CONST_LOOKUP *  pResult,             /* OUT */
                              CORINFO_ACCESS_FLAGS    accessFlags = CORINFO_ACCESS_ANY) = 0;

    // return a directly callable address. This can be used similarly to the
    // value returned by getFunctionEntryPoint() except that it is
    // guaranteed to be multi callable entrypoint.
    virtual void getFunctionFixedEntryPoint(
                              CORINFO_METHOD_HANDLE   ftn,
                              CORINFO_CONST_LOOKUP *  pResult) = 0;

    // get the synchronization handle that is passed to monXstatic function
    virtual void* getMethodSync(
                    CORINFO_METHOD_HANDLE               ftn,
                    void                  **ppIndirection = NULL
                    ) = 0;

    // get slow lazy string literal helper to use (CORINFO_HELP_STRCNS*). 
    // Returns CORINFO_HELP_UNDEF if lazy string literal helper cannot be used.
    virtual CorInfoHelpFunc getLazyStringLiteralHelper(
                    CORINFO_MODULE_HANDLE   handle
                    ) = 0;

    virtual CORINFO_MODULE_HANDLE embedModuleHandle(
                    CORINFO_MODULE_HANDLE   handle,
                    void                  **ppIndirection = NULL
                    ) = 0;

    virtual CORINFO_CLASS_HANDLE embedClassHandle(
                    CORINFO_CLASS_HANDLE    handle,
                    void                  **ppIndirection = NULL
                    ) = 0;

    virtual CORINFO_METHOD_HANDLE embedMethodHandle(
                    CORINFO_METHOD_HANDLE   handle,
                    void                  **ppIndirection = NULL
                    ) = 0;

    virtual CORINFO_FIELD_HANDLE embedFieldHandle(
                    CORINFO_FIELD_HANDLE    handle,
                    void                  **ppIndirection = NULL
                    ) = 0;

    // Given a module scope (module), a method handle (context) and
    // a metadata token (metaTOK), fetch the handle
    // (type, field or method) associated with the token.
    // If this is not possible at compile-time (because the current method's
    // code is shared and the token contains generic parameters)
    // then indicate how the handle should be looked up at run-time.
    //
    virtual void embedGenericHandle(
                        CORINFO_RESOLVED_TOKEN *        pResolvedToken,
                        BOOL                            fEmbedParent, // TRUE - embeds parent type handle of the field/method handle
                        CORINFO_GENERICHANDLE_RESULT *  pResult) = 0;

    // Return information used to locate the exact enclosing type of the current method.
    // Used only to invoke .cctor method from code shared across generic instantiations
    //   !needsRuntimeLookup       statically known (enclosing type of method itself)
    //   needsRuntimeLookup:
    //      CORINFO_LOOKUP_THISOBJ     use vtable pointer of 'this' param
    //      CORINFO_LOOKUP_CLASSPARAM  use vtable hidden param
    //      CORINFO_LOOKUP_METHODPARAM use enclosing type of method-desc hidden param
    virtual CORINFO_LOOKUP_KIND getLocationOfThisType(
                    CORINFO_METHOD_HANDLE context
                    ) = 0;

    // NOTE: the two methods below--getPInvokeUnmanagedTarget and getAddressOfPInvokeFixup--are
    //       deprecated. New code should instead use getAddressOfPInvokeTarget, which subsumes the
    //       functionality of these methods.

    // return the unmanaged target *if method has already been prelinked.*
    virtual void* getPInvokeUnmanagedTarget(
                    CORINFO_METHOD_HANDLE   method,
                    void                  **ppIndirection = NULL
                    ) = 0;

    // return address of fixup area for late-bound PInvoke calls.
    virtual void* getAddressOfPInvokeFixup(
                    CORINFO_METHOD_HANDLE   method,
                    void                  **ppIndirection = NULL
                    ) = 0;

    // return the address of the PInvoke target. May be a fixup area in the
    // case of late-bound PInvoke calls.
    virtual void getAddressOfPInvokeTarget(
                    CORINFO_METHOD_HANDLE  method,
                    CORINFO_CONST_LOOKUP  *pLookup
                    ) = 0;

    // Generate a cookie based on the signature that would needs to be passed
    // to CORINFO_HELP_PINVOKE_CALLI
    virtual LPVOID GetCookieForPInvokeCalliSig(
            CORINFO_SIG_INFO* szMetaSig,
            void           ** ppIndirection = NULL
            ) = 0;

    // returns true if a VM cookie can be generated for it (might be false due to cross-module
    // inlining, in which case the inlining should be aborted)
    virtual bool canGetCookieForPInvokeCalliSig(
                    CORINFO_SIG_INFO* szMetaSig
                    ) = 0;

    // Gets a handle that is checked to see if the current method is
    // included in "JustMyCode"
    virtual CORINFO_JUST_MY_CODE_HANDLE getJustMyCodeHandle(
                    CORINFO_METHOD_HANDLE       method,
                    CORINFO_JUST_MY_CODE_HANDLE**ppIndirection = NULL
                    ) = 0;

    // Gets a method handle that can be used to correlate profiling data.
    // This is the IP of a native method, or the address of the descriptor struct
    // for IL.  Always guaranteed to be unique per process, and not to move. */
    virtual void GetProfilingHandle(
                    BOOL                      *pbHookFunction,
                    void                     **pProfilerHandle,
                    BOOL                      *pbIndirectedHandles
                    ) = 0;

    // Returns instructions on how to make the call. See code:CORINFO_CALL_INFO for possible return values.
    virtual void getCallInfo(
                        // Token info
                        CORINFO_RESOLVED_TOKEN * pResolvedToken,

                        //Generics info
                        CORINFO_RESOLVED_TOKEN * pConstrainedResolvedToken,

                        //Security info
                        CORINFO_METHOD_HANDLE   callerHandle,

                        //Jit info
                        CORINFO_CALLINFO_FLAGS  flags,

                        //out params
                        CORINFO_CALL_INFO       *pResult
                        ) = 0;

    virtual BOOL canAccessFamily(CORINFO_METHOD_HANDLE hCaller,
                                           CORINFO_CLASS_HANDLE hInstanceType) = 0;

    // Returns TRUE if the Class Domain ID is the RID of the class (currently true for every class
    // except reflection emitted classes and generics)
    virtual BOOL isRIDClassDomainID(CORINFO_CLASS_HANDLE cls) = 0;

    // returns the class's domain ID for accessing shared statics
    virtual unsigned getClassDomainID (
                    CORINFO_CLASS_HANDLE    cls,
                    void                  **ppIndirection = NULL
                    ) = 0;


    // return the data's address (for static fields only)
    virtual void* getFieldAddress(
                    CORINFO_FIELD_HANDLE    field,
                    void                  **ppIndirection = NULL
                    ) = 0;

    // If pIsSpeculative is NULL, return the class handle for the value of ref-class typed
    // static readonly fields, if there is a unique location for the static and the class
    // is already initialized.
    // 
    // If pIsSpeculative is not NULL, fetch the class handle for the value of all ref-class
    // typed static fields, if there is a unique location for the static and the field is
    // not null.
    //
    // Set *pIsSpeculative true if this type may change over time (field is not readonly or
    // is readonly but class has not yet finished initialization). Set *pIsSpeculative false
    // if this type will not change.
    virtual CORINFO_CLASS_HANDLE getStaticFieldCurrentClass(
                    CORINFO_FIELD_HANDLE    field,
                    bool                   *pIsSpeculative = NULL
                    ) = 0;

    // registers a vararg sig & returns a VM cookie for it (which can contain other stuff)
    virtual CORINFO_VARARGS_HANDLE getVarArgsHandle(
                    CORINFO_SIG_INFO       *pSig,
                    void                  **ppIndirection = NULL
                    ) = 0;

    // returns true if a VM cookie can be generated for it (might be false due to cross-module
    // inlining, in which case the inlining should be aborted)
    virtual bool canGetVarArgsHandle(
                    CORINFO_SIG_INFO       *pSig
                    ) = 0;

    // Allocate a string literal on the heap and return a handle to it
    virtual InfoAccessType constructStringLiteral(
                    CORINFO_MODULE_HANDLE   module,
                    mdToken                 metaTok,
                    void                  **ppValue
                    ) = 0;

    virtual InfoAccessType emptyStringLiteral(
                    void                  **ppValue
                    ) = 0;

    // (static fields only) given that 'field' refers to thread local store,
    // return the ID (TLS index), which is used to find the begining of the
    // TLS data area for the particular DLL 'field' is associated with.
    virtual DWORD getFieldThreadLocalStoreID (
                    CORINFO_FIELD_HANDLE    field,
                    void                  **ppIndirection = NULL
                    ) = 0;

    // Sets another object to intercept calls to "self" and current method being compiled
    virtual void setOverride(
                ICorDynamicInfo             *pOverride,
                CORINFO_METHOD_HANDLE       currentMethod
                ) = 0;

    // Adds an active dependency from the context method's module to the given module
    // This is internal callback for the EE. JIT should not call it directly.
    virtual void addActiveDependency(
               CORINFO_MODULE_HANDLE       moduleFrom,
               CORINFO_MODULE_HANDLE       moduleTo
                ) = 0;

    virtual CORINFO_METHOD_HANDLE GetDelegateCtor(
            CORINFO_METHOD_HANDLE  methHnd,
            CORINFO_CLASS_HANDLE   clsHnd,
            CORINFO_METHOD_HANDLE  targetMethodHnd,
            DelegateCtorArgs *     pCtorData
            ) = 0;

    virtual void MethodCompileComplete(
                CORINFO_METHOD_HANDLE methHnd
                ) = 0;

    // return a thunk that will copy the arguments for the given signature.
    virtual void* getTailCallCopyArgsThunk (
                    CORINFO_SIG_INFO       *pSig,
                    CorInfoHelperTailCallSpecialHandling flags
                    ) = 0;

    // Optionally, convert calli to regular method call. This is for PInvoke argument marshalling.
    virtual bool convertPInvokeCalliToCall(
                    CORINFO_RESOLVED_TOKEN * pResolvedToken,
                    bool fMustConvert
                    ) = 0;
};

/**********************************************************************************/

// It would be nicer to use existing IMAGE_REL_XXX constants instead of defining our own here...
#define IMAGE_REL_BASED_REL32           0x10
#define IMAGE_REL_BASED_THUMB_BRANCH24  0x13

// The identifier for ARM32-specific PC-relative address
// computation corresponds to the following instruction
// sequence:
//  l0: movw rX, #imm_lo  // 4 byte
//  l4: movt rX, #imm_hi  // 4 byte
//  l8: add  rX, pc <- after this instruction rX = relocTarget
//
// Program counter at l8 is address of l8 + 4
// Address of relocated movw/movt is l0
// So, imm should be calculated as the following:
//  imm = relocTarget - (l8 + 4) = relocTarget - (l0 + 8 + 4) = relocTarget - (l_0 + 12)
// So, the value of offset correction is 12
//
#define IMAGE_REL_BASED_REL_THUMB_MOV32_PCREL   0x14

#endif // _COR_INFO_H_
