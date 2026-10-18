// ImportRTTILabels.java - Recover RTTI vtable labels from Itanium ABI typeinfo
// Scans .data.rel.ro for typeinfo structs, finds vtables, and labels them.
//
// Itanium ABI layout:
//   vtable:     points to typeinfo (at vtable[-1] i.e. vtable_ptr - 8)
//   typeinfo:   [vtable_ptr_to_type_info_class, name_ptr]
//   name:       mangled type name string (e.g. "10CCharacter")
//
// We find typeinfo name strings, trace back to typeinfo structs, then find vtables.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;

import java.util.*;

public class ImportRTTILabels extends GhidraScript {

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        SymbolTable symTable = currentProgram.getSymbolTable();
        AddressSpace addrSpace = currentProgram.getAddressFactory().getDefaultAddressSpace();

        // Find .rodata or equivalent section with typeinfo name strings
        // In ELF, typeinfo names are in .rodata, typeinfo structs in .data.rel.ro

        MemoryBlock rodataBlock = null;
        MemoryBlock dataRelRoBlock = null;

        for (MemoryBlock block : mem.getBlocks()) {
            String name = block.getName();
            if (name.equals(".rodata")) rodataBlock = block;
            if (name.equals(".data.rel.ro")) dataRelRoBlock = block;
        }

        if (rodataBlock == null) {
            println("ERROR: .rodata block not found");
            return;
        }

        println("Found .rodata: " + rodataBlock.getStart() + " - " + rodataBlock.getEnd() +
                " (" + rodataBlock.getSize() + " bytes)");

        if (dataRelRoBlock != null) {
            println("Found .data.rel.ro: " + dataRelRoBlock.getStart() + " - " + dataRelRoBlock.getEnd() +
                    " (" + dataRelRoBlock.getSize() + " bytes)");
        }

        // Strategy: scan .data.rel.ro for pointers that point into .rodata
        // where the target looks like a mangled RTTI name (starts with digit)
        // The typeinfo struct is: [ptr_to_type_info_vtable, ptr_to_name_string]
        // So if we find a pointer to a name string, the typeinfo is at (pointer_location - 8)
        // The vtable pointing to this typeinfo has typeinfo_addr at vtable_start - 8

        if (dataRelRoBlock == null) {
            println("ERROR: .data.rel.ro block not found, cannot find typeinfo structs");
            return;
        }

        Address dataStart = dataRelRoBlock.getStart();
        Address dataEnd = dataRelRoBlock.getEnd();
        Address roStart = rodataBlock.getStart();
        Address roEnd = rodataBlock.getEnd();

        int vtablesFound = 0;
        int typeinfoFound = 0;
        int errors = 0;

        // Scan every 8-byte aligned position in .data.rel.ro
        long dataSize = dataRelRoBlock.getSize();
        long scanCount = dataSize / 8;

        println("Scanning " + scanCount + " qwords in .data.rel.ro...");

        // We need to find typeinfo structs. A typeinfo struct has:
        //   offset 0: pointer to __class_type_info vtable (or __si_class_type_info, __vmi_class_type_info)
        //   offset 8: pointer to name string in .rodata
        // The name string starts with a digit (mangled length prefix)

        // So scan for: [any_ptr, rodata_ptr_to_digit_string]
        // Then the typeinfo is at that address - 8 (the name ptr is at +8 in typeinfo)
        // Actually, let me scan for the name pointer at offset +8 by checking ptr at each position

        Map<Long, String> typeinfoToName = new HashMap<>();

        for (long offset = 0; offset + 16 <= dataSize; offset += 8) {
            if (monitor.isCancelled()) break;

            Address checkAddr = dataStart.add(offset);

            try {
                // Read the value at this address (potential pointer to name)
                long val = mem.getLong(checkAddr);

                // Check if it points into .rodata
                if (val < roStart.getOffset() || val > roEnd.getOffset()) continue;

                Address nameAddr = addrSpace.getAddress(val);

                // Check if the target starts with a digit (mangled name)
                byte firstByte = mem.getByte(nameAddr);
                if (firstByte < '0' || firstByte > '9') continue;

                // Read the name string (up to 256 chars)
                byte[] nameBytes = new byte[256];
                int nameLen = 0;
                for (int i = 0; i < 256; i++) {
                    byte b = mem.getByte(nameAddr.add(i));
                    if (b == 0) break;
                    nameBytes[i] = b;
                    nameLen++;
                }
                if (nameLen < 2 || nameLen > 200) continue;

                String mangledName = new String(nameBytes, 0, nameLen, "ASCII");

                // Validate: should be a reasonable mangled name
                // Format: <length><name> possibly repeated for nested names
                if (!isValidMangledName(mangledName)) continue;

                // This looks like a name pointer at offset +8 of a typeinfo struct
                // So the typeinfo starts at checkAddr - 8
                Address typeinfoAddr = checkAddr.subtract(8);

                // Verify the typeinfo is in .data.rel.ro
                if (typeinfoAddr.compareTo(dataStart) < 0) continue;

                // Read the first qword of typeinfo (should be a pointer to type_info class vtable)
                long tiVtablePtr = mem.getLong(typeinfoAddr);
                // It should point somewhere in .data.rel.ro or .rodata
                if (tiVtablePtr < dataStart.getOffset() || tiVtablePtr > dataEnd.getOffset()) continue;

                // Demangle the name
                String className = demangleSimple(mangledName);
                if (className == null || className.isEmpty()) continue;

                typeinfoToName.put(typeinfoAddr.getOffset(), className);
                typeinfoFound++;

                // Label the typeinfo struct
                createOrUpdateLabel(symTable, typeinfoAddr, "typeinfo_" + className);

            } catch (Exception e) {
                errors++;
                continue;
            }
        }

        println("Found " + typeinfoFound + " typeinfo structs, " + errors + " errors during scan");

        // Now find vtables: scan .data.rel.ro for pointers to typeinfo addresses
        // In Itanium ABI, vtable has: [offset_to_top, typeinfo_ptr, vfunc1, vfunc2, ...]
        // So typeinfo_ptr is at vtable_start + 8
        // The "vtable" label conventionally goes at vtable_start + 16 (first vfunc pointer)

        println("Scanning for vtables referencing typeinfo structs...");

        for (long offset = 8; offset + 8 <= dataSize; offset += 8) {
            if (monitor.isCancelled()) break;

            Address checkAddr = dataStart.add(offset);

            try {
                long val = mem.getLong(checkAddr);

                String className = typeinfoToName.get(val);
                if (className == null) continue;

                // This position holds a typeinfo pointer
                // The vtable "start" (offset_to_top) is at checkAddr - 8
                // The conventional vtable label (first vfunc) is at checkAddr + 8
                Address vtableStart = checkAddr.subtract(8);
                Address vtableVfunc = checkAddr.add(8);

                // Verify offset_to_top is 0 or a small value for primary vtable
                long offsetToTop = mem.getLong(vtableStart);

                // Label the vtable
                String vtableName;
                if (offsetToTop == 0) {
                    vtableName = "vtable_" + className;
                } else {
                    vtableName = "vtable_" + className + "_secondary";
                }

                createOrUpdateLabel(symTable, vtableVfunc, vtableName);
                vtablesFound++;

            } catch (Exception e) {
                continue;
            }
        }

        println("=== RTTI Import Complete ===");
        println("Typeinfo structs found: " + typeinfoFound);
        println("Vtable labels created: " + vtablesFound);

        // Now try to rename functions that are virtual calls
        // For each vtable, read the first vfunc pointer and try to name the function
        // We'll name vtable[0] as ClassName::vfunc0, etc.
        // Actually, let's just do the constructor heuristic: if a function stores
        // a vtable pointer, it's likely a constructor
    }

    private boolean isValidMangledName(String name) {
        // Basic validation of Itanium ABI mangled type names
        // They start with a length prefix followed by the name
        int i = 0;
        while (i < name.length() && Character.isDigit(name.charAt(i))) i++;
        if (i == 0 || i > 5) return false; // length prefix 1-5 digits

        int len = Integer.parseInt(name.substring(0, i));
        if (len <= 0 || len > 200) return false;

        // The remaining string after the prefix should be at least as long as 'len'
        // (it could be longer for nested/template names)
        if (name.length() < i + len) return false;

        // Check that the name part contains reasonable characters
        String namePart = name.substring(i, i + len);
        for (char c : namePart.toCharArray()) {
            if (!Character.isLetterOrDigit(c) && c != '_' && c != ':' && c != '<' && c != '>' && c != ',') {
                return false;
            }
        }

        return true;
    }

    private String demangleSimple(String mangled) {
        // Simple demangling: extract the class name from mangled typeinfo name
        // Format examples: "10CCharacter", "10CAIManager", "10CGameState"
        // Nested: "N4NLog9ELogLevelE"

        if (mangled.startsWith("N")) {
            // Nested name - just use the full mangled form as-is for now
            return mangled;
        }

        int i = 0;
        StringBuilder result = new StringBuilder();

        while (i < mangled.length()) {
            if (!Character.isDigit(mangled.charAt(i))) break;

            int numStart = i;
            while (i < mangled.length() && Character.isDigit(mangled.charAt(i))) i++;
            int len = Integer.parseInt(mangled.substring(numStart, i));

            if (i + len > mangled.length()) break;

            if (result.length() > 0) result.append("::");
            result.append(mangled.substring(i, i + len));
            i += len;
        }

        return result.toString();
    }

    private void createOrUpdateLabel(SymbolTable symTable, Address addr, String name) {
        try {
            // Clean up name for Ghidra (no special chars)
            name = name.replace('<', '_').replace('>', '_').replace(',', '_').replace(' ', '_').replace(':', '_');

            Symbol[] existing = symTable.getSymbols(addr);
            boolean hasUserDefined = false;
            for (Symbol s : existing) {
                if (s.getSource() == SourceType.USER_DEFINED) {
                    hasUserDefined = true;
                    break;
                }
            }

            if (!hasUserDefined) {
                symTable.createLabel(addr, name, SourceType.USER_DEFINED);
            }
        } catch (Exception e) {
            // ignore duplicates
        }
    }
}
