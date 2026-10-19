// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0
// Synthetic two-CPU SW_ALL domain. UID10/20 map through MADT to APIC2/4;
// neither UID nor APIC ID equals its dense CPU index.
DefinitionBlock ("", "DSDT", 2, "R4TEST", "POWER030", 1)
{
    OperationRegion (TCFG,SystemIO,0x6F0,1)
    Field (TCFG,ByteAcc,NoLock,Preserve) {TEST,8}
    OperationRegion (ICFG,SystemIO,0x6F1,1)
    Field (ICFG,ByteAcc,NoLock,Preserve) {IMOD,8}
    Scope (\_SB)
    {
        Device (CPU0)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 10)
            If (LNotEqual(IMOD,0)) {
                Name (_CST, Package () {2,
                    Package () {ResourceTemplate(){Register(FFixedHW,0,0,0,0)},1,1,1000},
                    Package () {ResourceTemplate(){Register(FFixedHW,1,2,0x10,1)},2,20,300}
                })
            }
            If (LOr(LEqual(IMOD,7),LEqual(IMOD,10))) {Name (_CSD,Package(){Package(){6,0,7,0xFC,2,1}})}
            If (LOr(LEqual(IMOD,8),LEqual(IMOD,9))) {
                Name (_LPI,Package(){0,0,1,Package(){100,30,1,1,0,0,
                    ResourceTemplate(){Register(FFixedHW,1,2,0x20,1)},
                    ResourceTemplate(){Register(SystemMemory,0,0,0,0)},
                    ResourceTemplate(){Register(SystemMemory,0,0,0,0)},"idle"}})
            }
            Name (_PSD, Package () { Package () {5,0,7,0xFC,2} })
#ifdef CPPCTEST
            Name (_CPC, Package () {
                23,3,200,150,80,20,
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},
                ResourceTemplate(){Register(SystemIO,8,0,0x702,1)},
                ResourceTemplate(){Register(SystemIO,8,0,0x700,1)},
                ResourceTemplate(){Register(SystemIO,8,0,0x701,1)},
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},0,
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},1,
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},
                ResourceTemplate(){Register(SystemIO,8,0,0x703,1)},100,800,3000
            })
#else
            Name (_PSS, Package () {
                Package () {3000,35000,100,10,3,3},
                Package () {2000,20000,100,10,2,2},
                Package () {1000,10000,100,10,1,1}
            })
            Name (_PCT, Package () {
                ResourceTemplate(){Register(SystemIO,8,0,0x700,1)},
                ResourceTemplate(){Register(SystemIO,8,0,0x700,1)}
            })
            Name (_PPC,Zero)
#endif
        }
        Device (CPU1)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 20)
            If (LAnd(LNotEqual(IMOD,0),LNotEqual(IMOD,1))) {Alias(\_SB.CPU0._CST,_CST)}
            Name (_PSD, Package () { Package () {5,0,7,0xFC,2} })
#ifdef CPPCTEST
            Name (_CPC, Package () {
                23,3,200,150,80,20,
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},
                ResourceTemplate(){Register(SystemIO,8,0,0x712,1)},
                ResourceTemplate(){Register(SystemIO,8,0,0x710,1)},
                ResourceTemplate(){Register(SystemIO,8,0,0x711,1)},
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},0,
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},1,
                ResourceTemplate(){Register(SystemMemory,0,0,0,0)},
                ResourceTemplate(){Register(SystemIO,8,0,0x713,1)},100,800,3000
            })
#else
            Name (_PSS, Package () {
                Package () {3000,35000,100,10,3,3},
                Package () {2000,20000,100,10,2,2},
                Package () {1000,10000,100,10,1,1}
            })
            Name (_PCT, Package () {
                ResourceTemplate(){Register(SystemIO,8,0,0x710,1)},
                ResourceTemplate(){Register(SystemIO,8,0,0x710,1)}
            })
            Name (_PPC,Zero)
#endif
        }
    }
    If (LEqual(IMOD,16)) {
        Scope (\_SB) {
            Device (CPU2) {Name (_HID,"ACPI0007") Name (_UID,10) Alias(\_SB.CPU0._CST,_CST)}
        }
    }
    If (LEqual(TEST,1)) {Store(Zero,Index(DerefOf(Index(\_SB.CPU0._PSD,0)),4))}
    If (LEqual(TEST,2)) {Store(10,\_SB.CPU1._UID)}
    If (LEqual(IMOD,2)) {Store(3,Index(\_SB.CPU0._CST,0))}
    If (LEqual(IMOD,3)) {Store(0,Index(DerefOf(Index(\_SB.CPU0._CST,2)),1))}
    If (LEqual(IMOD,4)) {Store(3,Index(DerefOf(Index(DerefOf(Index(\_SB.CPU0._CST,2)),0)),6))}
    If (LEqual(IMOD,5)) {Store(3,Index(DerefOf(Index(\_SB.CPU0._CST,2)),1))}
    If (LEqual(IMOD,6)) {Store(One,Index(DerefOf(Index(DerefOf(Index(\_SB.CPU0._CST,2)),0)),5))}
    If (LEqual(IMOD,9)) {Store(2,Index(DerefOf(Index(\_SB.CPU0._LPI,3)),2))}
    If (LEqual(IMOD,10)) {Store(5,Index(DerefOf(Index(\_SB.CPU0._CSD,0)),0))}
    If (LEqual(IMOD,11)) {Store(One,Index(DerefOf(Index(DerefOf(Index(\_SB.CPU0._CST,2)),0)),16))}
    If (LEqual(IMOD,12)) {Store(3,Index(DerefOf(Index(DerefOf(Index(\_SB.CPU0._CST,2)),0)),5))}
    If (LEqual(IMOD,13)) {Store(65536,Index(DerefOf(Index(\_SB.CPU0._CST,2)),2))}
    If (LEqual(IMOD,14)) {Store(Zero,Index(DerefOf(Index(DerefOf(Index(\_SB.CPU0._CST,2)),0)),7))}
    Name (MEM0,ResourceTemplate(){Register(SystemMemory,8,0,0x90000700,1)})
    Name (MEM1,ResourceTemplate(){Register(SystemMemory,8,0,0x90000710,1)})
#ifdef CPPCTEST
    If (LEqual(TEST,3)) {Store(22,Index(\_SB.CPU0._CPC,0))}
    Name (BADG,ResourceTemplate(){Register(FFixedHW,8,0,0x199,1)})
    If (LEqual(TEST,4)) {Store(BADG,Index(\_SB.CPU0._CPC,7))}
    Name (MAX0,ResourceTemplate(){Register(SystemMemory,8,0,0x90000701,1)})
    Name (DES0,ResourceTemplate(){Register(SystemMemory,8,0,0x90000702,1)})
    Name (EPP0,ResourceTemplate(){Register(SystemMemory,8,0,0x90000703,1)})
    Name (MAX1,ResourceTemplate(){Register(SystemMemory,8,0,0x90000711,1)})
    Name (DES1,ResourceTemplate(){Register(SystemMemory,8,0,0x90000712,1)})
    Name (EPP1,ResourceTemplate(){Register(SystemMemory,8,0,0x90000713,1)})
    If (LEqual(TEST,6)) {Store(65,Index(DES0,4)) Store(DES0,Index(\_SB.CPU0._CPC,7))}
    If (LEqual(TEST,5)) {
        Store(MEM0,Index(\_SB.CPU0._CPC,8)) Store(MAX0,Index(\_SB.CPU0._CPC,9))
        Store(DES0,Index(\_SB.CPU0._CPC,7)) Store(EPP0,Index(\_SB.CPU0._CPC,19))
        Store(MEM1,Index(\_SB.CPU1._CPC,8)) Store(MAX1,Index(\_SB.CPU1._CPC,9))
        Store(DES1,Index(\_SB.CPU1._CPC,7)) Store(EPP1,Index(\_SB.CPU1._CPC,19))
    }
#else
    If (LEqual(TEST,3)) {Store(4000,Index(DerefOf(Index(\_SB.CPU0._PSS,1)),0))}
    Name (BADG,ResourceTemplate(){Register(FFixedHW,8,0,0x199,1)})
    If (LEqual(TEST,4)) {Store(BADG,Index(\_SB.CPU0._PCT,0))}
    If (LEqual(TEST,5)) {
        Store(MEM0,Index(\_SB.CPU0._PCT,0)) Store(MEM0,Index(\_SB.CPU0._PCT,1))
        Store(MEM1,Index(\_SB.CPU1._PCT,0)) Store(MEM1,Index(\_SB.CPU1._PCT,1))
    }
    Name (STS0,ResourceTemplate(){Register(SystemIO,8,0,0x720,1)})
    Name (STS1,ResourceTemplate(){Register(SystemIO,8,0,0x730,1)})
    If (LEqual(TEST,6)) {Store(STS0,Index(\_SB.CPU0._PCT,1)) Store(STS1,Index(\_SB.CPU1._PCT,1))}
#endif
}
