#!/usr/bin/env python3
# Writes the OpenXR API trace (docs/openxr.md, API trace) from the registry: a printer per
# enum and struct, a dispatch over XrStructureType for next chains and base headers, and a
# wrapper per command that prints its arguments, calls the runtime and prints its outputs and
# result. src/oxr_trace.cpp includes the output. Python 3.6, as on the Linux release image.
#
#   gen_trace.py <xr.xml> <output.h>

import sys
import xml.etree.ElementTree as ET

MAX_DEPTH = 6

INTEGERS = {"uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", "size_t", "XrTime", "XrDuration"}
FLOATS = {"float", "double"}


def text(node):
    return node.text or "" if node is not None else ""


class Field:
    """A struct member or a command parameter as the registry writes it."""

    def __init__(self, node):
        type_node = node.find("type")
        name_node = node.find("name")
        self.type = text(type_node)
        self.name = text(name_node)
        prefix = text(node)
        between = type_node.tail or "" if type_node is not None else ""
        after = name_node.tail or "" if name_node is not None else ""
        enum_node = node.find("enum")
        self.const = "const" in prefix
        self.pointers = between.count("*")
        self.const_pointee = "const" in between.split("*")[-1] if self.pointers > 1 else False
        self.array = None
        self.dims = after.count("[")
        if "[" in after:
            self.array = text(enum_node) if enum_node is not None else after.strip()[1:].split("]")[0]
        self.len = [part.strip() for part in node.get("len", "").split(",") if part.strip()]
        self.values = node.get("values")


class Registry:
    def __init__(self, path):
        root = ET.parse(path).getroot()
        self.categories = {}
        self.structs = {}
        self.aliases = {}
        self.external = set()
        self.enums = {}
        self.bitmasks = set()
        for node in (n for block in root.findall("types") for n in block):
            if node.tag != "type":
                continue
            name = node.get("name") or text(node.find("name"))
            category = node.get("category")
            if node.get("alias"):
                self.aliases[name] = node.get("alias")
                continue
            # C's basic types come from openxr_platform_defines; the rest from platform headers
            if node.get("requires") and category is None:
                if node.get("requires") != "openxr_platform_defines":
                    self.external.add(name)
                continue
            self.categories[name] = category
            if category == "struct":
                self.structs[name] = (node, [Field(m) for m in node.findall("member")])
            if category == "bitmask":
                self.bitmasks.add(name)
        for node in root.findall("enums"):
            name = node.get("name")
            if node.get("type") == "enum":
                self.enums[name] = [e.get("name") for e in node.findall("enum") if not e.get("alias")]
        self.commands = {}
        for node in (n for block in root.findall("commands") for n in block):
            if node.get("alias"):
                continue
            proto = node.find("proto")
            self.commands[text(proto.find("name"))] = (text(proto.find("type")), [Field(p) for p in node.findall("param")])
        # Fields name aliases (XrUuidEXT) by the types they stand for
        for _, fields in list(self.structs.values()) + [(None, params) for _, params in self.commands.values()]:
            for field in fields:
                while field.type in self.aliases:
                    field.type = self.aliases[field.type]
        # What a platform header declares stays out: the extension's or the type's protect
        self.protected = set()
        for node in (n for block in root.findall("types") for n in block):
            if node.tag == "type" and node.get("protect"):
                self.protected.add(node.get("name"))
        for ext in root.find("extensions"):
            for require in ext.findall("require"):
                for enum in require.findall("enum"):
                    extends = enum.get("extends")
                    if extends in self.enums and not enum.get("alias") and enum.get("name") not in self.enums[extends]:
                        self.enums[extends].append(enum.get("name"))
                if ext.get("protect") or ext.get("supported") == "disabled":
                    for item in require:
                        if item.tag in ("type", "command"):
                            self.protected.add(item.get("name"))
        for ext in root.find("extensions"):
            if ext.get("supported") == "disabled":
                for require in ext.findall("require"):
                    for enum in require.findall("enum"):
                        extends = enum.get("extends")
                        if extends in self.enums and enum.get("name") in self.enums[extends]:
                            self.enums[extends].remove(enum.get("name"))

    def struct_known(self, name, seen=None):
        if name not in self.structs or name in self.protected:
            return False
        seen = seen or set()
        if name in seen:
            return True
        seen.add(name)
        for field in self.structs[name][1]:
            if field.type in self.external or field.type in self.protected:
                return False
        return True

    def typed(self, name):
        node, fields = self.structs[name]
        return fields and fields[0].name == "type" and fields[0].values


def printer_name(type_name):
    return "trace_struct_" + type_name


def emit_value(reg, out, field, expr, depth, indent):
    """Lines that print the value `expr` of field's type, without pointers or arrays."""
    t = field.type
    category = reg.categories.get(t)
    if t == "XrPath":
        out.append(f"{indent}trace_path(t, {expr});")
    elif t == "XrUuid":
        out.append(f"{indent}trace_uuid(t, ({expr}).data);")
    elif t == "XrBool32":
        out.append(f"{indent}trace_text(t, {expr} ? \"XR_TRUE\" : \"XR_FALSE\");")
    elif t in INTEGERS or t.endswith("_t"):
        out.append(f"{indent}trace_integer(t, (i64)({expr}), {str(t.startswith('u') or t == 'size_t').lower()});")
    elif t in FLOATS:
        out.append(f"{indent}trace_float(t, (f64)({expr}));")
    elif t == "char":
        out.append(f"{indent}trace_integer(t, (i64)({expr}), false);")
    elif category == "handle":
        out.append(f"{indent}trace_handle(t, (u64)(uintptr_t)({expr}));")
    elif t in reg.bitmasks or category == "bitmask":
        out.append(f"{indent}trace_hex(t, (u64)({expr}));")
    elif category == "enum" and t in reg.enums:
        out.append(f"{indent}trace_enum_{t}(t, (i32)({expr}));")
    elif category == "struct" and reg.struct_known(t):
        out.append(f"{indent}{printer_name(t)}(t, &({expr}), {depth} + 1);")
    elif category == "basetype":
        out.append(f"{indent}trace_integer(t, (i64)({expr}), true);")
    elif category == "funcpointer":
        out.append(f"{indent}trace_hex(t, (u64)(uintptr_t)({expr}));")
    else:
        out.append(f"{indent}trace_text(t, \"?\");")


def emit_field(reg, out, field, base, count_expr, depth, indent):
    """Lines that print `base` (the field read from its struct or parameter)."""
    t = field.type
    if field.array and field.dims > 1:
        out.append(f"{indent}trace_text(t, \"[...]\");")
        return
    if field.array:
        if t == "char":
            out.append(f"{indent}trace_string(t, {base});")
        else:
            out.append(f"{indent}trace_text(t, \"[\");")
            out.append(f"{indent}for (u32 i = 0; i < (u32)ARRAY_SIZE({base}) && i < TRACE_MAX_ELEMENTS; i++)")
            out.append(f"{indent}{{")
            out.append(f"{indent}    trace_text(t, i ? \", \" : \"\");")
            emit_value(reg, out, field, f"{base}[i]", depth, indent + "    ")
            out.append(f"{indent}}}")
            out.append(f"{indent}trace_text(t, \"]\");")
        return
    if field.pointers == 0:
        emit_value(reg, out, field, base, depth, indent)
        return
    if t == "void" and field.pointers == 1:
        if field.name == "next":
            out.append(f"{indent}trace_chain(t, {base}, {depth});")
        else:
            out.append(f"{indent}trace_hex(t, (u64)(uintptr_t)({base}));")
        return
    if t == "char" and field.pointers == 1:
        if count_expr and field.len and field.len[0] != "null-terminated":
            out.append(f"{indent}trace_string_n(t, {base}, (usize)({count_expr}));")
        else:
            out.append(f"{indent}trace_string(t, {base});")
        return
    if t == "char" and field.pointers == 2:
        out.append(f"{indent}if (!{base})")
        out.append(f"{indent}{{")
        out.append(f"{indent}    trace_text(t, \"NULL\");")
        out.append(f"{indent}}}")
        out.append(f"{indent}else")
        out.append(f"{indent}{{")
        out.append(f"{indent}    trace_text(t, \"[\");")
        out.append(f"{indent}    for (u64 i = 0; i < (u64)({count_expr or 0}) && i < TRACE_MAX_ELEMENTS; i++)")
        out.append(f"{indent}    {{")
        out.append(f"{indent}        trace_text(t, i ? \", \" : \"\");")
        out.append(f"{indent}        trace_string(t, {base}[i]);")
        out.append(f"{indent}    }}")
        out.append(f"{indent}    trace_more(t, (u64)({count_expr or 0}));")
        out.append(f"{indent}    trace_text(t, \"]\");")
        out.append(f"{indent}}}")
        return
    category = reg.categories.get(t)
    # An output pointer the runtime sets, as xrTriangleMeshGetVertexBufferFB's: where it points
    if field.pointers == 2 and not field.const_pointee and count_expr is None:
        out.append(f"{indent}trace_hex(t, {base} ? (u64)(uintptr_t)*{base} : 0);")
        return
    # An array of base headers is of the derived type the type member names, of unknown size:
    # its count and its first element
    if field.pointers == 1 and count_expr is not None and t.endswith("BaseHeader"):
        out.append(f"{indent}if (!{base} || ({count_expr}) == 0)")
        out.append(f"{indent}{{")
        out.append(f"{indent}    trace_text(t, {base} ? \"[]\" : \"NULL\");")
        out.append(f"{indent}}}")
        out.append(f"{indent}else")
        out.append(f"{indent}{{")
        out.append(f"{indent}    trace_integer(t, (i64)({count_expr}), true);")
        out.append(f"{indent}    trace_text(t, \" of \");")
        out.append(f"{indent}    trace_typed(t, {base}, {depth} + 1);")
        out.append(f"{indent}}}")
        return
    elements = field.pointers == 2 or count_expr is not None
    out.append(f"{indent}if (!{base})")
    out.append(f"{indent}{{")
    out.append(f"{indent}    trace_text(t, \"NULL\");")
    out.append(f"{indent}}}")
    out.append(f"{indent}else if ({depth} >= TRACE_MAX_DEPTH)")
    out.append(f"{indent}{{")
    out.append(f"{indent}    trace_text(t, \"{{...}}\");")
    out.append(f"{indent}}}")
    out.append(f"{indent}else")
    out.append(f"{indent}{{")
    inner = indent + "    "
    if elements and count_expr is not None:
        out.append(f"{inner}trace_text(t, \"[\");")
        out.append(f"{inner}for (u64 i = 0; i < (u64)({count_expr}) && i < TRACE_MAX_ELEMENTS; i++)")
        out.append(f"{inner}{{")
        out.append(f"{inner}    trace_text(t, i ? \", \" : \"\");")
        element = f"{base}[i]"
        if field.pointers == 2:
            emit_pointee(reg, out, field, element, depth, inner + "    ")
        elif category == "struct" and reg.struct_known(t) and reg.typed(t) and t.endswith("BaseHeader"):
            out.append(f"{inner}    trace_typed(t, &{element}, {depth} + 1);")
        else:
            emit_value(reg, out, field, element, depth, inner + "    ")
        out.append(f"{inner}}}")
        out.append(f"{inner}trace_more(t, (u64)({count_expr}));")
        out.append(f"{inner}trace_text(t, \"]\");")
    else:
        emit_pointee(reg, out, field, base, depth, inner)
    out.append(f"{indent}}}")


def emit_pointee(reg, out, field, pointer, depth, indent):
    """Lines that print what one level of `pointer` points at."""
    t = field.type
    category = reg.categories.get(t)
    if category == "struct" and reg.struct_known(t):
        if reg.typed(t):
            out.append(f"{indent}trace_typed(t, {pointer}, {depth} + 1);")
        else:
            out.append(f"{indent}{printer_name(t)}(t, {pointer}, {depth} + 1);")
    elif category == "struct":
        out.append(f"{indent}trace_typed(t, {pointer}, {depth} + 1);")
    else:
        emit_value(reg, out, field, f"*{pointer}", depth, indent)


def count_of(fields, field, prefix):
    names = {f.name for f in fields}
    for part in field.len:
        if part in names:
            return prefix + part
    return None


def generate(reg):
    out = ["// Generated by tools/gen_trace.py from the OpenXR registry; do not edit", ""]
    for enum, values in sorted(reg.enums.items()):
        if enum in reg.protected or reg.categories.get(enum) != "enum":
            continue
        out.append(f"[[maybe_unused]] static void trace_enum_{enum} (TraceOut& t, i32 value)")
        out.append("{")
        if not values:
            out.append("    trace_integer(t, value, false);")
            out.append("}")
            out.append("")
            continue
        out.append("    switch (value)")
        out.append("    {")
        seen = set()
        for value in values:
            if value in seen:
                continue
            seen.add(value)
            out.append(f"    case {value}:")
            out.append(f"        trace_text(t, \"{value}\");")
            out.append("        return;")
        out.append("    default:")
        out.append("        trace_integer(t, value, false);")
        out.append("        return;")
        out.append("    }")
        out.append("}")
        out.append("")

    structs = sorted(name for name in reg.structs if reg.struct_known(name))
    for name in structs:
        out.append(f"[[maybe_unused]] static void {printer_name(name)} (TraceOut& t, const {name}* s, u32 depth);")
    out.append("static void trace_typed (TraceOut& t, const void* base, u32 depth);")
    out.append("")
    for name in structs:
        fields = reg.structs[name][1]
        out.append(f"static void {printer_name(name)} (TraceOut& t, const {name}* s, u32 depth)")
        out.append("{")
        out.append("    (void)depth;")
        out.append("    trace_text(t, \"{\");")
        for i, field in enumerate(fields):
            out.append(f"    trace_text(t, \"{', ' if i else ''}{field.name}=\");")
            emit_field(reg, out, field, f"s->{field.name}", count_of(fields, field, "s->"), "depth", "    ")
        out.append("    trace_text(t, \"}\");")
        out.append("}")
        out.append("")

    typed = [name for name in structs if reg.typed(name) and not name.endswith("BaseHeader")]
    out.append("// A struct by its type member: next chains and arrays of base headers")
    out.append("static void trace_typed (TraceOut& t, const void* base, u32 depth)")
    out.append("{")
    out.append("    const XrBaseInStructure* header = (const XrBaseInStructure*)base;")
    out.append("    switch (header->type)")
    out.append("    {")
    seen = set()
    for name in typed:
        value = reg.structs[name][1][0].values
        if value in seen:
            continue
        seen.add(value)
        out.append(f"    case {value}:")
        out.append(f"        {printer_name(name)}(t, (const {name}*)base, depth);")
        out.append("        return;")
    out.append("    default:")
    out.append("        if (trace_preview(t, base, depth))")
    out.append("        {")
    out.append("            return;")
    out.append("        }")
    out.append("        trace_text(t, \"{type=\");")
    out.append("        trace_enum_XrStructureType(t, (i32)header->type);")
    out.append("        trace_text(t, \", next=\");")
    out.append("        trace_chain(t, header->next, depth);")
    out.append("        trace_text(t, \"}\");")
    out.append("        return;")
    out.append("    }")
    out.append("}")
    out.append("")
    out.append("static void trace_chain (TraceOut& t, const void* next, u32 depth)")
    out.append("{")
    out.append("    if (!next)")
    out.append("    {")
    out.append("        trace_text(t, \"NULL\");")
    out.append("        return;")
    out.append("    }")
    out.append("    if (depth >= TRACE_MAX_DEPTH)")
    out.append("    {")
    out.append("        trace_text(t, \"{...}\");")
    out.append("        return;")
    out.append("    }")
    out.append("    trace_typed(t, next, depth + 1);")
    out.append("}")
    out.append("")

    wrapped = []
    for name, (result, params) in sorted(reg.commands.items()):
        if name in reg.protected or name in ("xrGetInstanceProcAddr", "xrNegotiateLoaderRuntimeInterface", "xrNegotiateLoaderApiLayerInterface"):
            continue
        if result != "XrResult" or any(p.type in reg.external or p.type in reg.protected for p in params):
            continue
        if any(reg.categories.get(p.type) == "struct" and not reg.struct_known(p.type) and not p.type.endswith("BaseHeader") for p in params):
            continue
        wrapped.append(name)
        signature = ", ".join(declare(p) for p in params)
        arguments = ", ".join(p.name for p in params)
        out.append(f"static PFN_{name} trace_real_{name} = nullptr;")
        out.append(f"static XRAPI_ATTR XrResult XRAPI_CALL trace_{name} ({signature})")
        out.append("{")
        out.append("    TraceOut t = {};")
        out.append(f"    trace_text(t, \"trace: {name}(\");")
        inputs = [p for p in params if is_input(p)]
        for i, p in enumerate(inputs):
            out.append(f"    trace_text(t, \"{', ' if i else ''}{p.name}=\");")
            emit_param(reg, out, params, p, "    ")
        out.append(f"    const XrResult result = trace_real_{name}({arguments});")
        out.append("    trace_text(t, \") -> \");")
        out.append("    trace_enum_XrResult(t, (i32)result);")
        outputs = [p for p in params if not is_input(p)]
        if outputs:
            out.append("    if (XR_SUCCEEDED(result))")
            out.append("    {")
            out.append("        trace_text(t, \" {\");")
            for i, p in enumerate(outputs):
                out.append(f"        trace_text(t, \"{', ' if i else ''}{p.name}=\");")
                emit_param(reg, out, params, p, "        ", output=True)
            out.append("        trace_text(t, \"}\");")
            out.append("    }")
        out.append("    trace_end(t);")
        out.append("    return result;")
        out.append("}")
        out.append("")
    out.append("static PFN_xrVoidFunction trace_wrap_generated (const char* name, PFN_xrVoidFunction real)")
    out.append("{")
    for name in wrapped:
        out.append(f"    if (cstr_equal(name, \"{name}\"))")
        out.append("    {")
        out.append(f"        trace_real_{name} = (PFN_{name})real;")
        out.append(f"        return (PFN_xrVoidFunction)trace_{name};")
        out.append("    }")
    out.append("    return nullptr;")
    out.append("}")
    return "\n".join(out) + "\n"


def declare(p):
    pointers = "*" * p.pointers
    if p.pointers == 2 and p.const_pointee:
        pointers = "* const*"
    array = f"[{p.array}]" if p.array else ""
    return f"{'const ' if p.const else ''}{p.type}{pointers} {p.name}{array}"


def is_input(p):
    if p.array:
        return p.const
    return p.pointers == 0 or p.const


def emit_param(reg, out, params, p, indent, output=False):
    count = None
    if p.len:
        names = {q.name: q for q in params}
        for part in p.len:
            if part in names:
                q = names[part]
                if output and q.pointers == 1:
                    capacity = [x for x in params if x.name.endswith("CapacityInput") and x.name[:-13] == q.name[:-11]]
                    count = f"({capacity[0].name} < *{q.name} ? {capacity[0].name} : *{q.name})" if capacity else f"*{q.name}"
                else:
                    count = q.name if q.pointers == 0 else f"*{q.name}"
                break
    if output and p.len and count and p.type == "char":
        out.append(f"{indent}trace_string_n(t, {p.name}, (usize)({count}));")
        return
    emit_field(reg, out, p, p.name, count, "0", indent)


def main():
    reg = Registry(sys.argv[1])
    with open(sys.argv[2], "w", newline="\n") as f:
        f.write(generate(reg))


if __name__ == "__main__":
    main()
