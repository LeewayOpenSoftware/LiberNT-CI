<!--
  MODULOS-FORMA.md — Módulos que portan iconos Orosi Forma + análisis de versión GPL
  Soporte para COMPAT-GPL.md. NO ES ASESORÍA LEGAL / NOT LEGAL ADVICE.
-->

# Módulos con iconos Orosi Forma + análisis de versión GPL

Autor: Meredith · 2026-11-23 · **No es asesoría legal.**

Lista autoritativa de rutas Forma: `media/graphics/orosi-forma/forma-owned.txt` en el árbol (la genera el pipeline de Stanley, `t2owned.py`; ~785 rutas de tandas 1+2). Esta tabla se deriva del manifiesto de la tanda 2 (517 iconos, 145 módulos) agrupada por área de primer nivel; el patrón de versión GPL es idéntico para las rutas de tanda 1 (mismos componentes).

## 1. Áreas que portan Forma (conteo tanda 2)

| Área | Nº iconos | Versión de licencia del componente |
|---|---|---|
| `base/applications` | 133 | GPL-2.0-or-later / LGPL |
| `dll/win32` | 127 | GPL-2.0-or-later / LGPL (muchos de Wine = LGPL-2.1+) |
| `dll/cpl` | 120 | GPL-2.0-or-later |
| `dll/shellext` | 83 | GPL-2.0-or-later / LGPL |
| `modules/rosapps` | 20 | GPL-2.0-or-later (**ver §3**: bordes v2) |
| `modules/rostests` | 12 | GPL-2.0-or-later / LGPL |
| `base/shell` | 7 | GPL-2.0-or-later |
| `base/system` | 4 | GPL-2.0-or-later |
| `base/ctf` | 4 | GPL-2.0-or-later |
| `subsystems/linux` | 2 | GPL-2.0-or-later |
| `drivers/wdm` | 2 | GPL-2.0-or-later |
| `base/setup` | 2 | GPL-2.0-or-later |
| `boot/freeldr` | 1 | GPL-2.0-or-later (**ver §3**: bordes v2) |

## 2. Resultado del escaneo de versión GPL

Se escanearon los **114 directorios de componente** que portan Forma (fuente: `.c/.h/.rc/.cpp/.idl`, cabeceras de licencia).

- **Conclusión: ningún componente que porta Forma es uniformemente GPLv2-only.** El código principal de todos declara **«version 2 … or (at your option) any later version»** (GPL-2.0-or-later) o es **LGPL** — ambos elevables a GPLv3, así que los términos **§7(b)/(e) pueden adjuntarse** (ver `COMPAT-GPL.md`).
- Metodología: detección de «or later / any later version / 2.0-or-later / LGPL» por fichero. Los primeros falsos positivos («version 2 of the License,» seguido de «or … later» en la línea siguiente) se verificaron a mano y resultaron ser **or-later**.

## 3. Ficheros de borde a verificar (no bloquean A+C)

Comprobación multilínea estricta → **8 ficheros** mencionan GPL v2 sin «or later» cercano. Son **casos de borde**, no componentes completos:

| Fichero | Tipo | Nota |
|---|---|---|
| `base/applications/charmap/lang/{fr-FR,no-NO,sv-SE}.rc` | .rc de localización | recurso traducido sin cabecera «or later»; el núcleo de charmap es or-later |
| `boot/freeldr/fdebug/lang/{fr-FR,no-NO}.rc` | .rc de localización | ídem; el núcleo de fdebug es or-later |
| `modules/rosapps/templates/old_wordpad/lang/{fr-FR,no-NO}.rc` | .rc de localización (plantilla heredada) | plantilla antigua |
| `modules/rosapps/applications/sysutils/pedump/pedump.c` | herramienta heredada | aviso **académico no-GPL** («written by Sang Cho»), licencia no estándar — verificar aparte |

**Implicación práctica:**
- Los `lang/*.rc` son de otras localizaciones (fr/no/sv). Bajo la política de congelación de idiomas (en-US + es-ES en árbol; el resto por el marco l10n), puede que ni se compilen; aun así, conviene **añadir «or (at your option) any later version»** a sus cabeceras para alinearlos con su módulo, o confirmar que no entran en el binario.
- `pedump` tiene licencia propia no-GPL: si porta un icono Forma, aplicar **solo la Opción A** al asset de ese módulo y **no** adjuntar términos §7 a ese binario (ver `COMPAT-GPL.md`, sección «Módulos GPLv2-only»).

## 4. Regla de aplicación

- **Caso normal (todos los componentes salvo los de §3):** Opción A en el asset + Opción C (§7(b)/(e)) en el binario.
- **Caso de borde (binario que resultara GPLv2-only tras verificación):** **solo Opción A** en el asset; la marca vive como atribución conservada + derecho de marca independiente; **sin** términos §7.
- **Arte de terceros (Adwaita/Papirus/Fluent/oxygen/morewaita/cardlib):** fuera de alcance; conserva su licencia sin cambios.

## 5. Dónde van las referencias

- `LICENSE-FORMA.md` → junto al arte: `media/graphics/orosi-forma/LICENSE-FORMA.md` (y referenciado desde cada carpeta Forma).
- `COMPAT-GPL.md`, `TRADEMARK-OROSI.md` → junto a `LICENSE-FORMA.md`.
- Manifiestos `Icon_Licence/` por módulo: cada entrada Forma lleva
  `license: "Forma (Licencia de arte Forma, basada en CC BY-SA 4.0 + condición de marca Orosi)"`,
  `trademark: "Orosi — Leeway Open Software (debe permanecer visible en derivados)"`,
  `provenance: "Orosi original (Leeway Open Software)"`.
  Los recursos de terceros mantienen su `license`/`provenance` actuales **sin cambios**.
