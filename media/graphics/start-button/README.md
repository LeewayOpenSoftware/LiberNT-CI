# Logo Inicio Orosi neón

`default.png`, `hover.png` y `pressed.png` son imágenes RGBA de 640 × 640 generadas con imagegen desde el tablero Orosi aportado por el usuario.

Los tres `reactos-start-*.svg` conservan sus nombres por compatibilidad y contienen el PNG correspondiente incrustado. No son maestros vectoriales. Usan `viewBox="-4 -4 40 40"`; la imagen ocupa ese mismo rectángulo. El `render.js` existente puede volver a exportarlos con `@resvg/resvg-js`.

El ICO de reserva incluye nueve tamaños: 16, 20, 24, 28, 32, 36, 40, 48 y 64 px, codificados como DIB BGRA con máscaras AND. Las vistas usan un lienzo de logo de 36 px dentro del botón de 48 px.

Los estados se generaron individualmente y pueden presentar pequeñas diferencias de contorno. Deshabilitado y foco reutilizan el recurso normal según el código descrito en la extracción original.

Consultar `../../LEEME.md` para integración y procedencia.
