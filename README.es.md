# rockchip-vaapi

*[English](README.md) · Español*

Driver VA-API para Rockchip RK3588 / RK3576 que hace de puente entre **libva**
y **librockchip-mpp** (MPP), para decodificar vídeo por hardware en
aplicaciones como Firefox.

**Autor:** Eduardo García-Mádico Portabella — EGP Sistemas
**Contacto:** woodyst@gmail.com
**Licencia:** LGPL-2.1-or-later

---

## Qué hace

El SoC Rockchip RK3588 incluye una VPU dedicada capaz de decodificar H.264,
HEVC, VP9 y AV1 hasta 8K. Sin embargo, el fabricante no ofrece ningún driver
VA-API para ella. Este proyecto cubre ese hueco: implementa la
`VADriverVTable` completa (VA-API 1.20) y delega la decodificación en la
biblioteca MPP de Rockchip, que a su vez usa la VPU.

Características principales:

- Decodificación por hardware de H.264 / HEVC / VP9 (AV1 no se anuncia, ver abajo)
- Exportación de superficies DRM PRIME 2 sin copia (NV12, DMABUF)
- Compatible con Firefox 128+ (ruta PDM de VA-API, proceso RDD)
- Implementa la vtable completa de VA-API 1.20 (`__vaDriverInit_1_20`)

## Hardware soportado

| SoC | Placa (probada) |
|-----|---------------|
| RK3588 | Orange Pi 5 Plus |
| RK3588S | Orange Pi 5 / Rock 5B (sin probar, debería funcionar) |
| RK3576 | Probablemente compatible (sin probar) |

## Códecs soportados

| Códec | Perfil | Resolución máxima |
|-------|---------|---------------|
| H.264 | Constrained Baseline, Main, High, High10 | 4K |
| HEVC | Main, Main10 | 8K |
| VP9 | Perfil 0, 2 | 8K |

AV1 **no se anuncia**: MPP necesita el flujo OBU completo, que VA-API no
proporciona, así que los navegadores recurren a VP9, que sí se decodifica por
hardware.

## Novedades de la 1.0.12

Corrige los vídeos H.264 de YouTube que se recargaban una y otra vez en
Firefox (`NS_ERROR_DOM_MEDIA_FATAL_ERR` en bucle):

- **Referencias por defecto en el PPS de H.264.** El PPS reconstruido decía
  siempre «1 referencia por defecto». Los flujos con otro valor (YouTube 720p60
  usa 3, con predicción ponderada) los interpretaba mal la VPU: a los pocos
  cientos de frames la imagen se desfasaba y la decodificación acababa en
  timeout. El driver aprende ahora los valores reales de las cabeceras de slice.
- **Interbloqueo con B-frames.** MPP retenía frames que el reproductor estaba
  esperando. Ahora los frames H.264 salen en cuanto se decodifican.
- **Contrapresión y log** (del [PR #2](https://github.com/woodyst/rockchip-vaapi/pull/2)
  de TruongSinh Tran-Nguyen): la cola de entrada de MPP llena ya no es un error
  fatal, y `RK_VAAPI_LOG` se vuelca tras cada línea.

Resultado con un clip H.264 720p60 de YouTube: 2107 de 2107 frames idénticos
bit a bit a la decodificación por software (1.0.11: 506 de 2107). Detalles en
[debian/changelog](debian/changelog) y
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#pps-num_ref_idx-defaults-since-1012)
(en inglés).

## Dependencias

En ejecución:
- `libva2` (>= 2.0)
- `librockchip-mpp1`

Para compilar:
- `libva-dev`
- `librockchip-mpp-dev`
- `pkg-config`, `gcc`

## Inicio rápido

```bash
# Compilar e instalar
make
sudo make install

# Lanzar Firefox con decodificación por hardware
LIBVA_DRIVER_NAME=rockchip \
LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
MOZ_DISABLE_RDD_SANDBOX=1 \
firefox
```

En Firefox, activa además en `about:config`:

| Preferencia | Valor |
|-----------|-------|
| `media.hardware-video-decoding.enabled` | `true` |
| `media.ffmpeg.vaapi.enabled` | `true` |
| `media.rdd-ffmpeg.enabled` | `true` |

## Comprobar la decodificación por hardware

El log está desactivado por defecto. Pon en `RK_VAAPI_LOG` la ruta de un
fichero, reproduce un vídeo y mira el log:

```bash
LIBVA_DRIVER_NAME=rockchip MOZ_DISABLE_RDD_SANDBOX=1 RK_VAAPI_LOG=/tmp/rk.log firefox
# Debe mostrar BeginPicture, EndPicture, assign_mpp_frame, ExportSurfaceHandle
grep -c assign_mpp_frame /tmp/rk.log
```

Si un vídeo se recarga una y otra vez, cuenta los errores del decodificador
de Firefox:

```bash
journalctl -b --since "-10min" | grep -c NS_ERROR_DOM_MEDIA_FATAL_ERR
```

También puedes ver la actividad de la VPU:

```bash
cat /sys/class/devfreq/*/cur_freq   # la frecuencia de la VPU sube con carga
```

## Lanzador permanente de Firefox

Crea `/usr/local/bin/firefox-hw`:

```bash
#!/bin/sh
export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri
export MOZ_DISABLE_RDD_SANDBOX=1
exec /usr/bin/firefox "$@"
```

```bash
chmod +x /usr/local/bin/firefox-hw
```

## Desarrollo

Consulta [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) (en inglés) para la
arquitectura, los detalles internos de VA-API, la integración con MPP y cómo
añadir códecs nuevos.

## Desarrollo asistido por IA

Este driver se diseñó e implementó con ayuda de **Claude Sonnet 4.6**
(ID del modelo: `claude-sonnet-4-6`), un modelo de IA desarrollado por
Anthropic. Tiempo total de desarrollo interactivo: unas **3–4 horas** en dos
sesiones (24 de abril de 2026).

La IA ayudó con: el diseño de la arquitectura, la implementación de la vtable
de VA-API, la reconstrucción de SPS/PPS H.264 en Annex B con codificación
Exp-Golomb, la integración con la API de MPP, la exportación de superficies
DMABUF/DRM PRIME 2 y la depuración iterativa de la integración con Firefox.

Los arreglos de H.264 de la 1.0.12 (referencias por defecto del PPS,
`fast_out`) se diagnosticaron e implementaron con **Claude Opus 5.5**
(`claude-opus-5-5`) el 24 de septiembre de 2026, reproduciendo el fallo de
Firefox con mpv y comparando cada frame con la decodificación por software.

Todo el código lo ha revisado, probado y validado en hardware real
Eduardo García-Mádico Portabella — EGP Sistemas.
