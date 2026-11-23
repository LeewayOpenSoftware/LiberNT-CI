<!--
  COMPAT-GPL.md — Orosi Forma inside GPL LiberNT binaries
  Opción C elegida por el propietario: términos adicionales GPLv3 §7(b)/(e).
  Complementa LICENSE-FORMA.md (opción A, Licencia de arte Forma basada en CC BY-SA 4.0 + marca).
  NO ES ASESORÍA LEGAL / NOT LEGAL ADVICE.
-->

# Compatibilidad GPL de los iconos Orosi Forma · GPL compatibility of the Orosi Forma icons

> **⚠️ No es asesoría legal / Not legal advice.** La validación jurídica corresponde al propietario o a su abogado.

---

## 🇪🇸 Español

Los iconos Orosi Forma se incrustan como **recursos** (ICON/BITMAP/RCDATA) dentro de binarios de LiberNT que son, en su gran mayoría, **GPL-2.0-or-later** o **LGPL** (ver análisis por módulo en `MODULOS-FORMA.md`). La GPL prohíbe imponer «restricciones adicionales» (*further restrictions*, GPLv2 §6 / GPLv3 §10) sobre los derechos que concede. Para que la exigencia de **mantener visible la marca «Orosi»** (LICENSE-FORMA.md, cláusula 3) **no** sea una restricción incompatible, se usa el **artículo 7 de la GPLv3**, que **permite expresamente** ciertos términos adicionales:

- **GPLv3 §7(b)** — permite exigir «la conservación de avisos de atribución razonables» (atribución de autor/origen). La obligación de conservar visible la atribución **«Orosi Forma © Leeway Open Software»** cabe aquí.
- **GPLv3 §7(e)** — permite «declinar conceder derechos bajo la ley de marcas para el uso de algunos nombres comerciales, marcas o marcas de servicio». Esto permite **reservar la marca «Orosi»** sin romper la GPLv3.

Como los términos del §7 son **permitidos** por la propia licencia, **no cuentan como *further restrictions*** y el binario combinado **sigue siendo redistribuible** bajo GPLv3. Esto funciona porque los componentes de LiberNT son **GPL-2.0-or-later**: la cláusula «o posterior» permite elevarlos a GPLv3 y, por tanto, acogerse al §7.

### Términos adicionales declarados (texto para el aviso del binario)

> *Additional terms under GNU GPL version 3, section 7, apply to the Orosi Forma icon resources embedded in this binary:*
> *(b) You must preserve the author/origin attribution "Orosi Forma © Leeway Open Software" as described in LICENSE-FORMA.md.*
> *(e) The names "Orosi" and "Leeway Open Software" are trademarks of Leeway Open Software; no trademark rights are granted, and their visible use as origin identification must be preserved per LICENSE-FORMA.md and TRADEMARK-OROSI.md.*
> *These additional terms apply only to the Forma icon resources, not to the rest of the program.*

### Lo que hay que EVITAR (sería *further restriction* prohibida)

- Redactar la condición como **«no se puede modificar»** el icono o **«solo usos aprobados»** → restringe derechos GPL y rompe la compatibilidad.
- Exigir la marca visible sobre **todo el binario o toda la UI**, no solo sobre los recursos que contienen Forma → se saldría del asset y tocaría la obra GPL completa.
- Imponer obligaciones de notificación, registro o campo-de-uso sobre el software.

### Doble vía (recomendada)

- El **arte Forma suelto** (ficheros `.ico/.svg/.png` distribuidos como assets) → **Opción A** (`LICENSE-FORMA.md`, Licencia de arte Forma basada en CC BY-SA 4.0 + marca).
- El **mismo arte incrustado** en un binario GPL → **Opción C** (estos términos §7(b)/(e) a nivel del binario).

### Advertencia de compatibilidad CC → GPL (nota de Dwight)

Creative Commons publica una compatibilidad **unidireccional** de **CC BY-SA 4.0 → GPLv3**, pero esa compatibilidad se refiere a la licencia **CC BY-SA 4.0 sin modificar**. Como la Licencia de arte Forma **añade** una condición de marca, **no** debe asumirse que esa compatibilidad CC se traslade automáticamente a la concesión modificada. Por tanto, la base jurídica para **incrustar** el arte Forma dentro de binarios GPL **descansa en la Opción C** (términos adicionales GPLv3 §7(b)/(e)), **no** en el mecanismo de compatibilidad CC→GPL. El arte Forma **suelto** (no combinado en el binario) se distribuye bajo la Opción A como asset independiente, donde esta cuestión no se plantea.

### Módulos GPLv2-only (sin «or later»)

Si algún binario resultara ser **GPLv2-only** (sin «or later»), **no** puede elevarse a GPLv3 y, por tanto, **no** se le pueden adjuntar los términos §7(b)/(e). Para esos casos: **solo aplica la Opción A al asset suelto** (Licencia de arte Forma basada en CC BY-SA 4.0 + marca), y la marca se mantiene como **atribución** (conservar avisos de copyright/atribución **sí** es compatible con GPLv2, porque la propia GPL exige conservarlos) y como **derecho de marca independiente** de la licencia de copyright. **No** se añade ningún término §7 a ese binario.

Resultado del escaneo (ver `MODULOS-FORMA.md`): **ningún componente que porta Forma es uniformemente GPLv2-only**; el código principal es GPL-2.0-or-later o LGPL. Solo unos pocos ficheros de borde (algunos `lang/*.rc` de otras localizaciones y la herramienta heredada `pedump.c` con aviso académico no-GPL) deben verificarse por fichero antes de confiar en el §7 para esos binarios concretos.

---

## 🇬🇧 English

The Orosi Forma icons are embedded as **resources** (ICON/BITMAP/RCDATA) inside LiberNT binaries that are overwhelmingly **GPL-2.0-or-later** or **LGPL** (see per-module analysis in `MODULOS-FORMA.md`). The GPL forbids imposing "further restrictions" (GPLv2 §6 / GPLv3 §10) on the rights it grants. So that the requirement to **keep the "Orosi" trademark visible** (LICENSE-FORMA.md, clause 3) is **not** an incompatible restriction, we rely on **section 7 of GPLv3**, which **expressly permits** certain additional terms:

- **GPLv3 §7(b)** — allows requiring "preservation of specified reasonable legal notices or author attributions". The obligation to keep the attribution **"Orosi Forma © Leeway Open Software"** visible fits here.
- **GPLv3 §7(e)** — allows "declining to grant rights under trademark law for use of some trade names, trademarks, or service marks". This lets us **reserve the "Orosi" mark** without breaking GPLv3.

Because §7 terms are **permitted** by the licence itself, they **do not count as further restrictions**, and the combined binary **remains redistributable** under GPLv3. This works because LiberNT components are **GPL-2.0-or-later**: the "or later" clause allows elevation to GPLv3 and hence reliance on §7.

### Declared additional terms (notice text for the binary)

> *Additional terms under GNU GPL version 3, section 7, apply to the Orosi Forma icon resources embedded in this binary:*
> *(b) You must preserve the author/origin attribution "Orosi Forma © Leeway Open Software" as described in LICENSE-FORMA.md.*
> *(e) The names "Orosi" and "Leeway Open Software" are trademarks of Leeway Open Software; no trademark rights are granted, and their visible use as origin identification must be preserved per LICENSE-FORMA.md and TRADEMARK-OROSI.md.*
> *These additional terms apply only to the Forma icon resources, not to the rest of the program.*

### What to AVOID (would be a prohibited further restriction)

- Phrasing the condition as **"no modifying"** the icon or **"approved uses only"** → restricts GPL rights and breaks compatibility.
- Requiring the mark visible across the **whole binary or whole UI**, not only on Forma-containing resources → steps outside the asset onto the full GPL work.
- Imposing notification, registration, or field-of-use obligations on the software.

### Dual track (recommended)

- The **loose Forma art** (`.ico/.svg/.png` distributed as assets) → **Option A** (`LICENSE-FORMA.md`, Forma Art License based on CC BY-SA 4.0 + trademark).
- The **same art embedded** in a GPL binary → **Option C** (these §7(b)/(e) terms at the binary level).

### CC → GPL compatibility caveat (Dwight's note)

Creative Commons publishes a **one-way** compatibility from **CC BY-SA 4.0 → GPLv3**, but that compatibility concerns the **unmodified** CC BY-SA 4.0 licence. Because the Forma Art License **adds** a trademark condition, that CC compatibility must **not** be assumed to carry over automatically to the modified grant. Therefore, the legal basis for **embedding** Forma art inside GPL binaries **rests on Option C** (GPLv3 §7(b)/(e) additional terms), **not** on the CC→GPL compatibility mechanism. **Loose** Forma art (not combined into the binary) is distributed under Option A as a standalone asset, where this question does not arise.

### GPLv2-only modules (no "or later")

If a binary turns out to be **GPLv2-only** (no "or later"), it **cannot** be elevated to GPLv3, so the §7(b)/(e) terms **cannot** be attached. For those: **only Option A applies to the loose asset** (Forma Art License based on CC BY-SA 4.0 + trademark), and the mark is kept as **attribution** (preserving copyright/attribution notices **is** GPLv2-compatible, since the GPL itself requires keeping them) and as a **trademark right independent** of the copyright licence. **No** §7 term is added to that binary.

Scan result (see `MODULOS-FORMA.md`): **no Forma-carrying component is uniformly GPLv2-only**; the main code is GPL-2.0-or-later or LGPL. Only a few edge files (some other-locale `lang/*.rc` and the legacy `pedump.c` tool with a non-GPL academic notice) should be verified per file before relying on §7 for those specific binaries.
