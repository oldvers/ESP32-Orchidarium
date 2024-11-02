#ifndef __TYPES_H__
#define __TYPES_H__

#ifdef NULL
#   undef NULL
#endif
#define NULL                 ((void *)0)

#include <stdint.h>

typedef uint8_t              U8;
typedef int8_t               S8;
typedef uint16_t             U16;
typedef int16_t              S16;
typedef uint32_t             U32;
typedef int32_t              S32;
typedef uint64_t             U64;
typedef int64_t              S64;

typedef enum
{
    FW_FALSE     = 0,
    FW_TRUE      = 1,
} FW_BOOLEAN;

typedef enum
{
    FW_SUCCESS    = 0x0000,
    FW_COMPLETE   = 0x0001,
    FW_FULL       = 0x0002,
    FW_EMPTY      = 0x0003,
    FW_INPROGRESS = 0x0004,
    FW_ERROR      = 0xBAD0,
    FW_FAIL       = 0xFAC0,
    FW_TIMEOUT    = 0xFFFF,
} FW_RESULT;

#ifndef TEST
#  define STATIC             static
#else
#  define STATIC
#endif

#define CORE0  (0)
/* Only define xCoreID CORE1 as 1 if this is a multiple core processor target,
 * else define it as tskNO_AFFINITY */
#define CORE1  ((CONFIG_FREERTOS_NUMBER_OF_CORES > 1) ? 1 : tskNO_AFFINITY)

#endif /* __TYPES_H__ */
