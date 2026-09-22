// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0
// Synthetic hardware only: no address or method is captured from a laptop.
DefinitionBlock ("", "DSDT", 2, "R4TEST", "INPUT035", 1)
{
    Scope (\_SB)
    {
        Device (EC0)
        {
            Name (_HID, EisaId ("PNP0C09"))
            Name (_UID, Zero)
            Name (_GPE, 0x12)
            Name (_GLK, Zero)
            Name (_CRS, ResourceTemplate () {
                IO (Decode16, 0x0062, 0x0062, 1, 1)
                IO (Decode16, 0x0066, 0x0066, 1, 1)
            })
            OperationRegion (ERAM, EmbeddedControl, 0, 0x100)
            Field (ERAM, ByteAcc, NoLock, Preserve) { LDST, 8, TEST, 8 }
            Method (_REG, 2) { }
            Method (_Q01, 0) { Notify (\_SB.LID0, 0x80) }
            Method (_Q02, 0) { Notify (\_SB.VGA0.LCD0, 0x86) }
            Method (_Q03, 0) { Notify (\_SB.VGA0.LCD0, 0x87) }
            Method (_Q04, 0) { Notify (\_SB.LID0, 0x86) }
            Method (_Q05, 0) { Store (0xA5, TEST) }
        }
        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_LID, 0) { Return (\_SB.EC0.LDST) }
        }
        Device (VGA0)
        {
            Name (_HID, "RFT0001")
            Name (MODE, One)
            Method (_DOS, 1) { Store (Arg0, MODE) }
            Method (_DOD, 0) { Return (Package () { 0x0110 }) }
            Device (LCD0)
            {
                Name (_ADR, 0x0110)
                Method (_BCL, 0) { Return (Package () { 100, 50, 10, 50, 100 }) }
                Method (_BCM, 1) { Fatal (1, 0, Arg0) } // must NEVER be invoked
            }
        }
    }
    Scope (\_GPE)
    {
        Method (_L11, 0) { Notify (\_SB.VGA0.LCD0, 0x86) }
    }
}
