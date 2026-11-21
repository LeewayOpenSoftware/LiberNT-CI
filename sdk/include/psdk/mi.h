/*
 * PROJECT:     LiberNT Management Infrastructure (MI) client
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal MI client ABI used by Microsoft.Management.Infrastructure
 *              (pwsh Get-CimInstance). Clean-room declarations authored from the
 *              public MI API reference; ABI (member order/types) matches the
 *              documented mi.h so the stock MMI native bridge binds mi_clientFT_V1.
 *              No text is copied from the Windows SDK mi.h; the layout was
 *              cross-checked against the Apache-2.0 OMI MI.h (one-way compatible
 *              into GPL). See dotnet-powershell/MI-ABI.md.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#ifndef _MI_H_
#define _MI_H_

#include <stddef.h>  /* ptrdiff_t */

#ifdef __cplusplus
# define MI_EXTERN_C extern "C"
#else
# define MI_EXTERN_C extern
#endif

/* Calling convention: __stdcall on i386 (the stock MMI bridge calls the FT
 * members as __stdcall; a function-pointer call bypasses the link-time arity
 * check, so a mismatch corrupts the i386 stack silently). Ignored on x64.
 * Exported names stay undecorated via mi.spec. */
#if defined(_M_IX86) || defined(__i386__)
# define MI_CALL      __stdcall
# define MI_MAIN_CALL __stdcall
#else
# define MI_CALL
# define MI_MAIN_CALL
#endif

#if defined(__GNUC__) || defined(__clang__)
# define MI_INLINE static __inline__
#else
# define MI_INLINE static __inline
#endif

/* MI_Char is UTF-16 on Windows (matches wbemprox BSTR/WCHAR). */
#ifndef MI_CHAR_TYPE
# define MI_CHAR_TYPE 2
#endif
#if (MI_CHAR_TYPE == 2)
typedef wchar_t MI_Char;
# define MI_T(str) L##str
#else
typedef char MI_Char;
# define MI_T(str) str
#endif

/* ---- scalar types ---- */
typedef unsigned char       MI_Boolean;
typedef unsigned char       MI_Uint8;
typedef signed char         MI_Sint8;
typedef unsigned short      MI_Uint16;
typedef signed short        MI_Sint16;
typedef unsigned int        MI_Uint32;
typedef signed int          MI_Sint32;
typedef unsigned long long  MI_Uint64;
typedef signed long long    MI_Sint64;
typedef float               MI_Real32;
typedef double              MI_Real64;
typedef unsigned short      MI_Char16;

#define MI_TRUE  ((MI_Boolean)1)
#define MI_FALSE ((MI_Boolean)0)

typedef enum _MI_Result
{
    MI_RESULT_OK = 0,
    MI_RESULT_FAILED = 1,
    MI_RESULT_ACCESS_DENIED = 2,
    MI_RESULT_INVALID_NAMESPACE = 3,
    MI_RESULT_INVALID_PARAMETER = 4,
    MI_RESULT_INVALID_CLASS = 5,
    MI_RESULT_NOT_FOUND = 6,
    MI_RESULT_NOT_SUPPORTED = 7,
    MI_RESULT_CLASS_HAS_CHILDREN = 8,
    MI_RESULT_CLASS_HAS_INSTANCES = 9,
    MI_RESULT_INVALID_SUPERCLASS = 10,
    MI_RESULT_ALREADY_EXISTS = 11,
    MI_RESULT_NO_SUCH_PROPERTY = 12,
    MI_RESULT_TYPE_MISMATCH = 13,
    MI_RESULT_QUERY_LANGUAGE_NOT_SUPPORTED = 14,
    MI_RESULT_INVALID_QUERY = 15,
    MI_RESULT_METHOD_NOT_AVAILABLE = 16,
    MI_RESULT_METHOD_NOT_FOUND = 17,
    MI_RESULT_NAMESPACE_NOT_EMPTY = 20,
    MI_RESULT_INVALID_ENUMERATION_CONTEXT = 21,
    MI_RESULT_INVALID_OPERATION_TIMEOUT = 22,
    MI_RESULT_PULL_HAS_BEEN_ABANDONED = 23,
    MI_RESULT_PULL_CANNOT_BE_ABANDONED = 24,
    MI_RESULT_FILTERED_ENUMERATION_NOT_SUPPORTED = 25,
    MI_RESULT_CONTINUATION_ON_ERROR_NOT_SUPPORTED = 26,
    MI_RESULT_SERVER_LIMITS_EXCEEDED = 27,
    MI_RESULT_SERVER_IS_SHUTTING_DOWN = 28
}
MI_Result;

#define MI_ARRAY_TYPE 16
typedef enum _MI_Type
{
    MI_BOOLEAN = 0, MI_UINT8 = 1, MI_SINT8 = 2, MI_UINT16 = 3, MI_SINT16 = 4,
    MI_UINT32 = 5, MI_SINT32 = 6, MI_UINT64 = 7, MI_SINT64 = 8, MI_REAL32 = 9,
    MI_REAL64 = 10, MI_CHAR16 = 11, MI_DATETIME = 12, MI_STRING = 13,
    MI_REFERENCE = 14, MI_INSTANCE = 15,
    MI_BOOLEANA = 16, MI_UINT8A = 17, MI_SINT8A = 18, MI_UINT16A = 19,
    MI_SINT16A = 20, MI_UINT32A = 21, MI_SINT32A = 22, MI_UINT64A = 23,
    MI_SINT64A = 24, MI_REAL32A = 25, MI_REAL64A = 26, MI_CHAR16A = 27,
    MI_DATETIMEA = 28, MI_STRINGA = 29, MI_REFERENCEA = 30, MI_INSTANCEA = 31
}
MI_Type;

/* element flags (MI_Flags subset used at the instance API) */
#define MI_FLAG_NULL        0x00000020
#define MI_FLAG_BORROW      0x00000040
#define MI_FLAG_ADOPT       0x00000080

typedef struct _MI_Timestamp
{
    MI_Uint32 year, month, day, hour, minute, second, microseconds;
    MI_Sint32 utc;
}
MI_Timestamp;

typedef struct _MI_Interval
{
    MI_Uint32 days, hours, minutes, seconds, microseconds;
    MI_Uint32 __padding1, __padding2, __padding3;
}
MI_Interval;

typedef struct _MI_Datetime
{
    MI_Uint32 isTimestamp;
    union
    {
        MI_Timestamp timestamp;
        MI_Interval interval;
    } u;
}
MI_Datetime;

/* forward declarations */
typedef struct _MI_Instance             MI_Instance;
typedef struct _MI_InstanceFT           MI_InstanceFT;
typedef struct _MI_Class                MI_Class;
typedef struct _MI_ClassDecl            MI_ClassDecl;
typedef struct _MI_Application          MI_Application;
typedef struct _MI_ApplicationFT        MI_ApplicationFT;
typedef struct _MI_Session              MI_Session;
typedef struct _MI_SessionFT            MI_SessionFT;
typedef struct _MI_Operation            MI_Operation;
typedef struct _MI_OperationFT          MI_OperationFT;
typedef struct _MI_OperationCallbacks   MI_OperationCallbacks;
typedef struct _MI_SessionCallbacks     MI_SessionCallbacks;
typedef struct _MI_DestinationOptions   MI_DestinationOptions;
typedef struct _MI_DestinationOptionsFT MI_DestinationOptionsFT;
typedef struct _MI_OperationOptions     MI_OperationOptions;
typedef struct _MI_OperationOptionsFT   MI_OperationOptionsFT;
typedef struct _MI_UtilitiesFT          MI_UtilitiesFT;
typedef struct _MI_UserCredentials      MI_UserCredentials;
/* FTs declared only so MI_ClientFT_V1 can hold pointers; left incomplete
 * because v1 publishes them as NULL (serializer/provider/subscription). */
typedef struct _MI_HostedProvider       MI_HostedProvider;
typedef struct _MI_HostedProviderFT     MI_HostedProviderFT;
typedef struct _MI_Serializer           MI_Serializer;
typedef struct _MI_SerializerFT         MI_SerializerFT;
typedef struct _MI_Deserializer         MI_Deserializer;
typedef struct _MI_DeserializerFT       MI_DeserializerFT;
typedef struct _MI_SubscriptionDeliveryOptions   MI_SubscriptionDeliveryOptions;
typedef struct _MI_SubscriptionDeliveryOptionsFT MI_SubscriptionDeliveryOptionsFT;

/* ---- array types ---- */
typedef struct _MI_Array    { void*          data; MI_Uint32 size; } MI_Array;
typedef struct _MI_BooleanA { MI_Boolean*    data; MI_Uint32 size; } MI_BooleanA;
typedef struct _MI_Uint8A   { MI_Uint8*      data; MI_Uint32 size; } MI_Uint8A;
typedef struct _MI_Sint8A   { MI_Sint8*      data; MI_Uint32 size; } MI_Sint8A;
typedef struct _MI_Uint16A  { MI_Uint16*     data; MI_Uint32 size; } MI_Uint16A;
typedef struct _MI_Sint16A  { MI_Sint16*     data; MI_Uint32 size; } MI_Sint16A;
typedef struct _MI_Uint32A  { MI_Uint32*     data; MI_Uint32 size; } MI_Uint32A;
typedef struct _MI_Sint32A  { MI_Sint32*     data; MI_Uint32 size; } MI_Sint32A;
typedef struct _MI_Uint64A  { MI_Uint64*     data; MI_Uint32 size; } MI_Uint64A;
typedef struct _MI_Sint64A  { MI_Sint64*     data; MI_Uint32 size; } MI_Sint64A;
typedef struct _MI_Real32A  { MI_Real32*     data; MI_Uint32 size; } MI_Real32A;
typedef struct _MI_Real64A  { MI_Real64*     data; MI_Uint32 size; } MI_Real64A;
typedef struct _MI_Char16A  { MI_Char16*     data; MI_Uint32 size; } MI_Char16A;
typedef struct _MI_DatetimeA{ MI_Datetime*   data; MI_Uint32 size; } MI_DatetimeA;
typedef struct _MI_StringA  { MI_Char**      data; MI_Uint32 size; } MI_StringA;
typedef struct _MI_ReferenceA { MI_Instance**data; MI_Uint32 size; } MI_ReferenceA;
typedef struct _MI_InstanceA  { MI_Instance**data; MI_Uint32 size; } MI_InstanceA;

typedef union _MI_Value
{
    MI_Boolean   boolean;
    MI_Uint8     uint8;
    MI_Sint8     sint8;
    MI_Uint16    uint16;
    MI_Sint16    sint16;
    MI_Uint32    uint32;
    MI_Sint32    sint32;
    MI_Uint64    uint64;
    MI_Sint64    sint64;
    MI_Real32    real32;
    MI_Real64    real64;
    MI_Char16    char16;
    MI_Datetime  datetime;
    MI_Char*     string;
    MI_Instance* instance;
    MI_Instance* reference;
    MI_BooleanA  booleana;
    MI_Uint8A    uint8a;
    MI_Sint8A    sint8a;
    MI_Uint16A   uint16a;
    MI_Sint16A   sint16a;
    MI_Uint32A   uint32a;
    MI_Sint32A   sint32a;
    MI_Uint64A   uint64a;
    MI_Sint64A   sint64a;
    MI_Real32A   real32a;
    MI_Real64A   real64a;
    MI_Char16A   char16a;
    MI_DatetimeA datetimea;
    MI_StringA   stringa;
    MI_ReferenceA referencea;
    MI_InstanceA instancea;
    MI_Array     array;
}
MI_Value;

/* ---- MI_Instance ---- */
struct _MI_Instance
{
    const MI_InstanceFT* ft;
    const MI_ClassDecl*  classDecl;
    const MI_Char*       serverName;
    const MI_Char*       nameSpace;
    ptrdiff_t            reserved[4];
};

struct _MI_InstanceFT
{
    MI_Result (MI_CALL *Clone)(const MI_Instance* self, MI_Instance** newInstance);
    MI_Result (MI_CALL *Destruct)(MI_Instance* self);
    MI_Result (MI_CALL *Delete)(MI_Instance* self);
    MI_Result (MI_CALL *IsA)(const MI_Instance* self, const MI_ClassDecl* classDecl, MI_Boolean* flag);
    MI_Result (MI_CALL *GetClassName)(const MI_Instance* self, const MI_Char** className);
    MI_Result (MI_CALL *SetNameSpace)(MI_Instance* self, const MI_Char* nameSpace);
    MI_Result (MI_CALL *GetNameSpace)(const MI_Instance* self, const MI_Char** nameSpace);
    MI_Result (MI_CALL *GetElementCount)(const MI_Instance* self, MI_Uint32* count);
    MI_Result (MI_CALL *AddElement)(MI_Instance* self, const MI_Char* name, const MI_Value* value, MI_Type type, MI_Uint32 flags);
    MI_Result (MI_CALL *SetElement)(MI_Instance* self, const MI_Char* name, const MI_Value* value, MI_Type type, MI_Uint32 flags);
    MI_Result (MI_CALL *SetElementAt)(MI_Instance* self, MI_Uint32 index, const MI_Value* value, MI_Type type, MI_Uint32 flags);
    MI_Result (MI_CALL *GetElement)(const MI_Instance* self, const MI_Char* name, MI_Value* value, MI_Type* type, MI_Uint32* flags, MI_Uint32* index);
    MI_Result (MI_CALL *GetElementAt)(const MI_Instance* self, MI_Uint32 index, const MI_Char** name, MI_Value* value, MI_Type* type, MI_Uint32* flags);
    MI_Result (MI_CALL *ClearElement)(MI_Instance* self, const MI_Char* name);
    MI_Result (MI_CALL *ClearElementAt)(MI_Instance* self, MI_Uint32 index);
    MI_Result (MI_CALL *GetServerName)(const MI_Instance* self, const MI_Char** name);
    MI_Result (MI_CALL *SetServerName)(MI_Instance* self, const MI_Char* name);
    MI_Result (MI_CALL *GetClass)(const MI_Instance* self, MI_Class** instanceClass);
};

/* ---- handles share one shape: {reserved1, reserved2, ft} ---- */
struct _MI_Application   { MI_Uint64 reserved1; ptrdiff_t reserved2; const MI_ApplicationFT* ft; };
struct _MI_Session       { MI_Uint64 reserved1; ptrdiff_t reserved2; const MI_SessionFT* ft; };
struct _MI_Operation     { MI_Uint64 reserved1; ptrdiff_t reserved2; const MI_OperationFT* ft; };
struct _MI_DestinationOptions { MI_Uint64 reserved1; ptrdiff_t reserved2; const MI_DestinationOptionsFT* ft; };
struct _MI_OperationOptions   { MI_Uint64 reserved1; ptrdiff_t reserved2; const MI_OperationOptionsFT* ft; };

#define MI_APPLICATION_NULL   { 0, 0, 0 }
#define MI_SESSION_NULL       { 0, 0, 0 }
#define MI_OPERATION_NULL     { 0, 0, 0 }
#define MI_DESTINATIONOPTIONS_NULL { 0, 0, 0 }
#define MI_OPERATIONOPTIONS_NULL   { 0, 0, 0 }

/* ---- operation callbacks ---- */
typedef enum _MI_CancellationReason { MI_REASON_NONE = 0, MI_REASON_TIMEOUT = 1, MI_REASON_SHUTDOWN = 2, MI_REASON_SERVICESTOP = 3 } MI_CancellationReason;
typedef enum _MI_PromptType { MI_PROMPTTYPE_NORMAL = 0, MI_PROMPTTYPE_CRITICAL = 1 } MI_PromptType;
typedef enum _MI_OperationCallback_ResponseType { MI_OPERATIONCALLBACK_RESPONSE_NO = 0, MI_OPERATIONCALLBACK_RESPONSE_YES = 1, MI_OPERATIONCALLBACK_RESPONSE_NOTOALL = 2, MI_OPERATIONCALLBACK_RESPONSE_YESTOALL = 3 } MI_OperationCallback_ResponseType;

typedef void (MI_CALL *MI_OperationCallback_PromptUser)(
    MI_Operation* operation, void* callbackContext, const MI_Char* message, MI_PromptType promptType,
    MI_Result (MI_CALL *promptUserResult)(MI_Operation* operation, MI_OperationCallback_ResponseType response));

typedef void (MI_CALL *MI_OperationCallback_WriteError)(
    MI_Operation* operation, void* callbackContext, MI_Instance* instance,
    MI_Result (MI_CALL *writeErrorResult)(MI_Operation* operation, MI_OperationCallback_ResponseType response));

typedef void (MI_CALL *MI_OperationCallback_WriteMessage)(
    MI_Operation* operation, void* callbackContext, MI_Uint32 channel, const MI_Char* message);

typedef void (MI_CALL *MI_OperationCallback_WriteProgress)(
    MI_Operation* operation, void* callbackContext, const MI_Char* activity, const MI_Char* currentOperation,
    const MI_Char* statusDescription, MI_Uint32 percentageComplete, MI_Uint32 secondsRemaining);

typedef void (MI_CALL *MI_OperationCallback_Instance)(
    MI_Operation* operation, void* callbackContext, const MI_Instance* instance, MI_Boolean moreResults,
    MI_Result resultCode, const MI_Char* errorString, const MI_Instance* errorDetails,
    MI_Result (MI_CALL *resultAcknowledgement)(MI_Operation* operation));

typedef void (MI_CALL *MI_OperationCallback_Indication)(
    MI_Operation* operation, void* callbackContext, const MI_Instance* instance, const MI_Char* bookmark,
    const MI_Char* machineID, MI_Boolean moreResults, MI_Result resultCode, const MI_Char* errorString,
    const MI_Instance* errorDetails, MI_Result (MI_CALL *resultAcknowledgement)(MI_Operation* operation));

typedef void (MI_CALL *MI_OperationCallback_Class)(
    MI_Operation* operation, void* callbackContext, const MI_Class* classResult, MI_Boolean moreResults,
    MI_Result resultCode, const MI_Char* errorString, const MI_Instance* errorDetails,
    MI_Result (MI_CALL *resultAcknowledgement)(MI_Operation* operation));

typedef void (MI_CALL *MI_OperationCallback_StreamedParameter)(
    MI_Operation* operation, void* callbackContext, const MI_Char* parameterName, MI_Type resultType,
    const MI_Value* result, MI_Result (MI_CALL *resultAcknowledgement)(MI_Operation* operation));

struct _MI_OperationCallbacks
{
    void* callbackContext;
    MI_OperationCallback_PromptUser        promptUser;
    MI_OperationCallback_WriteError        writeError;
    MI_OperationCallback_WriteMessage      writeMessage;
    MI_OperationCallback_WriteProgress     writeProgress;
    MI_OperationCallback_Instance          instanceResult;
    MI_OperationCallback_Indication        indicationResult;
    MI_OperationCallback_Class             classResult;
    MI_OperationCallback_StreamedParameter streamedParameterResult;
};
#define MI_OPERATIONCALLBACKS_NULL { 0 }

/* ---- function tables ---- */
struct _MI_OperationFT
{
    MI_Result (MI_CALL *Close)(MI_Operation* operation);
    MI_Result (MI_CALL *Cancel)(MI_Operation* operation, MI_CancellationReason reason);
    MI_Result (MI_CALL *GetSession)(MI_Operation* operation, MI_Session* session);
    MI_Result (MI_CALL *GetInstance)(MI_Operation* operation, const MI_Instance** instance, MI_Boolean* moreResults,
                                     MI_Result* result, const MI_Char** errorMessage, const MI_Instance** completionDetails);
    MI_Result (MI_CALL *GetIndication)(MI_Operation* operation, const MI_Instance** instance, const MI_Char** bookmark,
                                       const MI_Char** machineID, MI_Boolean* moreResults, MI_Result* result,
                                       const MI_Char** errorMessage, const MI_Instance** completionDetails);
    MI_Result (MI_CALL *GetClass)(MI_Operation* operation, const MI_Class** classResult, MI_Boolean* moreResults,
                                  MI_Result* result, const MI_Char** errorMessage, const MI_Instance** completionDetails);
};

struct _MI_SessionFT
{
    MI_Result (MI_CALL *Close)(MI_Session* session, void* completionContext, void (MI_CALL *completionCallback)(void* completionContext));
    MI_Result (MI_CALL *GetApplication)(MI_Session* session, MI_Application* application);
    void (MI_CALL *GetInstance)(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options, const MI_Char* namespaceName,
                                const MI_Instance* inboundInstance, MI_OperationCallbacks* callbacks, MI_Operation* operation);
    void (MI_CALL *ModifyInstance)(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options, const MI_Char* namespaceName,
                                   const MI_Instance* inboundInstance, MI_OperationCallbacks* callbacks, MI_Operation* operation);
    void (MI_CALL *CreateInstance)(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options, const MI_Char* namespaceName,
                                   const MI_Instance* inboundInstance, MI_OperationCallbacks* callbacks, MI_Operation* operation);
    void (MI_CALL *DeleteInstance)(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options, const MI_Char* namespaceName,
                                   const MI_Instance* inboundInstance, MI_OperationCallbacks* callbacks, MI_Operation* operation);
    void (MI_CALL *Invoke)(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options, const MI_Char* namespaceName,
                           const MI_Char* className, const MI_Char* methodName, const MI_Instance* inboundInstance,
                           const MI_Instance* inboundProperties, MI_OperationCallbacks* callbacks, MI_Operation* operation);
    void (MI_CALL *EnumerateInstances)(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options, const MI_Char* namespaceName,
                                       const MI_Char* className, MI_Boolean keysOnly, MI_OperationCallbacks* callbacks, MI_Operation* operation);
    void (MI_CALL *QueryInstances)(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options, const MI_Char* namespaceName,
                                   const MI_Char* queryDialect, const MI_Char* queryExpression, MI_OperationCallbacks* callbacks, MI_Operation* operation);
    void (MI_CALL *AssociatorInstances)(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options, const MI_Char* namespaceName,
                                        const MI_Instance* instanceKeys, const MI_Char* assocClass, const MI_Char* resultClass,
                                        const MI_Char* role, const MI_Char* resultRole, MI_Boolean keysOnly,
                                        MI_OperationCallbacks* callbacks, MI_Operation* operation);
    void (MI_CALL *ReferenceInstances)(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options, const MI_Char* namespaceName,
                                       const MI_Instance* instanceKeys, const MI_Char* resultClass, const MI_Char* role, MI_Boolean keysOnly,
                                       MI_OperationCallbacks* callbacks, MI_Operation* operation);
    void (MI_CALL *Subscribe)(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options, const MI_Char* namespaceName,
                              const MI_Char* queryDialect, const MI_Char* queryExpression,
                              const MI_SubscriptionDeliveryOptions* deliverOptions, MI_OperationCallbacks* callbacks, MI_Operation* operation);
    void (MI_CALL *GetClass)(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options, const MI_Char* namespaceName,
                             const MI_Char* className, MI_OperationCallbacks* callbacks, MI_Operation* operation);
    void (MI_CALL *EnumerateClasses)(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options, const MI_Char* namespaceName,
                                     const MI_Char* className, MI_Boolean classNamesOnly, MI_OperationCallbacks* callbacks, MI_Operation* operation);
    void (MI_CALL *TestConnection)(MI_Session* session, MI_Uint32 flags, MI_OperationCallbacks* callbacks, MI_Operation* operation);
};

struct _MI_ApplicationFT
{
    MI_Result (MI_CALL *Close)(MI_Application* application);
    MI_Result (MI_CALL *NewSession)(MI_Application* application, const MI_Char* protocol, const MI_Char* destination,
                                    MI_DestinationOptions* options, MI_SessionCallbacks* callbacks, MI_Instance** extendedError, MI_Session* session);
    MI_Result (MI_CALL *NewHostedProvider)(MI_Application* application, const MI_Char* namespaceName, const MI_Char* providerName,
                                           void* mi_Main, MI_Instance** extendedError, MI_HostedProvider* provider);
    MI_Result (MI_CALL *NewInstance)(MI_Application* application, const MI_Char* className, const MI_ClassDecl* classRTTI, MI_Instance** instance);
    MI_Result (MI_CALL *NewDestinationOptions)(MI_Application* application, MI_DestinationOptions* options);
    MI_Result (MI_CALL *NewOperationOptions)(MI_Application* application, MI_Boolean customOptionsMustUnderstand, MI_OperationOptions* options);
    MI_Result (MI_CALL *NewSubscriptionDeliveryOptions)(MI_Application* application, MI_Uint32 deliveryType, MI_SubscriptionDeliveryOptions* deliveryOptions);
    MI_Result (MI_CALL *NewSerializer)(MI_Application* application, MI_Uint32 flags, MI_Char* format, MI_Serializer* serializer);
    MI_Result (MI_CALL *NewDeserializer)(MI_Application* application, MI_Uint32 flags, MI_Char* format, MI_Deserializer* deserializer);
    MI_Result (MI_CALL *NewInstanceFromClass)(MI_Application* application, const MI_Char* className, const MI_Class* classObject, MI_Instance** instance);
    MI_Result (MI_CALL *NewClass)(MI_Application* application, const MI_ClassDecl* classDecl, const MI_Char* namespaceName, const MI_Char* serverName, MI_Class** classObject);
};

struct _MI_DestinationOptionsFT
{
    void (MI_CALL *Delete)(MI_DestinationOptions* options);
    MI_Result (MI_CALL *SetString)(MI_DestinationOptions* options, const MI_Char* optionName, const MI_Char* value, MI_Uint32 flags);
    MI_Result (MI_CALL *SetNumber)(MI_DestinationOptions* options, const MI_Char* optionName, MI_Uint32 value, MI_Uint32 flags);
    MI_Result (MI_CALL *AddCredentials)(MI_DestinationOptions* options, const MI_Char* optionName, const MI_UserCredentials* credentials, MI_Uint32 flags);
    MI_Result (MI_CALL *GetString)(MI_DestinationOptions* options, const MI_Char* optionName, const MI_Char** value, MI_Uint32* index, MI_Uint32* flags);
    MI_Result (MI_CALL *GetNumber)(MI_DestinationOptions* options, const MI_Char* optionName, MI_Uint32* value, MI_Uint32* index, MI_Uint32* flags);
    MI_Result (MI_CALL *GetOptionCount)(MI_DestinationOptions* options, MI_Uint32* count);
    MI_Result (MI_CALL *GetOptionAt)(MI_DestinationOptions* options, MI_Uint32 index, const MI_Char** optionName, MI_Value* value, MI_Type* type, MI_Uint32* flags);
    MI_Result (MI_CALL *GetOption)(MI_DestinationOptions* options, const MI_Char* optionName, MI_Value* value, MI_Type* type, MI_Uint32* index, MI_Uint32* flags);
    MI_Result (MI_CALL *GetCredentialsCount)(MI_DestinationOptions* options, MI_Uint32* count);
    MI_Result (MI_CALL *GetCredentialsAt)(MI_DestinationOptions* options, MI_Uint32 index, const MI_Char** optionName, MI_UserCredentials* credentials, MI_Uint32* flags);
    MI_Result (MI_CALL *GetCredentialsPasswordAt)(MI_DestinationOptions* options, MI_Uint32 index, const MI_Char** optionName, MI_Char* password, MI_Uint32 bufferLength, MI_Uint32* passwordLength, MI_Uint32* flags);
    MI_Result (MI_CALL *Clone)(const MI_DestinationOptions* self, MI_DestinationOptions* newDestinationOptions);
    MI_Result (MI_CALL *SetInterval)(MI_DestinationOptions* options, const MI_Char* optionName, const MI_Interval* value, MI_Uint32 flags);
    MI_Result (MI_CALL *GetInterval)(MI_DestinationOptions* options, const MI_Char* optionName, MI_Interval* value, MI_Uint32* index, MI_Uint32* flags);
};

struct _MI_OperationOptionsFT
{
    void (MI_CALL *Delete)(MI_OperationOptions* options);
    MI_Result (MI_CALL *SetString)(MI_OperationOptions* options, const MI_Char* optionName, const MI_Char* value, MI_Uint32 flags);
    MI_Result (MI_CALL *SetNumber)(MI_OperationOptions* options, const MI_Char* optionName, MI_Uint32 value, MI_Uint32 flags);
    MI_Result (MI_CALL *SetCustomOption)(MI_OperationOptions* options, const MI_Char* optionName, MI_Type valueType, const MI_Value* value, MI_Boolean mustComply, MI_Uint32 flags);
    MI_Result (MI_CALL *GetString)(MI_OperationOptions* options, const MI_Char* optionName, const MI_Char** value, MI_Uint32* index, MI_Uint32* flags);
    MI_Result (MI_CALL *GetNumber)(MI_OperationOptions* options, const MI_Char* optionName, MI_Uint32* value, MI_Uint32* index, MI_Uint32* flags);
    MI_Result (MI_CALL *GetOptionCount)(MI_OperationOptions* options, MI_Uint32* count);
    MI_Result (MI_CALL *GetOptionAt)(MI_OperationOptions* options, MI_Uint32 index, const MI_Char** optionName, MI_Value* value, MI_Type* type, MI_Uint32* flags);
    MI_Result (MI_CALL *GetOption)(MI_OperationOptions* options, const MI_Char* optionName, MI_Value* value, MI_Type* type, MI_Uint32* index, MI_Uint32* flags);
    MI_Result (MI_CALL *GetEnabledChannels)(MI_OperationOptions* options, const MI_Char* optionName, MI_Uint32* channels, MI_Uint32 bufferLength, MI_Uint32* channelCount, MI_Uint32* flags);
    MI_Result (MI_CALL *Clone)(const MI_OperationOptions* self, MI_OperationOptions* newOperationOptions);
    MI_Result (MI_CALL *SetInterval)(MI_OperationOptions* options, const MI_Char* optionName, const MI_Interval* value, MI_Uint32 flags);
    MI_Result (MI_CALL *GetInterval)(MI_OperationOptions* options, const MI_Char* optionName, MI_Interval* value, MI_Uint32* index, MI_Uint32* flags);
};

struct _MI_UtilitiesFT
{
    MI_Uint32 (MI_CALL *MapErrorToMiErrorCategory)(const MI_Char* errorType, MI_Uint32 error);
    MI_Result (MI_CALL *CimErrorFromErrorCode)(MI_Uint32 error, const MI_Char* errorType, const MI_Char* errorMessage, MI_Instance** cimError);
};

typedef struct _MI_ClientFT_V1
{
    const MI_ApplicationFT* applicationFT;
    const MI_SessionFT* sessionFT;
    const MI_OperationFT* operationFT;
    const MI_HostedProviderFT* hostedProviderFT;
    const MI_SerializerFT* serializerFT;
    const MI_DeserializerFT* deserializerFT;
    const MI_SubscriptionDeliveryOptionsFT* subscribeDeliveryOptionsFT;
    const MI_DestinationOptionsFT* destinationOptionsFT;
    const MI_OperationOptionsFT* operationOptionsFT;
    const MI_UtilitiesFT* utilitiesFT;
}
MI_ClientFT_V1;

/* The one data export the stock MMI native bridge binds. */
MI_EXTERN_C const MI_ClientFT_V1* mi_clientFT_V1;
#define mi_clientFT mi_clientFT_V1

/* Also exported for callers that use the function form (real mi.dll exports it). */
MI_EXTERN_C MI_Result MI_MAIN_CALL MI_Application_InitializeV1(
    MI_Uint32 flags, const MI_Char* applicationID, MI_Instance** extendedError, MI_Application* application);

#define MI_Application_Initialize(flags, applicationID, extendedError, application) \
    MI_Application_InitializeV1((flags), (applicationID), (extendedError), (application))

/* ---- convenience inline wrappers (used by clients/apitest) ---- */
MI_INLINE MI_Result MI_Application_NewSession(MI_Application* application, const MI_Char* protocol,
    const MI_Char* destination, MI_DestinationOptions* options, MI_SessionCallbacks* callbacks,
    MI_Instance** extendedError, MI_Session* session)
{
    return application->ft->NewSession(application, protocol, destination, options, callbacks, extendedError, session);
}
MI_INLINE MI_Result MI_Application_Close(MI_Application* application)
{
    return application->ft->Close(application);
}
MI_INLINE void MI_Session_EnumerateInstances(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options,
    const MI_Char* namespaceName, const MI_Char* className, MI_Boolean keysOnly, MI_OperationCallbacks* callbacks, MI_Operation* operation)
{
    session->ft->EnumerateInstances(session, flags, options, namespaceName, className, keysOnly, callbacks, operation);
}
MI_INLINE void MI_Session_QueryInstances(MI_Session* session, MI_Uint32 flags, MI_OperationOptions* options,
    const MI_Char* namespaceName, const MI_Char* queryDialect, const MI_Char* queryExpression, MI_OperationCallbacks* callbacks, MI_Operation* operation)
{
    session->ft->QueryInstances(session, flags, options, namespaceName, queryDialect, queryExpression, callbacks, operation);
}
MI_INLINE MI_Result MI_Session_Close(MI_Session* session, void* completionContext, void (MI_CALL *completionCallback)(void*))
{
    return session->ft->Close(session, completionContext, completionCallback);
}
MI_INLINE MI_Result MI_Operation_GetInstance(MI_Operation* operation, const MI_Instance** instance, MI_Boolean* moreResults,
    MI_Result* result, const MI_Char** errorMessage, const MI_Instance** completionDetails)
{
    return operation->ft->GetInstance(operation, instance, moreResults, result, errorMessage, completionDetails);
}
MI_INLINE MI_Result MI_Operation_Close(MI_Operation* operation)
{
    return operation->ft->Close(operation);
}
MI_INLINE MI_Result MI_Operation_Cancel(MI_Operation* operation, MI_CancellationReason reason)
{
    return operation->ft->Cancel(operation, reason);
}
MI_INLINE MI_Result MI_Instance_GetClassName(const MI_Instance* self, const MI_Char** className)
{
    return self->ft->GetClassName(self, className);
}
MI_INLINE MI_Result MI_Instance_GetElementCount(const MI_Instance* self, MI_Uint32* count)
{
    return self->ft->GetElementCount(self, count);
}
MI_INLINE MI_Result MI_Instance_GetElement(const MI_Instance* self, const MI_Char* name, MI_Value* value, MI_Type* type, MI_Uint32* flags, MI_Uint32* index)
{
    return self->ft->GetElement(self, name, value, type, flags, index);
}
MI_INLINE MI_Result MI_Instance_GetElementAt(const MI_Instance* self, MI_Uint32 index, const MI_Char** name, MI_Value* value, MI_Type* type, MI_Uint32* flags)
{
    return self->ft->GetElementAt(self, index, name, value, type, flags);
}

#endif /* _MI_H_ */
