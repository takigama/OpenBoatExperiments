// Compiled in place of secrets.h when that file doesn't exist (see
// full_featured.cpp's __has_include check) - e.g. a fresh clone, or someone
// building the release from source who hasn't filled in their own tag EIKs
// yet. Defines the same KNOWN_TAGS[] structure secrets.h would, just empty -
// so the firmware still builds and runs, it simply won't recognize any
// tags until a real secrets.h is provided.
//
// To add your own tags: copy this file to secrets.h (gitignored, see
// ../.gitignore) and fill in real EIKs, extracted via GoogleFindMyTools -
// see FINDINGS.md section 1.2 for how. Each entry is the 32-byte Identity
// Key printed by the patched decrypt_locations.py, for a tag you actually
// own and have permission to track, e.g.:
//
//   static const KnownTag KNOWN_TAGS[] = {
//       {"My Tag", {0x01, 0x02, 0x03, /* ... 32 bytes total ... */}},
//   };

static const KnownTag KNOWN_TAGS[] = {};
