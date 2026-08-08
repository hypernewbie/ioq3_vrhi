/*
===========================================================================
VRHI bounded Quake shader-script parser.

Dependency-free (C++11) helper shared by renderer_vrhi and its standalone
unit test. It exposes two slices:

  - VRHI_ParseShaderScript / VRHI_ParseShaderBlock: the conservative
    first-stage opaque-diffuse lookup described below;
  - VRHI_ParseShaderScriptStages / VRHI_ParseSimpleShaderBlock: the bounded
    simple multi-stage material slice (at most VRHI_SHADER_MAX_STAGES
    direct-image stages, stage 0 opaque, later stages source-alpha or
    additive) used by static BSP batches.

Both share the same tokenizer and bounds. The first-stage helper parses the
small subset of Quake 3 shader scripts that is semantically equivalent to
the renderer's fixed opaque diffuse pass, so otherwise opaque first-stage
BSP shader definitions no longer fall back to the lightmap/solid path. It
is intentionally NOT a shader/material parser:

  - tokenization mirrors the GL renderers' GetToken loop: whitespace and
    NULs separate tokens, '//' line comments and slash-star block comments
    are skipped, '{'/'}' are single-character tokens, and double-quoted
    tokens may contain whitespace;
  - every list, text buffer, token count, and token length is bounded
    (constants below); malformed or oversized input is rejected safely;
  - a shader block contributes a candidate only when its stage(s) carry
    semantics identical to the fixed opaque diffuse pass: the default-value
    statements `blendFunc GL_ONE GL_ZERO` (exactly that pair), `rgbGen
    identity`, `alphaGen identity`, `depthWrite`, and `depthFunc lequal`/
    `less` are accepted only with their exact validated arguments, while
    alpha/additive blends, tcGen/tcMod variants, deform/fog/animMap/
    videoMap/normal/specular/portal/sky, and any malformed or incomplete
    sequence reject the whole shader;
  - statement validation never reads past the enclosing stage or shader
    body, so an incomplete statement at the end of a stage fails instead
    of borrowing tokens from the next block;
  - the first non-special `map`/`clampmap` TGA/JPG/JPEG/PNG candidate
    (skipping `$lightmap`/`$whiteimage` style special textures) is selected
    and retained as a copied string; a second real map anywhere in the
    shader rejects it, so multi-stage blending is never faked.

Every bound is a compile-time constant below. The parser copies only bounded
strings into caller-owned containers and never retains a pointer into the
input buffer, so no FS storage can be referenced after FS_FreeFile.
===========================================================================
*/
#ifndef __VRHI_SHADER_SCRIPT_H
#define __VRHI_SHADER_SCRIPT_H

#include <cctype>
#include <cstddef>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

// Bounded simple material stages accepted by the VRHI BSP path.
static const std::size_t VRHI_SHADER_MAX_STAGES = 4;
enum VRHI_ShaderBlendMode {
	VRHI_SHADER_BLEND_OPAQUE = 0,
	VRHI_SHADER_BLEND_ALPHA = 1,
	VRHI_SHADER_BLEND_ADDITIVE = 2
};
struct VRHI_ShaderStage {
	std::string path;
	int blendMode = VRHI_SHADER_BLEND_OPAQUE;
};

// Strict bounds, independent of the legacy qfiles.h limits so malformed or
// hostile files cannot consume unbounded memory inside the renderer. The
// qpath bound mirrors the engine's MAX_QPATH (64).
static const std::size_t VRHI_SCRIPT_MAX_QPATH = 64;
static const std::size_t VRHI_SCRIPT_MAX_TEXT_BYTES = 8u * 1024u * 1024u;
static const std::size_t VRHI_SCRIPT_MAX_TOKENS = 131072;
static const std::size_t VRHI_SCRIPT_MAX_TOKEN_BYTES = 1024;

struct VRHI_ShaderScriptToken {
	std::string text;
};

inline std::string VRHI_ShaderLowerASCII(const std::string &text) {
	std::string lower = text;
	for (std::size_t i = 0; i < lower.size(); ++i) {
		lower[i] = static_cast<char>(
			std::tolower(static_cast<unsigned char>(lower[i])));
	}
	return lower;
}

inline bool VRHI_ShaderTokenIs(const VRHI_ShaderScriptToken &token,
	const char *value) {
	return VRHI_ShaderLowerASCII(token.text) == value;
}

// Tokenizes bounded shader-script text into single tokens. Returns false for
// null input, an unclosed block comment or quote, an overlong token, or a
// token count beyond VRHI_SCRIPT_MAX_TOKENS.
inline bool VRHI_TokenizeShaderScript(const unsigned char *data, std::size_t size,
	std::vector<VRHI_ShaderScriptToken> *tokens) {
	if (data == nullptr || tokens == nullptr) return false;
	tokens->clear();
	std::size_t cursor = 0;
	while (cursor < size) {
		const unsigned char ch = data[cursor];
		if (std::isspace(ch) || ch == '\0') {
			++cursor;
			continue;
		}
		if (ch == '/' && cursor + 1 < size && data[cursor + 1] == '/') {
			cursor += 2;
			while (cursor < size && data[cursor] != '\n') ++cursor;
			continue;
		}
		if (ch == '/' && cursor + 1 < size && data[cursor + 1] == '*') {
			cursor += 2;
			bool closed = false;
			while (cursor + 1 < size) {
				if (data[cursor] == '*' && data[cursor + 1] == '/') {
					cursor += 2;
					closed = true;
					break;
				}
				++cursor;
			}
			if (!closed) return false;
			continue;
		}
		if (tokens->size() >= VRHI_SCRIPT_MAX_TOKENS) return false;
		VRHI_ShaderScriptToken token;
		if (ch == '{' || ch == '}') {
			token.text.assign(1, static_cast<char>(ch));
			++cursor;
		} else if (ch == '"') {
			++cursor;
			while (cursor < size && data[cursor] != '"') {
				if (data[cursor] == '\\' && cursor + 1 < size) ++cursor;
				if (cursor >= size ||
					token.text.size() >= VRHI_SCRIPT_MAX_TOKEN_BYTES) return false;
				token.text.push_back(static_cast<char>(data[cursor++]));
			}
			if (cursor >= size) return false;
			++cursor;
		} else {
			while (cursor < size && !std::isspace(data[cursor]) &&
				data[cursor] != '{' && data[cursor] != '}') {
				if (token.text.size() >= VRHI_SCRIPT_MAX_TOKEN_BYTES) return false;
				token.text.push_back(static_cast<char>(data[cursor++]));
			}
		}
		tokens->push_back(std::move(token));
	}
	return true;
}

// Validates a shader/texture qpath: non-empty, shorter than
// VRHI_SCRIPT_MAX_QPATH, no drive letter or traversal, and no empty/'.'/'..'
// path parts. Used for both script paths and map/clampmap texture names.
inline bool VRHI_ShaderPathIsSafe(const std::string &path) {
	if (path.empty() || path.size() >= VRHI_SCRIPT_MAX_QPATH || path[0] == '/' ||
		path[0] == '\\' || path.find(':') != std::string::npos) return false;
	std::size_t begin = 0;
	while (begin <= path.size()) {
		const std::size_t end = path.find('/', begin);
		const std::string part = path.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin);
		if (part.empty() || part == "." || part == "..") return false;
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return true;
}

// Resolves a direct-image name into a retained qpath string. Explicit
// extensions must be tga/jpg/jpeg/png; a bare name is retained as-is and the
// bounded loader probes the four supported suffixes later. Returns false for
// unsafe, empty, overlong, or unsupported-extension names.
inline bool VRHI_ResolveDirectImagePath(const char *name, std::string *resolved) {
	if (resolved != nullptr) resolved->clear();
	if (name == nullptr || resolved == nullptr) return false;
	std::size_t length = 0;
	while (length < VRHI_SCRIPT_MAX_QPATH && name[length] != '\0') ++length;
	if (length == 0 || length >= VRHI_SCRIPT_MAX_QPATH ||
		name[0] == '/' || name[0] == '\\') return false;
	const std::string path(name, length);
	if (path.find("..") != std::string::npos) return false;
	const std::size_t slash = path.find_last_of("/\\");
	const std::size_t dot = path.find_last_of('.');
	const bool hasExtension = dot != std::string::npos &&
		(slash == std::string::npos || dot > slash) && dot + 1 < path.size();
	if (hasExtension) {
		std::string ext = path.substr(dot + 1);
		for (std::size_t i = 0; i < ext.size(); ++i) {
			ext[i] = static_cast<char>(
				std::tolower(static_cast<unsigned char>(ext[i])));
		}
		if (ext != "tga" && ext != "jpg" && ext != "jpeg" && ext != "png")
			return false;
	}
	// A bare name is intentionally retained as-is. The bounded loader below
	// probes the four supported suffixes; explicit extensions are never
	// rewritten.
	*resolved = std::move(path);
	return true;
}

// Token-level keywords that reject a shader even when a valid map is present.
// The allowlisted default-value statements (blendFunc GL_ONE GL_ZERO, rgbGen/
// alphaGen identity, depthWrite, depthFunc lequal/less) are validated
// separately by VRHI_ShaderAllowlistedStatement and are intentionally absent
// from this list.
inline bool VRHI_ShaderTokenIsUnsupported(const VRHI_ShaderScriptToken &token) {
	const std::string lower = VRHI_ShaderLowerASCII(token.text);
	static const char *unsupported[] = {
		"blend", "deform", "deformvertexes", "fogparms", "foggen",
		"alphafunc", "tcgen", "tcmod",
		"animmap", "videomap", "normalmap", "specularmap",
		"polygonoffset", "portal", "skyparms"
	};
	for (const char *value : unsupported) {
		if (lower == value) return true;
	}
	return false;
}

// Validates one allowlisted default-value statement inside `[pos, scopeEnd)`.
// Returns 0 when `pos` is not an allowlisted keyword (the caller keeps its
// normal unsupported/skip policy), SIZE_MAX when the keyword is allowlisted
// but its arguments are missing or unsupported (the whole shader must be
// rejected), and 1..3 when the statement is syntactically complete and
// semantically equivalent to the fixed opaque diffuse pass; the return value
// is the number of tokens the statement consumed. Never reads past scopeEnd.
inline std::size_t VRHI_ShaderAllowlistedStatement(
	const std::vector<VRHI_ShaderScriptToken> &tokens, std::size_t pos,
	std::size_t scopeEnd) {
	if (pos >= scopeEnd || scopeEnd > tokens.size()) return 0;
	const std::string lower = VRHI_ShaderLowerASCII(tokens[pos].text);
	if (lower == "depthwrite") return 1;
	if (lower == "rgbgen" || lower == "alphagen") {
		if (pos + 1 >= scopeEnd) return std::numeric_limits<std::size_t>::max();
		if (VRHI_ShaderLowerASCII(tokens[pos + 1].text) != "identity")
			return std::numeric_limits<std::size_t>::max();
		return 2;
	}
	if (lower == "depthfunc") {
		if (pos + 1 >= scopeEnd) return std::numeric_limits<std::size_t>::max();
		const std::string arg = VRHI_ShaderLowerASCII(tokens[pos + 1].text);
		if (arg != "lequal" && arg != "less")
			return std::numeric_limits<std::size_t>::max();
		return 2;
	}
	if (lower == "blendfunc") {
		if (pos + 2 >= scopeEnd) return std::numeric_limits<std::size_t>::max();
		if (VRHI_ShaderLowerASCII(tokens[pos + 1].text) != "gl_one" ||
			VRHI_ShaderLowerASCII(tokens[pos + 2].text) != "gl_zero")
			return std::numeric_limits<std::size_t>::max();
		return 3;
	}
	return 0;
}

// Parses one shader body `[begin, end)` and returns the first non-special
// map/clampmap candidate (a copied safe qpath), or false when the body uses
// any unsupported statement, any malformed/incomplete statement, no usable
// map, or more than one real map anywhere in the body.
inline bool VRHI_ParseShaderBlock(const std::vector<VRHI_ShaderScriptToken> &tokens,
	std::size_t begin, std::size_t end, std::string *candidate) {
	if (candidate != nullptr) candidate->clear();
	if (candidate == nullptr || begin >= end || end > tokens.size()) return false;
	bool found = false;
	for (std::size_t i = begin; i < end;) {
		if (tokens[i].text != "{") {
			const std::size_t consumed = VRHI_ShaderAllowlistedStatement(tokens, i, end);
			if (consumed == std::numeric_limits<std::size_t>::max()) return false;
			if (consumed > 0) {
				i += consumed;
				continue;
			}
			if (VRHI_ShaderTokenIsUnsupported(tokens[i])) return false;
			++i;
			continue;
		}
		const std::size_t stageBegin = ++i;
		int depth = 1;
		while (i < end && depth > 0) {
			if (tokens[i].text == "{") ++depth;
			else if (tokens[i].text == "}") --depth;
			++i;
		}
		if (depth != 0) return false;
		const std::size_t stageEnd = i - 1;
		std::size_t j = stageBegin;
		while (j < stageEnd) {
			if (VRHI_ShaderTokenIs(tokens[j], "map") ||
				VRHI_ShaderTokenIs(tokens[j], "clampmap")) {
				if (j + 1 >= stageEnd) return false;
				const std::string &texture = tokens[j + 1].text;
				j += 2;
				if (!texture.empty() && texture[0] == '$') continue;
				if (!VRHI_ShaderPathIsSafe(texture) ||
					texture.find('\\') != std::string::npos) return false;
				std::string path;
				if (found || !VRHI_ResolveDirectImagePath(texture.c_str(), &path))
					return false;
				*candidate = std::move(path);
				found = true;
				continue;
			}
			const std::size_t consumed = VRHI_ShaderAllowlistedStatement(tokens, j, stageEnd);
			if (consumed == std::numeric_limits<std::size_t>::max()) return false;
			if (consumed > 0) {
				j += consumed;
				continue;
			}
			if (VRHI_ShaderTokenIsUnsupported(tokens[j])) return false;
			++j;
		}
	}
	return found;
}

// Parses one bounded shader-script text buffer, extracting the first-stage
// opaque candidate for every shader name present in `wanted` (compared
// case-insensitively) into `results`. Returns false only for null input,
// tokenization failure, or unbalanced braces; blocks that reject simply
// produce no result entry.
inline bool VRHI_ParseShaderScript(const unsigned char *data, std::size_t size,
	const std::unordered_map<std::string, bool> &wanted,
	std::unordered_map<std::string, std::string> *results) {
	std::vector<VRHI_ShaderScriptToken> tokens;
	if (!VRHI_TokenizeShaderScript(data, size, &tokens) || results == nullptr)
		return false;
	for (std::size_t i = 0; i < tokens.size();) {
		const std::string shaderName = VRHI_ShaderLowerASCII(tokens[i++].text);
		if (i >= tokens.size() || tokens[i].text != "{") continue;
		const std::size_t bodyBegin = ++i;
		int depth = 1;
		while (i < tokens.size() && depth > 0) {
			if (tokens[i].text == "{") ++depth;
			else if (tokens[i].text == "}") --depth;
			++i;
		}
		if (depth != 0) return false;
		const std::size_t bodyEnd = i - 1;
		if (wanted.find(shaderName) == wanted.end()) continue;
		std::string path;
		if (VRHI_ParseShaderBlock(tokens, bodyBegin, bodyEnd, &path)) {
			results->emplace(shaderName, std::move(path));
		}
	}
	return true;
}

// Parses one bounded shader body for the deliberately small multi-stage
// material slice. Every stage has exactly one real map and no stage may use
// unsupported shader semantics. Stage zero is opaque; subsequent stages may
// use only source-alpha or additive blending. This remains separate from the
// conservative first-stage helper above.
inline bool VRHI_ParseSimpleShaderBlock(
	const std::vector<VRHI_ShaderScriptToken> &tokens, std::size_t begin,
	std::size_t end, std::vector<VRHI_ShaderStage> *stages) {
	if (stages != nullptr) stages->clear();
	if (stages == nullptr || begin >= end || end > tokens.size()) return false;
	for (std::size_t i = begin; i < end;) {
		if (tokens[i].text != "{" || stages->size() >= VRHI_SHADER_MAX_STAGES) return false;
		const std::size_t stageBegin = ++i;
		int depth = 1;
		while (i < end && depth > 0) {
			if (tokens[i].text == "{") ++depth;
			else if (tokens[i].text == "}") --depth;
			++i;
		}
		if (depth != 0) return false;
		const std::size_t stageEnd = i - 1;
		VRHI_ShaderStage stage;
		bool foundMap = false;
		bool blendSpecified = false;
		for (std::size_t j = stageBegin; j < stageEnd;) {
			if (tokens[j].text == "{" || tokens[j].text == "}") return false;
			const std::string keyword = VRHI_ShaderLowerASCII(tokens[j].text);
			if (keyword == "map" || keyword == "clampmap") {
				if (foundMap || j + 1 >= stageEnd) return false;
				const std::string &texture = tokens[j + 1].text;
				if (texture.empty() || texture[0] == '$' ||
					!VRHI_ShaderPathIsSafe(texture) ||
					texture.find('\\') != std::string::npos ||
					!VRHI_ResolveDirectImagePath(texture.c_str(), &stage.path)) return false;
				foundMap = true;
				j += 2;
				continue;
			}
			if (keyword == "blendfunc") {
				if (blendSpecified || j + 1 >= stageEnd) return false;
				const std::string arg = VRHI_ShaderLowerASCII(tokens[j + 1].text);
				std::size_t consumed = 0;
				if (arg == "blend") {
					stage.blendMode = VRHI_SHADER_BLEND_ALPHA;
					consumed = 2;
				} else {
					if (j + 2 >= stageEnd) return false;
					const std::string dst = VRHI_ShaderLowerASCII(tokens[j + 2].text);
					if (arg == "gl_src_alpha" && dst == "gl_one_minus_src_alpha")
						stage.blendMode = VRHI_SHADER_BLEND_ALPHA;
					else if (arg == "gl_one" && dst == "gl_one")
						stage.blendMode = VRHI_SHADER_BLEND_ADDITIVE;
					else if (arg == "gl_one" && dst == "gl_zero")
						stage.blendMode = VRHI_SHADER_BLEND_OPAQUE;
					else return false;
					consumed = 3;
				}
				blendSpecified = true;
				j += consumed;
				continue;
			}
			const std::size_t consumed = VRHI_ShaderAllowlistedStatement(tokens, j, stageEnd);
			if (consumed == std::numeric_limits<std::size_t>::max() || consumed == 0) return false;
			j += consumed;
		}
		if (!foundMap || (stages->empty() && stage.blendMode != VRHI_SHADER_BLEND_OPAQUE) ||
			(!stages->empty() && (!blendSpecified || stage.blendMode == VRHI_SHADER_BLEND_OPAQUE))) return false;
		stages->push_back(std::move(stage));
	}
	return !stages->empty();
}

// Parses every wanted shader and copies all accepted stage paths into results.
inline bool VRHI_ParseShaderScriptStages(
	const unsigned char *data, std::size_t size,
	const std::unordered_map<std::string, bool> &wanted,
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > *results) {
	std::vector<VRHI_ShaderScriptToken> tokens;
	if (results == nullptr || !VRHI_TokenizeShaderScript(data, size, &tokens)) return false;
	for (std::size_t i = 0; i < tokens.size();) {
		const std::string shaderName = VRHI_ShaderLowerASCII(tokens[i++].text);
		if (i >= tokens.size() || tokens[i].text != "{") continue;
		const std::size_t bodyBegin = ++i;
		int depth = 1;
		while (i < tokens.size() && depth > 0) {
			if (tokens[i].text == "{") ++depth;
			else if (tokens[i].text == "}") --depth;
			++i;
		}
		if (depth != 0) return false;
		const std::size_t bodyEnd = i - 1;
		if (wanted.find(shaderName) == wanted.end()) continue;
		std::vector<VRHI_ShaderStage> stages;
		if (VRHI_ParseSimpleShaderBlock(tokens, bodyBegin, bodyEnd, &stages))
			results->emplace(shaderName, std::move(stages));
	}
	return true;
}

#endif // __VRHI_SHADER_SCRIPT_H
