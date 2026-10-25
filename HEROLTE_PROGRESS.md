# HEROLTE (Galaxy S7 SM-G930F, Exynos 8890): progreso

Registrar por separado: compilación, marcadores recuperados, lo que se ve en
pantalla y comportamiento de iOS. Plan y mapa de hardware en
`docs/herolte-bringup.md`.

## 2026-10-20

- Dispositivo: SM-G930F, bootloader desbloqueado, TWRP instalado.
- `shim/herolte/probe.S` + `probe.c` compilados con Arm GNU Toolchain 14.2.rel1
  (`aarch64-none-elf`): Image de 10.784 bytes, solo relocaciones PC-relativas
  (ADR_PREL_PG_HI21, ADD_ABS_LO12_NC, CALL26), sin llamadas a librería.
  **Solo compilación**; no se ejecutó en el teléfono.
- `tools/herolte-pack-boot.py` probado con una imagen BOOT sintética
  (página de 2048, DTBH v2 con chip 8890): conserva DTBH y ramdisk, copia
  `text_offset`/flags del kernel de stock, recalcula el SHA-1 del header.
  Todavía no se probó con el BOOT real del S7.
- BOOT del teléfono (custom ROM, no stock), leído con `dd` desde TWRP:
  partición de 41.943.040 bytes, página 2048, kernel arm64 sin comprimir de
  38 MB con `text_offset=0x80000`, DTBH v2 con 3 DTB Exynos 8890
  (hw_rev 4–7, 8, 9). DTB: RAM `0x80000000+0x7e400000` y `0x880000000+2G`,
  initrd en `0x82000000`, reservas TIMA `0xb1000000`, ION desde
  `0xc0000000`, módem `0xf0000000`. `0xe2a00000` cae en un hueco libre.
- `probe-boot.img` armado desde ese BOOT (1.495.056 bytes).
- **Primera prueba en el teléfono (reporte del usuario):** el probe muestra
  texto blanco sobre el logo y la cuenta "TWRP EN 60 S"; al terminar, el
  teléfono reinicia solo a TWRP. Prueba que S-Boot ejecuta nuestro Image
  (header arm64 + DTBH correctos), que el framebuffer `0xe2a00000` con el HW
  trigger de DECON_F funciona, que CNTPCT avanza y que el reinicio a
  recovery por PMU `0x105c0000` (INFORM2/INFORM3/SWRESET) funciona.
- **Foto del probe:** EL 1; SCTLR `0x30c5183e` (MMU off); FDT `0x8a000000`;
  IMAGE `0x800bc000`; MIDR `0x410fd034` (Cortex-A53 r0p4); MPIDR
  `0x80000100`; MMFR0 `0x1122` (sin 16 KiB); PFR0 `0x2222`; ISAR0 `0x11120`
  (AES/SHA1/SHA2/CRC32, **sin LSE**); CNTFRQ **0** (como el A5: frecuencia
  desde el ADT, 26 MHz); DECON trigger previo `0x12b0`; initrd
  `0x89000000..0x890dbf7e` (el ramdisk de la ROM).
- Consecuencia: entre initrd y FDT hay 16 MiB, poco para el payload de XNU.
  `xnu_boot` ahora busca el payload pegado a su Image
  (`herolte-pack-boot.py --append`) y usa el initrd solo como alternativa.
- Serie de parches normalizada sobre `xnu-8796.141.3` (ver
  `docs/herolte-bringup.md`): 123 parches aplican con `git apply` simple,
  incluido `0200-herolte-board-target`. 12 movidos a `xnu/patches-pending/`
  (los 10 rotos m?s `0133`/`0134` de GPU del A5, publicados despu?s).
  **El kernel HEROLTE todavía no se compiló**: Actions pendiente.
- `shim/herolte/xnu_boot` (loader) y `tools/mkadt_herolte.py` escritos y
  compilados/ejecutados localmente; sin prueba en el teléfono.
- `0106`/`0107` reescritos como `0201-herolte-amfi-bridge` y
  `0202-herolte-4k-unsigned-dyld-cache`; 125 parches aplican en orden.
- **Primer kernel HEROLTE compilado en Actions** (fork `wardevsa/ios-a5xelte`,
  rama `herolte`, run 39477885586, commit fb6194b): artefacto `xnu-kernel`
  de 61 MB. El chequeo de LSE encontró una sola coincidencia, dentro de
  `K256` (constantes de SHA-256 en `__TEXT`), es decir, datos: el código del
  kernel no tiene atómicas LSE. El chequeo ahora ignora `K256`.
- Actions publica los errores de compilación como anotaciones públicas
  (los logs requieren sesión de GitHub).
- **Primer arranque de XNU en el S7** (`xnu-boot.img`: loader + payload
  pegado de 16.474.720 bytes; kernel sha256 `61a9dd49…ca5d`, cmdline
  `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`).
  Foto del usuario: consola de XNU en el framebuffer con VM bootstrap,
  zone_init, firehose, scheduler, KDP serial, `iBoot version: herolte-entry`,
  `SWEET PRNG`, `A5XELTE pthread: libpthread-514.100.2 registered`,
  `SWEET AMFI: built-in interface registered`, platform expert iniciado.
  Pánico en `SweetCPU.cpp:86`: `unsupported topology cpu=0 mpidr=0x100
  count=1` (el chequeo esperaba MPIDR 0 del A5).
- `0203-herolte-boot-mpidr`: el MPIDR de arranque es constante de placa
  (`A5XELTE_BOOT_MPIDR`: A5 `0x0`, herolte `0x100`).
- **Kernel completo hasta la espera del root** (run 39481021599, kernel
  sha256 `ffa3d8ea…432f`, misma cmdline). Foto del usuario: `A5XELTE CPU:
  cpu=0 mpidr=0x100 came alive; SGI1 OK`, `1/1 CPUs initialized, SGI1
  verified`, BSD init, AppleKeyStore, IOWatchdog, IOCoreSurfaceRoot, HID de
  botones/touch, SWEET AMFI (0201) y Sandbox registrados, HFS 627.100.6,
  self-tests de Skywalk `PASSED`, `lo0`/`gif0`/`stf0`, y finalmente
  `Waiting on <dict>IOProviderClass IOMedia, Content Apple_HFS</dict>`.
  Es el punto esperado: no hay driver de almacenamiento todavía.
- **Primer probe UFS** (`0204`, run 39484347438, kernel `d3905386…45f2`,
  `ufsprobe=1`): `cap 0x383ff0f ver 0x200 hcs 0x10f hce 0x1 is 0
  utrlba 0x0:ef002000 dbr 0 rsr 1`, `txprdt 0xc rxprdt 0xc nexus 0xffff`.
  El host quedó vivo tras S-Boot y tomó nuestra lista. TEST UNIT READY en
  LUN 0 (3 veces): `ocs 0x7 upiu 0x21 response 0x1 status 0x2`, o sea un
  UPIU de respuesta real con CHECK CONDITION: el transporte (UTRD con
  offsets en bytes, PRDT, NEXUS, doorbell) funciona. El probe no siguió a
  READ porque abortaba tras el TUR. Siguiente: imprimir el sense, probar
  LUN 0..7 y leer LBA1 aunque falle el TUR.
- **Segundo probe UFS** (run 39486812960, kernel `00f0b0da…deb1`): LUN 0–3
  responden NOT READY (sense key 2, ASC/ASCQ 0/0); LUN 4–7 ILLEGAL REQUEST
  ASC 0x25 (no existen: el S7 tiene 4 LU). READ(10) LBA1 en LUN 0: NOT
  READY. Hipótesis: S-Boot deja el dispositivo en un modo de bajo consumo;
  Linux lo reactiva con START STOP UNIT (ACTIVE) al W-LUN UFS Device
  (`0xD0`). `0204` ahora lo hace antes del primer comando (probe e init).
- **Tercer probe UFS: lectura funcionando** (run 39489906319, kernel
  `869c4c5f…d74a`). W-LUN `0xD0` TUR: NOT READY ASC/ASCQ 04/02
  ("initializing command required"); `START STOP UNIT (ACTIVE) 0`; LUN 0–3
  TEST UNIT READY 0; `READ(10) LBA1 0, signature EFI PART`; GPT de 128
  entradas de 128 bytes en LBA 2; particiones p1 `BOTA0` … p17 `USERDATA`
  (~21,5 GB, bloques de 4 KiB). Primera lectura del UFS desde XNU.
- **Primer root montado desde UFS.** Volumen HFSX de 32 MiB
  (`hfs-test-image`, sha256 `043f1459…8b94`) escrito por adb/TWRP al inicio
  de USERDATA (`/dev/block/sda18`); BOOT con `rd=ufsroot0 ufsroot=USERDATA
  cs_enforcement_disable=1` (mismo kernel `869c4c5f…`); ambos verificados por
  readback. Foto: `"USERDATA" on LUN 0: first block 0x22e0ac, 21566 MiB,
  read-only`, `BSD root: ufsroot0, major 1, minor 0`, `hfs: mounted HEROTEST
  on device b(1, 0)`, `root volume is not authenticated (error 25);
  continuing unsealed`, `devfs_kernel_mount: kernel_mount failed: 2` (la
  imagen no tiene `/dev`), y pánico esperado `exec of /sbin/launchd failed,
  errno 2`. Siguiente: imagen raíz de iOS 16.
- **Root de iOS 16.7.16 (iPhone X, D22AP) en USERDATA.** Workflow `ios-root`
  en el repo privado `wardevsa/ios-herolte-root` (la imagen es software de
  Apple; no se publica): HFSX de 20480 MiB, 3 partes gzip verificadas por
  SHA-256. Escrita con `tools/herolte-flash-root.py` (bloques de 64 MiB con
  readback SHA-256, ceros escritos en el teléfono; un corte del servidor adb
  se retomó con el archivo de estado): 320/320 bloques, imagen sha256
  `2e200c51…13ca8`, firma `HX` en el offset 1024. ADT con `/product` real de
  D22AP (63 propiedades) y kernel `d706ccd8…e271` (incluye 0205). Boot:
  `rd=ufsroot0 ufsroot=USERDATA ufsrw=1 rootrw=1 cs_enforcement_disable=1
  unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1
  wdt=-1 -wdt_no_pmu wdt_test=no_user_checkin`, verificado por readback.
- **Primer arranque de iOS** (log persistente 0206, kernel `012d056a…af10`,
  53 flushes): launchd hace todo el early boot (fsck, mount-phase-1/2,
  keybag, obliteration, launchd_cache_loader con `launchd_unsecure_cache`) y
  llega a `Early boot complete`; lanza SpringBoard, backboardd, keybagd,
  bluetoothd, fairplayd.H2, etc. SpringBoard aborta en loop (24 veces):
  `NSInternalInconsistencyException: We expect a valid bounds for
  configuration ... Main-External ... CADisplay.name = Wireless ... bounds
  {{0,0},{0,0}}` — no había display principal: el `disp0` del ADT decía
  `samsung,herolte-display` y AppleCLCD matchea `samsung,a5xelte-display`.
- **Con el ADT corregido** (boot `67953d9b…ea19`, mismo kernel): `SWEET FB:
  AppleCLCD 1440x2560 stride 5760 at 0xe2a00000 ... presented as 1125x2436`,
  backboardd configura el display (`display on`) y crea superficies
  1440x2560 y 1125x2436; **SpringBoard ya no crashea** (solo CommCenter, sin
  módem, y driverkitd). En ~5 min de log todavía no hay ningún `swap`.
- **Arranque largo (~20 min) con el ADT corregido:** log hasta el flush #37
  (46 KB). backboardd hace `SwapBegin` (sel 4) y sel 52, pero nunca
  `SwapEnd` (sel 5): ningún `SWEET FB: swap`, ninguna registración de
  notificaciones de vsync. El log deja de crecer bastante antes del final.
  `0207`: sampler de hilos (`sweet_sample.c`, SpringBoard/backboardd, a los
  90 s y cada 120 s) y latido `HEROLTE LOG: alive N s` cada 60 s.
- **Muestreos 0207** (kernel con sampler, ~14 min, latido estable cada
  60 s: el sistema no está colgado). Simbolizado contra la dyld cache de la
  imagen: el hilo `IOMFBServer` de backboardd rasteriza con el renderer de
  CPU de QuartzCore (`CA::OGL::SW::scanline`, `vAlpha_Blend` de vImage) y
  reintenta `eagl_init` (busca GPU) en cada `render_for_time`; el hilo
  principal de backboardd queda en `CA::Display::Display::update` (mach_msg)
  y SpringBoard espera en `FBSystemShellInitialize` (check-in síncrono con
  backboardd). Con un solo A53 el primer frame no termina.
- `0208`: SMP en los 4 A53 (MPIDR `0x100..0x103`) por PSCI `CPU_ON`, IPIs
  GICv2 dirigidas por la máscara de ITARGETSR0 de cada núcleo (adaptado de
  los pendientes 0112/0113 del A5). Requiere `cpus=4`.
- **Primer arranque con 4 núcleos** (`cpus=4`, boot `77af8df8…d298`):
  foto `A5XELTE CPU: cpu=1..3 PSCI CPU_ON rc=0 ... came alive; SGI1 OK`,
  `4/4 CPUs initialized, SGI1 verified`; el arranque va notablemente más
  rápido. Log (flush #67, 11 min): el hilo principal de backboardd ya no se
  bloquea; ahora espera en `DMPerformMigration` (XPC síncrono) mientras
  `com.apple.migrationpluginwrapper` aborta (3 veces), y SpringBoard sigue en
  `FBSystemShellInitialize`. CommCenter crashea 45 veces (sin módem). Sin
  `swap` todavía.
- `tools/herolte-tune-ios-root.sh` (en el workflow `ios-root`): como las
  imágenes v12/v13 del donor, `LastSystemVersion` en
  `com.apple.migration.plist`, CommCenter fuera de `launchd.plist`,
  `BKNoWatchdogs` + excepción de watchdog para Setup y el esqueleto
  `/private/var/containers`. `herolte-flash-root.py --diff` reescribe solo
  los bloques de 64 MiB que cambiaron.
- **Root ajustado** (22 bloques reescritos con `--diff`, imagen
  `6e77614c…c156`): sin migración ni CommCenter. **Primer SwapEnd de
  backboardd** (`sel 5`, registro de 1132 bytes), seguido de inmediato por
  `display off` (el driver hace el fundido a negro y no presenta), luego
  `display on`; SpringBoard crea sus primeras IOSurfaces (ids 7–10). Muestreo
  a los ~6 min: SpringBoard en `GSEventRunModal` (UI arrancada), backboardd
  rasterizando en 4 hilos (`CA::OGL::SW::image_sampler`). Siguiente BOOT
  (`a3bce0c3…0758`): `a5fb_keep_on=1 a5fb_noconsole=1 a5fb_perf=1`.
- **0209 (volcado de registros de swap):** los dos primeros swaps son
  frames negros a propósito (`0x104=0x80000007`, todos los ids de superficie
  0, fondo `0x10c=0xff000000`); después no llega ningún swap con contenido.
  backboardd crea sus superficies de render como `0x62336138` ('b3a8', 2
  planos) de 1440x2560 y 1125x2436: la superficie por defecto (panel
  1440x2560) contradice el tamaño presentado (1125x2436).
- **🍎 iOS en pantalla (2026-10-22, boot `a2d97ebe…2239`, `sweetfb_native=1`):**
  con el tamaño real del panel, primero el logo de Apple y después la
  pantalla de configuración "Hola" (bonjour), con hora, batería e indicador
  de inicio de iPhone X. Táctil y botones sin driver todavía.
- `0210`: botones por GPIO ALIVE (`0x10580000`, del DTB del BOOT): Power
  GPA2 pin 4; Vol+/Vol-/Home GPA0 pines 3/4/5, activos en bajo, solo lectura
  con antirrebote (driver del A5 parametrizado). Táctil: `sec_ts` en 0x48 de
  HSI2C `0x14E10000`, IRQ GPA1 pin 0 (pendiente).
- **Botones en el teléfono (boot `8c9c5843…dd25`, 0210):** los cuatro se
  detectan y llegan a iOS (`SWEET HID: key 0xe9/0xea/0x40/0x30`). Power
  bloquea/desbloquea (con `a5fb_keep_on` la imagen queda congelada y al
  despertar vuelve "Hola"); Home y volumen no cambian nada en "Hola"
  (identidad iPhone X: se pasa deslizando hacia arriba).
- `0211`: driver táctil `bsd/dev/herolte_touch.c` (sec_ts en 0x48 de
  HSI2C `0x14E10000`, pines GPF1-0/1 función 2, INT GPA1-0 por sondeo cada
  5 ms; eventos `0x71` de 8 bytes, coordenadas 0..4095 escaladas a
  1440x2560) y `SweetHID` con la geometría del panel del S7. No toca las
  LDO32/33 del PMIC: si no responde, lo deja anotado.
- **0211 en el teléfono:** GPF1 `dat=0` y GPA1 `dat=0` (SDA/SCL e INT en
  bajo), el HSI2C táctil queda en `trans=0x2000c` con el primer byte en la
  FIFO: el sec_ts está sin alimentación después de S-Boot.
- **0212 en el teléfono:** el batcher del PMIC (`0x10550000`) completa el
  envío del registro, pero la lectura queda en `bat_state=0x200` con la FIFO
  RX vacía; no se encendió nada (la verificación de voltaje no llegó a
  correr). `0213`: init del bus como el probe de Linux (reset, master, código
  HS `0x0f`, HS|AUTO), tres formas de lectura (ext de Linux, 7 bits, con STOP)
  y volcado de registros en cada falla.
- **0213/0214:** ambos congelaron el teléfono (0213 tras resetear el HSI2C
  del PMIC; 0214 al arrancar el primer programa del batcher a ~56 s).
- **0215 (sondeo de solo lectura, arrancó bien):** PMU APM
  `config=0x1 status=0x10000 option=0` (M3 alimentado pero retenido, sin
  `ENABLE_APM`), mailbox `0x11240000` todo en 0, SRAM `0x11200000` con una
  tabla de vectores Cortex-M válida (`00004000 0000009d …`): S-Boot deja el
  firmware del APM cargado y el M3 detenido.
- `0216`: alimentación del táctil por el APM, como Linux con la pantalla
  encendida: liberar el M3 (`APM_CONFIG |= 1`, IRQ del mailbox a 0,
  `APM_OPTION |= ENABLE_APM`) y pedirle por mailbox lecturas/escrituras del
  S2MPS16 (LDO32/33). El batcher queda tras `herotouch=2`.
- **🖐 Táctil funcionando (0216, boot `626b33c7…2d1d`):** el APM responde
  por mailbox (S2MPS16 `id=0x2`, `L32CTRL=0x2e`, `L33CTRL=0x3c`;
  escrituras `0xee`/`0xfc` con respuesta `0xa`, readback correcto); con las
  LDO encendidas SDA/SCL suben (`GPF1 dat=0x3`), el sec_ts está en modo app
  (`0x20`, id `ac a5 52`, tx=15 rx=26) y los toques llegan a iOS
  (coordenadas 0..4095 confirmadas). Muy lento y el gesto de deslizar hacia
  arriba no se completa: lecturas I2C con `int=0x400` (NACK) justo después
  de apoyar el dedo pierden los eventos de movimiento.
- `0217`: reintentos de lectura como `sec_ts_i2c_read()` (hasta 10),
  contadores `moves/i2c_retries/lost_reads`; las pausas de 11 s solo con
  `herotouchslow=1` (el táctil queda listo a los pocos segundos).
- **0217 en el teléfono:** `moves=835 i2c_retries=6 lost_reads=0`: los
  movimientos llegan completos; el gesto en "Hola" no avanza porque Setup
  espera CoreTelephony/activación (igual que el donor).
- **🏠 Pantalla de inicio de iOS (2026-10-23):** imagen con Setup marcado
  como completo (imagen `035edb65…d60a`, flasheo diferencial de 155
  bloques): pantalla de inicio y Centro de control con el táctil. Sin apps
  (el staging excluía `/Applications`; dos iconos "Waiting…") y muy lento.
- `STAGE_ALL_APPS=1` en `ios-root` (todas las apps, ~215 MiB) y `0218` +
  ADT `cpu4..7`: los 4 Mongoose (MPIDR `0x0..0x3`) como lógicos 4–7 con
  `cpus=8`.
- **8 núcleos (ADT con `arm,cortex-a53` también en los Mongoose):**
  `8/8 CPUs initialized`; `0219` confirma contador común a los dos clusters
  (lag < 1 ms) y temporizadores que despiertan en los 8 (21 ms sobre 20).
  Los Mongoose reportan `midr=0x531f0011`, `ctr=0x84448005` (línea de
  I-cache de 128 B frente a 64 B en los A53).
- Con todas las apps se quedaba en el logo: `lsd` no terminaba de sembrar la
  base de LaunchServices porque MobileGestalt calculaba el UDID y esperaba
  para siempre `mac-address-bluetooth0` (no estaba en `/chosen`). Con MACs
  sintéticas en el ADT (`02:48:45:52:4f:0x`): **pantalla de inicio con
  iconos** (Fotos, App Store, Ajustes, Teléfono, Safari, Mensajes) y
  Ajustes > Información abre ("iPhone X", iOS 16.7.16, 21 GB).
- **Batería (0220/0221):** el fuel gauge es el del MAX77854 (`0x36` en
  HSI2C_1 `0x13650000`, pines GPD1-2/3; en HSI2C_0 no hay ACK). Lecturas
  reales en el teléfono: 100 %, 4339 mV, +114 mA con cargador; 4043 mV,
  −749 mA sin él; FULLCAP ≈ 2000 mAh. iOS muestra 100 % → 99 %.
- **Botón de encendido (0220):** con `a5fb_keep_on` se acepta el apagado de
  pantalla si llega < 3 s después de pulsar Power; bloquea y apaga la
  pantalla, y vuelve con la pantalla de bloqueo.
- `0222`: `sweetfb_scale=1` experimental, iOS dibuja 960x1706 y el kernel
  escala x1.5 (menos píxeles para el renderizado por CPU).
- **Más RAM (2026-10-23):** el segundo banco de DRAM (`0x880000000`,
  2 GiB, sin `reserved-memory`) para XNU con `mkadt_herolte.py --bank2`.
  Con 2 GiB el kernel ve la memoria pero entra en pánico en
  `kmem_range_init` (no entran sus rangos virtuales). Con
  `--bank2-mb 1024` arranca; los secundarios no llegaban (CPU_ON rc=0 con
  entrada > 4 GiB) y `0223` entra por un trampolín en `0x8fffe000`:
  **1 GiB y 8/8 núcleos**, pantalla de inicio "bastante más fluida, abre
  todo mucho más rápido".
- **0224:** copia a pantalla en paralelo (3 hilos ayudantes): de 10–20 ms a
  ~5 ms por cuadro, hasta 16 fps con movimiento. El log de `kmem` mostró
  por qué 2 GiB no entraba: el kernel (configuración macOS) reserva un
  compresor de 16 × RAM; `vm_compression_limit` lo limita. Con 2 GiB pasa
  ese punto pero entra en pánico en `write_pte_fast` (PTE no alineada,
  páginas de 16 KiB sobre 4 KiB). **Configuración actual: `--bank2
  --bank2-mb 1536`, `vm_compression_limit=98304`, `cpus=8`: 1.5 GiB,
  arranca.**
- **GPU Mali-T880 (0225/0226):** el dominio G3D y los relojes ya vienen
  encendidos de S-Boot; `GPU_ID=0x08802000` (Mali-T880 r2p0), 12 shader
  cores (`0xff0f`), 1 tiler, 1 L2. Con `herogpu=3`: L2, shader core 0 y
  tiler encendidos (ready `0x1`) y **primer trabajo en la GPU**:
  `WRITE_VALUE` en JS1/AS0 con `irq=0x2 mmu=0 exception=1 output=0`, sin
  fallos y sin afectar a iOS.
- **🎨 La GPU dibuja (0227–0230, 2026-10-24):** el descriptor simple de la
  T720 (SFBD) da `DATA_INVALID_FAULT`; con el multi-target de Mesa v5
  (MFBD) y el tiler "desactivado con jerarquía" (máscara 4096, cabecera de
  512 bytes) el fragment job escribe **256/256 píxeles** en memoria
  privada, y con `herogpu=6` **pinta toda la pantalla 1440x2560 de verde
  en 16.6 ms con un solo shader core** (foto del usuario). La copia de iOS
  se pausa 8 s (`sweet_fb_hold_until`) para que se vea.
- **0231:** los **12 shader cores** (`0xff0f`) encienden con la tensión de
  S-Boot y pintan la pantalla entera en **3.26 ms** (5× más que un núcleo),
  estable. `COHERENCY_FEATURES=0`: la GPU no ve las cachés de la CPU, así
  que leer una superficie de iOS exige limpiarla antes (3.8 ms, un núcleo).
  La copia por "preload" del render target da `DATA_INVALID_FAULT`; Mesa
  no usa preload en Midgard (copia con un fragment shader). Copiar con la
  GPU frames dibujados por la CPU no compensa: la aceleración real pasa
  por shaders (primero un triángulo) y, a la larga, por traducir lo que
  pide CoreAnimation/Metal.
- **0232/0233: primer dibujo 3D, un triángulo con shader** (foto del
  usuario). Un tiler job (JS1) guarda el triángulo en la lista de
  polígonos (cabecera 32 palabras, cuerpo 48) y un fragment job (JS0)
  limpia la pantalla y ejecuta un shader Midgard de un bundle, ensamblado
  a mano (`tools/herolte-midgard-shader.py`). Todo en 16 ms. En 0232 no
  se veía nada porque el alpha test fijo de Midgard quedó en NEVER; con
  ALWAYS (0233) el triángulo aparece. Sale casi negro (`02020202`): el
  shader escribe r0 como fp32, y las constantes fp16 1.0 se leen como
  ~0.008. Siguiente: texturas → escalado por GPU (iOS a 720p).
- **0234: primera textura, escalado por GPU** (`herogpu=9`, foto del
  usuario). Una imagen de 180x320 se dibuja como un quad de pantalla
  completa con filtro bilineal: la GPU la agranda 8x a 1440x2560. Sin
  vertex shader, como el preload de Mesa: un varying con coordenadas de
  texel sin normalizar. Shader de 4 bundles ensamblado a mano (ld_vary,
  imov a r28, textura, writeout). Los píxeles leídos coinciden con sus
  texels, y en el borde de un cuadro sale la mezcla del filtro.
  **3.26 ms con los 12 cores** (34 ms con uno). Siguiente: que iOS
  dibuje a 720x1280 y la GPU escale cada frame a la pantalla.
- **0235: iOS en pantalla a través de la GPU** (`sweetfb_scale=1
  sweetfb_gpu=1`). Cada frame de iOS es la textura del quad de 0234:
  ~4.4 ms por frame con la limpieza de caché, sin fallos. Más fluido, pero
  el cuello de botella es CoreAnimation dibujando en la CPU (3-11 fps según
  la pantalla). 960x1706 son 320 puntos @3x, más angosto que cualquier
  iPhone de iOS 16: la interfaz se rompe. 1125x2001 (0236) tampoco sirve:
  UIKit elige la barra de estado por ancho en píxeles y notch.
- **0237:** como syncmodz/ios-s9, el ADT describe un iPhone XR entero
  (`--scale2`: 828x1792 @2x, subtype 1792). La GPU lo dibuja a alto
  completo con barras negras a los lados; el touch se normaliza sobre esa
  franja. Pendiente de probar.
- **0238 (de ios-s9):** los hilos de UI y render van a los Mongoose
  (cpus 4-7) en vez de los A53, y AppleKeyStore responde wrap/unwrap
  (securityd reintentaba sin fin y cada llamada iba al log). Pendiente de
  probar.
- `sweetfb_scale=1` (0222) muestra la imagen rayada y duplicada (las
  superficies a 960x1706 tienen otro formato): queda desactivado.
- Pendiente: nodo `/product` del ADT a partir del IPSW; root (UFS).

## 2026-10-25 — hero2lte (Galaxy S7 Edge SM-G935K, Exynos 8890)

- Dispositivo: SM-G935K (`G935KKKU3EVG2`, `hw_rev=8`), TWRP en `RECOVERY`.
  Backups SHA-256 verificados de `BOOT` (`sda5`, 40 MiB), `EFS` (`sda3`,
  20 MiB) y `RECOVERY` (`sda6`, 46 MiB) fuera de git (`backup/hero2lte/`).
- Mapa de hardware verificado en vivo desde `/proc/device-tree`, `/proc/iomem`
  y `dmesg`: idéntico a `herolte` en CPU (4× A53 `0x100..0x103` + 4× Mongoose
  `0x0..0x3`), GIC-400 (`0x11001000`/`0x11002000`), RAM (`0x80000000+0x7e400000`
  y Bank 2 `0x880000000+2G`), framebuffer `0xe2a00000` (`1440x2560`), DECON_F
  `0x13960000`, UFS 2.0 `0x155a0000` (`KLUBG4G1CE-B0B1`, `USERDATA` `sda18` de
  25.228 MiB), GPIO keys en ALIVE `0x10580000` (Power `GPA2-4`, Vol+/Vol−/Home
  `GPA0-3/4/5`), `sec_ts` en `0x48` de `0x14e10000` (`LDO32=1,85V`,
  `LDO33=3,3V` en `s2mps16@66` vía APM `0x11240000`), fuel gauge `max77854` en
  `0x36` de `0x13650000` y Mali-T880 en `0x14ac0000`.
- **Probe etapa 1 en `hero2lte`** (`probe-boot.img`, espejo en `sec_log`
  `0x91a00000` leído desde `/proc/last_kmsg` tras el reinicio automático a
  TWRP): `EL=1`, `SCTLR=0x30c5183e`, `FDT=0x8a000000`, `IMAGE=0x80104000`,
  `MIDR=0x410fd034`, `MPIDR=0x80000100`, `MMFR0=0x1122` (`16K PAGES: NO`),
  `PFR0=0x2222`, `ISAR0=0x11120`, `CNTFRQ=0`, `TRIG=0x12b0`, `INITRD`
  `0x89000000..0x890dbf7f`.
- **Primer arranque de XNU + UFS + PID 1 en `hero2lte`** (`herolog-1.txt`,
  flush #4, 14.013 bytes): 1,5 GiB Bank 2 (`0x880000000`), `8/8 CPUs
  initialized, SGI1 verified` (temporizadores ~21 ms en los 8 núcleos),
  `SWEET BATTERY: 14% 3381mV -632mA rem=475mAh full=3436mAh`, GPIO keys
  detectados (`key=0`, `key=2`), UFS LUN 0–3 listos + GPT de 18 particiones
  (`p18 USERDATA 1348252+6458468 (25228 MiB)`), `hfs: mounted HEROTEST on
  device b(1, 0)`, `*** SWEET userspace: init running as PID 1 ***`, y
  Mali-T880 (`GPU_ID=0x8802000`, `shader=0xff0f`).
- **Hallazgo APM SRAM tras reset duro de PMIC:** tras un reset de 7 s
  (`MRSTB`), la SRAM del APM (`0x11200000`) pierde el firmware que carga Linux
  (`SRAM 35cac56b 60985ce0…` en vez de `00004000 0000009d…`). `xnu_boot.c`
  ahora recarga los 12.136 bytes del firmware Cortex-M3 en `0x11200000` si no
  está presente, y si `Vol+` (`GPA0-3`) está pulsado al arrancar pide TWRP.
- **0239 compilado en Actions y probado en `hero2lte` (run `40194847651` en
  `biennvops/ios-a5xelte`, kernel `104d70f4…63f9`, `xnu-boot.img`
  `2eaa33fa…b4a7`):**
  - `xnu_boot.c` conmuta el MUIC `MAX77854` (`0x25` en `HSI2C_1` `0x13650000`,
    `CONTROL1 0x0c = 0x49`) a `USB_AP` tras S-Boot.
  - `bsd/dev/sweet_usb.c` activado en `HEROLTE` (`DWC3 0x15400000`, `USBDRD PHY
    0x15500000`, `PMU +0x704 = 1`): **macOS enumera en vivo `Sweet iOS (USB
    Ethernet)` (`en10`, `idVendor=4617`, `idProduct=1`, `status: active`)**
    mientras corre XNU/iOS 16, con streaming UDP del log a `192.168.7.1:5140`
    y comando remoto UDP `"twrp"` en `:5140` para reiniciar a TWRP desde la Mac.
    En `herolog-ios3.txt` (flush #31, 79.687 bytes): `SWEET USB: ue0 rx 116 tx
    198 frames` intercambiados con macOS.
  - `pmap_create_commpages` alineado a `PAGE_RATIO` (`16 KiB`), `sweetfb_stretch=1`
    (estirado a pantalla completa `1440x2560` sin barras negras en `@2x`), y
    combinación física **`Vol+` + `Home` → TWRP** en `a5xelte_keys.c`
    (confirmada en el teléfono sin palillos).
  - `xnu-boot.img` `6c10073a…c3f7`: kernel `0239` (`104d70f4…63f9`) con
    resolución nativa `1440x2560` `@3x` (`sweetfb_native=1`). `en10` configurado
    en `192.168.7.1/24` sin `sudo` mediante `networksetup -setmanual "Sweet iOS
    (USB Ethernet)" 192.168.7.1 255.255.255.0`: `ping 192.168.7.2` responde con
    0 % de pérdida (`1,53 ms` RTT) y el stream UDP en vivo (`192.168.7.1:5140`)
    entrega en tiempo real los eventos `HEROLTE TOUCH:`, `A5XELTE FB:` y `exec`
    mientras iOS 16 corre en el teléfono.
- **Modo 16:9 `@2x` (`--iphone8` en `mkadt_herolte.py`, `23bdd15d…4c1f`):**
  canvas `750x1334` (`375x667` puntos `@2x`, relación 16:9 exacta sin
  estiramiento ni barras negras, `home-button-type = 0` con botón físico Home
  activo y `artwork-device-subtype = 1334`), escalado a `1440x2560` por los 12
  shader cores de la Mali-T880 (`sweetfb_scale=1 sweetfb_lw=750 sweetfb_lh=1334
  sweetfb_gpu=1`) en **`avg_copy_us=3967` (~3,97 ms/cuadro)** con 3,7× menos
  píxeles para el rasterizador de `backboardd`. Reinicio remoto a TWRP por UDP
  (`"twrp"` → `192.168.7.2:5140`) verificado sin tocar el teléfono.
- **Causa raíz del Dock/Widgets blancos y pérdida de blur al terminar la
  animación (`artwork-display-gamut = "sRGB"` en `mkadt_herolte.py`,
  `25027469…6458`):** `d22ap.adt` declaraba `artwork-display-gamut = "P3"`, lo
  que hacía que `SpringBoard` y `backboardd` creasen las superficies en caché
  del Dock (`1368x294`), fondo de pantalla y App Switcher (`1440x2560`,
  `1007x1792`) en formatos P3 de 10 bits (`0x62336138` `'b3a8'` de 2 planos y
  `0x77333072` `'w30r'`), que el rasterizador por software `CA::OGL::SW` no
  soporta. Con `artwork-display-gamut = "sRGB"`, `SWEET SURF:` confirma que
  todas esas superficies (`id 9..13, 16`) se crean como `0x42475241` (`'BGRA'`,
  `planes 0`).
- **0239 actualizado (run `40201034349`, kernel `98962a00…d649`, `xnu-boot.img`
  `dd40910c…0e6b`):** `SweetSurface.cpp` rechaza con `kIOReturnUnsupported` los
  formatos hardware YUV/JPEG/10 bits (`0x34323066` `'420f'`, `0x4a504547`
  `'JPEG'`, `0x62336138` `'b3a8'`, `0x77333072` `'w30r'`) para que `SpringBoard`
  conserve las superficies `'BGRA'` (`0x42475241`) al finalizar las animaciones
  del App Switcher en vez de sustituirlas por buffers `'420f'` vacíos (grises).
  Además, `sweet_usb.c` y `SweetHID.cpp` aceptan `"type <texto>"` por UDP en
  `192.168.7.2:5140` para inyectar pulsaciones de teclado USB HID desde la Mac.
- **Causa de los pánicos intermitentes en reinicios en caliente (`xnu_boot.c`):**
  S-Boot entra a `xnu_boot` con MMU y D-cache apagadas (`SCTLR=0x30c5183e`), de
  modo que `copy_bytes`/`zero_bytes` escriben directo a DRAM; hacer solo
  `dc cvac` sin invalidar antes dejaba líneas sucias/viejas en L2 (de TWRP o del
  arranque previo) sobre `0x8fffe000` (trampolín SMP) y sobre las tablas de
  páginas de bootstrap encima de `topOfKernelData`. `xnu_boot.c` ahora ejecuta
  `dc ivac` sobre `0x8fffe000` y los primeros 64 MiB de la ventana de RAM antes
  de copiar, y `dc civac` + `ic iallu` al terminar.
- **🍎 iOS 16.7.16 arrancado en `hero2lte` (SM-G935K, `xnu-boot.img`
  `b20ed162…145d`, root `72031acb…b86e7`, `herolog-ios1.txt` flush #72,
  112.713 bytes, `alive 720 s`):** 20 GiB HFSX en `USERDATA` (320/320 bloques
  verificados); APM SRAM cargada por `xnu_boot.c` (`00004000 0000009d…`,
  `S2MPS16 id=0x2 L32CTRL=0x2e->0xee L33CTRL=0x3c->0xfc`), `sec_ts`
  (`id ac a5 52`, `tx=15 rx=27`, `moves=1539 i2c_retries=6 lost_reads=0`,
  multitouch 0..2); batería `25% 3836mV +191mA rem=863mAh full=3484mAh
  charging=1`; Mali-T880 presentando cuadros en `avg_copy_us=4462` (hasta
  9,2 fps).
- **Prueba a resolución nativa `1440x2560` `@3x` (`f45b402a…31d5`,
  `sweetfb_native=1`):** pánico diagnóstico `0226` en pantalla (foto del
  usuario, OCR): `HEROLTE: unaligned pmap_enter va=0xfffe37000
  pte_p=0xfffffff824eb31b8 pte=0x4200088032acec3 kernel=0 bt
  0xfffffff00740c68c ... 0xfffffff007414af8 0xfffffff007439be0
  0xfffffff0073d5a7c 0xfffffff007aa8160 @pmap.c:5609`. Simbolizado contra
  `kernel.development.herolte`: `_pmap_enter_pte` ←
  `_pmap_enter_options_internal` ← **`_pmap_create_commpages + 0x1e8`** ←
  `_commpage_populate` ← `_vm_commpage_init` ← `_zone_init`.
- **Causa raíz de `unaligned pmap_enter` (el 75 % de los arranques):** en
  `_pmap_create_commpages`, XNU elige la dirección aleatoria de
  `commpage_text64` desplazando `random()` 12 bits (`4 KiB`, `0xfffe00000 |
  (rand << 12)`), pero `commpage_pmap` usa páginas lógicas de `16 KiB`
  (`write_pte_fast` exige `pte_p` alineado a 32 bytes, es decir `(rand & 3) ==
  0`). En 3 de cada 4 arranques `(rand & 3) != 0` y entra en pánico en
  `pmap_enter_pte`. Con `rand & 0xfc` en `_pmap_create_commpages` (`60978314…19c0`),
  `commpage_text64` queda siempre alineado a 16 KiB y **arranca a la primera en
  `1440x2560` `@3x` nativo (`sweetfb_native=1`)** a pantalla completa, con el
  desenfoque del Centro de control funcionando.
