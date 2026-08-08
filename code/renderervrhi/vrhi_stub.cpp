#include <stdio.h>

#include "q_shared.h"
#include "renderercommon/tr_public.h"

namespace {

void VRHI_Diagnostic(refimport_t *rimp, const char *message) {
	if (rimp != nullptr && rimp->Printf != nullptr) {
		rimp->Printf(PRINT_ALL, "%s", message);
		return;
	}

	fputs(message, stderr);
}

} // namespace

extern "C" Q_EXPORT refexport_t * QDECL GetRefAPI(int apiVersion,
	refimport_t *rimp) {
	if (apiVersion != REF_API_VERSION) {
		VRHI_Diagnostic(rimp,
			"renderer_vrhi stub: mismatched REF_API_VERSION; refusing to load\n");
		return nullptr;
	}

	VRHI_Diagnostic(rimp,
		"renderer_vrhi stub: renderer implementation is not available\n");
	return nullptr;
}
