#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Установка / удаление MouseFilter.sys

.DESCRIPTION
    Скрипт устанавливает драйвер двумя способами:
    1. Через pnputil (рекомендуется, через .inf) — для подписанного драйвера
    2. Через sc.exe (ручная регистрация службы) — для тестовой среды с отключённым DSE

.PARAMETER Action
    Install   — установить драйвер
    Uninstall — удалить драйвер

.PARAMETER Method
    INF    — через pnputil + .inf (нужна подпись)
    Manual — ручная регистрация через sc.exe (нужен test mode)

.EXAMPLE
    .\Install.ps1 -Action Install -Method Manual
#>

param(
    [ValidateSet("Install","Uninstall")]
    [string]$Action = "Install",

    [ValidateSet("INF","Manual")]
    [string]$Method = "Manual"
)

$DriverName = "MouseFilter"
$SysFile    = Join-Path $PSScriptRoot "..\driver\bin\x64\Release\MouseFilter.sys"
$InfFile    = Join-Path $PSScriptRoot "MouseFilter.inf"
$SysTarget  = "$env:SystemRoot\System32\drivers\MouseFilter.sys"

function Write-Step($msg) { Write-Host "`n[>>] $msg" -ForegroundColor Cyan }
function Write-OK($msg)   { Write-Host "[ OK] $msg" -ForegroundColor Green }
function Write-Err($msg)  { Write-Host "[ERR] $msg" -ForegroundColor Red }

# -------------------------------------------------------
# Проверка тестового режима
# -------------------------------------------------------
function Check-TestMode {
    $bcdedit = & bcdedit /enum "{current}" 2>&1
    if ($bcdedit -match "testsigning\s+Yes") {
        Write-OK "Test Signing Mode активен"
        return $true
    } else {
        Write-Host "[WARN] Test Signing Mode НЕ активен." -ForegroundColor Yellow
        Write-Host "       Для неподписанного драйвера выполните:" -ForegroundColor Yellow
        Write-Host "       bcdedit /set testsigning on" -ForegroundColor White
        Write-Host "       ... и перезагрузите систему." -ForegroundColor Yellow
        return $false
    }
}

# -------------------------------------------------------
# УСТАНОВКА
# -------------------------------------------------------
if ($Action -eq "Install") {

    Write-Step "Проверяем тестовый режим..."
    Check-TestMode | Out-Null

    if ($Method -eq "Manual") {
        # --- Ручная установка через sc.exe ---

        Write-Step "Копируем $DriverName.sys в System32\drivers..."
        if (-not (Test-Path $SysFile)) {
            Write-Err "Файл не найден: $SysFile"
            Write-Host "       Сначала соберите драйвер в Visual Studio." -ForegroundColor Yellow
            exit 1
        }
        Copy-Item $SysFile $SysTarget -Force
        Write-OK "Скопировано: $SysTarget"

        Write-Step "Регистрируем службу..."
        & sc.exe create $DriverName `
            type= kernel `
            start= demand `
            error= normal `
            binPath= $SysTarget `
            DisplayName= "Mouse Filter Driver"

        Write-Step "Запускаем службу..."
        & sc.exe start $DriverName

        $status = & sc.exe query $DriverName
        if ($status -match "RUNNING") {
            Write-OK "Служба запущена!"
        } else {
            Write-Err "Служба не запустилась. Проверьте Event Viewer > System."
            Write-Host $status
        }

    } else {
        # --- Установка через pnputil + INF ---
        Write-Step "Устанавливаем через pnputil..."
        if (-not (Test-Path $InfFile)) {
            Write-Err "Не найден INF: $InfFile"
            exit 1
        }
        & pnputil /add-driver $InfFile /install
        Write-OK "pnputil завершён"
    }

    Write-Host ""
    Write-OK "Установка завершена!"
    Write-Host "Запустите MouseMonitor.exe от имени Администратора." -ForegroundColor White
}

# -------------------------------------------------------
# УДАЛЕНИЕ
# -------------------------------------------------------
elseif ($Action -eq "Uninstall") {

    Write-Step "Останавливаем службу $DriverName..."
    & sc.exe stop $DriverName

    Write-Step "Удаляем службу..."
    & sc.exe delete $DriverName

    Write-Step "Удаляем .sys файл..."
    if (Test-Path $SysTarget) {
        Remove-Item $SysTarget -Force
        Write-OK "Удалено: $SysTarget"
    }

    Write-OK "Удаление завершено."
}
