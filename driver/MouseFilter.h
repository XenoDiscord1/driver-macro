#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <kbdmou.h>   // MOUSE_INPUT_DATA

// -------------------------------------------------------
// IOCTL-коды для общения user-mode app <-> драйвер
// -------------------------------------------------------
#define MOUSE_FILTER_DEVICE_TYPE  0x8000

#define IOCTL_MOUSE_GET_EVENTS \
    CTL_CODE(MOUSE_FILTER_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_READ_DATA)

#define IOCTL_MOUSE_CLEAR_EVENTS \
    CTL_CODE(MOUSE_FILTER_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_MOUSE_SET_CAPTURE \
    CTL_CODE(MOUSE_FILTER_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_WRITE_DATA)

// -------------------------------------------------------
// Разделяемые структуры (driver <-> user-mode)
// -------------------------------------------------------
#define MAX_MOUSE_EVENTS 1024

#pragma pack(push, 1)

typedef struct _MOUSE_EVENT_RECORD {
    LONG        dx;             // Относительное смещение X
    LONG        dy;             // Относительное смещение Y
    USHORT      ButtonFlags;    // Флаги кнопок (из MOUSE_INPUT_DATA)
    USHORT      ButtonData;     // Данные кнопок (колесо и т.д.)
    ULONG       RawButtons;     // Состояние всех кнопок
    LARGE_INTEGER Timestamp;   // KeQuerySystemTime
} MOUSE_EVENT_RECORD, *PMOUSE_EVENT_RECORD;

typedef struct _MOUSE_EVENTS_BUFFER {
    ULONG               Count;
    MOUSE_EVENT_RECORD  Events[MAX_MOUSE_EVENTS];
} MOUSE_EVENTS_BUFFER, *PMOUSE_EVENTS_BUFFER;

typedef struct _MOUSE_CAPTURE_REQUEST {
    BOOLEAN Enable;   // TRUE = перехватывать (не пропускать в систему)
} MOUSE_CAPTURE_REQUEST, *PMOUSE_CAPTURE_REQUEST;

#pragma pack(pop)
