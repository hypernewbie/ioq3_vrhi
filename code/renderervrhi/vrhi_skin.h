/*
===========================================================================
VRHI bounded Quake .skin text parser.

Dependency-free (C++11) helper shared by renderer_vrhi and its standalone
unit test. It parses the small "surface,shader" text format used by Quake 3
player/model skins into a bounded list of (lowercased surface name, shader
qpath) entries. It is intentionally NOT a shader/material parser:

  - tokenization mirrors the GL renderers' CommaParse skin loop: whitespace
    (any char <= ' '), ',' and '//' line comments separate tokens, and CRLF
    line endings are plain whitespace;
  - a surface token containing "tag_" is skipped WITHOUT consuming a shader
    token, exactly like the GL renderers, because real .skin files list tag
    surfaces as bare single-token lines;
  - overlong tokens (>= VRHI_SKIN_MAX_QPATH) can never form a usable qpath,
    so the whole malformed entry is skipped: an overlong surface also
    consumes its paired shader token, while an overlong shader drops the
    pending surface, keeping the surface/shader alternation aligned;
  - a surface whose shader token is missing at end-of-text is dropped;
  - surface names are lowercased for case-insensitive MD3 surface matching;
    shader qpaths keep their original case.

Every bound is a compile-time constant below. The parser copies only bounded
strings into caller-owned vectors and never retains a pointer into the input
buffer, so no FS storage can be referenced after FS_FreeFile.
===========================================================================
*/
#ifndef __VRHI_SKIN_H
#define __VRHI_SKIN_H

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

// Strict bounds, independent of the legacy qfiles.h limits so malformed or
// hostile files cannot consume unbounded memory inside the renderer.
static const std::size_t VRHI_SKIN_MAX_FILE_BYTES = 1024u * 1024u; // FS_ReadFile cap
static const std::size_t VRHI_SKIN_MAX_TEXT_BYTES = 1024u * 1024u; // parser input cap
static const std::size_t VRHI_SKIN_MAX_SURFACES = 256;             // entries per skin
static const std::size_t VRHI_SKIN_MAX_QPATH = 64;                 // token length cap

struct VRHI_SkinEntry {
	std::string surface; // lowercased MD3 surface name (never empty, < MAX_QPATH)
	std::string shader;  // shader qpath (original case, never empty, < MAX_QPATH)
};

// Parses up to VRHI_SKIN_MAX_SURFACES valid "surface,shader" entries from
// `textBytes` bytes of .skin text. Returns false only for a null buffer or
// oversized input; true otherwise (entries may legitimately be empty, e.g. a
// file that contains only tag surfaces). On success `entries` holds only
// complete, bounded pairs in file order.
inline bool VRHI_ParseSkinText(const char *text, std::size_t textBytes,
	std::vector<VRHI_SkinEntry> &entries) {
	entries.clear();
	if (text == nullptr || textBytes > VRHI_SKIN_MAX_TEXT_BYTES) return false;
	std::size_t pos = 0;
	std::string surface;
	bool haveSurface = false;
	// Skips whitespace, commas, and '//' line comments; returns false at
	// end-of-text. After a true return `pos` points at a token character.
	const auto skipSeparators = [&]() -> bool {
		for (;;) {
			if (pos >= textBytes) return false;
			const char c = text[pos];
			if (c == '\0') return false; // embedded NUL ends the text safely
			if (c == '/' && pos + 1 < textBytes && text[pos + 1] == '/') {
				while (pos < textBytes && text[pos] != '\n') ++pos;
				continue;
			}
			if (c <= ' ' || c == ',') {
				++pos;
				continue;
			}
			return true;
		}
	};
	// Reads one token: chars with c > ' ' && c != ','. Call only after
	// skipSeparators() returned true, so the returned length is never 0.
	const auto readToken = [&](std::size_t *lengthOut) -> const char * {
		const std::size_t start = pos;
		while (pos < textBytes) {
			const char c = text[pos];
			if (c == '\0' || c <= ' ' || c == ',') break;
			++pos;
		}
		if (lengthOut != nullptr) *lengthOut = pos - start;
		return text + start;
	};
	while (skipSeparators()) {
		std::size_t tokenLength = 0;
		const char *tokenStart = readToken(&tokenLength);
		if (tokenLength == 0) continue;
		if (tokenLength >= VRHI_SKIN_MAX_QPATH) {
			// Overlong: this entry can never form a usable qpath/surface pair.
			// Skip the whole malformed entry so the alternation stays aligned:
			// an overlong surface also consumes its paired shader token, while
			// an overlong shader drops the pending surface.
			if (!haveSurface) {
				if (skipSeparators()) {
					std::size_t discardLength = 0;
					readToken(&discardLength);
				}
			} else {
				surface.clear();
				haveSurface = false;
			}
			continue;
		}
		std::string token(tokenStart, tokenLength);
		if (!haveSurface) {
			// Surface role: lowercase for case-insensitive MD3 surface
			// compares. tag surfaces are skipped without consuming a shader
			// token (GL parity: real .skin files list them as bare lines).
			for (std::size_t i = 0; i < token.size(); ++i) {
				token[i] = static_cast<char>(
					std::tolower(static_cast<unsigned char>(token[i])));
			}
			if (token.find("tag_") != std::string::npos) continue;
			surface.swap(token);
			haveSurface = true;
		} else {
			// Shader role: complete the entry.
			entries.emplace_back();
			entries.back().surface.swap(surface);
			entries.back().shader.swap(token);
			haveSurface = false;
			if (entries.size() >= VRHI_SKIN_MAX_SURFACES) return true; // entry cap
		}
	}
	return true;
}

#endif // __VRHI_SKIN_H
