/*
 * GNU ld's stock PSP script does not KEEP the module metadata and import-stub
 * sections.  They are discovered by psp-fixup-imports and the PSP loader
 * rather than referenced from ordinary C code, so --gc-sections would
 * otherwise discard them.  This augmenting script roots only those ABI
 * sections and leaves normal code/data eligible for section collection.
 */
SECTIONS
{
  .sceStub.text :
  {
    KEEP(*(.sceStub.text))
    KEEP(*(SORT(.sceStub.text.*)))
  }
  .lib.ent.top : { KEEP(*(.lib.ent.top)) }
  .lib.ent : { KEEP(*(.lib.ent)) }
  .lib.ent.btm : { KEEP(*(.lib.ent.btm)) }
  .lib.stub.top : { KEEP(*(.lib.stub.top)) }
  .lib.stub : { KEEP(*(.lib.stub)) }
  .lib.stub.btm : { KEEP(*(.lib.stub.btm)) }
  .rodata.sceModuleInfo : { KEEP(*(.rodata.sceModuleInfo)) }
  .rodata.sceResident : { KEEP(*(.rodata.sceResident)) }
  .rodata.sceNid : { KEEP(*(.rodata.sceNid)) }
  .rodata.sceVstub :
  {
    KEEP(*(.rodata.sceVstub))
    KEEP(*(SORT(.rodata.sceVstub.*)))
  }
}
INSERT AFTER .fini;
