// Count recovered classes, vtables, and renamed functions from RTTI recovery
// @category Analysis
// @keybinding
// @menupath
// @toolbar

import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.data.*;

public class CountRecoveredClasses extends GhidraScript {

    @Override
    public void run() throws Exception {
        SymbolTable symbolTable = currentProgram.getSymbolTable();
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        FunctionManager funcManager = currentProgram.getFunctionManager();

        // Count namespaces that look like recovered classes
        int classNamespaceCount = 0;
        int vtableCount = 0;
        int vfuncCount = 0;
        int totalFunctions = 0;
        int namedFunctions = 0;

        // Count class namespaces
        SymbolIterator symbols = symbolTable.getAllSymbols(false);
        java.util.Set<String> classNames = new java.util.HashSet<>();
        java.util.Set<String> vtableLabels = new java.util.HashSet<>();

        while (symbols.hasNext()) {
            Symbol sym = symbols.next();
            String name = sym.getName();

            // Count vtable labels
            if (name.startsWith("vtable") || name.contains("_vtable") || name.equals("vftable")) {
                vtableLabels.add(sym.getAddress().toString());
                vtableCount++;
            }

            // Count virtual function labels
            if (name.startsWith("vfunction") || name.contains("::vfunction")) {
                vfuncCount++;
            }

            // Collect class namespace names
            Namespace ns = sym.getParentNamespace();
            if (ns != null && !ns.isGlobal() && ns.getSymbol().getSymbolType() == SymbolType.CLASS) {
                classNames.add(ns.getName());
            }
        }

        // Count functions
        FunctionIterator funcIter = funcManager.getFunctions(true);
        while (funcIter.hasNext()) {
            Function func = funcIter.next();
            totalFunctions++;
            String name = func.getName();
            if (!name.startsWith("FUN_") && !name.startsWith("thunk_FUN_")) {
                namedFunctions++;
            }
        }

        // Count data types in recovered class categories
        int recoveredDataTypes = 0;
        java.util.Iterator<DataType> dtIter = dtm.getAllDataTypes();
        while (dtIter.hasNext()) {
            DataType dt = dtIter.next();
            CategoryPath cp = dt.getCategoryPath();
            if (cp.toString().contains("RTTI") || cp.toString().contains("recovered")) {
                recoveredDataTypes++;
            }
        }

        // Count GhidraClass symbol types
        int ghidraClassCount = 0;
        SymbolIterator allSyms = symbolTable.getAllSymbols(false);
        java.util.Set<String> ghidraClasses = new java.util.HashSet<>();
        while (allSyms.hasNext()) {
            Symbol sym = allSyms.next();
            if (sym.getSymbolType() == SymbolType.CLASS) {
                ghidraClasses.add(sym.getName());
                ghidraClassCount++;
            }
        }

        println("=== RTTI Recovery Results ===");
        println("GhidraClass symbols (unique): " + ghidraClasses.size());
        println("GhidraClass symbols (total):  " + ghidraClassCount);
        println("Class namespaces referenced:   " + classNames.size());
        println("Vtable labels:                 " + vtableCount);
        println("Virtual function labels:       " + vfuncCount);
        println("Total functions:               " + totalFunctions);
        println("Named functions (non-FUN_):    " + namedFunctions);
        println("RTTI/recovered data types:     " + recoveredDataTypes);
        println("============================");
    }
}
