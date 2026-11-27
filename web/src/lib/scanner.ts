// Why the scanner page cannot scan: no printer attached, or one whose scanner
// the firmware does not drive (printer_driver_t.scanner).
export function noScannerMessage(printerConnected: boolean | undefined, model?: string): string {
  return printerConnected
    ? `The ${model || 'printer'}'s scanner is not supported; printing works.`
    : 'No scanner: is the printer on and connected?';
}
