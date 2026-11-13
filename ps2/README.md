# Geometry Dash PSP -> PS2: plan de port

El proyecto guarda la logica de Geometry Dash en `source/`, basada en GD3DS.
En PSP se compila casi el mismo juego con `psp/include/` (API compatible con
3DS) y `psp/src/` (implementaciones para hardware PSP). La ruta mas directa
para PS2 consiste en crear un backend equivalente en `ps2/`, conservando
`source/` y las hojas de niveles originales.

**No confundir los ELF:** `gdps2-prototype.elf` es solo una prueba de GS
con un cuadrado; `gdps2-menu.elf` es una demo aislada del menu; el port
del **motor Geometry Dash original** es `gdps2-engine-experimental.elf`.
Para abrir este ultimo en PCSX2 desde Windows, ejecutar:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\ps2\tools\run_engine_pcsx2.ps1
```

Puede indicarse `-PCSX2 "ruta\pcsx2-qt.exe"` si el launcher no lo detecta.
La prueba de PCSX2-Sanity v1.0.3 del 16/10/2026 confirmo que el ELF
original abre ROMFS, carga atlas, dibuja frames y entra al bucle de menu.
Tras corregir el blending de gsKit y fusionar el titulo de PSP con el menu,
la captura actual del **motor original** ya muestra imagenes, fondo, titulo
y botones correctamente coloreados. Quedan efectos de jugador que pueden
producir lineas visuales anormales; el port sigue siendo experimental y
**no es un release jugable**.

### PCSX2: escena real de Stereo Madness (16/10/2026)

Se avanzo del menu al primer nivel original sin reemplazar `source/main.c`:
una compilacion de **diagnostico separada** inyecto un pulso de `KEY_START`
en el menu principal y otro en el selector. El log registro:

```text
GDPS2 STATE: MAIN_MENU
GDPS2 DIAG: simulated START state=0 scan=90
GDPS2 STATE: LEVEL_SELECT
GDPS2 DIAG: simulated START state=1 scan=110
GDPS2 STATE: GAME
```

En ese mismo arranque se abrio `romfs:/main_levels/StereoMadness.gmd` y
se descomprimieron **42.384 bytes** de datos de nivel. Las primeras
capturas mostraban solo el fondo, el progreso visual y el boton pausa:
los objetos del nivel quedaban tapados por el fondo de la pantalla
inferior, que se enviaba al **mismo framebuffer GS** tras terminar la
pantalla superior. `source/main.c` omite ahora ese segundo fondo y sus
suelos unicamente bajo `__PS2__`, manteniendo la UI inferior. Capturas
posteriores en PCSX2 confirman **jugador, pinchos, suelo y HUD**, ademas
de cambio de posicion/rotacion entre frames.

El menu tambien dibujaba las estelas dos veces con camaras distintas;
en PS2 se conserva la pasada inferior visible del jugador y se evita
repetir los efectos fuera de pantalla desde la pasada superior. En la
prueba con cada pasada aislada no se observaron las diagonales largas;
queda comprobar estabilidad de los efectos durante gameplay prolongado.

Para reproducir el test de navegacion sin mando, construir un ELF que
**no debe utilizarse para jugar normalmente**:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\ps2\tools\build_engine.ps1 -DiagnosticAutoInput -Output .\ps2\out\gdps2-diag-autonav.elf
```

La macro `PS2_DIAG_AUTONAV` existe exclusivamente en el backend de entrada
y solo se activa mediante ese switch. El ELF habitual se genera sin esa
macro ejecutando `build_engine.ps1` normalmente. El modo diagnostico
no demuestra mandos manuales, salto al pulsar, audio audible, colisiones
correctas ni rendimiento estable. Los registros de PCSX2 incluyen
advertencias de cambios inesperados de cabecera MPEG, pendientes de
investigar en el reproductor musical.

Capturas locales de la prueba (no versionadas):
`ps2/out/pcsx2-smoke/engine-game-fixed-0.png` y
`ps2/out/pcsx2-smoke/engine-game-fixed-1.png`.

### Pruebas de mando, cubo rotado y nivel pesado (16/10/2026)

**Entrada real de PCSX2:** `ctru_ps2_input.c` lee el puerto 0 de libpad
(mando de jugador 1) y mapea Cruz a `KEY_A`, Circulo a `KEY_B`,
Start a `KEY_START`, cruceta y sticks a sus teclas compatibles 3DS.
Se agregaron logs de la primera lectura valida `padRead` y de un numero
limitado de pulsaciones `GDPS2 PAD` para diagnosticar el mando sin saturar
la consola. En PCSX2-Sanity el perfil de prueba tiene `[Pad1]` en
`DualShock2`, con entradas `SDL-0`. El log de una prueba de Stereo Madness
mostro lecturas validas y pulsaciones `KEY_A`, `KEY_B` y `KEY_START` en
`STATE_GAME` **sin inyeccion automatica en ese estado**. Una captura muestra
el menu de pausa abierto. Esto confirma comunicacion con el mando emulado
por PCSX2, pero **no es una prueba sobre hardware DualShock 2 fisico** ni
demuestra por si solo que el salto manual sea exacto en todas las condiciones.

**Cubo recortado al rotar:** el backend `ps2/src/c2d_ps2.c` utilizaba
`gsKit_prim_quad_texture_3d()` para las cuatro esquinas rotadas.
Ahora dibuja los dos triangulos `TL-TR-BR` y `TL-BR-BL` por separado,
con UV correspondientes. La ruta rapida `GS SPRITE` se conserva para
sprites sin rotacion ni cizalla. En las nuevas capturas de PCSX2 el cubo
ya conserva las cuatro esquinas, incluso en escenas de muchos efectos.
Conviene seguir comprobando giros rapidos, naves, efectos y sprites con
escala negativa.

**Nivel mas pesado:** se probo `Theory of Everything 2` (indice 17),
cuyo archivo `.gmd` ocupa 93.622 bytes frente a los 11.374 de
Stereo Madness. El loader extrajo **388.036 bytes** y reporto alrededor
de **1.843.200 bytes de objetos**. Se mostraron portales, bloques,
pinchos, efectos y el cubo durante distintas capturas, sin bloqueo
inmediato del emulador. El working set de PCSX2 en Windows no es la RAM
EE de PS2. **No se midieron FPS, memoria EE/GS ni audio reproducido.**

El selector automatico admite nivel optativo solo para diagnostico:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\ps2\tools\build_engine.ps1 -DiagnosticAutoInput -DiagnosticLevelIndex 17 -Output .\ps2\out\gdps2-diag-toe2.elf
```

El parametro `-DiagnosticLevelIndex` acepta 0..17 y exige el modo de
autonavegacion. El selector del test fija el indice antes del `START`
simulado, y `GDPS2 LEVEL` registra indice y ruta elegida al comenzar el
juego. Para compilar la variante **sin pulsaciones sinteticas**, omitir
`-DiagnosticAutoInput` y `-DiagnosticLevelIndex`.

Capturas locales (no versionadas):
`ps2/out/pcsx2-smoke/toe2-triangulated-0.png`,
`toe2-triangulated-2.png`, y `stereo-fixed-verified-0.png`.
En pausa, la interfaz superior/inferior puede seguir superponiendose de
forma incorrecta en GS; requiere un ajuste propio en una fase siguiente.

## Prueba de hardware disponible

`prototype.c` es una **prueba de arranque nativa**, no ejecuta aun el motor
original. Abre el GS con gsKit, muestra un escenario 2D, un cuadrado con
salto, y comprueba la lectura de un DualShock mediante libpad.

La segunda prueba, `menu_demo.c`, enlaza el backend `src/c2d_ps2.c` y el
driver `src/ctru_ps2_input.c`. El generador `tools/make_menu_ps2.py` lee
**directamente** `psp/menus/main_menu_psp.txt` y
`psp/menus/main_menu_top_psp.txt` para conservar posiciones, tamanos,
acciones y keyBinds. Valida ademas que las acciones existan en
`source/menus/main_menu.c`. Empaqueta solo los ocho sprites referenciados,
desde `gfx/ui/ui.t3s` y `gfx/ui/ui_2/ui_2.t3s` (incluido el titulo alternativo)
en **una pagina GS T8 de 512x512**.

La prueba genera un menu graficamente basado en el layout original:
cruceta navega segun la posicion de botones, Cross activa el elemento
seleccionado, Start abre Level Select, Triangle abre Icon Kit, Square abre
Creator Menu, Select alterna el titulo y Circle vuelve de la pantalla
de estado. Las acciones muestran por ahora `NOT YET PORTED`: los submenus
originales aun necesitan sus propios subsistemas.

El driver PS2 expone `hidScanInput`, `hidKeysDown`, `hidKeysHeld`, `hidKeysUp`,
`hidTouchRead`, `hidCircleRead` y `hidSharedMem`. Mapea el mando PS2 a las
teclas 3DS y actualiza el anillo HID usado en `precise_input.c`. Todavia
falta probar la reconstruccion temporal de pulsaciones en gameplay real.

El menu ya utiliza el ciclo de Citro3D (`C3D_FrameBegin` / `C3D_FrameEnd`)
y dos `C3D_RenderTarget` logicos: superior (400x240) e inferior (320x240).
Los elementos conservan sus coordenadas originales en el layout, sin sumar
manualmente el desplazamiento `@merge`. El backend aplica `@merge 40 0 1`
al dibujar la pantalla inferior sobre la superior. El objetivo inferior
no borra el superior cuando se llama a `C2D_TargetClear`, como en PSP.

Con el SDK Windows ya presente en `psdevwindows/`:

```powershell
powershell -ExecutionPolicy Bypass -File .\ps2\build.ps1
```

La salida es `ps2/out/gdps2-prototype.elf`, ejecutable en PCSX2 o mediante
el cargador ELF de la PS2. El prototipo se ha visto en PCSX2; es un
cuadrado amarillo y un suelo de prueba, **no el gameplay original**.
No se ha validado en consola fisica. Si hay un SDK externo, usar
`-ToolchainRoot <ruta>` o las variables
`PS2DEV`, `PS2SDK`, `GSKIT`.

Compilar la segunda prueba, que integra el atlas en el ELF (sin archivos
adicionales para cargarlo al arrancar):

```powershell
powershell -ExecutionPolicy Bypass -File .\ps2\build.ps1 -MenuDemo
```

Salida: `ps2/out/gdps2-menu.elf`. Necesita Python 3 y Pillow durante la
conversion de los graficos; esta herramienta usa FASTOCTREE para no depender
de `libimagequant` como el empaquetador de PSP. El archivo contiene su propio
atlas y la tabla de elementos generada en `ps2/out/menu_layout.h`.

En Linux o un shell compatible con el toolchain instalado:

```sh
make -f Makefile.ps2
make -f Makefile.ps2 menu
```

Pruebas reproducibles del generador y estructura de atlas:

```sh
python -m unittest discover -s ps2/tests -v
```

## Compilacion del motor original (offline)

Se incorporo **cJSON v1.7.19** al port PS2 en
`ps2/third_party/cjson/` (licencia MIT, hashes y origen en `SOURCE.md`).
El adaptador de `psp/include/json-c/json.h` se reutiliza sin cambiarlo.
El SDK incluido tambien dispone de `zlib` y `mpg123` en
`psdevwindows/ps2sdk/ports/`; el checker los incluye en sus cabeceras y
el analisis de enlace prueba `-lz` y `-lmpg123`.

Para compilar **los objetos originales** del juego con GCC de PS2:

```powershell
powershell -ExecutionPolicy Bypass -File .\ps2\tools\check_engine.ps1
```

Resultado verificado: **95/95 objetos MIPS** generados correctamente (93
`source/*.c` sin `source/utils/network.c`, mas `libraries/color.c` y cJSON).
El comando crea `ps2/out/engine/compile_report.json`, objetos y logs por
archivo. Tambien admite `-SyntaxOnly`.

Para auditar el **enlazado del motor completo** con backends PS2 nativos:

```powershell
powershell -ExecutionPolicy Bypass -File .\ps2\tools\check_link.ps1
```

Este comando compila tambien `ps2/src/ctru_ps2.c`,
`ps2/src/c2d_ps2.c`, `ps2/src/ctru_ps2_input.c`,
`ps2/src/romfs_paths.c`/`romfs_ps2.c`,
`ps2/src/ndsp_mixer.c`/`ndsp_ps2.c`/`audsrv_stream.c` y
`ps2/src/swkbd_edit.c`/`swkbd_ps2.c`, ademas de
`ps2/audio_irx.S`. Genera
`ps2/out/engine/link_report.json` y `link_probe.log` y enlaza los wrappers
`--wrap=fopen,open,stat,opendir,access,remove,unlink,rename,mkdir`.
El ultimo enlace diagnostico **resolvio los 53 simbolos** inicialmente
pendientes. La tabla indica el estado de cada subsistema:

| Pendiente | Simbolos distintos | Motivo |
|---|---:|---|
| NDSP/audio | 0 | Mixer EE + audsrv/SPU2: falta prueba de audio en hardware |
| `C2Di_*` | 0 | Triangulos texturizados Gouraud con indices GS (sin prueba visual) |
| `swkbd*` | 0 | Editor ASCII con D-pad y botones, falta prueba visual |
| `romfsInit`/`romfsExit` | 0 | Mapeo a dispositivos PS2 (sin prueba en hardware) |

`ps2/src/ctru_ps2.c` ya incluye temporizadores, memoria alineada,
semaforos/eventos, hilos EE, deteccion de modelo y arranque grafico con
gsKit. Es una capa en desarrollo: no se ha verificado su ejecucion en PS2.
Los assets originales ya tienen conversor PS2, y las texturas estan bajo
el gestor de gsKit, pero faltan pruebas reales de la carga, del uso de
RAM/VRAM y del primer frame. **Enlazar correctamente no significa que
funcione el juego**. La salida del checker sigue siendo solo diagnostica.

Un ELF experimental con `source/main.c` se puede generar de forma explicita:

```powershell
powershell -ExecutionPolicy Bypass -File .\ps2\tools\build_engine.ps1
```

**Build normal para Windows (sin autonav ni tests):** ejecutar
`build.bat` desde la raiz del repositorio (tambien sirve doble clic).
Compila el motor original en modo `-O2`, sin ningun flag `Diagnostic*`,
sin iniciar PCSX2 ni ejecutar los tests de audio. Empaqueta los recursos
originales en `ps2/out/release/romfs/` y genera
`ps2/out/release/GeometryDashPS2.elf`. Los SFX WAV no se convierten. Las
canciones de `romfs/songs/*.mp3` se convierten con `ffmpeg` (debe estar en el
`PATH`, o `--ffmpeg RUTA`) a WAV IMA-ADPCM de 4 bits (~56 MiB las 21) y el
paquete ya no lleva los MP3 (`--keep-mp3` los copia tambien). El juego pide
`X.mp3` y `source/music_player.c` reproduce `X.wav` si existe (decoder en
`ps2/src/ima_wav.h`, probado bit a bit contra ffmpeg con
`ps2/tools/test_ima_wav.ps1`). Las canciones custom (`saved_songs`) siguen
siendo MP3 con `mpg123`.
Con HostFs habilitado, PCSX2 expone `host:` relativo al directorio del ELF,
asi que esta distribucion tiene las rutas `host:romfs/...` junto al binario.
Se trata de una **compilacion de distribucion experimental**, todavia sin
certificacion en una PS2 fisica. El empaquetado deja intactos SDK y fuentes.

Para tirones a 60 FPS, el port PS2 ahora evita aumentar el salto temporal
de la fisica tras una iteracion lenta (mantiene el acumulador a 240 Hz) y
reduce trigonometria en sprites sin rotacion. Existe un perfilador separado
con `-DiagnosticFrameTiming` para medir los picos del update, GS flip y
streaming de texturas; ver `ps2/DOCS/FRAMETIME_ESTABILIDAD_20261016.md`.
No se habilita en `build.bat` ni en el ELF normal.

**Interfaz 4:3 de PS2:** la zona de juego llena las 448 lineas del GS NTSC
en vez de usar un marco con franjas negras. **No se comprime toda la imagen
de 400x240:** la ventana visible se recorta a 320x240 logicos para mantener
las proporciones fisicas de sprites en 4:3. `gameplay_ps2.txt` y
`gameplay_top_ps2.txt` presentan una sola ventana de pausa, barras de
progreso separadas de botones/volumen y ocultan las monedas solo del HUD,
sin alterar la recoleccion. Ver `ps2/DOCS/INTERFAZ_4_3_20261016.md`.
Verificacion de layout: `python ps2/tools/test_ps2_layout.py`.
El salto fantasma al entrar al nivel usando CRUZ se suprime hasta soltar
la pulsacion inicial, sin afectar los saltos posteriores; ver
`ps2/DOCS/FIX_SALTO_INICIAL_20261016.md` y
`ps2/tools/test_ps2_aspect_and_jump.ps1`.

Se genera `ps2/out/gdps2-engine-experimental.elf` solo si la compilacion
original y el enlazado pasan. Para incluir el empaquetado local ROMFS en
el mismo comando, usar `-PackageRomfs`. El ELF es **experimental**:
el arranque se ha probado en PCSX2, pero faltan corregir graficos,
controles en el menu, entrada a niveles y sonido audible.

### Empaquetado de los atlas y VRAM

La carpeta `ps2/out/package/romfs/` contiene recursos del juego preparados
para PS2 (no se modifican los archivos originales ni el SDK). Generacion:

```powershell
python ps2/tools/package_romfs.py
```

La herramienta convierte los **24** archivos `gfx/**/*.t3s` a paginas
T8 + CLUT GS (`.p2sheet`) manteniendo los indices originales (2895 sprites,
43 paginas). Copia otros recursos de `romfs/` como menus, `.gmd`, `.mp3`
y `.wav`. Omite los viejos `.t3x` porque el backend sustituye esas rutas
por `.p2sheet`. **No distribuye ni descarga recursos**: solo empaqueta los
que estan en el proyecto local. El reporte verificable se crea en
`ps2/out/package/ps2_assets_report.json` e incluye paginas, bytes y SHA-256.

La suma de las paginas T8 completas es aproximadamente **8,72 MiB**, mayor
que los 4 MiB de VRAM GS, incluso sin los buffers de pantalla. Los dos
framebuffers NTSC 640x448 CT16 representan por si solos un minimo de
1.094 MiB, sin contar alineaciones. Por ello, en `gfxInitDefault()` se
inicializa el `gsKit_TexManager` en modo `ETM_INLINE` y el backend
`C2D_SpriteSheetLoadFromMem()` conserva las paginas en RAM EE. La subida
a VRAM se solicita con `gsKit_TexManager_bind()` al dibujar; el gestor
puede reciclar VRAM entre paginas, y `gsKit_TexManager_nextFrame()` avanza
su mantenimiento despues de cada frame. La demo independiente conserva su
carga directa tradicional para evitar regresiones.

**Comprobacion grafica en PCSX2 (16/10/2026):** el atlas PS2 reconstruido
en CPU tenia sus colores intactos. Antes, la demo de menu y el motor
mostraban contornos y colores parcialmente desaparecidos. El problema
principal NO era T8/CLUT ni TexManager: las aplicaciones usaban
`GS_BLEND_BACK2FRONT` (`0x01`) que no representa el `source-over` esperado
por Citro2D. Ahora `ps2/menu_demo.c`, `ps2/src/ctru_ps2.c` y
`C3D_AlphaBlend()` configuran `GS_SETREG_ALPHA(0,1,0,1,0)` para mezcla
normal, sin PABE. La mezcla aditiva usa `(Cs-0)*As+Cd`; las imagenes sin
rotacion utilizan `gsKit_prim_sprite_texture_3d()`.

Se capturo el ELF independiente con fondo azul, logo y botones completos;
el ELF original con menu, fondo rojo, sprites y titulo. Para el titulo
original se cargan los layouts `romfs:/menus/main_menu_psp.txt` y
`main_menu_top_psp.txt` (solo PS2). La pantalla superior se dibuja al final
del frame del menu PS2, para impedir que el fondo de la inferior la tape.
Quedan por validar rutas de geometria rotada, efectos complejos, uso de
VRAM, menu de niveles, gameplay y guardado.
La RAM EE sigue necesitando alojar paginas descomprimidas, estado del
juego y musica (32 MiB en total). Tampoco se conocen aun la cantidad de
reenvios de textura o los tirones de bus GIF en escenas con muchas paginas.
Se requieren mediciones reales antes de considerar el streaming estable.

Para una comprobacion rapida de empaquetado, puede pasarse
`--only-atlas ui_2 --no-media`; `--dry-run` analiza sin escribir archivos.
Para ejecutar con `host:` en PCSX2/ps2link, el directorio raiz expuesto
por el host debe contener `romfs/menus/main_menu.txt` y la carpeta
`romfs/gfx/*.p2sheet`. En nuestro caso se puede usar como raiz
`ps2/out/package/`. `romfsInit()` aun depende de que el dispositivo `host:`
se encuentre operativo y no monta drivers USB automaticamente.

### Instrumentacion del arranque experimental

El ELF de motor ahora incluye `ps2/src/boot_trace.[ch]` y marcas
`GDPS2 BOOT 00` a `GDPS2 BOOT 17` en la secuencia **real** de
`source/main.c`. Son trazas de consola EE (stdout), no un registro en
disco: pueden imprimirse incluso antes de que `host:` este disponible.
Se hace `fflush` y se configura stdout sin buffering desde la primera
etapa. Las marcas muestran la **entrada** en cada etapa, no garantizan
por si solas que haya terminado correctamente.

La secuencia relevante es: `00` entrada, `01` SIF/RPC, `02` ROMFS,
`03` gsKit/GS, `04` C3D/C2D, `05` configuracion, `06` targets,
`07` NDSP, `08` atlas de carga, `09` UI de carga, `10` primer dibujo,
`11` atlas UI, `12` atlas del juego, `13` progreso/estadisticas,
`14` cache de sprites, `15` efectos, `16` WAV y `17` bucle de estados.
Las inicializaciones con codigo de retorno emiten `GDPS2 RESULT`.

**Importante:** el orden anterior probaba ROMFS antes de inicializar SIF/RPC.
En PS2 ahora se llama a `sceSifInitRpc(0)` antes del sondeo de `host:`.
Si no aparece `romfs/menus/main_menu.txt` en los dispositivos probados,
`romfsInit()` registra cada ruta y su `errno`, y `main()` termina en PS2
con error, en vez de continuar hasta un `svcBreak()` posterior.

El backend de atlas informa `GDPS2 ATLAS: OK ...` con ruta original,
ruta `.p2sheet`, bytes, sprites y paginas. Ante un fallo muestra la
ruta original y/o causa: apertura/`errno`, tamaño, lectura, metadatos,
reserva de RAM EE o VRAM GS. Los tres primeros `C3D_FrameEnd()` en modo
streaming imprimen `GDPS2 GS: submitted frame=... bind_requests=...`:
**submitted no significa imagen correcta en pantalla**, y
`bind_requests` contabiliza llamadas a TexManager, no transferencias
DMA reales ni paginas unicas.

Para una comprobacion reproducible:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\ps2\tools\build_engine.ps1
```

Preparar la raiz de archivos antes de iniciar el ELF (si hace falta
reconstruirla: `python ps2/tools/package_romfs.py`). Debe exponerse
`ps2/out/package/` al dispositivo PS2 `host:` de forma que este resuelva
**exactamente** `host:romfs/menus/main_menu.txt`. En PCSX2, la mera
ubicacion de una carpeta en Windows no monta necesariamente `host:`;
se necesita un entorno/configuracion que proporcione ese dispositivo
(por ejemplo, un host soportado por la herramienta de desarrollo o
ps2link). Consultar la consola de depuracion/EE stdout del entorno
de ejecucion. Si no se dispone de `host:`, se requiere previamente un
driver USB `mass:` o soporte `cdfs:` que realmente permita acceder a
`romfs/` en ese medio.

En la instalacion de PCSX2-Sanity v1.0.3 probada, `HostFs = true` y
el emulador fijo `host:` en la **carpeta del ELF**, `ps2/out/`, no en
el directorio de trabajo del proceso. Para este caso, el adaptador
prueba adicionalmente `host:package/romfs/...`, que corresponde a
`ps2/out/package/romfs/...`. Esto permite ejecutar el ELF desde `ps2/out`
sin mover ni duplicar el empaquetado. Si se ejecuta con ps2link y una
raiz `host:` distinta, la primera opcion `host:romfs/...` sigue disponible.

Buscar la ultima linea `GDPS2 BOOT` y los `GDPS2 RESULT`, `ROMFS`,
`ATLAS` o `GS` cercanos para encontrar el primer fallo. Adjuntar ese
extracto a la siguiente investigacion. Prueba real PCSX2-Sanity v1.0.3:
`romfsInit=0`, `C3D_Init=0`, `C2D_Init=0`, `ndspInit=0`, atlas
`ui`, `ui_2`, `sprites`, `icons`, `portals` y el resto del arranque
cargados, tres primeros frames enviados al GS, y `GDPS2 BOOT 17`
alcanzado. La captura inicial tenia corrupcion visual y luego se corrigio
la mezcla de colores y la capa del titulo (ver arriba). Esto no
verifica que los niveles sean jugables, que el audio se oiga ni que
funcione en una PS2 fisica.

### Audio NDSP de PS2

El backend `ps2/src/ndsp_ps2.c` implementa el API NDSP que requiere el
juego. Mezcla en un hilo EE hasta 24 canales de PCM8/PCM16 mono/estereo,
aplicando volumen por canal, interpolacion lineal al convertir tasas de
muestreo y el estado `FREE/QUEUED/PLAYING/DONE` de `ndspWaveBuf`.
El resultado se entrega a `audsrv` como PCM16 estereo a 44100 Hz.
El driver lo envia al SPU2 por SIF RPC. El callback de refill se invoca
fuera del mutex del mezclador. Los formatos NDSP ADPCM y los modos
estereo/surround especiales no estan implementados.

El ELF incluye `audsrv.irx` desde el SDK local mediante
`ps2/audio_irx.S` (sin copiar ni modificar `psdevwindows/`); el
backend lo intenta cargar en el IOP con `SifExecModuleBuffer`.
Depende de `rom0:LIBSD` y del parche LOADFILE. En un sistema sin soporte
de IOP audio, `ndspInit` devuelve error y lo registra. No hay pruebas
de sonido audible en PS2 fisica ni mediciones de latencia, cortes o CPU/IOP.

**Correcciones de audio comprobadas en PCSX2-Sanity (16/10/2026):**

- Los 21 MP3 del paquete de juego tienen 32000 Hz, dos canales. La ruta
  `source/music_player.c` solo permitia 44100/48000 en `mpg123_format`.
  En la compilacion `__PS2__` se permite tambien PCM16 32000 mono/estereo;
  el mezclador NDSP lo convierte a 44100 Hz para `audsrv`/SPU2.
- Los registros anteriores mostraban `audsrv_play_audio wrote 664 of 2048`
  y el worker abandonaba el streaming despues de una escritura parcial.
  `ps2/src/audsrv_stream.[ch]` reintenta el resto, avanzando el puntero
  **solo** por la cantidad realmente aceptada. Esperas/escrituras sin
  progreso siguen produciendo errores diagnosticos, sin bucles infinitos.
- Se arreglo tambien el numero de frames usado para calcular RMS del MP3:
  `decode_mp3` devuelve muestras PCM16, no bytes.
- En el motor completo, PCSX2 registro `GDPS2 MP3: decoder open rate=32000
  channels=2`, el primer bloque PCM no nulo del mixer y nuevas aperturas
  de canciones al cambiar de menu/nivel. Esto prueba decodificacion y
  produccion PCM en EE, **no que el audio haya sido escuchado**.
- El ELF aislado `gdps2-audio-test.elf` termino la prueba de dos tonos
  sin errores de inicializacion. Una variante diagnostica fuerza envios
  parciales de 664 bytes y tambien completo la prueba en PCSX2 con
  registros de los reintentos. No sustituye escuchar los tonos con
  altavoces ni probar hardware real.
- La auditoria de `mpg123` detecto el EOF con buffer de cero muestras y
  la sincronizacion insegura entre `seek_music` y el encolado. La ruta PS2
  ahora clasifica `DONE`, `NEW_FORMAT`, `NEED_MORE`, y evita encolar cero
  muestras. La musica sigue siendo MP3 y los efectos siguen siendo WAV.
- **Hallazgo de depuracion de prioridades:** PS2SDK iniciaba el hilo
  principal con prioridad EE **1**, dejando sin tiempo al worker de audio
  (prioridad **24**). En `source/main.c`, despues de `ndspInit` exitoso,
  se normaliza el main thread a prioridad **32**; la pausa artificial
  de 1 ms por frame empleada en experimentos fue retirada. PCSX2-Sanity
  reprodujo la cadencia de **256 bloques PCM16 estereo de 512 frames en
  aproximadamente 2,97 s**, con musica activa y contenido no nulo, incluso
  por encima de 10.000 bloques. El menu mantuvo unos 60 frames/s.
- Una ruta rapida para un solo canal PCM16 estereo evita recorrer 24 canales
  por muestra; los efectos concurrentes usan el mezclador general. La
  comparacion host muestra los mismos PCM y transiciones de wave buffers.
  En un ELF independiente los 256 bloques se calcularon en 225 ms con el
  mixer generico y 67 ms con la ruta rapida. La variante de punto fijo
  permanece solo en diagnostico: no se habilita en builds normales.
- Un ELF de diagnostico cargo **Theory of Everything 2** y registro tanto
  decodificacion MP3 del nivel como efectos WAV. No prueba por si solo
  audibilidad en hardware, perfeccion de sincronizacion ni gameplay entero.

Metodologia, mediciones y logs:
`ps2/DOCS/AUDIO_AUDITORIA_PS2SDK_MPG123.md`,
`ps2/DOCS/AUDIO_CORRECCION_EOF_20261016.md` y
`ps2/DOCS/AUDIO_DEPURACION_HILOS_Y_MIXER_20261016.md`.

Prueba aislada del mixer (GCC de PC):

```powershell
powershell -ExecutionPolicy Bypass -File .\ps2\tools\test_audio.ps1
powershell -ExecutionPolicy Bypass -File .\ps2\tools\test_audsrv_stream.ps1
powershell -ExecutionPolicy Bypass -File .\ps2\tools\test_mp3_decode_flow.ps1
powershell -ExecutionPolicy Bypass -File .\ps2\tools\test_mixer_fast_reference.ps1
powershell -ExecutionPolicy Bypass -File .\ps2\tools\test_mixer_fixed_reference.ps1
```

Para comprobar manualmente la salida real de sonido en PCSX2, tambien hay
un **ELF independiente de los recursos del juego**:

```powershell
powershell -ExecutionPolicy Bypass -File .\ps2\tools\build_audio_test.ps1
```

Genera `ps2/out/gdps2-audio-test.elf`. Debe reproducir durante ~1 segundo
un tono de 440 Hz en el lado izquierdo y otro de 660 Hz en el derecho,
y terminar despues. El codigo comprueba el estado `NDSP_WBUF_DONE`.
Si falla la carga de audsrv o los RPC, registra el resultado para
la consola EE. **Compilar el ELF no equivale a haber oido el audio**:
la prueba ya se ejecuto en PCSX2, pero falta confirmacion auditiva.

Para repetir la prueba de fragmentos parciales **sin cambiar el ELF del
juego**:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\ps2\tools\build_audio_test.ps1 -PartialWriteDiagnostic
```

La salida es `ps2/out/gdps2-audio-partial-test.elf`. La macro de prueba
`PS2_AUDIO_TEST_PARTIAL_SEND` no se incluye en el ELF normal ni en el
audio-test habitual. Logs de esta prueba: `NDSP PS2: partial IOP write
664/2048; retrying remainder` y `GDPS2 AUDIO TEST: finished`.

### Teclado virtual con mando

`ps2/src/swkbd_ps2.c` implementa una ventana basica sobre Citro2D
(sin fuentes externas) y `swkbd_edit.c` gestiona el estado. Usa D-pad
para recorrer caracteres, Cruz para escribir, Circulo para cancelar,
Start para aceptar, Cuadrado para borrar y Triangulo para alternar
mayusculas/minusculas. Soporta letras ASCII, numeros y espacios; no
soporta aun Unicode, teclado USB, acentos ni puntuacion ampliada.
La entrada se conserva sin modificar si el usuario cancela.

```powershell
powershell -ExecutionPolicy Bypass -File .\ps2\tools\test_keyboard.ps1
```

El smoke test del adaptador JSON puede ejecutarse con GCC de PC:

```powershell
powershell -ExecutionPolicy Bypass -File .\ps2\tools\test_json.ps1
```

### ROMFS y rutas de guardado PS2

`ps2/src/romfs_ps2.c` prueba archivos reales en `host:` (PCSX2 o ps2link),
`mass:/GDPS2` (USB) y `cdfs:` (disco), en ese orden. Se comprueba la
existencia de `romfs/menus/main_menu.txt` antes de elegir la ubicacion.
Por ejemplo, el menu debe estar en `host:romfs/menus/main_menu.txt` o
`mass:/GDPS2/romfs/menus/main_menu.txt`.

**El port no carga los drivers USB Mass o de disco por si solo.** Deben
estar operativos antes de usar `mass:` o `cdfs:`. La seleccion de dispositivo
se ha probado en PCSX2 con `host:package/`; la ruta USB y `cdfs:` siguen
sin validacion real. Los atlas `.t3x` ya estan convertidos a `.p2sheet`
con una estrategia inicial de streaming para los 4 MB de VRAM;
falta corregir la imagen y medir uso de memoria real.

Los archivos `/3ds/gd3ds/` y `sdmc:/3ds/gd3ds/` se redirigen al directorio
`save/` del dispositivo elegible: `host:save/config.json`,
`mass:/GDPS2/save/config.json` o `mc0:GDPS2/save/config.json` y equivalente
de `mc1:`. El port ahora usa `libmc` (`rom0:MCMAN` + `rom0:MCSERV`) para
detectar tarjetas PS2 formateadas, prioriza partidas preexistentes y evita
escribir los 18 progresos por defecto en cada arranque. **No formatea ni
borra tarjetas**, y se informa al usuario por consola de la ruta elegida.
El logger PS2 no crea `output.txt` en Memory Card. Falta verificar lectura
y escritura reales en hardware/imagen PCSX2, e icono del navegador PS2.
Ver `ps2/DOCS/GUARDADO_MEMCARD_20261016.md` para el orden de seleccion,
limitaciones y las rutas distintas de las builds antiguas/release.
Al guardar se crean carpetas padre.
`romfs:/` es de solo lectura. Se rechazan intentos de traversal (`..`),
caracteres de dispositivo inesperados y rutas truncadas. Prueba en PC:

```powershell
powershell -ExecutionPolicy Bypass -File .\ps2\tools\test_romfs.ps1
powershell -ExecutionPolicy Bypass -File .\ps2\tools\test_save_selection.ps1
```

### Vertices internos Citro2D

`ps2/src/c2d_ps2.c` implementa `__C2Di_Context`, `C2Di_AppendVtx`,
`C2Di_AppendTri`, `C2Di_AppendQuad`, `C2Di_Update`, `C2Di_CalcQuad` y
`C2Di_FlushVtxBuf`. Conserva el orden de los indices y utiliza
`gsKit_prim_triangle_goraud_texture_3d` para textura y color por vertice.
Se vacia el buffer antes de cambiar la vista, escena, scissor, textura o
modo de tinte. **Falta validacion visual y de rendimiento.** Varios modos
de mezcla avanzada de Citro3D siguen siendo aproximaciones.

La fuente `source/main.c` ahora omite `<curl/curl.h>` solo en `__PS2__`:
el modo offline no compila `source/utils/network.c`, y `main.c` no necesita
la API curl. `source/music_player.c` usa `s32` para prioridad de hilo (tipo
compatibilidad libctru), ya que `int32_t` y `s32` tienen representacion C
distinta en el ABI del compilador PS2 instalado.

## Correspondencia de capas

| Componente actual | Adaptacion PS2 | Prioridad |
|---|---|---|
| `source/player/`, `source/level/`, `source/objects.c` | Reutilizar logica y datos, corregir dependencias puntuales | Alta |
| `psp/include/3ds.h`, `ctru_psp.c` | Nuevo `ps2/include/3ds.h`, `ctru_ps2.c`: reloj, hilos, libpad, rutas | Alta |
| `psp/src/c2d_psp.c` | `c2d_ps2.c`: Citro2D sobre GS mediante gsKit / dmaKit | Alta |
| `psp/tools/mkatlas.py` y `.psheet` | Conversor nuevo a texturas GS T8/T4 o CT16, CLUT y UV compatibles | Alta |
| `psp/src/ndsp_psp.c` | Mezcla y colas `ndspWaveBuf` sobre audsrv / SPU2 | Media |
| `psp/src/mp3_me_psp.c` | Musica reproducida por SPU2, preferentemente precodificada | Media |
| `psp/src/ctru_psp.c` wrappers de rutas | Acceso a `mass:/`, `mc0:/` o `cdfs:/` segun el medio | Alta |
| `psp/menus/*_psp.txt` | Reutilizar disposicion de pantalla unica y remapear cursor | Media |
| `source/network.c` | Inicialmente deshabilitar como en Makefile.psp | Baja |

## Orden de implementacion

1. **Arranque:** validar ELF nativo con gsKit, libpad, imagen estable y VBlank.
2. **Compatibilidad basica:** trasladar tipos, temporizadores, asignacion,
   input y funciones de archivos de `psp/include` a `ps2/include`, implementando
   `ctru_ps2.c`. La API `hidSharedMem` de `source/utils/precise_input.c` exige
   simular el anillo o introducir una abstraccion compartida.
3. **Dibujo:** implementar `C3D_FrameBegin/End`, objetivos top/bottom y las
   operaciones `C2D_DrawImage`, sprites, lineas, matrices, tintes, mezcla y
   scissor. `source/utils/gfx.c` usa detalles internos de Citro2D y requerira
   adaptar `utils/c2d_internal.h` al nuevo backend.

   **Estado actual**: `ps2/src/c2d_ps2.c` implementa sprites
   escalados/rotados, tinte uniforme, matrices 2D compatibles con PSP,
   rectangulos y degradados de cuatro vertices, triangulos, lineas con
   grosor, elipses y lectura de hojas de sprites. Implementa tambien el ciclo
   de fotograma con doble buffer de gsKit, targets logicos top/bottom,
   superposicion y clear selectivo, fade, scissor basico de GS y los puntos de
   entrada de C3D para blend y TexEnv. Ya existe el buffer interno `C2Di_`
   y dibujo de triangulos de textura Gouraud. Faltan algunos modos de tinte,
   scissor invertido, render-to-texture, mezcla avanzada y objetivos estereo.
   El ELF del menu actual ya reutiliza layout y acciones declaradas en el
   proyecto, pero todavia no enlaza ni ejecuta `main_menu_loop()`.

   **Comprobacion con PS2 GCC**: los 95 archivos de la lista de compilacion
   offline pasan ya compilacion a `.o` (incluido `source/main.c`); los
   107 objetos del link diagnostico tambien enlazan sin simbolos pendientes.
   Aun faltan la ejecucion y las pruebas funcionales de plataforma.
4. **Sprites:** ya se convierten los atlas desde `gfx/*.t3s` y PNG originales.
   Los datos `.psheet` estan swizzled para GE de PSP y **no son directamente
   texturas GS**. Conservar indices y orden de sprites, subtexturas y palettes.
   Existe streaming TexManager; falta medirlo y verificar su orden de dibujo.
5. **Juego offline:** montar rutas `romfs:/` y `sdmc:/` sobre el dispositivo de
   destino; cargar menues, una cancion, un nivel, probar colisiones y guardado.
6. **Audio y sincronizacion:** sustituir NDSP y la decodificacion del Media
   Engine de PSP, manteniendo buffers de PCM, tiempos y sincronismo. Emplear
   preconversion de musica cuando sea razonable para evitar gasto de CPU.
7. **Optimizacion:** medir picos de memoria, sprites, cambios de textura y
   audio; agrupar quads por atlas/material, reducir particulas y ofrecer
   opciones de resolucion. Probar NTSC y PAL; el prototipo usa NTSC y un paso
   por fotograma, pero el motor debe conservar el timestep fijo propio.

## Particularidades PS2

- CPU EE MIPS R5900, GS para graficos y SPU2 para sonido. El render PSP usa
  `sceGu*` y texturas GE; no se puede enlazar directamente en PS2.
- 32 MB de RAM EE y 4 MB de VRAM GS requieren controlar simultaneamente los
  atlas cargados, los buffers de fotogramas y la fragmentacion.
- La pantalla de juego original trabaja en 400x240 (zona superior 3DS); la
  version PSP resuelve dos pantallas sobre una. En PS2 se puede dibujar en una
  zona logica 400x240 y escalarla a 640x448/512, con ajustes de relacion de
  aspecto y margen seguro para televisores CRT.
- El prototipo unicamente verifica el entorno: no incluye menus, niveles,
  assets, efectos de sonido ni logica de colision del Geometry Dash original.
- El segundo ELF incorpora el atlas UI real y puede mostrarlo sin rutas
  `mass:/` o `host:`. Aun hay que verificar visualmente el resultado en PCSX2
  y ajustar la mezcla alfa del GS si difiere de la PSP.
- Los targets son por ahora transformaciones logicas sobre el mismo GS;
  **no** hay framebuffer independiente para cada pantalla ni soporte
  de render targets offscreen. `C3D_GetProcessingTime` y los contadores
  de cmd buffer siguen sin implementarse, devuelven cero.

## Criterios de validacion

- El ELF arranca y dibuja sin corrupcion en PCSX2 y en hardware real.
- Cross provoca un salto, Start reinicia, Select pausa la escena de prueba.
- Despues, la nueva API carga una hoja de sprites y dibuja un menu real.
- Finalmente, se reproduce un nivel completo con colisiones, sonido y guardado.
