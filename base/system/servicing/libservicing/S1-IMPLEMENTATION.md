# LiberNT Update etapa 1: implementacion del nucleo (libservicing + libernt-update.exe)

Autora: Meredith, 2026-10-30. Rama `feature/libernt-update-s1-core-meredith` sobre integration-1031 (483daf4db0). Interno de Leeway.
**Contrato unico: `shared/leeway/libernt-update/S1-CONTRACTS.md`** (formatos de plan, journal, attempt, transactionId y estados; incluye la seccion "Reconciliacion con S1-CORE.md"). `S1-CORE.md` (Oscar) queda superado. Condiciones del dueno: `OWNER-STAGE1-CONDITIONS.md`. Revision de seguridad: Pam (B1-B4, N1-N7).

**Estado: compilado y probado en host (Windows 11, wintrust real). No probado en la VM de LiberNT ni en la dx2400.** Los informes de compilacion y de pruebas van separados (secciones 8 y 9).

## 1. Piezas

| pieza | ubicacion | funcion |
|---|---|---|
| libservicing | `base/system/servicing/libservicing/` | manifest, store, journal, transacciones, verificacion ENFORCE, plan/attempt, targets Online/Image, arbol protegido |
| libernt-update.exe | `base/applications/cmdutils/libernt-update/` | CLI: init, query, verify, add-package, apply, confirm, rollback, remove-package, pending, export-log |
| scripts de host | `tools/libernt-update/` | `New-DevSigningCert.ps1`, `New-FixturePackage.ps1`, `Build-And-Test.ps1`, certificado publico en `certs/` |
| tests | `libservicing/tests/tests.c` | 311 comprobaciones (con ganchos `LN_TESTING`, solo en la copia de pruebas de la libreria) |

CMake propio y desconectado: ningun `CMakeLists.txt` padre incluye estas carpetas (ni hivesys.inf, ni la ISO, ni DISM).
El CLI y la libreria necesitan privilegios de administrador (o SYSTEM) para crear/abrir el arbol protegido.

## 2. Modelo de seguridad

El arbol `TARGET\LiberNT\Servicing` (store, ancla de confianza, journal, plan, backups) decide que instala el motor como administrador:
- se crea con DACL **protegido** SYSTEM + Administrators; al abrir un target se exige duenio SYSTEM/Administrators/TrustedInstaller, DACL protegido y solo ACE de permiso para esos principales (tambien en `TARGET\LiberNT`); si no, `LN_E_INSECURE_TARGET` antes de leer nada. Sin ACL (FAT) falla cerrado.
- `add-package` copia el paquete a un staging privado dentro del arbol protegido, verifica SOLO la copia e importa de ella.
- `apply` y `confirm` vuelven a verificar el paquete guardado (secciones 3 y 4).
- `pending` valida el plan contra el paquete firmado (`LnServicingValidatePlan`): digest, transactionId, y cada operacion contra el manifest.
- journal: linea no parseable solo si es la ultima y sin salto de linea; `journal\anchor.json` con `(seq, hash)` de la ultima entrada.

## 3. Verificacion ENFORCE (sin audit), en este orden; el primer fallo corta y no escribe nada en el target
1. `manifest.json`: esquema v1, ids `[A-Za-z0-9._-]` sin nombres de dispositivo, hashes SHA-256, sin destinos duplicados, <= 4096 items -> `LN_E_MANIFEST_INVALID`.
2. Lista blanca de destinos -> `LN_E_FORBIDDEN_TARGET`: DLL/EXE bajo `Windows\System32`, `Windows\SysWOW64`, `Program Files`, `Program Files (x86)`, excepto smss, csrss, wininit, winlogon, services, lsass, ntdll, ci.dll, kernel, HAL, win32k*, bootvid, kdcom, autochk y la ruta de arranque; `.sys` solo bajo `System32\drivers` con `driverStart` 2..4 declarado en el manifest firmado (boot-start y system-start fuera). Sin `..`, raiz, unidad, `:`, componentes vacios o con punto/espacio final, nombres 8.3 (`~` + digito), nombres de dispositivo.
3. Cada payload existe y su SHA-256 coincide con el manifest -> `LN_E_HASH_MISMATCH`.
4. `catalog.cat`: ausente o sin firma -> `LN_E_UNSIGNED`; `WinVerifyTrust` (archivo, sin UI, sin revocacion online) distinto de OK/raiz-no-confiable -> `LN_E_CATALOG_INVALID`.
5. Los miembros del catalogo cubren `manifest.json` y cada payload por SHA-256 -> `LN_E_CATALOG_INVALID`.
6. El hash del certificado firmante == `certificateSha256` del manifest == certificado instalado en `trust\` del target -> `LN_E_CERT_MISMATCH` / `LN_E_NO_TRUST_ANCHOR`.
7. `manifest.sig` (RSA-SHA256 PKCS#1 v1.5, hecho al empaquetar con la clave DEV) verifica sobre `manifest.json` con la clave publica del firmante -> `LN_E_UNSIGNED` (ausente) / `LN_E_CATALOG_INVALID`.

El certificado DEV es autofirmado: `WinVerifyTrust` devuelve `CERT_E_UNTRUSTEDROOT` tras comprobar la firma. Solo ese resultado se acepta, y solo porque el paso 6 ancla el certificado exacto. No se toca el almacen de raices del sistema.

## 4. Transacciones (una transaccion `install` por paquete; contrato S1-CONTRACTS.md)
- **add-package:** stage -> verifica la copia -> `verified` -> importa objetos (`store\objects\<sha256>`), manifest, catalogo y firma -> comprueba que lo guardado es lo verificado -> `staged`. Un rechazo queda como `failed` con su codigo hex.
- **apply:** re-verifica el paquete del store, valida cada destino (sin reparse points, camino real == escrito), toma y verifica el backup de cada destino ANTES del primer cambio (`backup\<tx>\NNNN`, `undo.json`), `prepared` -> `applying`; por item: copia a temporal, hash, reemplazo atomico, hash final, evento `applying` con `op`. Todo bien -> `applied`. Un item falla -> `failed` + `rollback-prepared` (`verifiable-failure`) -> restaurar -> `rolled-back`. Destino en uso (Online): el item pasa al `plan.json` y la fase es `pending-first-boot` (un borrado de archivo en uso no se difiere: falla y revierte). `ACCESS_DENIED` cuenta como "en uso" solo para un archivo escribible y no de solo lectura.
- **confirm:** re-verifica el paquete y recalcula TODOS los hashes finales; solo entonces `confirmed` + `healthConfirmed`. Si falla, la historia no se reescribe: el estado sigue `applied` y se registra `failureRecorded` (salvo plan aun pendiente).
- **rollback / remove-package:** restaura desde el backup verificado, valida el camino real, comprueba el resultado; funciona con los componentes nuevos danados. `remove-package` de un paquete instalado = rollback + quitar del store (los objetos son inmutables y no se borran).
- **idempotencia:** el mismo request reanuda o devuelve el resultado; `prepared/applying` se reanuda desde `undo.json`.

## 5. Certificado de desarrollo y scripts
`New-DevSigningCert.ps1` crea `CN=Leeway LiberNT Update DEV` (RSA 3072, SHA-256, 5 anos); la clave privada solo en `C:\Users\Nicolas\.leeway-dev-signing\` (PFX + contrasena aleatoria, ACL solo del usuario), nunca en el repo ni en las VMs (se niega a usar una carpeta dentro del repo). Al repo solo el `.cer` publico (SHA-256 `829459d8db6adc0a4fe7500e0572149e1d469b6a3707cf48e374b7660d754a34`); `.gitignore` bloquea pfx/key/contrasena. No se genero ni publico clave de distribucion. `New-FixturePackage.ps1` arma y firma paquetes (manifest + `manifest.sig` + payload + catalogo v2 firmado) con interruptores para fixtures negativos.

## 6. Dependencias y limites
- Usa kernel32, advapi32 (seguridad, CryptoAPI), crypt32 y wintrust. SHA-256, JSON y UTF-8 son propios. Sin user32.
- Sin lock entre procesos sobre un mismo target (pendiente). Sin GC del store. El rollback de un archivo en uso no se difiere a reinicio (plan `mode` solo `install`).
- `driverStart` es el declarado por el manifest firmado; no se contrasta con el registro del target (requiere leer el hive).
- Entre la comprobacion del camino real y el reemplazo queda una ventana que solo puede usar quien ya pueda escribir en el target (administrador).
- El aplicador de arranque (Kevin) debe verificar `plan.signature` (firma del manifest) y `package.digest` sin WinVerifyTrust; falta confirmar con Kevin los puntos PROPUESTA 1-6 de S1-CONTRACTS.md.
- Sin dependencias entre paquetes, CAB/MSU/deltas, red, kernel/SMSS.

## 7. Cambios de la revision de Pam
B1 arbol protegido + re-verificacion en apply/confirm/pending; B2 staging privado; B3 lista blanca, 8.3, dispositivos, reparse points y camino real; B4 journal estricto + ancla; N1 solo `CERT_E_UNTRUSTEDROOT`; N3 ACCESS_DENIED acotado; N4 ganchos de prueba solo con `LN_TESTING`; N5 ids sin nombres de dispositivo; N6 plan validado contra el paquete. N2 (prueba negativa del `.cat` con la firma CMS alterada en la VM de LiberNT, y SIP de `.cat` registrado) y N7 (lock, GC, vencimiento del certificado DEV en 5 anos) quedan pendientes.

## 8. Informe de COMPILACION
- **Host** (`tools/libernt-update/Build-And-Test.ps1`, clang llvm-mingw x86_64, `-Wall -Wextra`): libservicing (10 archivos), libernt-update.exe y libservicing-tests.exe (con `-DLN_TESTING`) -> 0 avisos, `COMPILATION: OK`.
- **Contra LiberNT** (headers `sdk/include`, toolchain Clang de RosBE, amd64, `ninja -j1` con los dos `add_subdirectory` aplicados solo en un arbol local, sin commitear): `libservicing`, `libservicing_testlib`, `libernt-update` y `libservicing_tests` compilan y enlazan contra las bibliotecas de importacion de LiberNT. Salio un error real que el host no daba (en los headers de LiberNT `GetNamedSecurityInfoW` recibe `LPWSTR`, no `LPCWSTR`): corregido con un cast. `libernt-update.exe` importa solo advapi32, crypt32, kernel32, msvcrt y wintrust; las pruebas ademas user32. Esto confirma que las APIs usadas existen en LiberNT; NO prueba que se comporten igual.
- Los `CMakeLists.txt` padres no se modificaron en la rama (el nucleo sigue desconectado).

## 9. Informe de PRUEBAS
Host, Windows 11 sin elevar, wintrust real: 311 comprobaciones, 0 fallos (SHA-256, JSON, rutas y lista blanca, esquema del manifest, transactionId, journal incl. ancla, identidad del target, arbol protegido, verificacion con firma real, rechazo antes de staging, ciclo completo, fallo a mitad con rollback, objeto corrupto, re-verificacion en apply/confirm, copia privada ante cambio del origen, junction, plan pendiente y validacion del plan, reglas de intentos, export-log). No probado: VM LiberNT/dx2400, `WinVerifyTrust` de LiberNT sobre un `.cat`, target Online real, archivo realmente en uso, corte de luz real, aplicador de arranque/BootExecute, servicio, ejecucion elevada con el DACL solo SYSTEM+Administrators (los tests aceptan ademas la cuenta que los corre).
