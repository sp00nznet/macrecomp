import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.*;
import java.util.*;

public class DecompLoader extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outPath = args.length > 0 ? args[0] : "decomp.txt";

        // Entry points into the plaintext loader (stage-1 already applied).
        long[] entries = {0x46, 0x42, 0x162, 0x2e, 0x38, 0x286, 0x2a8, 0x2c4, 0x4ba};
        for (long e : entries) {
            Address a = toAddr(e);
            DisassembleCommand cmd = new DisassembleCommand(a, null, true);
            cmd.applyTo(currentProgram, monitor);
            if (getFunctionAt(a) == null) {
                createFunction(a, "sub_" + Long.toHexString(e));
            }
        }

        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);

        StringBuilder sb = new StringBuilder();
        FunctionManager fm = currentProgram.getFunctionManager();
        int n = 0;
        for (Function f : fm.getFunctions(true)) {
            DecompileResults res = di.decompileFunction(f, 60, new ConsoleTaskMonitor());
            sb.append("/* ==== ").append(f.getName())
              .append(" @ ").append(f.getEntryPoint()).append(" ==== */\n");
            if (res != null && res.getDecompiledFunction() != null) {
                sb.append(res.getDecompiledFunction().getC());
            } else {
                sb.append("// <decompile failed>\n");
            }
            sb.append("\n");
            n++;
        }
        try (PrintWriter w = new PrintWriter(new FileWriter(outPath))) {
            w.print(sb.toString());
        }
        println("Decompiled " + n + " functions -> " + outPath);
    }
}
