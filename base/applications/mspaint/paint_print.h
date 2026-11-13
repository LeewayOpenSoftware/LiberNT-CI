/*
 * PROJECT:    PAINT for ReactOS (Leeway ribbon build)
 * LICENSE:    LGPL-2.0-or-later
 * PURPOSE:    Print, page setup and print preview through the shared LeewayUI print path
 */
#pragma once

void PaintPrintPreview(HWND hwndMain);      /* enter print preview */
void PaintPrintNow(HWND hwndMain);          /* print dialog + print the image */
void PaintPrintPageSetup(HWND hwndMain);
BOOL PaintPrintIsActive(void);
void PaintPrintLayout(HWND hwndMain);       /* keep the frame over the client area */
void PaintPrintSetDPI(UINT dpi);
void PaintPrintThemeChanged(void);
BOOL PaintPrintPreTranslate(MSG *pMsg);
void PaintPrintCleanup(void);
