import ghidra.app.script.GhidraScript;
import ghidra.app.emulator.EmulatorHelper;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import java.io.*;
import java.math.BigInteger;
import java.nio.file.*;

public class EmuDecrypt extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        String dir = a[0], outDir = a[1];
        byte[] dec = Files.readAllBytes(Paths.get(dir, "work_test", "CODE0_a5region.bin"));

        Memory mem = currentProgram.getMemory();
        int tx = currentProgram.startTransaction("setup");
        mkblk("a5",   0x50000, 0x8000);
        mkblk("ctx",  0x58000, 0x1000);
        mkblk("code", 0x60000, 0x10000);            // fits the biggest segment
        mkblk("stk",  0x90000, 0x10000);
        mem.setBytes(toAddr(0x50020), dec);         // A5 world (decrypted jump table + handlers)
        putLong(0x58000, 0x60000);                  // *(a3) -> CODE data
        putWord(0x58102, 0x32e6);                   // poly at *(a4+2)
        currentProgram.endTransaction(tx, true);

        int[] segs = {1, 2, 5};                     // encrypted code segments; resID == seg id
        for (int seg : segs) {
            byte[] code = Files.readAllBytes(Paths.get(dir, "work_test", "code", "CODE_" + seg + ".bin"));
            code[2] &= ~0x40;                       // handler's bclr #6,$2(a1)
            int SIZE = code.length;
            int t2 = currentProgram.startTransaction("seg" + seg);
            mem.setBytes(toAddr(0x60000), code);
            putLong(0x9FFFC, SIZE);                 // SizeRsrc result on stack
            currentProgram.endTransaction(t2, true);

            EmulatorHelper emu = new EmulatorHelper(currentProgram);
            emu.writeRegister("A5", 0x50000);
            emu.writeRegister("A3", 0x58000);
            emu.writeRegister("A4", 0x58100);
            emu.writeRegister("D7", seg);           // resource ID
            emu.writeRegister("SP", 0x9FFFC);
            emu.writeRegister("PC", 0x507a6);       // CRC key-gen
            emu.setBreakpoint(toAddr(0x507fa));     // after decrypt loop
            boolean ok = emu.run(monitor);
            byte[] out = new byte[SIZE];
            for (int i = 0; i < SIZE; i++)
                out[i] = (byte) (emu.readMemoryByte(toAddr(0x60000 + i)) & 0xff);
            Files.write(Paths.get(outDir, "CODE_" + seg + ".dec.bin"), out);
            println("CODE " + seg + ": ok=" + ok
                    + " key=" + Long.toHexString(emu.readRegister("D0").longValue() & 0xffff)
                    + " -> CODE_" + seg + ".dec.bin (" + SIZE + "B)");
            emu.dispose();
        }
    }
    private void mkblk(String n, long a, int len) throws Exception {
        currentProgram.getMemory().createInitializedBlock(n, toAddr(a), len, (byte)0,
            monitor, false);
    }
    private void putLong(long a, long v) throws Exception {
        currentProgram.getMemory().setBytes(toAddr(a), new byte[]{
            (byte)(v>>24),(byte)(v>>16),(byte)(v>>8),(byte)v});
    }
    private void putWord(long a, int v) throws Exception {
        currentProgram.getMemory().setBytes(toAddr(a), new byte[]{(byte)(v>>8),(byte)v});
    }
}
