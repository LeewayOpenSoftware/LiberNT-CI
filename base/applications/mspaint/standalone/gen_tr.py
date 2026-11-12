import re
ES = {
'1 px':'1 px','100 %':'100 %','12 px':'12 px','12.5 %':'12,5 %','16 px':'16 px','2 px':'2 px','200 %':'200 %','25 %':'25 %','3 px':'3 px','4 px':'4 px','400 %':'400 %','5 px':'5 px','50 %':'50 %','8 px':'8 px','800 %':'800 %',
'About Paint':'Acerca de Paint','Airbrush':'Aerógrafo','Background':'Fondo','Background color':'Color de fondo','Black':'Negro','Blue-gray':'Gris azulado','Bold':'Negrita','Brown':'Marrón','Brush':'Pincel','Brushes':'Pinceles',
'Calligraphy 1':'Caligrafía 1','Calligraphy 2':'Caligrafía 2','Centered':'Centrado','Clipboard':'Portapapeles','Color 1':'Color 1','Color 2':'Color 2','Color picker':'Selector de color','Colours':'Colores',
'Copy':'Copiar','Copy to clipboard':'Copiar al Portapapeles','Crop':'Recortar','Crop image':'Recortar imagen','Curve':'Curva','Cut':'Cortar','Cut to clipboard':'Cortar al Portapapeles','Dark red':'Rojo oscuro',
'Delete':'Eliminar','Delete image':'Eliminar imagen','Delete selection':'Eliminar selección','Display':'Mostrar','Edit palette':'Editar colores','Ellipse':'Elipse','Eraser':'Borrador','Exit':'Salir','Exit Paint':'Salir de Paint',
'Fill':'Rellenar','Fill with color':'Rellenar con color','Flip horizontal':'Voltear horizontalmente','Flip vertical':'Voltear verticalmente','Font':'Fuente','Font family':'Tipo de fuente','Font size':'Tamaño de fuente','Foreground color':'Color de primer plano',
'Free form':'Forma libre','Free form selection':'Selección de forma libre','Free-form selection':'Selección de forma libre','From scanner':'Desde escáner','From scanner or camera':'Desde escáner o cámara','Full screen':'Pantalla completa',
'Gold':'Oro','Gray-25%':'Gris 25%','Gray-50%':'Gris 50%','Grayscale':'Escala de grises','Green':'Verde','Gridlines':'Líneas de cuadrícula','Home':'Inicio','Image':'Imagen','Image properties':'Propiedades de la imagen',
'Indigo':'Índigo','Invert colors':'Invertir colores','Invert selection':'Invertir selección','Italic':'Cursiva','JPEG':'JPEG','Lavender':'Lavanda','Light turquoise':'Turquesa claro','Light yellow':'Amarillo claro','Lime':'Lima',
'Line':'Línea','Line width':'Grosor de línea','Magnifier':'Lupa','Modern palette':'Paleta moderna','Monochrome':'Monocromo','New':'Nuevo','New image':'Nueva imagen','No fill':'Sin relleno','No outline':'Sin contorno','Old palette':'Paleta clásica',
'Opaque':'Opaco','Opaque text background':'Fondo de texto opaco','Open':'Abrir','Open image':'Abrir imagen','Orange':'Naranja','Other formats':'Otros formatos','Outline':'Contorno','Page setup':'Configurar página','Paint':'Paint','Palette':'Paleta',
'Paste':'Pegar','Paste from clipboard':'Pegar desde el Portapapeles','Paste from file':'Pegar desde archivo','Paste from...':'Pegar desde...','Pencil':'Lápiz','Polygon':'Polígono','Print':'Imprimir','Print image':'Imprimir imagen',
'Print preview':'Vista previa de impresión','Properties':'Propiedades','Purple':'Púrpura','Rectangle':'Rectángulo','Rectangle selection':'Selección rectangular','Rectangular selection':'Selección rectangular','Red':'Rojo',
'Redo':'Rehacer','Redo action':'Rehacer acción','Resize':'Cambiar tamaño','Resize or skew':'Cambiar tamaño o sesgar','Rose':'Rosa','Rotate':'Girar','Rotate 180\\u00B0':'Girar 180\\u00B0','Rotate image':'Girar imagen',
'Rotate left 90\\u00B0':'Girar 90\\u00B0 a la izquierda','Rotate or flip':'Girar o voltear','Rotate right 90\\u00B0':'Girar 90\\u00B0 a la derecha','Rotate/Flip':'Girar o voltear','Rounded rectangle':'Rectángulo redondeado',
'Save':'Guardar','Save as':'Guardar como','Save as BMP':'Guardar como BMP','Save as GIF':'Guardar como GIF','Save as JPEG':'Guardar como JPEG','Save as PNG':'Guardar como PNG','Save as other format':'Guardar en otro formato',
'Save image':'Guardar imagen','Save image as':'Guardar imagen como','Select':'Seleccionar','Select all':'Seleccionar todo','Select brush':'Seleccionar pincel','Select font':'Seleccionar fuente','Select font size':'Seleccionar tamaño de fuente',
'Select shape':'Seleccionar forma','Select zoom level':'Seleccionar nivel de zoom','Send in e-mail':'Enviar por correo electrónico','Set as background':'Establecer como fondo','Set as desktop background':'Establecer como fondo de escritorio',
'Shape fill':'Relleno de forma','Shape outline':'Contorno de forma','Shapes':'Formas','Show gridlines':'Mostrar líneas de cuadrícula','Show or hide':'Mostrar u ocultar','Show status bar':'Mostrar barra de estado',
'Show thumbnail':'Mostrar miniatura','Size':'Tamaño','Solid color':'Color sólido','Square brush':'Pincel cuadrado','Status bar':'Barra de estado','Stretched':'Expandido','Text':'Texto','Text tool':'Herramienta de texto','Thumbnail':'Miniatura',
'Toggle full screen':'Alternar pantalla completa','Toggle transparency':'Alternar transparencia','Tools':'Herramientas','Transparent':'Transparente','Transparent selection':'Selección transparente','Transparent text background':'Fondo de texto transparente',
'Turquoise':'Turquesa','Underline':'Subrayado','Undo':'Deshacer','Undo action':'Deshacer acción','Vertical':'Vertical','Vertical text':'Texto vertical','View':'Ver','White':'Blanco','Yellow':'Amarillo','Zoom':'Zoom','Zoom in':'Acercar','Zoom level':'Nivel de zoom','Zoom out':'Alejar','Zoom to 100 percent':'Zoom al 100 por ciento',
'File':'Archivo',
}
for i in range(1, 11):
    ES['Custom %d' % i] = 'Personalizado %d' % i
src = open('paint_ribbon_cmds.h', encoding='utf-8').read()
pat = re.compile('L"([^"]*)"')
ens = {x for x in pat.findall(src) if not re.fullmatch('[A-Z0-9]{1,3}', x) and x != 'IsTextTool'}
ens.add('File')
ens = sorted(ens)
missing = [e for e in ens if e not in ES]
assert not missing, missing
BASE = 3000
def cesc(t):
    # C/rc escapes: keep ° as the degree sign, rc source is UTF-8 (#pragma code_page(65001))
    return t.replace('\\u00B0', '°').replace('"', '""')
ids = ['#pragma once', '/* GENERATED by standalone/gen_tr.py: ribbon label string ids (en -> id). */']
tbl = ['/* GENERATED: English literal -> string-table id; the text comes from the .rc string tables. */', 'static const struct { const WCHAR *en; UINT id; } g_ribbonStrings[] = {']
en_rc = ['/* GENERATED: ribbon labels, English */', 'LANGUAGE LANG_ENGLISH, SUBLANG_ENGLISH_US', 'STRINGTABLE', 'BEGIN']
es_rc = ['/* GENERATED: ribbon labels, Spanish */', 'LANGUAGE LANG_SPANISH, SUBLANG_NEUTRAL', 'STRINGTABLE', 'BEGIN']
for i, e in enumerate(ens):
    n = BASE + i
    ids.append('#define IDS_RIB_%d %d' % (i, n))
    tbl.append('    { L"%s", %d },' % (e, n))
    en_rc.append('    %d "%s"' % (n, cesc(e)))
    es_rc.append('    %d "%s"' % (n, cesc(ES[e])))
tbl.append('};')
en_rc.append('END'); es_rc.append('END')
open('paint_ribbon_ids.h', 'w', encoding='utf-8', newline='\n').write('\n'.join(ids) + '\n')
open('paint_ribbon_strings.h', 'w', encoding='utf-8', newline='\n').write('\n'.join(tbl) + '\n')
open('lang/ribbon-en-US.rc', 'w', encoding='utf-8', newline='\n').write('\n'.join(en_rc) + '\n')
open('lang/ribbon-es-ES.rc', 'w', encoding='utf-8', newline='\n').write('\n'.join(es_rc) + '\n')
print(len(ens), 'strings')
