# LiberNT Update etapa 1: implementacion del nucleo (libservicing + libernt-update.exe)

Autora: Meredith, 2026-10-30. Rama `feature/libernt-update-s1-core-meredith` sobre integration-1031 (483daf4db0). Interno de Leeway.
Contrato adoptado: `shared/leeway/libernt-update/S1-CORE.md` (store, journal, plan, attempt, API) y `S1-PENDING.md` de Kevin (aplicador). `S1-CONTRACTS.md` no existia al empezar: no se invento nada fuera de esos dos. Condiciones del dueno: `OWNER-STAGE1-CONDITIONS.md`.

**Estado: COMPILADO en host y PROBADO con tests unitarios en host (Windows 11, wintrust real). NO compilado dentro del build de LiberNT, NO probado en la VM ni en la dx2400.** Los dos informes van separados (secciones 7 y 8).

## 1. Que contiene

| pieza | ubicacion | funcion |
|---|---|---|
| libservicing | `base/system/servicing/libservicing/` | manifest, store, journal, transacciones, verificacion ENFORCE, plan/attempt, target Online/Image |
| libernt-update.exe | `base/applications/cmdutils/libernt-update/` | CLI: init, query, verify, add-package, apply, confirm, rollback, remove-package, pending, export-log |
| scripts de host | `tools/libernt-update/` | `New-DevSigningCert.ps1`, `New-FixturePackage.ps1`, `Build-And-Test.ps1`, certificado publico en `certs/` |
| tests | `base/system/servicing/libservicing/tests/tests.c` | 183 comprobaciones |

CMake propio y desconectado: ningun `CMakeLists.txt` padre incluye estas carpetas (ni hivesys.inf, ni la ISO, ni DISM). `dism.exe` y WIM/FFU no se tocaron.

## 2. Verificacion (ENFORCE, sin modo audit)

`LnServicingVerifyPackage(pkgDir, pinnedCertSha256, &manifest)` en este orden; el primer fallo corta y NO escribe nada en el target ni en el store:

1. `manifest.json` parsea y cumple el esquema v1 (ids `[A-Za-z0-9._-]`, hashes SHA-256 en hex minuscula, `type` en system-component|driver|tool, `targetFamily` LiberNT, sin objetivos duplicados, maximo 4096 items) -> `LN_E_MANIFEST_INVALID`.
2. Reglas de ruta: sin `..`, sin raiz ni unidad, sin `:` (streams), sin componentes vacios/con punto o espacio final; el destino debe estar bajo `Windows\System32\`, `Windows\SysWOW64\`, `Program Files\` o `Program Files (x86)\` y NO puede ser el kernel ni la ruta de arranque (ntoskrnl*, ntkr*, hal*.dll, ntdll.dll, smss.exe, csrss.exe, win32k*.sys, bootvid.dll, kdcom.dll, autochk.exe, `config\`, `boot\`, `LiberNT\`) -> `LN_E_FORBIDDEN_TARGET` (condicion 5: el kernel no se actualiza).
3. Cada payload existe dentro del paquete y su SHA-256 coincide con el manifest -> `LN_E_HASH_MISMATCH`.
4. `catalog.cat`: ausente o sin firma -> `LN_E_UNSIGNED`; firma invalida / digest malo / expirado -> `LN_E_CATALOG_INVALID` (`WinVerifyTrust`, `WTD_CHOICE_FILE`, sin UI, sin revocacion online).
5. Los miembros del catalogo (`CryptCATOpen`/`CryptCATEnumerateMember`) cubren `manifest.json` y cada payload por SHA-256 -> `LN_E_CATALOG_INVALID`.
6. El hash SHA-256 del certificado firmante (de la cadena de `WinVerifyTrust`) debe ser igual a `certificateSha256` del manifest Y al certificado instalado en `TARGET\LiberNT\Servicing\trust\Leeway-LiberNT-Update-DEV.cer` -> `LN_E_CERT_MISMATCH`; sin certificado en el target -> `LN_E_NO_TRUST_ANCHOR`.

**Punto a revisar por el dueno:** el certificado de desarrollo es autofirmado, asi que `WinVerifyTrust` devuelve raiz no confiable (`CERT_E_UNTRUSTEDROOT`/`CERT_E_CHAINING`) despues de comprobar la firma. Ese resultado se acepta SOLO porque el paso 6 ancla el certificado exacto (pinning). Cualquier otro error de confianza rechaza. No se agrega nada al almacen de raices del sistema.

## 3. Store, journal, transacciones

Layout del contrato (`store\objects\<sha>\payload.bin`, `manifests`, `packages`, `staging`, `backup\<tx>`, `journal\transactions.jsonl`, `journal\state.json`, `pending\plan.json`, `pending\attempt.json`, `logs\<tx>.log`, `trust\`). Ademas: `target-id` (identidad estable del target, 64 hex, creada al inicializar) y `backup\<tx>\undo.json` + `files\N.bak`.

- **Journal:** JSONL UTF-8 append-only; cada append abre con `FILE_APPEND_DATA`, escribe la linea entera, `FlushFileBuffers`, cierra. `previousEntrySha256` encadena con el SHA-256 de la linea valida anterior. Una linea cortada por un corte de luz se ignora (`tailTruncated`) y el siguiente append la termina con salto de linea; una linea valida que no continua la cadena (editada o borrada) es `LN_E_JOURNAL_CORRUPT`. `state.json` es una vista derivada, se reconstruye.
- **transactionId:** SHA-256 de `targetIdentity NUL packageId NUL version NUL manifestSha256 NUL operation` (probado contra el calculo manual). Operaciones distintas = ids distintos (`add-package`, `apply`, `remove-package`).
- **add-package:** verifica (ENFORCE), importa objetos por hash (un objeto existente y correcto no se reescribe, el copiado se re-verifica), guarda manifest y catalogo; fase `staged`. Un rechazo queda en el journal como `failed` antes de salir y nada llega al store.
- **apply:** comprueba los objetos del store, toma y VERIFICA el backup de cada destino ANTES del primer cambio (`undo.json`), journal `prepared` -> `applying`; por item: copia del objeto a un temporal junto al destino, verifica hash, `MoveFileEx(REPLACE_EXISTING|WRITE_THROUGH)`, y vuelve a calcular el hash del destino: **nada se da por aplicado sin esa comprobacion final** (`LN_E_VERIFY_FINAL`). Si todo OK -> `applied`. Si un item falla -> `failed` + rollback inmediato con el backup -> `rolled-back`.
- **Online y destino en uso:** si el reemplazo es denegado por bloqueo, el item queda pendiente; se escribe `pending\plan.json` (contrato S1-CORE: `attemptLimit` 2, `healthToken`, `certificateSha256`, acciones `replace|delete` con `oldObjectSha256`/`newObjectSha256`, origen relativo al store) y la fase es `pending-first-boot` con `rebootRequired`. Los items no bloqueados se aplican en el momento. Image nunca difiere: un bloqueo es un error.
- **Idempotencia:** repetir `apply` de lo `applied`/`confirmed` devuelve exito sin tocar el journal; de lo `pending-first-boot` devuelve "reinicio requerido"; de lo `prepared`/`applying` (proceso muerto) reanuda leyendo `undo.json` y saltando lo que ya esta en su sitio; despues de un `rolled-back`/`failed` empieza de nuevo.
- **confirm-health:** vuelve a calcular el hash final de TODOS los destinos; solo si coinciden escribe `confirmed` + `healthConfirmed=true` (y marca `attempt.json`, y borra el plan). Si no coinciden: error y journal `failed`, no `confirmed`.
- **rollback / remove-package:** restaura desde el backup en orden inverso, verifica que cada destino quedo igual al original (o ausente) y escribe `rolled-back`. Funciona aunque los componentes actualizados esten danados: solo usa los `.bak` y este codigo (probado). `remove-package` de un paquete instalado = rollback + quitar del store; los objetos del store son inmutables y no se borran (sin GC en la etapa 1).
- **export-log:** copia journal, state, plan, attempt, target-id y logs a un directorio.

## 4. Regla de arranque (para el aplicador de Kevin)

`LnServicingMarkBootStarted` escribe `started` con flush antes de operar; si encuentra `started` sin desenlace, cuenta un `unexpectedRestartCount` (corte/reset), lo registra como `unexpected-restart` y suma el intento. `LnServicingShouldRollback`: salud confirmada -> no; `verifiedFailureCount > 0` -> revertir ya (fallo verificable); `bootAttemptCount >= 2` sin salud -> revertir (incluye reinicios inesperados, motivo distinguido); si no, seguir. Un reinicio inesperado solo NO revierte de inmediato pero consume el limite. `LnServicingConfirmHealth` es la unica via que escribe `healthConfirmed`; `pending` (CLI) solo lee.

## 5. Certificado de desarrollo y scripts

- `New-DevSigningCert.ps1` crea `CN=Leeway LiberNT Update DEV` (RSA 3072, SHA-256, 5 anos) y guarda la clave privada SOLO en `C:\Users\Nicolas\.leeway-dev-signing\` (PFX + contrasena aleatoria, ACL de solo el usuario); se niega a usar una carpeta dentro del repo y borra el certificado del almacen de Windows tras exportar. Al repo solo va el `.cer` publico (`tools/libernt-update/certs/`, SHA-256 `829459d8db6adc0a4fe7500e0572149e1d469b6a3707cf48e374b7660d754a34`). `.gitignore` bloquea `*.pfx`, `*.p12`, `*.key`, `pfx-password.txt`. No se genero ni publico ninguna clave de distribucion.
- `New-FixturePackage.ps1` arma y firma un paquete (manifest + payload + catalogo v2 de `New-FileCatalog`, firmado con `Set-AuthenticodeSignature`), con interruptores para fixtures negativos.
- `Build-And-Test.ps1` compila con clang de RosBE (llvm-mingw), arma los fixtures y corre los tests; informa COMPILACION y TESTS por separado.

## 6. Dependencias y limites

- APIs de LiberNT que usa: kernel32 (archivos, `MoveFileExW`, `FlushFileBuffers`), wintrust (`WinVerifyTrust`, `WTHelper*`, `CryptCAT*`), crypt32 (por wintrust). SHA-256, JSON y UTF-8 son propios (el aplicador de arranque y la recuperacion no dependen de nada que una actualizacion pueda reemplazar). Sin user32 ni msvcrt locale.
- Sin bloqueo entre procesos: dos `libernt-update` concurrentes sobre el mismo target no estan excluidos (el diseno pide un lock por target; pendiente).
- Rollback de un destino en uso (Online) falla con el error del sistema y deja la transaccion reanudable; el reemplazo en reinicio de un rollback no esta modelado (plan `mode` solo `apply`).
- Sin dependencias entre paquetes, sin CAB/MSU/deltas, sin red, sin kernel/SMSS, sin GC del store, sin firma del plan por separado (el plan queda cubierto por el catalogo del paquete verificado, como dice el contrato; el aplicador de Kevin decide como lo re-verifica en el entorno de arranque).
- `LnTestLockedAsPending` es un gancho de pruebas (simula destino en uso en un target Image); no se usa en produccion.

## 7. Informe de COMPILACION

- Host (clang llvm-mingw x86_64, `-Wall -Wextra`): libservicing (10 archivos), libernt-update.exe y libservicing-tests.exe compilan con 0 avisos. `Build-And-Test.ps1` -> `COMPILATION: OK`.
- Contra los headers de LiberNT (`-fsyntax-only` con `sdk/include`): NO verificado; `psdk/winnt.h` es un header generado por el build y no hay output. Las APIs usadas existen en `dll/win32/wintrust/wintrust.spec` (WinVerifyTrust, WTHelperProvDataFromStateData, WTHelperGetProvSignerFromChain, WTHelperGetProvCertFromChain, CryptCATOpen, CryptCATEnumerateMember, CryptCATClose).
- Build CMake dentro del arbol de LiberNT: NO hecho (esta etapa lo deja desconectado a proposito).

## 8. Informe de PRUEBAS

- Host, Windows 11: `libservicing-tests` -> **183 comprobaciones, 0 fallos**: SHA-256 (vectores NIST), JSON estricto (incl. NUL y profundidad), reglas de ruta y lista de destinos prohibidos, esquema del manifest, transactionId, journal (cadena, linea cortada, edicion y borrado), identidad del target, verificacion con firma real (good, payload alterado, manifest alterado tras firmar, sin catalogo, catalogo sin firma, firmado por otro certificado, destino kernel, payload ausente, sin ancla, ancla distinta), rechazos antes de staging (nada llega al store, quedan en el journal), ciclo completo (add idempotente, apply, reintento idempotente, backup verificado, confirmacion rechazada con componente danado, rollback sobre componente danado, re-apply, confirm, remove-package restaura), fallo a mitad con rollback inmediato y reintento, objeto del store corrupto rechazado antes de cambiar nada, destino bloqueado -> plan.json y reglas de intentos (reinicio inesperado, fallo verificable, salud), export-log.
- Humo del CLI real (`libernt-update.exe`) sobre un Image temporal: init, add-package (bueno y alterado), apply --confirm, query, pending, rollback, export-log, remove-package.
- NO probado: LiberNT (VM) ni dx2400; `WinVerifyTrust` del wintrust de LiberNT sobre un .cat (en el host se uso el de Windows); target Online real; reemplazo de un archivo realmente en uso; corte de luz real durante apply; integracion con `LiberNTUpdatePending.exe` y BootExecute; ejecucion como servicio. Cada uno es una prueba pendiente que no se declara exitosa.
