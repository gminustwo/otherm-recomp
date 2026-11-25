// Translated functions emitted resumable at their call sites (ResumableFunctionRewriter): every function
// on a guest thread's call chain at a save-state capture must be listed, or saving is refused. Found with
// OTHERM_STATE_PROBE (standalone_save_state.cpp), which logs "[state] probe needs resumable 0x...".
// save_state.cpp includes this file too.
0x80049578,  // fn_80049578 (thread entry: VIWaitForRetrace)
0x80049620,  // fn_80049620 (thread entry: VIWaitForRetrace)
0x80049678,  // fn_80049678 (thread entry: suspends itself, OSSuspendThread)
0x802019D0,  // fn_802019D0 (thread entry: OSReceiveMessage)
0x802062A0,  // fn_802062A0 (thread entry)
0x80206070,  // fn_80206070 (called by fn_802062A0: OSSleepThread)
0x80006310,  // __start (the boot thread's root)
0x80006CC4,  // fn_80006CC4 (main)
0x8013EE9C,  // fn_8013EE9C (main -> VIWaitForRetrace before game.rel runs)
0x805AF514,  // rel_805AF514 (game.rel, entered from main through the stub fn_80007078)
0x805AF65C,  // rel_805AF65C (game.rel frame loop)
0x805C1654,  // rel_805C1654
0x805C51A0,  // rel_805C51A0
