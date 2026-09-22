/*
 * MouseMonitor.cpp
 *
 * User-mode приложение для общения с MouseFilter.sys через IOCTL.
 * Показывает движения мыши, клики и состояние колеса в реальном времени.
 *
 * Клавиши управления:
 *   C — очистить буфер событий
 *   X — переключить режим захвата (клики/движение не проходят в систему)
 *   Q — выход
 */

#include <windows.h>
#include <stdio.h>
#include <conio.h>
#include "../driver/MouseFilter.h"

// Имя устройства для CreateFile
#define DEVICE_PATH  "\\\\.\\MouseFilter"

// -------------------------------------------------------
// Вспомогательная функция: красивый вывод флагов кнопок
// -------------------------------------------------------
static void PrintButtonFlags(USHORT flags, USHORT data)
{
    if (flags & MOUSE_LEFT_BUTTON_DOWN)   printf(" [LMB↓]");
    if (flags & MOUSE_LEFT_BUTTON_UP)     printf(" [LMB↑]");
    if (flags & MOUSE_RIGHT_BUTTON_DOWN)  printf(" [RMB↓]");
    if (flags & MOUSE_RIGHT_BUTTON_UP)    printf(" [RMB↑]");
    if (flags & MOUSE_MIDDLE_BUTTON_DOWN) printf(" [MMB↓]");
    if (flags & MOUSE_MIDDLE_BUTTON_UP)   printf(" [MMB↑]");
    if (flags & MOUSE_BUTTON_4_DOWN)      printf(" [X1↓]");
    if (flags & MOUSE_BUTTON_4_UP)        printf(" [X1↑]");
    if (flags & MOUSE_BUTTON_5_DOWN)      printf(" [X2↓]");
    if (flags & MOUSE_BUTTON_5_UP)        printf(" [X2↑]");
    if (flags & MOUSE_WHEEL) {
        SHORT delta = (SHORT)data;
        printf(" [WHEEL %+d]", (int)delta);
    }
    if (flags & MOUSE_HWHEEL) {
        SHORT delta = (SHORT)data;
        printf(" [HWHEEL %+d]", (int)delta);
    }
}

// -------------------------------------------------------
// Конвертация LARGE_INTEGER (100нс-тики) в читаемое время
// -------------------------------------------------------
static void PrintTimestamp(LARGE_INTEGER ts)
{
    FILETIME ft;
    ft.dwLowDateTime  = ts.LowPart;
    ft.dwHighDateTime = (DWORD)ts.HighPart;

    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    printf("%02d:%02d:%02d.%03d",
           st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

// -------------------------------------------------------
// main
// -------------------------------------------------------
int main()
{
    SetConsoleOutputCP(CP_UTF8);

    printf("================================================\n");
    printf("  MouseFilter Monitor — kernel-mode driver test\n");
    printf("================================================\n");
    printf("  C = очистить буфер   X = захват вкл/выкл   Q = выход\n\n");

    // --- Открываем устройство ---
    HANDLE hDev = CreateFileA(
        DEVICE_PATH,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (hDev == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        fprintf(stderr,
            "[ERROR] Не удалось открыть %s (ошибка %lu)\n"
            "        Убедитесь что:\n"
            "        1. Драйвер установлен и запущен\n"
            "        2. Приложение запущено от Администратора\n"
            "        3. DSE отключён или драйвер подписан\n",
            DEVICE_PATH, err);
        return 1;
    }

    printf("[OK] Устройство открыто: %s\n\n", DEVICE_PATH);

    // Буфер для событий
    MOUSE_EVENTS_BUFFER* buf =
        (MOUSE_EVENTS_BUFFER*)HeapAlloc(GetProcessHeap(), 0, sizeof(MOUSE_EVENTS_BUFFER));
    if (!buf) {
        fprintf(stderr, "[ERROR] Нет памяти\n");
        CloseHandle(hDev);
        return 1;
    }

    BOOL captureMode = FALSE;
    ULONG totalEvents = 0;

    // --- Основной цикл опроса ---
    while (TRUE)
    {
        // Проверяем нажатие клавиши (неблокирующий _kbhit)
        if (_kbhit()) {
            int ch = _getch();
            if (ch == 'q' || ch == 'Q') break;

            if (ch == 'c' || ch == 'C') {
                DWORD ret = 0;
                if (DeviceIoControl(hDev, IOCTL_MOUSE_CLEAR_EVENTS,
                                    NULL, 0, NULL, 0, &ret, NULL))
                    printf("[INFO] Буфер очищен\n");
                totalEvents = 0;
                continue;
            }

            if (ch == 'x' || ch == 'X') {
                captureMode = !captureMode;
                MOUSE_CAPTURE_REQUEST req = { captureMode };
                DWORD ret = 0;
                DeviceIoControl(hDev, IOCTL_MOUSE_SET_CAPTURE,
                                &req, sizeof(req), NULL, 0, &ret, NULL);
                printf("[INFO] Режим захвата: %s\n",
                       captureMode ? "ВКЛЮЧЁН (мышь заблокирована)" : "ВЫКЛЮЧЕН");
                continue;
            }
        }

        // --- Запрашиваем события из драйвера ---
        ZeroMemory(buf, sizeof(MOUSE_EVENTS_BUFFER));
        DWORD bytesRet = 0;

        BOOL ok = DeviceIoControl(
            hDev,
            IOCTL_MOUSE_GET_EVENTS,
            NULL, 0,
            buf, sizeof(MOUSE_EVENTS_BUFFER),
            &bytesRet,
            NULL);

        if (!ok) {
            fprintf(stderr, "[ERROR] DeviceIoControl: %lu\n", GetLastError());
            Sleep(500);
            continue;
        }

        // --- Выводим новые события ---
        if (buf->Count > 0) {
            // Показываем только те, что ещё не видели
            ULONG start = (buf->Count > totalEvents)
                ? buf->Count - totalEvents : 0;

            // Если буфер переполнился — покажем всё что есть
            if (buf->Count == MAX_MOUSE_EVENTS)
                start = 0;

            for (ULONG i = start; i < buf->Count; i++) {
                PMOUSE_EVENT_RECORD r = &buf->Events[i];

                printf("[");
                PrintTimestamp(r->Timestamp);
                printf("]");

                if (r->dx != 0 || r->dy != 0)
                    printf("  MOVE dX=%+4ld dY=%+4ld", r->dx, r->dy);

                if (r->ButtonFlags)
                    PrintButtonFlags(r->ButtonFlags, r->ButtonData);

                printf("\n");
            }
            totalEvents = buf->Count;
        }

        Sleep(10);  // 100 Гц опроса — достаточно для отладки
    }

    // --- Выключаем захват если был включён ---
    if (captureMode) {
        MOUSE_CAPTURE_REQUEST req = { FALSE };
        DWORD ret = 0;
        DeviceIoControl(hDev, IOCTL_MOUSE_SET_CAPTURE,
                        &req, sizeof(req), NULL, 0, &ret, NULL);
    }

    HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(hDev);
    printf("\n[INFO] Выход.\n");
    return 0;
}
