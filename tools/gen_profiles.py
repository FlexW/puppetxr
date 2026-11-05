#!/usr/bin/env python3
#
# Generates src/oxr_profiles.inc, the table of every interaction profile in the
# OpenXR registry with its binding paths and when each is available.
#
#   python3 tools/gen_profiles.py <xr.xml> src/oxr_profiles.inc
#
# Availability is an OR of ANDs of features (XR_VERSION_* or extension names),
# computed the way the registry defines it: <interaction_profile> includes in
# features and extensions, <extend interaction_profile_path>, predicate extends
# for components and user paths, require-level depends= and extension-level
# depends= chained transitively.

import sys
import xml.etree.ElementTree as et

DEFAULT_VERSION = "XR_VERSION_1_0"


def simplify(terms):
    out = []
    for term in sorted(set(terms), key=lambda t: (len(t), sorted(t))):
        if not any(kept <= term for kept in out):
            out.append(term)
    return out


# Mutable on purpose: a profile's own user paths and inline components share
# its Avail object, so another feature including the profile widens them all.
class Avail:
    def __init__(self, terms):
        self.terms = simplify(terms)

    def merge(self, other):
        self.terms = simplify(self.terms + other.terms)

    def anded(self, other):
        return Avail([x | y for x in self.terms for y in other.terms])

    def copy(self):
        return Avail(self.terms)


def clean_versions(avail):
    out = []
    for term in avail.terms:
        versions = sorted(f for f in term if f.startswith("XR_VERSION_"))
        term = set(term)
        if len(versions) == 1 and versions[0] == DEFAULT_VERSION and len(term) > 1:
            term.discard(DEFAULT_VERSION)
        term.difference_update(versions[:-1])
        out.append(frozenset(term))
    return Avail(out)


def parse_depends(text):
    if not text:
        return Avail([frozenset([DEFAULT_VERSION])])
    terms = []
    for alternative in text.split(","):
        term = {f.strip() for f in alternative.split("+")}
        if not any(f.startswith("XR_VERSION_") for f in term):
            term.add(DEFAULT_VERSION)
        terms.append(frozenset(term))
    return Avail(terms)


class Component:
    def __init__(self, user_paths, subpath, type, system, avail, integral):
        self.user_paths = user_paths
        self.subpath    = subpath
        self.type       = type
        self.system     = system
        self.avail      = avail
        self.integral   = integral


class Profile:
    def __init__(self, name, user_paths, avail):
        self.name       = name
        self.user_paths = user_paths
        self.avail      = avail
        self.components = {}

    def add_component(self, subpath, type, avail, integral, system, user_path):
        existing = self.components.get(subpath)
        if existing:
            assert existing.type == type and existing.system == system, f"{self.name}{subpath}: conflicting definitions"
            existing.avail.merge(avail)
            return
        user_paths = {user_path: avail} if user_path else self.user_paths
        self.components[subpath] = Component(user_paths, subpath, type, system, avail, integral)

    def add_user_path(self, path, avail, inherit):
        combined = avail.anded(self.avail)
        if path in self.user_paths:
            self.user_paths[path].merge(combined)
        else:
            self.user_paths[path] = combined
        if not inherit:
            return
        for component in self.components.values():
            inherited = component.user_paths.get(inherit)
            if inherited is None:
                continue
            if path in component.user_paths:
                component.user_paths[path].merge(combined.anded(inherited))
            else:
                component.user_paths[path] = combined.anded(inherited)


class Processor:
    def __init__(self, root):
        self.root       = root
        self.profiles   = {}
        self.ext_deps   = {}
        self.interfaces = []

    def chained(self, avail):
        out = Avail([])
        for term in avail.terms:
            result = Avail([term])
            for feature in term:
                deps = self.ext_deps.get(feature)
                if deps is not None:
                    result = result.anded(self.chained(deps))
            out.merge(result)
        return out

    def require_avail(self, interface, require):
        avail = Avail([frozenset([interface.get("name")])])
        if require.get("depends"):
            avail = avail.anded(parse_depends(require.get("depends")))
        return self.chained(clean_versions(avail))

    def add_components(self, profile, parent, avail, integral=False):
        for c in parent.findall("./component"):
            profile.add_component(c.get("subpath"), c.get("type"), avail, integral, c.get("system") is not None, c.get("user_path"))

    def include(self, name, avail):
        profile = self.profiles.get(name)
        if profile:
            profile.avail.merge(avail)
            for component in profile.components.values():
                if component.integral:
                    component.avail.merge(avail)
            return
        avail      = avail.copy()
        element    = self.root.find(f"./interaction_profiles/interaction_profile[@name='{name}']")
        user_paths = {u.get("path"): avail for u in element.findall("./user_path")}
        profile    = Profile(name, user_paths, avail)
        self.profiles[name] = profile
        self.add_components(profile, element, avail, integral=True)

    def matches(self, condition, profile):
        for u in condition.findall("./user_path"):
            if u.get("path") not in profile.user_paths:
                return False
        for c in condition.findall("./component"):
            component = profile.components.get(c.get("subpath"))
            if not component or component.type != c.get("type"):
                return False
        return True

    def predicate_extends(self, extender):
        for interface in self.interfaces:
            for require in interface.findall("./require[extend]"):
                avail = self.require_avail(interface, require)
                for extend in require.findall("./extend[@interaction_profile_predicate]"):
                    conditions = extend.findall("./condition")
                    for profile in self.profiles.values():
                        if any(self.matches(c, profile) for c in conditions):
                            extender(profile, extend, avail)

    def run(self):
        interfaces = self.root.findall("./feature")
        for ext in self.root.findall("./extensions/extension"):
            if "openxr" in ext.get("supported", "").split(","):
                interfaces.append(ext)
        for interface in interfaces:
            for require in interface.findall("./require[interaction_profile]"):
                avail = self.require_avail(interface, require)
                for include in require.findall("./interaction_profile"):
                    self.include(include.get("name"), avail)
            if interface.tag == "extension" and interface.get("depends"):
                self.ext_deps[interface.get("name")] = parse_depends(interface.get("depends"))
            self.interfaces.append(interface)

        for interface in self.interfaces:
            for require in interface.findall("./require[extend]"):
                avail = self.require_avail(interface, require)
                for extend in require.findall("./extend[@interaction_profile_path]"):
                    self.add_components(self.profiles[extend.get("interaction_profile_path")], extend, avail)
        self.predicate_extends(lambda p, e, a: self.add_components(p, e, a))

        def add_user_paths(profile, extend, avail):
            for u in extend.findall("./user_path"):
                profile.add_user_path(u.get("path"), avail, u.get("inherit"))
        self.predicate_extends(add_user_paths)


def main():
    root = et.parse(sys.argv[1]).getroot()
    proc = Processor(root)
    proc.run()

    profiles = []
    for name in sorted(proc.profiles):
        profile  = proc.profiles[name]
        bindings = []
        for user_path in sorted(profile.user_paths):
            for subpath in sorted(profile.components):
                component = profile.components[subpath]
                if user_path not in component.user_paths:
                    continue
                avail = clean_versions(profile.avail.anded(profile.user_paths[user_path]).anded(component.avail)).terms
                bindings.append((user_path + subpath, component.type, avail, component.system))
        profiles.append((name, clean_versions(profile.avail).terms, bindings))

    features = {DEFAULT_VERSION, "XR_VERSION_1_1"}
    for _, avail, bindings in profiles:
        for term in avail + [t for b in bindings for t in b[2]]:
            features |= term
    versions = sorted(f for f in features if f.startswith("XR_VERSION_"))
    features = versions + sorted(features - set(versions))
    assert len(features) <= 64, "feature masks are u64"
    index = {f: i for i, f in enumerate(features)}

    avails      = []
    avail_index = {}
    terms       = []

    def avail_id(avail):
        key = tuple(sorted(sum(1 << index[f] for f in term) for term in avail))
        if key not in avail_index:
            avail_index[key] = len(avails)
            avails.append((len(terms), len(key)))
            terms.extend(key)
        return avail_index[key]

    rows         = []
    profile_rows = []
    for name, avail, bindings in profiles:
        first = len(rows)
        for path, type, binding_avail, system in bindings:
            rows.append((path, type, avail_id(binding_avail), system))
        profile_rows.append((name, avail_id(avail), first, len(bindings)))

    out = []
    out.append("// Generated by tools/gen_profiles.py from the OpenXR registry, do not edit")
    out.append("")
    out.append(f"#define OXR_SPEC_FEATURES_COUNT {len(features)}")
    out.append(f"#define OXR_SPEC_PROFILES_COUNT {len(profile_rows)}")
    out.append("")
    out.append("static const char* const oxr_spec_features[OXR_SPEC_FEATURES_COUNT] = {")
    for f in features:
        out.append(f"    \"{f}\",")
    out.append("};")
    out.append("")
    out.append("static const u64 oxr_spec_terms[] = {")
    for t in terms:
        out.append(f"    0x{t:016x},")
    out.append("};")
    out.append("")
    out.append("static const OXR_SpecAvail oxr_spec_avails[] = {")
    for first, count in avails:
        out.append(f"    {{ {first}, {count} }},")
    out.append("};")
    out.append("")
    out.append("static const OXR_SpecBinding oxr_spec_bindings[] = {")
    for path, type, avail, system in rows:
        out.append(f"    {{ \"{path}\", {type}, {avail}, {'true' if system else 'false'} }},")
    out.append("};")
    out.append("")
    out.append("static const OXR_SpecProfile oxr_spec_profiles[OXR_SPEC_PROFILES_COUNT] = {")
    for name, avail, first, count in profile_rows:
        out.append(f"    {{ \"{name}\", {avail}, {first}, {count} }},")
    out.append("};")

    with open(sys.argv[2], "w") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
