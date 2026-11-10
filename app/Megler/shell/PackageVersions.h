// The versions of Megler's packages, as numbers (shell/AppPackage.h, which makes and lists them; tested by
// core/tests/mg_shell_windows.cpp).
//
// Megler's own package is "<major>.<minor>.0.<build>" (0.1.0.682). An app's own package had Megler's version until 0.1.0.681:
// an app updated in Megler -- Aurora Store's update of Max, 0.1.0.681 -- needed its package made again by the same Megler,
// and Windows refused the package, 0x80073CFB "the provided package has the same identity as an already-installed package
// but the contents are different". Since 0.1.0.682 an app's own package is "<major>.<minor>.<Megler's build>.<n>", the n-th
// package of the app that build made (0 the first): each build's packages are newer than every package an earlier build
// made (0.1.682.n after 0.1.0.681, 0.1.683.0 after 0.1.682.n), and a build makes an app's package again as an update of its
// own. What made a package is read back from its version (madeBy).
#pragma once

#include <cstdio>
#include <string>

namespace megler::app::packageVersions {

struct Parts {
    unsigned v[4] = {};
};

/// "a.b.c.d" as four numbers (0 where it says none).
inline Parts parse(const std::string& s) {
    Parts p;
    sscanf_s(s.c_str(), "%u.%u.%u.%u", &p.v[0], &p.v[1], &p.v[2], &p.v[3]);
    return p;
}

inline std::string text(unsigned a, unsigned b, unsigned c, unsigned d) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", a, b, c, d);
    return buffer;
}

/// Whether version `a` is older than `b`, as Windows orders them.
inline bool older(const std::string& a, const std::string& b) {
    const Parts x = parse(a), y = parse(b);
    for (int i = 0; i < 4; i++)
        if (x.v[i] != y.v[i]) return x.v[i] < y.v[i];
    return false;
}

/// The version of the Megler that made a package of version `packageVersion`: "0.1.0.682" for an app's "0.1.682.3";
/// Megler's own version, and an app's package made before 0.1.0.682, as they are.
inline std::string madeBy(const std::string& packageVersion) {
    const Parts p = parse(packageVersion);
    return p.v[2] == 0 ? packageVersion : text(p.v[0], p.v[1], 0, p.v[2]);
}

/// The version of the app's package that Megler `megler` makes, the app's package installed now being of
/// `installedVersion` (empty: none): the next of that build when `megler` made the installed one, else its first.
/// A Megler whose own version uses its third place has no room for this: its version, as before 0.1.0.682.
inline std::string next(const std::string& megler, const std::string& installedVersion) {
    const Parts m = parse(megler);
    if (m.v[2] != 0) return megler;
    const Parts i = parse(installedVersion);
    const bool same = !installedVersion.empty() && i.v[0] == m.v[0] && i.v[1] == m.v[1] && i.v[2] == m.v[3];
    return text(m.v[0], m.v[1], m.v[3], same ? i.v[3] + 1 : 0u);
}

} // namespace megler::app::packageVersions
