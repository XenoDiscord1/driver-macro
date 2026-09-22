# MouseFilter — Kernel-Mode Mouse Filter Driver

Настоящий kernel-mode upper filter driver для Windows.  
Встраивается в стек устройства мыши над `mouhid.sys`, перехватывает все события на уровне ядра.

---

## Архитектура

```
┌─────────────────────┐
│  MouseMonitor.exe   │  ← user-mode приложение
│  (IOCTL запросы)    │
└────────┬────────────┘
         │ \\.\MouseFilter
┌────────▼────────────┐
│   MouseFilter.sys   │  ← наш upper filter (kernel-mode)
│  ring-buffer событий│
│  захватывает callback│
└────────┬────────────┘
         │ MouseClassServiceCallback (подменённый)
┌────────▼────────────┐
│     mouhid.sys      │  ← HID mouse driver (встроенный)
└────────┬────────────┘
         │
┌────────▼────────────┐
│   USB / PS2 мышь    │
└─────────────────────┘
```

---

## Требования

| Компонент | Версия |
|-----------|--------|
| Windows | 10 / 11 x64 |
| Visual Studio | 2019 или 2022 |
| WDK | 10.0.22621+ (скачать с microsoft.com/en-us/download/details.aspx?id=11800) |
| Права | Администратор |

---

## Сборка

### 1. Установи WDK

Скачай и установи **Windows Driver Kit (WDK)** для Windows 11:  
https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk

WDK автоматически интегрируется в Visual Studio.

### 2. Открой проект в Visual Studio

```
File → Open → Project/Solution → driver/MouseFilter.vcxproj
```

Выбери конфигурацию **Release | x64** и нажми **Build (F7)**.

Артефакт: `bin\x64\Release\MouseFilter.sys`

### 3. Собери приложение-монитор

```
cl.exe /nologo /W3 /O2 /EHsc /I driver app\MouseMonitor.cpp /Fe:MouseMonitor.exe /link user32.lib
```

Или добавь `app/MouseMonitor.cpp` в отдельный проект типа Console Application.

---

## Установка и запуск

### Вариант A: тестовая машина / виртуалка (без подписи)

> ⚠️ Никогда не делай это на основной рабочей машине — только в ВМ или тестовой.

#### Шаг 1 — включи Test Signing Mode
```
bcdedit /set testsigning on
```
Перезагрузи Windows. В правом нижнем углу рабочего стола появится водяной знак «Test Mode».

#### Шаг 2 — установи драйвер
```powershell
# От Администратора:
.\install\Install.ps1 -Action Install -Method Manual
```

Или вручную:
```cmd
copy bin\x64\Release\MouseFilter.sys %SystemRoot%\System32\drivers\
sc create MouseFilter type= kernel start= demand binPath= %SystemRoot%\System32\drivers\MouseFilter.sys
sc start MouseFilter
```

#### Шаг 3 — запусти монитор
```cmd
MouseMonitor.exe
```

### Вариант B: продакшн-машина (с подписью)

Для этого нужен **EV (Extended Validation) сертификат** от DigiCert/Sectigo (~$500/год).  
После подписи установка через:
```
pnputil /add-driver install\MouseFilter.inf /install
```

---

## Управление монитором

| Клавиша | Действие |
|---------|----------|
| `C` | Очистить кольцевой буфер событий |
| `X` | Включить/выключить **захват** (мышь не двигается, клики не проходят) |
| `Q` | Выход |

---

## Пример вывода

```
[OK] Устройство открыто: \\.\MouseFilter

[14:23:01.042]  MOVE dX=  +3 dY=  -1
[14:23:01.052]  MOVE dX= +12 dY=  -8
[14:23:01.061]  MOVE dX=  +5 dY=   0  [LMB↓]
[14:23:01.071]                          [LMB↑]
[14:23:02.100]  MOVE dX=   0 dY= +20  [WHEEL -120]
```

---

## IOCTL API (для интеграции в своё приложение)

```cpp
#include "MouseFilter.h"

HANDLE h = CreateFile("\\\\.\\MouseFilter",
    GENERIC_READ | GENERIC_WRITE, 0, NULL,
    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

// Получить события
MOUSE_EVENTS_BUFFER buf{};
DWORD ret;
DeviceIoControl(h, IOCTL_MOUSE_GET_EVENTS,
    NULL, 0, &buf, sizeof(buf), &ret, NULL);

for (ULONG i = 0; i < buf.Count; i++) {
    printf("dX=%ld dY=%ld buttons=%04X\n",
        buf.Events[i].dx,
        buf.Events[i].dy,
        buf.Events[i].ButtonFlags);
}

// Включить захват (блокировать ввод в систему)
MOUSE_CAPTURE_REQUEST req = { TRUE };
DeviceIoControl(h, IOCTL_MOUSE_SET_CAPTURE,
    &req, sizeof(req), NULL, 0, &ret, NULL);

// Очистить буфер
DeviceIoControl(h, IOCTL_MOUSE_CLEAR_EVENTS,
    NULL, 0, NULL, 0, &ret, NULL);
```

---

## Удаление

```powershell
.\install\Install.ps1 -Action Uninstall
```

Или вручную:
```cmd
sc stop MouseFilter
sc delete MouseFilter
del %SystemRoot%\System32\drivers\MouseFilter.sys
bcdedit /set testsigning off
```

---

## Известные ограничения

- Работает только на **x64** (современные Windows не поддерживают 32-bit драйверы с DSE)
- WH_MOUSE_LL (предыдущая версия — user-mode) проще в установке, но может быть обнаружен антивирусами и cheat-detection системами
- Kernel-mode драйвер **не** обходит Secure Boot / HVCI — для этого нужен иной подход

---

## Структура проекта

```
mousedriver/
├── driver/
│   ├── MouseFilter.h        # Общие структуры и IOCTL коды
│   ├── MouseFilter.c        # Реализация драйвера
│   └── MouseFilter.vcxproj  # Visual Studio WDK проект
├── app/
│   └── MouseMonitor.cpp     # User-mode монитор
├── install/
│   ├── MouseFilter.inf      # INF для pnputil
│   └── Install.ps1          # PowerShell скрипт установки
└── README.md
```
