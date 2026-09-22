/*
 * MouseFilter.c
 *
 * Kernel-mode Upper Filter Driver для стека HID/мыши.
 * Встраивается над mouhid.sys, перехватывает IRP_MJ_INTERNAL_DEVICE_CONTROL
 * с кодом IOCTL_INTERNAL_MOUSE_CONNECT и подменяет callback на свой.
 *
 * Архитектура:
 *   [приложение]
 *       |  IOCTL_MOUSE_GET_EVENTS
 *       v
 *   [наш filter .sys]  ─── ring-buffer событий
 *       |  подменённый MouseClassServiceCallback
 *       v
 *   [mouhid.sys]
 *       |
 *   [USB/PS2 мышь]
 */

#include "MouseFilter.h"
#include <mouclass.h>   // CONNECT_DATA, MouseClassServiceCallback typedef

// -------------------------------------------------------
// Имена устройства и символьной ссылки (user-mode видит через \\.\MouseFilter)
// -------------------------------------------------------
#define DEVICE_NAME     L"\\Device\\MouseFilter"
#define SYMLINK_NAME    L"\\DosDevices\\MouseFilter"

// -------------------------------------------------------
// Расширение объекта устройства
// -------------------------------------------------------
typedef struct _DEVICE_EXTENSION {

    // Нижнее устройство в стеке
    PDEVICE_OBJECT  LowerDevice;

    // Оригинальный callback mouhid → mouclass (сохраняем для вызова)
    PVOID           UpperConnectData_ClassService;
    PVOID           UpperConnectData_ClassDeviceObject;

    // Ring-buffer событий (защищён спинлоком)
    KSPIN_LOCK      EventLock;
    MOUSE_EVENT_RECORD EventBuf[MAX_MOUSE_EVENTS];
    ULONG           WriteIdx;
    ULONG           EventCount;

    // Режим захвата: если TRUE — события НЕ передаются в mouclass
    BOOLEAN         CaptureMode;

    // Символьная ссылка создана?
    BOOLEAN         SymlinkCreated;

} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

// -------------------------------------------------------
// Прототипы
// -------------------------------------------------------
DRIVER_INITIALIZE               DriverEntry;
DRIVER_UNLOAD                   MouseFilterUnload;
DRIVER_ADD_DEVICE               MouseFilterAddDevice;

_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLOSE)
DRIVER_DISPATCH                 MouseFilterCreateClose;

_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH                 MouseFilterDeviceControl;

_Dispatch_type_(IRP_MJ_INTERNAL_DEVICE_CONTROL)
DRIVER_DISPATCH                 MouseFilterInternalDeviceControl;

_Dispatch_type_(IRP_MJ_PNP)
_Dispatch_type_(IRP_MJ_POWER)
DRIVER_DISPATCH                 MouseFilterPassThrough;

IO_COMPLETION_ROUTINE           MouseFilterComplete;

// Наш подменённый MouseClassServiceCallback
VOID MouseFilterServiceCallback(
    PDEVICE_OBJECT  DeviceObject,
    PMOUSE_INPUT_DATA InputDataStart,
    PMOUSE_INPUT_DATA InputDataEnd,
    PULONG          InputDataConsumed
);

// -------------------------------------------------------
// DriverEntry
// -------------------------------------------------------
NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    KdPrint(("[MouseFilter] DriverEntry\n"));

    // Регистрируем обработчики IRP
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = MouseFilterPassThrough;

    DriverObject->MajorFunction[IRP_MJ_CREATE]                   = MouseFilterCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]                    = MouseFilterCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]           = MouseFilterDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL]  = MouseFilterInternalDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP]                      = MouseFilterPassThrough;
    DriverObject->MajorFunction[IRP_MJ_POWER]                    = MouseFilterPassThrough;

    DriverObject->DriverUnload   = MouseFilterUnload;
    DriverObject->DriverExtension->AddDevice = MouseFilterAddDevice;

    return STATUS_SUCCESS;
}

// -------------------------------------------------------
// AddDevice — вызывается PnP менеджером при обнаружении мыши
// -------------------------------------------------------
NTSTATUS MouseFilterAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    NTSTATUS        status;
    PDEVICE_OBJECT  filterDO  = NULL;
    PDEVICE_EXTENSION ext     = NULL;
    UNICODE_STRING  devName, symName;

    KdPrint(("[MouseFilter] AddDevice\n"));

    RtlInitUnicodeString(&devName, DEVICE_NAME);
    RtlInitUnicodeString(&symName, SYMLINK_NAME);

    // Создаём filter device object
    status = IoCreateDevice(
        DriverObject,
        sizeof(DEVICE_EXTENSION),
        &devName,                   // именованное устройство
        FILE_DEVICE_MOUSE,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &filterDO);

    if (!NT_SUCCESS(status)) {
        KdPrint(("[MouseFilter] IoCreateDevice failed: 0x%X\n", status));
        return status;
    }

    ext = (PDEVICE_EXTENSION)filterDO->DeviceExtension;
    RtlZeroMemory(ext, sizeof(DEVICE_EXTENSION));

    KeInitializeSpinLock(&ext->EventLock);
    ext->WriteIdx    = 0;
    ext->EventCount  = 0;
    ext->CaptureMode = FALSE;

    // Создаём символьную ссылку для user-mode доступа
    status = IoCreateSymbolicLink(&symName, &devName);
    if (NT_SUCCESS(status))
        ext->SymlinkCreated = TRUE;

    // Присоединяемся к стеку над физическим устройством
    ext->LowerDevice = IoAttachDeviceToDeviceStack(filterDO, PhysicalDeviceObject);
    if (!ext->LowerDevice) {
        KdPrint(("[MouseFilter] IoAttachDeviceToDeviceStack failed\n"));
        if (ext->SymlinkCreated)
            IoDeleteSymbolicLink(&symName);
        IoDeleteDevice(filterDO);
        return STATUS_UNSUCCESSFUL;
    }

    // Копируем флаги нижнего устройства (буферизация, выравнивание)
    filterDO->Flags |= ext->LowerDevice->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    filterDO->Flags &= ~DO_DEVICE_INITIALIZING;

    KdPrint(("[MouseFilter] AddDevice OK, LowerDevice=%p\n", ext->LowerDevice));
    return STATUS_SUCCESS;
}

// -------------------------------------------------------
// Unload
// -------------------------------------------------------
VOID MouseFilterUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symName;
    RtlInitUnicodeString(&symName, SYMLINK_NAME);

    PDEVICE_OBJECT dev = DriverObject->DeviceObject;
    while (dev) {
        PDEVICE_EXTENSION ext = (PDEVICE_EXTENSION)dev->DeviceExtension;
        if (ext->SymlinkCreated)
            IoDeleteSymbolicLink(&symName);
        if (ext->LowerDevice)
            IoDetachDevice(ext->LowerDevice);
        PDEVICE_OBJECT next = dev->NextDevice;
        IoDeleteDevice(dev);
        dev = next;
    }

    KdPrint(("[MouseFilter] Unloaded\n"));
}

// -------------------------------------------------------
// Наш callback, подменяющий MouseClassServiceCallback
// Вызывается из контекста DPC (DISPATCH_LEVEL)
// -------------------------------------------------------
VOID MouseFilterServiceCallback(
    PDEVICE_OBJECT  DeviceObject,
    PMOUSE_INPUT_DATA InputDataStart,
    PMOUSE_INPUT_DATA InputDataEnd,
    PULONG           InputDataConsumed)
{
    PDEVICE_EXTENSION ext = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    KIRQL oldIrql;

    KeAcquireSpinLock(&ext->EventLock, &oldIrql);

    for (PMOUSE_INPUT_DATA cur = InputDataStart; cur < InputDataEnd; cur++) {

        // Пишем в кольцевой буфер
        ULONG idx = ext->WriteIdx % MAX_MOUSE_EVENTS;
        PMOUSE_EVENT_RECORD rec = &ext->EventBuf[idx];

        rec->dx          = cur->LastX;
        rec->dy          = cur->LastY;
        rec->ButtonFlags = cur->ButtonFlags;
        rec->ButtonData  = cur->ButtonData;
        rec->RawButtons  = cur->RawButtons;
        KeQuerySystemTime(&rec->Timestamp);

        ext->WriteIdx++;
        if (ext->EventCount < MAX_MOUSE_EVENTS)
            ext->EventCount++;
    }

    KeReleaseSpinLock(&ext->EventLock, oldIrql);

    // Если режим захвата выключен — передаём события в систему как обычно
    if (!ext->CaptureMode) {
        PSERVICE_CALLBACK_ROUTINE realCallback =
            (PSERVICE_CALLBACK_ROUTINE)ext->UpperConnectData_ClassService;
        if (realCallback) {
            realCallback(
                (PDEVICE_OBJECT)ext->UpperConnectData_ClassDeviceObject,
                InputDataStart,
                InputDataEnd,
                InputDataConsumed);
        }
    } else {
        // Поглощаем события — курсор не двигается, клики не проходят
        *InputDataConsumed = (ULONG)(InputDataEnd - InputDataStart);
    }
}

// -------------------------------------------------------
// Internal Device Control — перехватываем CONNECT от mouhid
// -------------------------------------------------------
NTSTATUS MouseFilterInternalDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp)
{
    PDEVICE_EXTENSION   ext = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION  stack = IoGetCurrentIrpStackLocation(Irp);

    if (stack->Parameters.DeviceIoControl.IoControlCode
            == IOCTL_INTERNAL_MOUSE_CONNECT)
    {
        // Сохраняем оригинальные данные соединения
        PCONNECT_DATA connectData =
            (PCONNECT_DATA)stack->Parameters.DeviceIoControl.Type3InputBuffer;

        ext->UpperConnectData_ClassDeviceObject = connectData->ClassDeviceObject;
        ext->UpperConnectData_ClassService      = connectData->ClassService;

        // Подменяем callback на наш
        connectData->ClassDeviceObject = DeviceObject;
        connectData->ClassService      = MouseFilterServiceCallback;

        KdPrint(("[MouseFilter] Hooked MouseClassServiceCallback\n"));
    }

    // Передаём IRP вниз по стеку
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDevice, Irp);
}

// -------------------------------------------------------
// Completion routine для pass-through IRP
// -------------------------------------------------------
NTSTATUS MouseFilterComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp,
    _In_opt_ PVOID      Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Context);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);
    return STATUS_CONTINUE_COMPLETION;
}

// -------------------------------------------------------
// Pass-through для PnP / Power / прочих IRP
// -------------------------------------------------------
NTSTATUS MouseFilterPassThrough(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp)
{
    PDEVICE_EXTENSION ext = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (!ext->LowerDevice) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDevice, Irp);
}

// -------------------------------------------------------
// Create / Close — открытие дескриптора из user-mode
// -------------------------------------------------------
NTSTATUS MouseFilterCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

// -------------------------------------------------------
// DeviceControl — обработка IOCTL от user-mode приложения
// -------------------------------------------------------
NTSTATUS MouseFilterDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp)
{
    PDEVICE_EXTENSION   ext   = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION  stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS            status = STATUS_SUCCESS;
    ULONG_PTR           info   = 0;
    KIRQL               oldIrql;

    switch (stack->Parameters.DeviceIoControl.IoControlCode)
    {

    // ---------- Получить накопленные события ----------
    case IOCTL_MOUSE_GET_EVENTS:
    {
        ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
        if (outLen < sizeof(MOUSE_EVENTS_BUFFER)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        PMOUSE_EVENTS_BUFFER out = (PMOUSE_EVENTS_BUFFER)Irp->AssociatedIrp.SystemBuffer;

        KeAcquireSpinLock(&ext->EventLock, &oldIrql);

        ULONG count = ext->EventCount;
        out->Count  = count;

        if (count > 0) {
            // Вычисляем начало в кольцевом буфере
            ULONG startIdx = (count >= MAX_MOUSE_EVENTS)
                ? ext->WriteIdx                       // буфер полный
                : ext->WriteIdx - count;              // буфер неполный

            for (ULONG i = 0; i < count; i++) {
                out->Events[i] = ext->EventBuf[(startIdx + i) % MAX_MOUSE_EVENTS];
            }
        }

        KeReleaseSpinLock(&ext->EventLock, oldIrql);

        info = sizeof(MOUSE_EVENTS_BUFFER);
        break;
    }

    // ---------- Очистить буфер событий ----------
    case IOCTL_MOUSE_CLEAR_EVENTS:
    {
        KeAcquireSpinLock(&ext->EventLock, &oldIrql);
        ext->WriteIdx   = 0;
        ext->EventCount = 0;
        KeReleaseSpinLock(&ext->EventLock, oldIrql);
        KdPrint(("[MouseFilter] Buffer cleared\n"));
        break;
    }

    // ---------- Включить/выключить захват ----------
    case IOCTL_MOUSE_SET_CAPTURE:
    {
        ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
        if (inLen < sizeof(MOUSE_CAPTURE_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        PMOUSE_CAPTURE_REQUEST req =
            (PMOUSE_CAPTURE_REQUEST)Irp->AssociatedIrp.SystemBuffer;
        ext->CaptureMode = req->Enable;
        KdPrint(("[MouseFilter] CaptureMode = %d\n", ext->CaptureMode));
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
