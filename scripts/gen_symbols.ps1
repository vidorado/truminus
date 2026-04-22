# gen_symbols.ps1 — genera src/symbols_14.c con los iconos FontAwesome
# necesarios para la pantalla CYD (fa-tint y fa-thermometer-half).
#
# Requisitos:
#   Node.js instalado  →  https://nodejs.org
#   lv_font_conv       →  npm install -g lv_font_conv
#
# Ejecutar desde la raíz del proyecto:
#   powershell -ExecutionPolicy Bypass -File scripts/gen_symbols.ps1

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = Split-Path -Parent $ScriptDir
$FontDir    = Join-Path $ScriptDir "fonts"
$FontFile   = Join-Path $FontDir   "FontAwesome5-Solid.woff"
$OutFile    = Join-Path $ProjectDir "src\symbols_14.c"

# ── 1. Descargar la fuente si no existe ──────────────────────────────────────
if (-not (Test-Path $FontDir)) { New-Item -ItemType Directory $FontDir | Out-Null }

if (-not (Test-Path $FontFile)) {
    Write-Host "[gen_symbols] Descargando FontAwesome5-Solid+Brands+Regular.woff..."
    $url = "https://github.com/lvgl/lvgl/raw/master/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
    Invoke-WebRequest -Uri $url -OutFile $FontFile
    Write-Host "[gen_symbols] Fuente descargada."
} else {
    Write-Host "[gen_symbols] Fuente ya existe: $FontFile"
}

# ── 2. Generar el fichero C ──────────────────────────────────────────────────
# Glifos:
#   0xF043  fa-tint             (gota de agua)
#   0xF2C7  fa-thermometer-half (termómetro)
Write-Host "[gen_symbols] Generando $OutFile ..."
lv_font_conv `
    --font $FontFile `
    -r 0xF043 `
    -r 0xF2C7 `
    --size 14 `
    --format lvgl `
    --bpp 4 `
    --no-compress `
    -o $OutFile

Write-Host "[gen_symbols] Listo: $OutFile"
Write-Host "  Símbolos generados:"
Write-Host "    fa-tint             U+F043  \xEF\x81\x83"
Write-Host "    fa-thermometer-half U+F2C7  \xEF\x8B\x87"
